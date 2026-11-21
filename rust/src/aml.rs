// SPDX-License-Identifier: BSD-2-Clause
// Copyright (c) 2026 zenmastercat and Magia contributors
//! A minimal ACPI Machine Language reader: the `\_S5` (soft-off) package of
//! a DSDT or SSDT, for poweroff through PM1a/PM1b control (ACPI 6.5 7.4.2,
//! 16.1.7). Not an interpreter: it scans the AML body for
//! `NameOp [\ ^...] _S5_ PackageOp` and reads the first two data objects
//! (ACPI 6.5 20.2: PkgLength, ZeroOp, OneOp, Byte/Word/DWord/QWordPrefix).
//! The table is untrusted firmware input: every read is bounds-checked, no
//! panics, every loop bounded by the table length. A `\_S5` built by a
//! method or at run time is not seen (NotFound; poweroff then says so).
//!
//! Since review 2b L2 the search walks the definition block object by
//! object, so `_S5_` bytes inside another object's data do not match; on AML
//! the walk does not know it falls back to the byte scan and says so.
//!
//! First draft by the local model (qwen3.6:27b); reviewed and fixed: wide
//! values were cut to 8 bits before the SLP_TYP range check, OnesOp gave
//! BadPackage instead of BadValue, its test tables had a 37-byte header.

#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum AmlError {
    Short,
    BadSignature,
    BadLength,
    NotFound,
    BadPackage,
    BadValue,
    BadTable, // the DSDT/SSDT itself unreadable (length, checksum, unmapped): set by acpi.rs
}

#[derive(Debug, Clone, Copy, PartialEq, Eq, Default)]
pub struct SleepType {
    pub slp_typa: u8,
    pub slp_typb: u8,
}

/// Reads the PkgLength value from the AML stream.
/// Returns (value, number_of_bytes_consumed).
/// Returns None if the length calculation overflows or exceeds bounds.
fn read_pkg_len(bytes: &[u8]) -> Option<(usize, usize)> {
    let lead = bytes.first()?;
    let n = (*lead >> 6) as usize; // bits 7-6

    if n == 0 {
        let val = (*lead & 0x3f) as usize;
        Some((val, 1))
    } else {
        // We need n more bytes
        if bytes.len() < 1 + n {
            return None;
        }

        // the lead's low nibble, then each extra byte 8 bits higher:
        // bits 4-11, 12-19, 20-27 (n <= 3, so no shift overflows); the draft
        // shifted by 4, 8, 12, wrong from the second extra byte on (found
        // by the object-level walk on QEMU's DSDT, review 2b L2)
        let mut val = (*lead & 0x0f) as usize;
        for i in 0..n {
            val |= (bytes[1 + i] as usize) << (4 + 8 * i);
        }
        Some((val, 1 + n))
    }
}

/// Reads one data object: Ok((value, bytes consumed)). The full width is
/// kept so the SLP_TYP range check sees what firmware wrote.
fn read_element(bytes: &[u8]) -> Result<(u64, usize), AmlError> {
    let le = |n: usize| -> Result<u64, AmlError> {
        let b = bytes.get(1..1 + n).ok_or(AmlError::BadPackage)?;
        Ok(b.iter().rev().fold(0u64, |v, &x| v << 8 | x as u64))
    };
    match bytes.first() {
        Some(0x00) => Ok((0, 1)),                 // ZeroOp
        Some(0x01) => Ok((1, 1)),                 // OneOp
        Some(0xff) => Err(AmlError::BadValue),    // OnesOp: all ones, never a sleep type
        Some(0x0a) => Ok((le(1)?, 2)),            // BytePrefix
        Some(0x0b) => Ok((le(2)?, 3)),            // WordPrefix
        Some(0x0c) => Ok((le(4)?, 5)),            // DWordPrefix
        Some(0x0e) => Ok((le(8)?, 9)),            // QWordPrefix
        _ => Err(AmlError::BadPackage),           // anything else (or nothing)
    }
}

/// A sleep type value: SLP_TYP is 3 bits.
fn slp_typ(v: u64) -> Result<u8, AmlError> {
    if v > 7 { Err(AmlError::BadValue) } else { Ok(v as u8) }
}

/// `\_S5`'s sleep types. A `Name(_S5_, Package)` found by the
/// object-level walk wins; if the walk gets stuck on AML it does not know,
/// the byte scan is the fallback (see `find_s5_ex`).
pub fn find_s5(table: &[u8]) -> Result<SleepType, AmlError> {
    find_s5_ex(table).map(|(s, _)| s)
}

/// As `find_s5`; the bool is true when the result came from the byte-scan
/// fallback rather than the object-level walk (review 2b L2: a scan can
/// match `_S5_` bytes inside another object's data).
pub fn find_s5_ex(table: &[u8]) -> Result<(SleepType, bool), AmlError> {
    find_s5_walk(table).map(|(s, stuck)| (s, stuck.is_some()))
}

