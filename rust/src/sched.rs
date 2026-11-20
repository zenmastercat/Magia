// SPDX-License-Identifier: BSD-2-Clause
// Copyright (c) 2026 zenmastercat and Magia contributors
//! EEVDF scheduling policy (Earliest Eligible Virtual Deadline First).
//!
//! Idea: Linux 6.6 replaced CFS's pick rule with EEVDF, which goes back to
//! Stoica & Abdel-Wahab (1995). Re-derived here from that description; no
//! Linux code is used. Each runnable thread is an entity with a weight w.
//!
//! * Virtual runtime v advances by `real_ns * 1024 / w` while it runs.
//! * V is the weighted average of v over all queued entities. An entity is
//!   *eligible* when v <= V (its lag `V - v` is not negative: the system
//!   owes it service).
//! * Each entity requests a slice; its virtual deadline is
//!   `v + slice * 1024 / w`. Among eligible entities the earliest deadline
//!   runs.
//! * A thread that sleeps keeps its lag (clamped to about one slice) and is
//!   placed back with it on wakeup, so sleeping neither earns nor loses an
//!   unbounded amount of CPU.
//!
//! Placement detail (re-derived): inserting an entity with weight w at
//! `v = V - l` moves V itself. For the lag to be exactly `l` after the
//! insert we must place it at `V - l*(W+w)/W` where W is the total weight
//! already queued.
//!
//! The C side owns threads and the context switch; it calls in here with
//! IRQs off (SCHED lock) and gets back the thread id to run.

use alloc::collections::BTreeMap;

pub const NICE_0_WEIGHT: u64 = 1024;
pub const DEFAULT_BASE_SLICE_NS: u64 = 3_000_000;
pub const MIN_BASE_SLICE_NS: u64 = 100_000;
pub const MAX_BASE_SLICE_NS: u64 = 100_000_000;
pub const TICK_NS: u64 = 1_000_000;
pub const MIN_WEIGHT: u32 = 1;
pub const MAX_WEIGHT: u32 = 1 << 24;

/// weight(nice) = round(1024 / 1.25^nice), nice in -20..=19.
///
/// Linux uses a hand-tuned table (`sched_prio_to_weight`) built around a
/// ~10% CPU change per nice step; we recompute the plain formula with
/// exact rational arithmetic instead (1.25 = 5/4), which gives e.g.
/// nice 1 -> 819, nice -1 -> 1280, nice 19 -> 15, nice -20 -> 88818.
pub const fn nice_to_weight(nice: i32) -> u32 {
    let n = if nice < -20 { -20 } else if nice > 19 { 19 } else { nice };
    let (num, den) = if n >= 0 {
        (1024u128 * pow(4, n as u32), pow(5, n as u32))
    } else {
        (1024u128 * pow(5, (-n) as u32), pow(4, (-n) as u32))
    };
    ((2 * num + den) / (2 * den)) as u32
}

const fn pow(b: u128, e: u32) -> u128 {
    let mut r = 1u128;
    let mut i = 0;
    while i < e {
        r *= b;
        i += 1;
    }
    r
}

#[derive(Clone, Copy, Debug)]
struct Ent {
    weight: u64,
    vruntime: i64,
    deadline: i64,
    on_rq: bool,
    lag: i64,
    exec_start: u64,
    runtime_ns: u64,
    seq: u64,
}

pub struct Sched {
    ents: BTreeMap<i32, Ent>,
    preset: BTreeMap<i32, u32>,
    current: Option<i32>,
    base_slice: u64,
    last_v: i64,
    seq: u64,
}

impl Default for Sched {
    fn default() -> Self {
        Self::new()
    }
}

impl Sched {
    pub const fn new() -> Self {
        Sched {
            ents: BTreeMap::new(),
            preset: BTreeMap::new(),
            current: None,
            base_slice: DEFAULT_BASE_SLICE_NS,
            last_v: 0,
            seq: 0,
        }
    }

