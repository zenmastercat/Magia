// SPDX-License-Identifier: BSD-2-Clause
// Copyright (c) 2026 zenmastercat and Magia contributors
//! Hierarchical jail memory accounting.
//!
//! Idea: Linux cgroup v2 `memory.max` (a charge succeeds only if no
//! ancestor would exceed its limit) folded into FreeBSD-style jails
//! (FreeBSD's RCTL/racct does per-jail limits too). Kept separate from the
//! jail table and behind its own lock (ACCT) because the C page allocator
//! charges user pages from inside `uvm_alloc`, possibly while Rust already
//! holds GIANT.

use crate::abi::*;
use alloc::collections::BTreeMap;

#[derive(Clone, Copy, Debug)]
pub struct AcctJail {
    pub parent: i32,
    /// 0 = no own limit (ancestors may still limit).
    pub mem_max: u64,
    /// Bytes charged to this jail and all its descendants.
    pub mem_cur: u64,
}

pub struct Acct {
    jails: BTreeMap<i32, AcctJail>,
}

impl Default for Acct {
    fn default() -> Self {
        Self::new()
    }
}

impl Acct {
    pub const fn new() -> Self {
        Acct { jails: BTreeMap::new() }
    }

    pub fn add_jail(&mut self, jid: i32, parent: i32, mem_max: u64) {
        self.jails.insert(jid, AcctJail { parent, mem_max, mem_cur: 0 });
    }

    pub fn set_max(&mut self, jid: i32, mem_max: u64) {
        if let Some(j) = self.jails.get_mut(&jid) {
            j.mem_max = mem_max;
        }
    }

    pub fn get(&self, jid: i32) -> Option<AcctJail> {
        self.jails.get(&jid).copied()
    }

    /// Charge `bytes` to `jid` and every ancestor, or fail with ENOMEM
    /// without changing anything if any of them would exceed its limit.
    pub fn charge(&mut self, jid: i32, bytes: u64) -> KResult<()> {
        if !self.jails.contains_key(&jid) {
            // Before rs_init creates jail 0, C may already allocate user
            // pages for the root jail: allow them uncounted.
            return if jid == 0 && self.jails.is_empty() { Ok(()) } else { Err(EINVAL) };
        }
        let mut j = jid;
        while j >= 0 {
            let a = &self.jails[&j];
            let new = a.mem_cur.checked_add(bytes).ok_or(ENOMEM)?;
            if a.mem_max != 0 && new > a.mem_max {
                return Err(ENOMEM);
            }
            j = a.parent;
        }
        let mut j = jid;
        while j >= 0 {
            let a = self.jails.get_mut(&j).unwrap();
            a.mem_cur += bytes;
            j = a.parent;
        }
        Ok(())
    }

    pub fn uncharge(&mut self, jid: i32, bytes: u64) {
        let mut j = jid;
        while j >= 0 {
            let Some(a) = self.jails.get_mut(&j) else { return };
            a.mem_cur = a.mem_cur.saturating_sub(bytes);
            j = a.parent;
        }
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    fn tree() -> Acct {
        let mut a = Acct::new();
        a.add_jail(0, -1, 0);
        a.add_jail(1, 0, 10000);
        a.add_jail(2, 1, 0);
        a
    }

    #[test]
    fn charge_propagates_to_ancestors() {
        let mut a = tree();
        a.charge(2, 4096).unwrap();
        assert_eq!(a.get(2).unwrap().mem_cur, 4096);
        assert_eq!(a.get(1).unwrap().mem_cur, 4096);
        assert_eq!(a.get(0).unwrap().mem_cur, 4096);
        a.uncharge(2, 4096);
        assert_eq!(a.get(0).unwrap().mem_cur, 0);
    }

    #[test]
    fn ancestor_limit_rejects_without_partial_charge() {
        let mut a = tree();
        a.charge(2, 8192).unwrap();
        assert_eq!(a.charge(2, 4096), Err(ENOMEM));
        assert_eq!(a.get(2).unwrap().mem_cur, 8192);
        assert_eq!(a.get(0).unwrap().mem_cur, 8192);
        // sibling of nothing: jail 0 itself is unlimited
        a.charge(0, 1 << 30).unwrap();
    }

    #[test]
    fn unknown_jail_and_overflow() {
        let mut a = tree();
        assert_eq!(a.charge(9, 1), Err(EINVAL));
        assert_eq!(Acct::new().charge(0, 4096), Ok(()), "root jail before init");
        assert_eq!(Acct::new().charge(1, 4096), Err(EINVAL));
        a.set_max(1, 0);
        a.charge(2, 1).unwrap();
        assert_eq!(a.charge(2, u64::MAX), Err(ENOMEM));
        a.uncharge(2, u64::MAX); // saturates
        assert_eq!(a.get(2).unwrap().mem_cur, 0);
    }
}