/// As `find_s5_ex`, with where the walk stopped (offset in the table, the
/// byte there) when the byte-scan fallback was used.
pub fn find_s5_walk(table: &[u8]) -> Result<(SleepType, Option<(usize, u8)>), AmlError> {
    if table.len() < 36 {
        return Err(AmlError::Short);
    }
    let sig = &table[0..4];
    if sig != b"DSDT" && sig != b"SSDT" {
        return Err(AmlError::BadSignature);
    }
    let length = u32::from_le_bytes([table[4], table[5], table[6], table[7]]) as usize;
    if length < 36 || length > table.len() {
        return Err(AmlError::BadLength);
    }
    let body = &table[36..length];
    match walk(body, 0, 0) {
        Walk::Found(r) => r.map(|s| (s, None)),
        Walk::NotHere => Err(AmlError::NotFound),
        Walk::Stuck(at, op) => scan_s5(body).map(|s| (s, Some((36 + at, op)))),
    }
}

/// The `\_S5` package whose PkgLength lead byte is at `b[at]` (right after
/// the PackageOp): NumElements, then the first two integers.
fn parse_s5_package(b: &[u8], at: usize) -> Result<SleepType, AmlError> {
    let (pkg_len, pkg_len_consumed) = read_pkg_len(b.get(at..).ok_or(AmlError::BadPackage)?).ok_or(AmlError::BadPackage)?;
    // PkgLength counts itself and everything after it
    let pkg_end = at.checked_add(pkg_len).ok_or(AmlError::BadPackage)?;
    if pkg_end > b.len() {
        return Err(AmlError::BadPackage);
    }
    let pkg_content_start = at + pkg_len_consumed;
    if pkg_content_start >= pkg_end {
        return Err(AmlError::BadPackage);
    }
    let num_elements = b[pkg_content_start];
    if num_elements == 0 {
        return Err(AmlError::BadPackage);
    }
    // elements must lie inside the package: read from a slice ending there
    let pkg = b.get(pkg_content_start + 1..pkg_end).ok_or(AmlError::BadPackage)?;
    let (va, used) = read_element(pkg)?;
    let slp_typa = slp_typ(va)?;
    let slp_typb = if num_elements >= 2 {
        let rest = pkg.get(used..).ok_or(AmlError::BadPackage)?;
        slp_typ(read_element(rest)?.0)?
    } else {
        slp_typa // one element: the same value for PM1b
    };
    Ok(SleepType { slp_typa, slp_typb })
}

/// The fallback: `NameOp [\ ^]* _S5_ PackageOp` at any byte offset; the
/// first match wins.
fn scan_s5(body: &[u8]) -> Result<SleepType, AmlError> {
    let mut idx = 0;
    while idx < body.len() {
        if body.get(idx) != Some(&0x08) {
            idx += 1;
            continue;
        }
        let name_op_start = idx;
        idx += 1;
        let mut prefix_count = 0;
        while prefix_count < 8 {
            match body.get(idx) {
                Some(&0x5c) | Some(&0x5e) => {
                    idx += 1;
                    prefix_count += 1;
                }
                _ => break,
            }
        }
        if body.get(idx..idx + 4) != Some(&b"_S5_"[..]) || body.get(idx + 4) != Some(&0x12) {
            idx = name_op_start + 1;
            continue;
        }
        return parse_s5_package(body, idx + 5);
    }
    Err(AmlError::NotFound)
}

// ---------------------------------------------------------------- object-level walk (review 2b L2)
//
// First draft of the walk by the local model (qwen3.6:27b, second try);
// reviewed and fixed: integer data objects were one byte too long, a
// name running past its Scope/Device package could panic the slice, its
// own PkgLength reader was not ACPI's and was dropped, its tests rewritten.

#[derive(Debug, PartialEq, Eq)]
enum Walk {
    Found(Result<SleepType, AmlError>),
    NotHere,
    Stuck(usize, u8), // offset in the AML body and the byte the walk did not know
}

const WALK_MAX_DEPTH: u32 = 16;

fn is_name_lead(c: u8) -> bool {
    c.is_ascii_uppercase() || c == b'_'
}

fn seg(b: &[u8], at: usize) -> Option<[u8; 4]> {
    let s: [u8; 4] = b.get(at..at.checked_add(4)?)?.try_into().ok()?;
    if is_name_lead(s[0]) { Some(s) } else { None }
}

/// NameString at b[at] (ACPI 6.5 20.2.2): (last NameSeg if any, bytes used).
fn name_string(b: &[u8], at: usize) -> Option<(Option<[u8; 4]>, usize)> {
    let mut o = at;
    if *b.get(o)? == 0x5c {
        o += 1;
    } else {
        let mut n = 0;
        while b.get(o) == Some(&0x5e) && n < 255 {
            o += 1;
            n += 1;
        }
    }
    let (last, end) = match *b.get(o)? {
        0x00 => (None, o + 1),
        0x2e => {
            seg(b, o + 1)?;
            (Some(seg(b, o + 5)?), o + 9)
        }
        0x2f => {
            let count = *b.get(o + 1)? as usize;
            if count == 0 {
                return None;
            }
            let mut last = None;
            for k in 0..count {
                last = Some(seg(b, o + 2 + 4 * k)?);
            }
            (last, o + 2 + 4 * count)
        }
        _ => (Some(seg(b, o)?), o + 4),
    };
    Some((last, end - at))
}

