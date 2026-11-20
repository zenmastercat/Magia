// SPDX-License-Identifier: BSD-2-Clause
// Copyright (c) 2026 zenmastercat and Magia contributors
//! Typed user-memory access. Every user structure is copied into kernel
//! memory exactly once and validated there (no double fetch).

use crate::abi::*;
use crate::kenv;
use alloc::vec;
use alloc::vec::Vec;

/// A user range must be non-null, must not wrap, and must end at or below
/// USER_TOP. copyin/copyout additionally fail on unmapped pages.
pub fn check_range(uaddr: u64, len: u64) -> KResult<()> {
    if len == 0 {
        return Ok(());
    }
    if uaddr == 0 {
        return Err(EFAULT);
    }
    let end = uaddr.checked_add(len).ok_or(EFAULT)?;
    if end > USER_TOP {
        return Err(EFAULT);
    }
    Ok(())
}

pub fn copyin(dst: &mut [u8], uaddr: u64) -> KResult<()> {
    check_range(uaddr, dst.len() as u64)?;
    kenv::copyin(dst, uaddr)
}

pub fn copyout(uaddr: u64, src: &[u8]) -> KResult<()> {
    check_range(uaddr, src.len() as u64)?;
    kenv::copyout(uaddr, src)
}

pub fn copyin_val<T: Pod>(uaddr: u64) -> KResult<T> {
    let mut v = T::zeroed();
    copyin(v.as_bytes_mut(), uaddr)?;
    Ok(v)
}

pub fn copyout_val<T: Pod>(uaddr: u64, v: &T) -> KResult<()> {
    copyout(uaddr, v.as_bytes())
}

pub fn copyin_vec(uaddr: u64, len: usize) -> KResult<Vec<u8>> {
    let mut v = vec![0u8; len];
    copyin(&mut v, uaddr)?;
    Ok(v)
}

/// Copy a NUL-terminated string of at most `max - 1` bytes. Reads page by
/// page so it never touches memory past the terminator's page.
/// ENAMETOOLONG when no NUL is found within `max` bytes.
pub fn copyin_str(uaddr: u64, max: usize) -> KResult<Vec<u8>> {
    if uaddr == 0 || uaddr >= USER_TOP {
        return Err(EFAULT);
    }
    let mut out = Vec::new();
    let mut a = uaddr;
    while out.len() < max {
        let to_page_end = (PAGE_SIZE - (a % PAGE_SIZE)) as usize;
        let n = to_page_end.min(max - out.len()).min(64);
        let n = n.min((USER_TOP - a) as usize);
        if n == 0 {
            return Err(EFAULT);
        }
        let mut chunk = [0u8; 64];
        copyin(&mut chunk[..n], a)?;
        if let Some(z) = chunk[..n].iter().position(|&b| b == 0) {
            out.extend_from_slice(&chunk[..z]);
            return Ok(out);
        }
        out.extend_from_slice(&chunk[..n]);
        a += n as u64;
    }
    Err(ENAMETOOLONG)
}

#[cfg(test)]
mod tests {
    use super::*;
    use crate::kenv::mock::{umap, ustr, uwrite};

    #[test]
    fn range_checks() {
        assert_eq!(check_range(0, 1), Err(EFAULT));
        assert_eq!(check_range(0, 0), Ok(()));
        assert_eq!(check_range(USER_TOP - 4, 8), Err(EFAULT));
        assert_eq!(check_range(u64::MAX - 2, 8), Err(EFAULT));
        assert_eq!(check_range(0xffff_8000_0000_0000, 8), Err(EFAULT));
        assert_eq!(check_range(0x1000, 8), Ok(()));
    }

    #[test]
    fn copy_unmapped_faults() {
        let mut b = [0u8; 4];
        assert_eq!(copyin(&mut b, 0x5000), Err(EFAULT));
        assert_eq!(copyout(0x5000, &b), Err(EFAULT));
    }

    #[test]
    fn strings() {
        let u = ustr("hello");
        assert_eq!(copyin_str(u, 16).unwrap(), b"hello");
        assert_eq!(copyin_str(u, 5), Err(ENAMETOOLONG));
        assert_eq!(copyin_str(u, 6).unwrap(), b"hello");
        assert_eq!(copyin_str(0, 6), Err(EFAULT));
        // string running into an unmapped page
        let p = umap(4096);
        uwrite(p, &[b'a'; 4096]);
        let r = copyin_str(p, 8192);
        assert!(r == Err(EFAULT) || r.as_ref().map(|v| v.len() > 4096).unwrap_or(false));
    }

    #[test]
    fn struct_roundtrip() {
        let u = umap(64);
        let k = KEvent { ident: 7, filter: EVFILT_USER, flags: EV_ADD, fflags: 0, data: -3, udata: 9 };
        copyout_val(u, &k).unwrap();
        let back: KEvent = copyin_val(u).unwrap();
        assert_eq!(back, k);
    }
}