    pub fn base_slice(&self) -> u64 {
        self.base_slice
    }

    pub fn set_base_slice(&mut self, ns: u64) -> bool {
        if !(MIN_BASE_SLICE_NS..=MAX_BASE_SLICE_NS).contains(&ns) {
            return false;
        }
        self.base_slice = ns;
        true
    }

    fn vslice(&self, w: u64) -> i64 {
        (self.base_slice as u128 * NICE_0_WEIGHT as u128 / w as u128) as i64
    }

    fn lag_limit(&self, w: u64) -> i64 {
        self.vslice(w) + (TICK_NS as u128 * NICE_0_WEIGHT as u128 / w as u128) as i64
    }

    /// (sum of weights, sum of weight*vruntime) over queued entities.
    fn sums(&self) -> (i128, i128) {
        let mut w = 0i128;
        let mut wv = 0i128;
        for e in self.ents.values().filter(|e| e.on_rq) {
            w += e.weight as i128;
            wv += e.weight as i128 * e.vruntime as i128;
        }
        (w, wv)
    }

    /// Weighted average virtual runtime V.
    pub fn avg_vruntime(&self) -> i64 {
        let (w, wv) = self.sums();
        if w == 0 { self.last_v } else { wv.div_euclid(w) as i64 }
    }

    fn eligible_ent(&self, e: &Ent) -> bool {
        let (w, wv) = self.sums();
        w == 0 || (e.vruntime as i128) * w <= wv
    }

    pub fn is_eligible(&self, tid: i32) -> bool {
        self.ents.get(&tid).is_some_and(|e| e.on_rq && self.eligible_ent(e))
    }

    fn update_curr(&mut self, now: u64) {
        let Some(c) = self.current else { return };
        let Some(e) = self.ents.get_mut(&c) else {
            self.current = None;
            return;
        };
        let delta = now.saturating_sub(e.exec_start);
        e.exec_start = now;
        e.runtime_ns += delta;
        e.vruntime += (delta as u128 * NICE_0_WEIGHT as u128 / e.weight as u128) as i64;
    }

    fn clamp_w(w: u32) -> u64 {
        w.clamp(MIN_WEIGHT, MAX_WEIGHT) as u64
    }

    /// New runnable entity, placed at V with zero lag. A weight preset by
    /// `set_weight` (from the process table) wins over `weight`.
    pub fn add(&mut self, tid: i32, weight: u32) {
        let w = Self::clamp_w(self.preset.remove(&tid).unwrap_or(weight));
        self.ents.remove(&tid);
        if self.current == Some(tid) {
            self.current = None;
        }
        let v = self.avg_vruntime();
        self.seq += 1;
        let e = Ent {
            weight: w,
            vruntime: v,
            deadline: v + self.vslice(w),
            on_rq: true,
            lag: 0,
            exec_start: 0,
            runtime_ns: 0,
            seq: self.seq,
        };
        self.ents.insert(tid, e);
    }

    pub fn remove(&mut self, tid: i32) {
        if self.ents.contains_key(&tid) && self.ents[&tid].on_rq {
            self.last_v = self.avg_vruntime();
        }
        self.ents.remove(&tid);
        self.preset.remove(&tid);
        if self.current == Some(tid) {
            self.current = None;
        }
    }

    pub fn contains(&self, tid: i32) -> bool {
        self.ents.contains_key(&tid)
    }

    pub fn current(&self) -> Option<i32> {
        self.current
    }

    pub fn runtime_ns(&self, tid: i32) -> u64 {
        self.ents.get(&tid).map_or(0, |e| e.runtime_ns)
    }

    pub fn weight(&self, tid: i32) -> Option<u32> {
        self.ents.get(&tid).map(|e| e.weight as u32).or_else(|| self.preset.get(&tid).copied())
    }