/// PkgLength at b[at]: (end of the package, first byte after the PkgLength).
fn pkg_end(b: &[u8], at: usize) -> Option<(usize, usize)> {
    let (len, used) = read_pkg_len(b.get(at..)?)?;
    let end = at.checked_add(len)?;
    let body = at + used;
    if end > b.len() || body > end { None } else { Some((end, body)) }
}

/// Size of an integer data object at b[at] (ZeroOp, OneOp, OnesOp, Byte/
/// Word/DWord/QWordPrefix), if it is one and complete.
fn int_arg(b: &[u8], at: usize) -> Option<usize> {
    let size = match *b.get(at)? {
        0x00 | 0x01 | 0xff => 1,
        0x0a => 2,
        0x0b => 3,
        0x0c => 5,
        0x0e => 9,
        _ => return None,
    };
    let end = at.checked_add(size)?;
    if end <= b.len() { Some(end) } else { None }
}

/// Offset after a DataRefObject at b[at]: an integer, a string, or a
/// Buffer / Package / VarPackage (skipped by PkgLength).
fn skip_data(b: &[u8], at: usize) -> Option<usize> {
    match *b.get(at)? {
        0x0d => {
            let rest = b.get(at + 1..)?;
            let nul = rest.iter().position(|&c| c == 0)?;
            Some(at + 1 + nul + 1)
        }
        0x11 | 0x12 | 0x13 => pkg_end(b, at + 1).map(|(end, _)| end),
        _ => int_arg(b, at),
    }
}

/// A named object with a body (Scope, Device, ...): the name at `body`,
/// `skip` fixed bytes after it, then the body as a TermList.
fn walk_body(b: &[u8], base: usize, end: usize, body: usize, skip: usize, depth: u32) -> Result<Walk, ()> {
    let (_, n) = name_string(b, body).ok_or(())?;
    let start = body.checked_add(n).and_then(|x| x.checked_add(skip)).ok_or(())?;
    let inner = b.get(start..end).ok_or(())?; /* the name must end inside the package */
    Ok(walk(inner, base + start, depth + 1))
}

/// Walk a TermList object by object; `base` is b's offset in the body.
fn walk(b: &[u8], base: usize, depth: u32) -> Walk {
    let stuck = |i: usize| Walk::Stuck(base + i, b.get(i).copied().unwrap_or(0));
    if depth > WALK_MAX_DEPTH {
        return stuck(0);
    }
    let mut i = 0;
    while i < b.len() {
        let next = match b[i] {
            0x08 => {
                // NameOp: \_S5 if its package follows, else skip the data
                let Some((last, n)) = name_string(b, i + 1) else { return stuck(i) };
                let after = i + 1 + n;
                if last == Some(*b"_S5_") && b.get(after) == Some(&0x12) {
                    return Walk::Found(parse_s5_package(b, after + 1));
                }
                skip_data(b, after)
            }
            0x10 => {
                // ScopeOp
                let Some((end, body)) = pkg_end(b, i + 1) else { return stuck(i) };
                match walk_body(b, base, end, body, 0, depth) {
                    Ok(Walk::NotHere) => Some(end),
                    Ok(w) => return w,
                    Err(()) => return stuck(i),
                }
            }
            // Method, If, Else, While, Buffer, Package, VarPackage: skipped whole
            0x14 | 0xa0 | 0xa1 | 0xa2 | 0x11 | 0x12 | 0x13 => pkg_end(b, i + 1).map(|(end, _)| end),
            0x15 => name_string(b, i + 1).and_then(|(_, n)| {
                let e = i + 1 + n + 2; // ObjectType, ArgumentCount
                if e <= b.len() { Some(e) } else { None }
            }),
            0x06 => name_string(b, i + 1)
                .and_then(|(_, n1)| name_string(b, i + 1 + n1).map(|(_, n2)| i + 1 + n1 + n2)),
            0x5b => match b.get(i + 1) {
                Some(&op @ (0x82 | 0x85 | 0x83 | 0x84)) => {
                    let skip = match op { 0x83 => 6, 0x84 => 3, _ => 0 };
                    let Some((end, body)) = pkg_end(b, i + 2) else { return stuck(i) };
                    match walk_body(b, base, end, body, skip, depth) {
                        Ok(Walk::NotHere) => Some(end),
                        Ok(w) => return w,
                        Err(()) => return stuck(i),
                    }
                }
                Some(0x81 | 0x86 | 0x87) => pkg_end(b, i + 2).map(|(end, _)| end),
                Some(0x01) => name_string(b, i + 2).and_then(|(_, n)| {
                    let e = i + 2 + n + 1; // SyncFlags
                    if e <= b.len() { Some(e) } else { None }
                }),
                Some(0x02) => name_string(b, i + 2).map(|(_, n)| i + 2 + n),
                Some(0x80) => name_string(b, i + 2).and_then(|(_, n)| {
                    let space = i + 2 + n; // RegionSpace byte
                    b.get(space)?;
                    int_arg(b, space + 1).and_then(|o| int_arg(b, o))
                }),
                _ => None,
            },
            _ => None,
        };
        match next {
            Some(n) if n > i => i = n,
            _ => return stuck(i),
        }
    }
    Walk::NotHere
}

