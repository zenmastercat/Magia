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

        let mut val = (*lead & 0x0f) as usize;
        for i in 0..n {
            let byte = bytes[1 + i];
            // Shift by 4, 12, 20...
            // Check for overflow before shifting
            if 4 * (i + 1) >= 64 {
                return None;
            }
            val |= (byte as usize) << (4 * (i + 1));
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

pub fn find_s5(table: &[u8]) -> Result<SleepType, AmlError> {
    // 1. Check minimum header length
    if table.len() < 36 {
        return Err(AmlError::Short);
    }

    // 2. Check Signature
    let sig = &table[0..4];
    if sig != b"DSDT" && sig != b"SSDT" {
        return Err(AmlError::BadSignature);
    }

    // 3. Check Length
    let len_bytes = &table[4..8];
    let length = u32::from_le_bytes([len_bytes[0], len_bytes[1], len_bytes[2], len_bytes[3]]) as usize;

    if length < 36 || length > table.len() {
        return Err(AmlError::BadLength);
    }

    // 4. Define AML body
    let body = &table[36..length];
    let mut idx = 0;

    // 5. Scan body
    while idx < body.len() {
        // Look for NameOp (0x08)
        if body.get(idx) != Some(&0x08) {
            idx += 1;
            continue;
        }

        let name_op_start = idx;
        idx += 1; // Skip NameOp

        // Skip optional prefixes: '\\' (0x5c) or '^' (0x5e)
        // Up to 8 prefixes
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

        // Check for NameSeg "_S5_"
        let name_seg = b"_S5_";
        if idx + name_seg.len() > body.len() {
            // Not enough room for name seg, skip this NameOp
            idx = name_op_start + 1;
            continue;
        }

        if &body[idx..idx + name_seg.len()] != name_seg {
            // Not _S5_, skip this NameOp
            idx = name_op_start + 1;
            continue;
        }

        idx += name_seg.len();

        // Check for PackageOp (0x12)
        if body.get(idx) != Some(&0x12) {
            // Not a package, skip this NameOp
            idx = name_op_start + 1;
            continue;
        }

        // We have a match: NameOp + Prefixes + "_S5_" + PackageOp
        // Now parse the package
        let pkg_op_offset = idx;
        idx += 1; // Skip PackageOp

        // Read PkgLength
        let pkg_len_bytes = &body[idx..];
        let (pkg_len, pkg_len_consumed) = match read_pkg_len(pkg_len_bytes) {
            Some(v) => v,
            None => return Err(AmlError::BadPackage),
        };

        // The PkgLength includes itself.
        // The package content starts after the PkgLength bytes.
        // The package ends at: pkg_op_offset + 1 (PackageOp) + pkg_len
        // Wait, ACPI spec says PkgLength counts itself and everything after it.
        // So the total size of the package object is 1 (PackageOp) + pkg_len.
        // The end of the package in the body is: pkg_op_offset + 1 + pkg_len.
        
        let pkg_end = match pkg_op_offset.checked_add(1).and_then(|x| x.checked_add(pkg_len)) {
            Some(v) => v,
            None => return Err(AmlError::BadPackage),
        };

        if pkg_end > body.len() {
            return Err(AmlError::BadPackage);
        }

        // The content of the package starts after the PkgLength bytes
        let pkg_content_start = idx + pkg_len_consumed;
        
        // Check if there is at least one byte for NumElements
        if pkg_content_start >= pkg_end {
            return Err(AmlError::BadPackage);
        }

        let num_elements = body[pkg_content_start];
        if num_elements == 0 {
            return Err(AmlError::BadPackage);
        }

        // elements must lie inside the package: read from a slice ending there
        let pkg = body.get(pkg_content_start + 1..pkg_end).ok_or(AmlError::BadPackage)?;
        let (va, used) = read_element(pkg)?;
        let slp_typa = slp_typ(va)?;
        let slp_typb = if num_elements >= 2 {
            let rest = pkg.get(used..).ok_or(AmlError::BadPackage)?;
            slp_typ(read_element(rest)?.0)?
        } else {
            slp_typa // one element: the same value for PM1b
        };
        return Ok(SleepType { slp_typa, slp_typb });
    }

    Err(AmlError::NotFound)
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