    pub fn lag(&self, tid: i32) -> Option<i64> {
        let e = self.ents.get(&tid)?;
        Some(if e.on_rq { self.avg_vruntime() - e.vruntime } else { e.lag })
    }

    pub fn nr_queued(&self) -> usize {
        self.ents.values().filter(|e| e.on_rq).count()
    }

    fn dequeue(&mut self, tid: i32) {
        let v = self.avg_vruntime();
        let limit = self.lag_limit(self.ents[&tid].weight);
        let e = self.ents.get_mut(&tid).unwrap();
        e.lag = (v - e.vruntime).clamp(-limit, limit);
        e.on_rq = false;
        self.last_v = self.avg_vruntime();
    }

    /// Sleeping -> runnable. Returns true when the woken entity should
    /// preempt the running one (it is eligible and its deadline is earlier),
    /// or when the CPU is idle.
    pub fn wakeup(&mut self, tid: i32, now: u64) -> bool {
        self.update_curr(now);
        let Some(e) = self.ents.get(&tid).copied() else { return false };
        if e.on_rq {
            return false;
        }
        let (wsum, _) = self.sums();
        let v_avg = self.avg_vruntime();
        let lag = if wsum > 0 {
            (e.lag as i128 * (wsum + e.weight as i128) / wsum) as i64
        } else {
            0
        };
        let v = v_avg - lag;
        let d = v + self.vslice(e.weight);
        self.seq += 1;
        let seq = self.seq;
        let ent = self.ents.get_mut(&tid).unwrap();
        ent.vruntime = v;
        ent.deadline = d;
        ent.on_rq = true;
        ent.lag = 0;
        ent.seq = seq;
        match self.current {
            None => true,
            Some(c) if c == tid => false,
            Some(c) => {
                let cd = self.ents[&c].deadline;
                self.eligible_ent(&self.ents[&tid]) && d < cd
            }
        }
    }

    /// Account the previous thread and choose the next one.
    /// `prev` = -1 means the CPU was idle. Returns -1 if nothing is runnable.
    pub fn pick(&mut self, prev: i32, prev_runnable: bool, now: u64) -> i32 {
        if let Some(c) = self.current {
            self.update_curr(now);
            if let Some(e) = self.ents.get(&c).copied() {
                if e.vruntime >= e.deadline {
                    // slice used up: issue a new request
                    let d = e.vruntime + self.vslice(e.weight);
                    self.ents.get_mut(&c).unwrap().deadline = d;
                }
            }
        }
        if prev >= 0 && !prev_runnable && self.ents.get(&prev).is_some_and(|e| e.on_rq) {
            self.dequeue(prev);
        }
        self.current = None;

        let (wsum, wv) = self.sums();
        let mut best: Option<(i32, i64, u64)> = None;
        for (&tid, e) in self.ents.iter().filter(|(_, e)| e.on_rq) {
            if (e.vruntime as i128) * wsum > wv {
                continue; // not eligible
            }
            if best.is_none_or(|(_, d, s)| (e.deadline, e.seq) < (d, s)) {
                best = Some((tid, e.deadline, e.seq));
            }
        }
        if best.is_none() {
            // Cannot happen with a non-empty queue (the minimum v is always
            // eligible) except for rounding; fall back to the minimum v.
            best = self
                .ents
                .iter()
                .filter(|(_, e)| e.on_rq)
                .min_by_key(|(_, e)| (e.vruntime, e.seq))
                .map(|(&t, e)| (t, e.deadline, e.seq));
        }
        match best {
            Some((tid, _, _)) => {
                self.current = Some(tid);
                self.ents.get_mut(&tid).unwrap().exec_start = now;
                self.last_v = self.avg_vruntime();
                tid
            }
            None => -1,
        }
    }

    /// Timer tick. True when the running entity has used up its slice (it
    /// reached its virtual deadline) or the CPU is idle with work queued.
    pub fn tick(&mut self, now: u64) -> bool {
        match self.current {
            Some(c) => {
                self.update_curr(now);
                self.ents.get(&c).is_some_and(|e| e.vruntime >= e.deadline)
                    || !self.ents.contains_key(&c)
            }
            None => self.nr_queued() > 0,
        }
    }