#[cfg(test)]
mod walk_tests {
    use super::*;
    extern crate std;
    use std::vec;
    use std::vec::Vec;

    const REAL: [u8; 12] = [0x08, 0x5f, 0x53, 0x35, 0x5f, 0x12, 0x06, 0x04, 0x00, 0x00, 0x00, 0x00];

    #[test]
    fn names() {
        assert_eq!(name_string(&[0x5c, 0x2e, 0x5f, 0x53, 0x42, 0x5f, 0x58, 0x5f, 0x5f, 0x5f], 0), Some((Some(*b"X___"), 10)));
        let mut m = vec![0x2f, 0x03];
        m.extend_from_slice(b"AAAABBBBCCCC");
        assert_eq!(name_string(&m, 0), Some((Some(*b"CCCC"), 14)));
        assert_eq!(name_string(&m[..13], 0), None);
        assert_eq!(name_string(&[0x2f, 0x00], 0), None);
        assert_eq!(name_string(&[0x00], 0), Some((None, 1)));
        assert_eq!(name_string(&[0x5c, 0x00], 0), Some((None, 2)));
        assert_eq!(name_string(&[0x5e, 0x5e, b'A', b'B', b'C', b'D'], 0), Some((Some(*b"ABCD"), 6)));
        assert_eq!(name_string(b"abcd", 0), None);
        assert_eq!(name_string(&[0x5f, 0x53, 0x35], 0), None);
        assert_eq!(name_string(&[], 0), None);
        assert_eq!(name_string(&[0x5e; 300], 0), None); // 255 prefixes, then not a name
    }

    #[test]
    fn packages_and_data() {
        assert_eq!(pkg_end(&[0x03, 0, 0], 0), Some((3, 1)));
        assert_eq!(pkg_end(&[0x05, 0], 0), None); // past the end
        let mut two = vec![0x41, 0x04]; // 2-byte PkgLength: 0x41 = 1 | 4 << 4 = 65
        two.resize(65, 0);
        assert_eq!(pkg_end(&two, 0), Some((65, 2)));
        assert_eq!(pkg_end(&two[..64], 0), None);
        // 3- and 4-byte PkgLength: the extra bytes are bits 4-11, 12-19, 20-27
        assert_eq!(read_pkg_len(&[0x81, 0x34, 0x12]), Some((0x12341, 3)));
        assert_eq!(read_pkg_len(&[0xc2, 0x34, 0x12, 0x05]), Some((0x0512342, 4)));
        assert_eq!(read_pkg_len(&[0x81, 0x34]), None);
        // QEMU q35's Scope(_SB) holds a Scope(PCI0) of 3747 bytes with a 3-byte length
        let mut sb = vec![0x10, 0x80 | 0x0a, 0xb0, 0x00, b'_', b'S', b'B', b'_', 0x10, 0x43, 0xea, b'P', b'C', b'I', b'0'];
        sb.resize(0x0b0a + 1, 0x00); // the outer package: 0xb0a bytes from its PkgLength
        assert_eq!(pkg_end(&sb, 1), Some((0xb0b, 4)));
        assert_eq!(skip_data(&[0x0a, 7], 0), Some(2));
        assert_eq!(skip_data(&[0x0b, 7, 0], 0), Some(3));
        assert_eq!(skip_data(&[0x0c, 7, 0, 0, 0], 0), Some(5));
        assert_eq!(skip_data(&[0x0e, 7, 0, 0, 0, 0, 0, 0, 0], 0), Some(9));
        assert_eq!(skip_data(&[0x0e, 7, 0, 0, 0, 0, 0, 0], 0), None);
        assert_eq!(skip_data(&[0x0d, b'h', b'i', 0, 9], 0), Some(4));
        assert_eq!(skip_data(&[0x0d, b'h', b'i'], 0), None);
        assert_eq!(skip_data(&[0x11, 0x02, 0x00], 0), Some(3));
        assert_eq!(skip_data(&[0x70], 0), None);
    }

    #[test]
    fn walks() {
        assert_eq!(walk(&REAL, 0, 0), Walk::Found(Ok(SleepType::default())));
        assert_eq!(walk(&[0x15, b'A', b'B', b'C', b'D', 0, 0, 0x70], 7, 0), Walk::Stuck(14, 0x70)); // absolute offset
        let mut buried = vec![0x11, 0x0f, 0x0a, 0x0c];
        buried.extend_from_slice(&REAL);
        assert_eq!(walk(&buried, 0, 0), Walk::NotHere);
        let mut dev = vec![0x5b, 0x82, 0x11, b'P', b'C', b'I', b'0'];
        dev.extend_from_slice(&REAL);
        assert_eq!(walk(&dev, 0, 0), Walk::Found(Ok(SleepType::default())));
        // Processor and PowerResource bodies after their fixed bytes
        let mut cpu = vec![0x5b, 0x83, 0x17, b'C', b'P', b'U', b'0', 0, 0, 0, 0, 0, 0];
        cpu.extend_from_slice(&REAL);
        assert_eq!(walk(&cpu, 0, 0), Walk::Found(Ok(SleepType::default())));
        let mut pwr = vec![0x5b, 0x84, 0x14, b'P', b'W', b'R', b'0', 0, 0, 0];
        pwr.extend_from_slice(&REAL);
        assert_eq!(walk(&pwr, 0, 0), Walk::Found(Ok(SleepType::default())));
        // a Scope whose name runs past its package: Stuck, no panic
        assert!(matches!(walk(&[0x10, 0x02, b'A', b'B', b'C', b'D'], 0, 0), Walk::Stuck(..)));
        assert!(matches!(walk(&[0x5b, 0x82, 0x02, b'A', b'B', b'C', b'D'], 0, 0), Walk::Stuck(..)));
        // External, Alias, Mutex, Event before the real one
        let mut misc = vec![0x15, b'E', b'X', b'T', b'_', 0x08, 0x00];
        misc.extend_from_slice(&[0x06, b'A', b'A', b'A', b'A', b'B', b'B', b'B', b'B']);
        misc.extend_from_slice(&[0x5b, 0x01, b'M', b'U', b'T', b'X', 0x00]);
        misc.extend_from_slice(&[0x5b, 0x02, b'E', b'V', b'N', b'T']);
        misc.extend_from_slice(&REAL);
        assert_eq!(walk(&misc, 0, 0), Walk::Found(Ok(SleepType::default())));
        // an OperationRegion whose length is not an integer: Stuck
        assert!(matches!(walk(&[0x5b, 0x80, b'G', b'N', b'V', b'S', 0x00, 0x0c, 0, 0, 0, 0x7f, 0x70], 0, 0), Walk::Stuck(..)));
        assert!(matches!(walk(&[0x70], 0, 0), Walk::Stuck(..)));
        assert!(matches!(walk(&[0x5b, 0x99], 0, 0), Walk::Stuck(..)));
        assert_eq!(walk(&[], 0, 0), Walk::NotHere);
    }