    /// Give up the rest of the slice: the deadline moves one slice later
    /// than a fresh request would get, so equally placed peers go first.
    pub fn yield_(&mut self, tid: i32) {
        let Some(e) = self.ents.get(&tid).copied() else { return };
        let d = e.vruntime + 2 * self.vslice(e.weight);
        self.ents.get_mut(&tid).unwrap().deadline = d;
    }

    /// Change the weight, keeping the entity's service lag (lag*weight is
    /// what the entity is owed in real time, so the virtual lag scales by
    /// old/new). Unknown tids keep the weight for a later `add`.
    pub fn set_weight(&mut self, tid: i32, weight: u32) {
        let w = Self::clamp_w(weight);
        let Some(e) = self.ents.get(&tid).copied() else {
            self.preset.insert(tid, w as u32);
            return;
        };
        let vs = self.vslice(w);
        if e.on_rq {
            let v_avg = self.avg_vruntime();
            let lag = (v_avg - e.vruntime) as i128 * e.weight as i128 / w as i128;
            let ent = self.ents.get_mut(&tid).unwrap();
            ent.vruntime = v_avg - lag as i64;
            ent.deadline = ent.vruntime + vs;
            ent.weight = w;
        } else {
            let ent = self.ents.get_mut(&tid).unwrap();
            ent.lag = (e.lag as i128 * e.weight as i128 / w as i128) as i64;
            ent.weight = w;
        }
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    const MS: u64 = 1_000_000;

    /// Run `ms` milliseconds of simulated time with a 1 ms tick.
    fn simulate(s: &mut Sched, cur: &mut i32, now: &mut u64, ms: u64) {
        for _ in 0..ms {
            *now += MS;
            if s.tick(*now) {
                *cur = s.pick(*cur, true, *now);
            }
        }
    }

    #[test]
    fn nice_weights_follow_formula() {
        assert_eq!(nice_to_weight(0), 1024);
        assert_eq!(nice_to_weight(1), 819);
        assert_eq!(nice_to_weight(-1), 1280);
        assert_eq!(nice_to_weight(5), 336);
        assert_eq!(nice_to_weight(19), 15);
        assert_eq!(nice_to_weight(-20), 88818);
        assert_eq!(nice_to_weight(100), 15); // clamped
        for n in -20..19 {
            assert!(nice_to_weight(n) > nice_to_weight(n + 1));
        }
    }

    #[test]
    fn empty_queue_picks_nothing() {
        let mut s = Sched::new();
        assert_eq!(s.pick(-1, false, 0), -1);
        assert!(!s.tick(MS));
    }

    #[test]
    fn single_entity_runs_and_is_charged() {
        let mut s = Sched::new();
        s.add(1, 1024);
        assert_eq!(s.pick(-1, false, 0), 1);
        assert!(!s.tick(MS));
        assert!(s.tick(3 * MS)); // 3 ms slice used up
        assert_eq!(s.pick(1, true, 3 * MS), 1);
        assert_eq!(s.runtime_ns(1), 3 * MS);
    }

    #[test]
    fn equal_weights_share_equally() {
        let mut s = Sched::new();
        s.add(1, 1024);
        s.add(2, 1024);
        let (mut now, mut cur) = (0, -1);
        cur = s.pick(cur, true, now);
        simulate(&mut s, &mut cur, &mut now, 3000);
        s.pick(cur, true, now);
        let (a, b) = (s.runtime_ns(1), s.runtime_ns(2));
        assert_eq!(a + b, 3000 * MS);
        assert!(a.abs_diff(b) <= 3 * MS, "a={a} b={b}");
    }

    #[test]
    fn double_weight_gets_double_cpu() {
        let mut s = Sched::new();
        s.add(1, 1024);
        s.add(2, 2048);
        let (mut now, mut cur) = (0, -1);
        cur = s.pick(cur, true, now);
        simulate(&mut s, &mut cur, &mut now, 6000);
        s.pick(cur, true, now);
        let (a, b) = (s.runtime_ns(1) as f64, s.runtime_ns(2) as f64);
        let ratio = b / a;
        assert!((1.9..=2.1).contains(&ratio), "ratio {ratio}");
    }

    #[test]
    fn three_way_weights_are_proportional() {
        let mut s = Sched::new();
        s.add(1, 1024);
        s.add(2, 2048);
        s.add(3, 4096);
        let (mut now, mut cur) = (0, -1);
        cur = s.pick(cur, true, now);
        simulate(&mut s, &mut cur, &mut now, 14000);
        s.pick(cur, true, now);
        let t: Vec<f64> = (1..=3).map(|i| s.runtime_ns(i) as f64).collect();
        assert!((1.85..=2.15).contains(&(t[1] / t[0])), "{t:?}");
        assert!((3.7..=4.3).contains(&(t[2] / t[0])), "{t:?}");
    }

    #[test]
    fn new_entity_starts_at_average_with_zero_lag() {
        let mut s = Sched::new();
        s.add(1, 1024);
        let (mut now, mut cur) = (0, -1);
        cur = s.pick(cur, true, now);
        simulate(&mut s, &mut cur, &mut now, 50);
        s.add(2, 1024);
        assert_eq!(s.lag(2), Some(0));
        assert!(s.is_eligible(2));
    }

    #[test]
    fn sleeper_keeps_bounded_lag() {
        let mut s = Sched::new();
        s.add(1, 1024);
        s.add(2, 1024);
        let mut now = 0;
        let mut cur = s.pick(-1, false, now);
        // whoever runs first sleeps after 1 ms, before using its slice
        now += MS;
        let sleeper = cur;
        let other = 3 - sleeper;
        cur = s.pick(cur, false, now);
        assert_eq!(cur, other);
        let lag_at_sleep = s.lag(sleeper).unwrap();
        // the other runs alone for a long time; the sleeper's lag stays put
        simulate(&mut s, &mut cur, &mut now, 1000);
        assert_eq!(s.lag(sleeper).unwrap(), lag_at_sleep);
        assert!(lag_at_sleep.abs() <= s.lag_limit(1024));
        // on wakeup it gets the same lag back (placement compensation)
        s.wakeup(sleeper, now);
        let l = s.lag(sleeper).unwrap();
        assert!((l - lag_at_sleep).abs() <= 1, "lag {l} vs {lag_at_sleep}");
    }

    #[test]
    fn lag_is_clamped_on_sleep() {
        let mut s = Sched::new();
        s.add(1, 1024);
        s.add(2, 1024);
        let mut now = 0;
        let cur = s.pick(-1, false, now);
        let other = 3 - cur;
        // force the other entity far behind by running cur a long time
        // without letting it go: simulate a slice that is never cut
        now += 500 * MS;
        let c2 = s.pick(cur, true, now);
        assert_eq!(c2, other);
        s.pick(other, false, now); // other sleeps immediately with big lag
        assert!(s.lag(other).unwrap() <= s.lag_limit(1024));
    }

    #[test]
    fn wakeup_preempts_when_eligible_with_earlier_deadline() {
        let mut s = Sched::new();
        s.add(1, 1024);
        s.add(2, 1024);
        assert_eq!(s.pick(-1, false, 0), 1);
        assert!(s.tick(3 * MS));
        assert_eq!(s.pick(1, true, 3 * MS), 2); // 1 renewed its deadline
        assert_eq!(s.pick(2, false, 3 * MS), 1); // 2 sleeps with positive lag
        assert!(s.lag(2).unwrap() > 0);
        assert!(s.wakeup(2, 4 * MS), "positive-lag waker with earlier deadline preempts");
        // waking an already-queued entity does nothing
        assert!(!s.wakeup(2, 4 * MS));
        // unknown tid
        assert!(!s.wakeup(99, 4 * MS));
    }

    #[test]
    fn wakeup_without_earlier_deadline_does_not_preempt() {
        let mut s = Sched::new();
        s.add(1, 1024);
        s.add(2, 1024);
        assert_eq!(s.pick(-1, false, 0), 1);
        assert_eq!(s.pick(1, true, MS), 2);
        assert_eq!(s.pick(2, false, MS), 1); // 2 sleeps at once
        // 1 still has its first deadline (3 ms), 2 would get 1+3 ms
        assert!(!s.wakeup(2, 2 * MS));
    }

    #[test]
    fn wakeup_on_idle_cpu_requests_resched() {
        let mut s = Sched::new();
        s.add(1, 1024);
        let t = s.pick(-1, false, 0);
        assert_eq!(s.pick(t, false, MS), -1);
        assert!(s.wakeup(1, 2 * MS));
        assert!(s.tick(3 * MS)); // idle with work queued
        assert_eq!(s.pick(-1, false, 3 * MS), 1);
    }

    #[test]
    fn ineligible_entity_is_not_picked() {
        let mut s = Sched::new();
        s.add(1, 1024);
        s.add(2, 1024);
        let mut now = 0;
        let a = s.pick(-1, false, now);
        now += 3 * MS;
        let b = s.pick(a, true, now);
        assert_ne!(a, b);
        assert!(!s.is_eligible(a), "a ran ahead of V");
        now += MS;
        // b used only 1 ms of its slice but a is ineligible anyway
        assert_eq!(s.pick(b, true, now), b);
    }

    #[test]
    fn yield_lets_peer_run_first() {
        let mut s = Sched::new();
        s.add(1, 1024);
        s.add(2, 1024);
        let a = s.pick(-1, false, 0);
        s.yield_(a);
        let b = s.pick(a, true, 0);
        assert_eq!(b, 3 - a);
        // yield of unknown tid is ignored
        s.yield_(77);
    }

    #[test]
    fn remove_and_unknown_tids_are_harmless() {
        let mut s = Sched::new();
        s.add(1, 1024);
        s.remove(55);
        assert_eq!(s.pick(55, false, 0), 1);
        s.remove(1);
        assert_eq!(s.current(), None);
        assert!(!s.contains(1));
        assert_eq!(s.pick(1, true, MS), -1);
        assert_eq!(s.runtime_ns(1), 0);
    }

    #[test]
    fn preset_weight_wins_over_add_weight() {
        let mut s = Sched::new();
        s.set_weight(5, 2048);
        s.add(5, 1024);
        assert_eq!(s.weight(5), Some(2048));
        s.set_weight(5, 0); // clamped to 1
        assert_eq!(s.weight(5), Some(1));
    }

    #[test]
    fn reweight_changes_share() {
        let mut s = Sched::new();
        s.add(1, 1024);
        s.add(2, 1024);
        s.set_weight(2, 3072);
        let (mut now, mut cur) = (0, -1);
        cur = s.pick(cur, true, now);
        simulate(&mut s, &mut cur, &mut now, 8000);
        s.pick(cur, true, now);
        let r = s.runtime_ns(2) as f64 / s.runtime_ns(1) as f64;
        assert!((2.8..=3.2).contains(&r), "ratio {r}");
    }

    #[test]
    fn base_slice_limits() {
        let mut s = Sched::new();
        assert!(!s.set_base_slice(99_999));
        assert!(!s.set_base_slice(100_000_001));
        assert!(s.set_base_slice(1_000_000));
        s.add(1, 1024);
        s.add(2, 1024);
        let a = s.pick(-1, false, 0);
        assert!(s.tick(MS)); // 1 ms slice
        assert_eq!(s.pick(a, true, MS), 3 - a);
    }
}