    #[test]
    fn fuzz_walk() {
        let mut s = 0x9e37_79b9_7f4a_7c15u64;
        let mut next = || {
            s ^= s << 13;
            s ^= s >> 7;
            s ^= s << 17;
            s
        };
        for round in 0..20000 {
            let len = (next() % 200) as usize;
            let mut b: Vec<u8> = (0..len).map(|_| next() as u8).collect();
            if round % 2 == 0 && len > 20 {
                let at = (next() % (len as u64 - 16)) as usize;
                b[at..at + 4].copy_from_slice(&[0x10, (len - at - 1).min(63) as u8, 0x5c, 0x00]);
                b[at + 4..at + 16].copy_from_slice(&REAL);
            }
            let _ = walk(&b, 0, 0);
            let _ = scan_s5(&b);
        }
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    extern crate std;
    use std::vec;
    use std::vec::Vec;

    fn build_table(sig: &[u8], body: &[u8]) -> Vec<u8> {
        let mut table = Vec::new();
        // Signature
        table.extend_from_slice(sig);
        // Length (placeholder, will be fixed)
        table.extend_from_slice(&[0u8; 4]);
        // Revision
        table.push(0);
        // Checksum (ignored)
        table.push(0);
        // OEM ID (6 bytes)
        table.extend_from_slice(b"TEST  ");
        // OEM Table ID (8 bytes)
        table.extend_from_slice(b"TESTTBL ");
        // OEM Revision
        table.extend_from_slice(&[0u8; 4]);
        // Creator ID
        table.extend_from_slice(b"TEST");
        // Creator Revision
        table.extend_from_slice(&[0u8; 4]);
        
        // Now append body
        table.extend_from_slice(body);
        
        // Fix length
        let len = table.len() as u32;
        let len_bytes = len.to_le_bytes();
        table[4] = len_bytes[0];
        table[5] = len_bytes[1];
        table[6] = len_bytes[2];
        table[7] = len_bytes[3];
        
        table
    }

    #[test]
    fn test_qemu_q35() {
        // 08 5f 53 35 5f 12 06 04 00 00 00 00
        // NameOp, _S5_, PackageOp, PkgLen=6, NumElem=4, 0, 0, 0, 0
        let body = vec![
            0x08, 0x5f, 0x53, 0x35, 0x5f, 0x12, 0x06, 0x04, 0x00, 0x00, 0x00, 0x00
        ];
        let table = build_table(b"DSDT", &body);
        let res = find_s5(&table);
        assert_eq!(res, Ok(SleepType { slp_typa: 0, slp_typb: 0 }));
    }

    #[test]
    fn test_root_prefix_byte_prefix() {
        // 08 5c 5f 53 35 5f 12 08 04 0a 07 0a 07 00 00
        // NameOp, \, _S5_, PackageOp, PkgLen=8, NumElem=4, BytePrefix 7, BytePrefix 7, 0, 0
        let body = vec![
            0x08, 0x5c, 0x5f, 0x53, 0x35, 0x5f, 0x12, 0x08, 0x04, 0x0a, 0x07, 0x0a, 0x07, 0x00, 0x00
        ];
        let table = build_table(b"DSDT", &body);
        let res = find_s5(&table);
        assert_eq!(res, Ok(SleepType { slp_typa: 7, slp_typb: 7 }));
    }

    #[test]
    fn test_element_encodings() {
        // OneOp (1), WordPrefix 5, DWordPrefix 3, QWordPrefix 2
        // PkgLen calculation:
        // NumElem (1) + OneOp (1) + WordPrefix (3) + DWordPrefix (5) + QWordPrefix (9) = 19
        // PkgLen = 19. Lead byte: 19 & 0x3f = 19. n=0.
        let body = vec![
            0x08, 0x5f, 0x53, 0x35, 0x5f, 0x12, // NameOp, _S5_, PackageOp
            0x13, // PkgLen = 19
            0x04, // NumElements = 4
            0x01, // OneOp -> 1
            0x0b, 0x05, 0x00, // WordPrefix 5
            0x0c, 0x03, 0x00, 0x00, 0x00, // DWordPrefix 3
            0x0e, 0x02, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, // QWordPrefix 2
        ];
        let table = build_table(b"DSDT", &body);
        let res = find_s5(&table);
        assert_eq!(res, Ok(SleepType { slp_typa: 1, slp_typb: 5 }));

        // One-element package
        let body2 = vec![
            0x08, 0x5f, 0x53, 0x35, 0x5f, 0x12,
            0x03, // PkgLen = 3: itself, NumElements, OneOp
            0x01, // NumElements = 1
            0x01, // OneOp -> 1
        ];
        let table2 = build_table(b"DSDT", &body2);
        let res2 = find_s5(&table2);
        assert_eq!(res2, Ok(SleepType { slp_typa: 1, slp_typb: 1 }));
    }

    #[test]
    fn test_two_byte_pkg_len() {
        // PkgLen > 63. Let's make PkgLen = 64 + 5 = 69.
        // Lead byte: n=1 (bits 7-6 = 01). Low nibble = 69 & 0x0f = 5.
        // Lead = 0x40 | 5 = 0x45.
        // Next byte: (69 >> 4) & 0xff = 4.
        // PkgLen = 69.
        // Content: NumElem (1) + 2 elements (2 bytes each if BytePrefix) + padding?
        // Let's say NumElem=2, BytePrefix 1, BytePrefix 2.
        // Content size = 1 + 2 + 2 = 5.
        // PkgLen should be 5. But we want to test 2-byte PkgLen.
        // Let's make PkgLen = 100.
        // Lead = 0x40 | (100 & 0x0f) = 0x40 | 4 = 0x44.
        // Next byte = (100 >> 4) & 0xff = 6.
        // Content size = 100 - 2 (PkgLen bytes) = 98.
        // NumElem = 2.
        // Elem1: BytePrefix 3.
        // Elem2: BytePrefix 4.
        // Padding: 98 - 1 - 2 - 2 = 93 bytes of 0.
        
        let mut body = vec![
            0x08, 0x5f, 0x53, 0x35, 0x5f, 0x12,
            0x44, 0x06, // PkgLen = 100
            0x02, // NumElem
            0x0a, 0x03, // BytePrefix 3
            0x0a, 0x04, // BytePrefix 4
        ];
        body.extend(vec![0u8; 93]);
        
        let table = build_table(b"DSDT", &body);
        let res = find_s5(&table);
        assert_eq!(res, Ok(SleepType { slp_typa: 3, slp_typb: 4 }));
    }

    #[test]
    fn test_decoys() {
        // Decoy 1: String containing _S5_
        // Decoy 2: NameOp _S5_ followed by non-package
        // Real: NameOp _S5_ Package
        
        let mut body = Vec::new();
        // Decoy 1: String 0x0d "_S5_" 0x00
        body.push(0x0d);
        body.extend_from_slice(b"_S5_");
        body.push(0x00);
        
        // Decoy 2: NameOp _S5_ BytePrefix 5
        body.push(0x08);
        body.extend_from_slice(b"_S5_");
        body.push(0x0a);
        body.push(0x05);
        
        // Real: NameOp _S5_ Package
        body.push(0x08);
        body.extend_from_slice(b"_S5_");
        body.push(0x12);
        body.push(0x04); // PkgLen = 4
        body.push(0x02); // NumElem = 2
        body.push(0x01); // OneOp = 1
        body.push(0x01); // OneOp = 1
        
        let table = build_table(b"DSDT", &body);
        let res = find_s5(&table);
        assert_eq!(res, Ok(SleepType { slp_typa: 1, slp_typb: 1 }));
    }

    #[test]
    fn test_errors() {
        // BadSignature
        let table = build_table(b"FACP", &[]);
        assert_eq!(find_s5(&table), Err(AmlError::BadSignature));
        assert_eq!(find_s5(&build_table(b"SSDT", &[0x08, 0x5f, 0x53, 0x35, 0x5f, 0x12, 0x03, 0x01, 0x01])),
                   Ok(SleepType { slp_typa: 1, slp_typb: 1 }));
        
        // Short
        let short_table = vec![0u8; 35];
        assert_eq!(find_s5(&short_table), Err(AmlError::Short));
        
        // BadLength (Length > len)
        let mut table = build_table(b"DSDT", &[]);
        table[4] = 100; // Length = 100, but table is smaller
        assert_eq!(find_s5(&table), Err(AmlError::BadLength));
        
        // NotFound
        let table = build_table(b"DSDT", &[0x08, 0x5f, 0x41, 0x43, 0x50, 0x49]); // NameOp _ACPI
        assert_eq!(find_s5(&table), Err(AmlError::NotFound));
        
        // BadPackage (PkgLength past body)
        let body = vec![
            0x08, 0x5f, 0x53, 0x35, 0x5f, 0x12,
            0x10, // PkgLen = 16, but body is shorter
            0x02,
            0x01,
            0x01,
        ];
        let table = build_table(b"DSDT", &body);
        assert_eq!(find_s5(&table), Err(AmlError::BadPackage));
        
        // BadPackage (NumElements 0)
        let body = vec![
            0x08, 0x5f, 0x53, 0x35, 0x5f, 0x12,
            0x02, // PkgLen = 2
            0x00, // NumElem = 0
        ];
        let table = build_table(b"DSDT", &body);
        assert_eq!(find_s5(&table), Err(AmlError::BadPackage));
        
        // BadPackage (Unknown opcode)
        let body = vec![
            0x08, 0x5f, 0x53, 0x35, 0x5f, 0x12,
            0x03, // PkgLen = 3
            0x01, // NumElem = 1
            0x70, // Unknown
        ];
        let table = build_table(b"DSDT", &body);
        assert_eq!(find_s5(&table), Err(AmlError::BadPackage));
        
        // BadValue (Value > 7)
        let body = vec![
            0x08, 0x5f, 0x53, 0x35, 0x5f, 0x12,
            0x04, // PkgLen = 4
            0x01, // NumElem = 1
            0x0a, 0x08, // BytePrefix 8
        ];
        let table = build_table(b"DSDT", &body);
        assert_eq!(find_s5(&table), Err(AmlError::BadValue));
        
        // BadValue: a wide value must not be cut to 8 bits first (review fix)
        let body = vec![0x08, 0x5f, 0x53, 0x35, 0x5f, 0x12, 0x05, 0x01, 0x0b, 0x00, 0x01]; // Word 0x0100
        assert_eq!(find_s5(&build_table(b"DSDT", &body)), Err(AmlError::BadValue));
        let body = vec![0x08, 0x5f, 0x53, 0x35, 0x5f, 0x12, 0x08, 0x02, 0x01, 0x0c, 0, 0, 0, 1]; // DWord 1 << 24
        assert_eq!(find_s5(&build_table(b"DSDT", &body)), Err(AmlError::BadValue));
        // an element running past the package end
        let body = vec![0x08, 0x5f, 0x53, 0x35, 0x5f, 0x12, 0x03, 0x01, 0x0b, 0x05, 0x00];
        assert_eq!(find_s5(&build_table(b"DSDT", &body)), Err(AmlError::BadPackage));

        // BadValue (OnesOp)
        let body = vec![
            0x08, 0x5f, 0x53, 0x35, 0x5f, 0x12,
            0x03, // PkgLen = 3
            0x01, // NumElem = 1
            0xff, // OnesOp
        ];
        let table = build_table(b"DSDT", &body);
        assert_eq!(find_s5(&table), Err(AmlError::BadValue));
    }

    #[test]
    fn test_truncation() {
        let body = vec![
            0x08, 0x5f, 0x53, 0x35, 0x5f, 0x12, 0x06, 0x04, 0x00, 0x00, 0x00, 0x00
        ];
        let full_table = build_table(b"DSDT", &body);
        let full_len = full_table.len();
        
        for n in 0..full_len {
            let mut truncated = full_table[..n].to_vec();
            if n >= 36 {
                // Fix length to n
                let len = n as u32;
                truncated[4] = (len & 0xff) as u8;
                truncated[5] = ((len >> 8) & 0xff) as u8;
                truncated[6] = ((len >> 16) & 0xff) as u8;
                truncated[7] = ((len >> 24) & 0xff) as u8;
            }
            // Should not panic
            let _ = find_s5(&truncated);
        }
    }

    /// Review 2b L2: `_S5_` package bytes inside another object's data
    /// must not match; only an object-level Name does.
    #[test]
    fn test_object_level_only() {
        let buried: [u8; 14] = [0x08, 0x5f, 0x53, 0x35, 0x5f, 0x12, 0x08, 0x04, 0x0a, 0x05, 0x0a, 0x05, 0x00, 0x00];
        let real: [u8; 12] = [0x08, 0x5f, 0x53, 0x35, 0x5f, 0x12, 0x06, 0x04, 0x00, 0x00, 0x00, 0x00];
        // in a Buffer: 11 <PkgLength> 0a <size> <bytes>
        let mut buf = vec![0x11, (3 + buried.len()) as u8, 0x0a, buried.len() as u8];
        buf.extend_from_slice(&buried);
        let mut b1 = buf.clone();
        b1.extend_from_slice(&real);
        assert_eq!(find_s5_ex(&build_table(b"DSDT", &b1)), Ok((SleepType { slp_typa: 0, slp_typb: 0 }, false)));
        assert_eq!(find_s5(&build_table(b"DSDT", &buf)), Err(AmlError::NotFound));
        // in a Method body: 14 <PkgLength> NameSeg flags <bytes>
        let mut m = vec![0x14, (6 + buried.len()) as u8, b'A', b'B', b'C', b'D', 0x00];
        m.extend_from_slice(&buried);
        assert_eq!(find_s5(&build_table(b"DSDT", &m)), Err(AmlError::NotFound));
        // in a string (no 0x00 inside an AML string: a zero-free copy)
        let mut st = vec![0x08, b'S', b'T', b'R', b'_', 0x0d];
        st.extend_from_slice(&[0x08, 0x5f, 0x53, 0x35, 0x5f, 0x12, 0x06, 0x02, 0x0a, 0x05, 0x0a, 0x05]);
        st.push(0x00);
        st.extend_from_slice(&real);
        assert_eq!(find_s5_ex(&build_table(b"DSDT", &st)), Ok((SleepType::default(), false)));
        // Scope(\) { Name(_S5_, ...) } and Device(PCI0) { ... }
        let mut sc = vec![0x10, (3 + real.len()) as u8, 0x5c, 0x00];
        sc.extend_from_slice(&real);
        assert_eq!(find_s5_ex(&build_table(b"DSDT", &sc)), Ok((SleepType::default(), false)));
        let mut dv = vec![0x5b, 0x82, (5 + buried.len()) as u8, b'P', b'C', b'I', b'0'];
        dv.extend_from_slice(&buried);
        assert_eq!(find_s5_ex(&build_table(b"SSDT", &dv)), Ok((SleepType { slp_typa: 5, slp_typb: 5 }, false)));
        // OperationRegion and Field before the real one: still the walk
        let mut op = vec![0x5b, 0x80, b'G', b'N', b'V', b'S', 0x00, 0x0c, 0, 0, 0, 0x7f, 0x0b, 0x00, 0x01];
        op.extend_from_slice(&[0x5b, 0x81, 0x07, b'G', b'N', b'V', b'S', 0x01, 0x00]);
        op.extend_from_slice(&real);
        assert_eq!(find_s5_ex(&build_table(b"DSDT", &op)), Ok((SleepType::default(), false)));
        // AML the walk does not know (StoreOp at top level): byte-scan fallback, flagged
        let mut un = vec![0x70, 0x0a, 0x01, 0x60];
        un.extend_from_slice(&real);
        assert_eq!(find_s5_ex(&build_table(b"DSDT", &un)), Ok((SleepType::default(), true)));
        // 20 nested Scopes: the walk stops at depth 16, the fallback finds it
        let mut deep = real.to_vec();
        for _ in 0..20 {
            let mut outer = vec![0x10];
            let len = 1 + 4 + deep.len();
            if len < 64 {
                outer.push(len as u8);
            } else {
                let len = len + 1;
                outer.push(0x40 | (len & 0x0f) as u8);
                outer.push((len >> 4) as u8);
            }
            outer.extend_from_slice(b"SCP_");
            outer.extend_from_slice(&deep);
            deep = outer;
        }
        assert_eq!(find_s5_ex(&build_table(b"DSDT", &deep)), Ok((SleepType::default(), true)));
    }

    #[test]
    fn test_fuzz() {
        // Simple xorshift RNG
        let mut state: u64 = 1234567890123456789;
        fn xorshift64(s: &mut u64) -> u64 {
            *s ^= *s << 13;
            *s ^= *s >> 7;
            *s ^= *s << 17;
            *s
        }

        for _ in 0..20000 {
            let len = (xorshift64(&mut state) % 200) as usize;
            let mut body = vec![0u8; len];
            for i in 0..len {
                body[i] = (xorshift64(&mut state) % 256) as u8;
            }
            
            // Half the time, insert the pattern
            if xorshift64(&mut state) % 2 == 0 {
                if len >= 6 {
                    let pos = (xorshift64(&mut state) % len as u64) as usize;
                    // Insert 08 5f 53 35 5f 12
                    // We need to make room. Just overwrite if possible, or skip if too short
                    if pos + 6 <= len {
                        body[pos] = 0x08;
                        body[pos+1] = 0x5f;
                        body[pos+2] = 0x53;
                        body[pos+3] = 0x35;
                        body[pos+4] = 0x5f;
                        body[pos+5] = 0x12;
                    }
                }
            }
            
            let table = build_table(b"DSDT", &body);
            let _ = find_s5(&table);
        }
    }
}
