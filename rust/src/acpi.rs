// SPDX-License-Identifier: BSD-2-Clause
// Copyright (c) 2026 zenmastercat and Magia contributors
//! Static ACPI tables (RSDP, XSDT/RSDT, MADT, HPET, MCFG, FADT), parsed as
//! untrusted input. No AML, no ACPICA. Semantics follow FreeBSD
//! sys/x86/acpica/madt.c (a CPU counts only with its "enabled" bit set,
//! interrupt source overrides are ISA-only, bus 0); the code is written
//! from the ACPI 6.5 specification.
//!
//! Every read goes through [`PhysMem`], which refuses ranges outside
//! mapped firmware memory; every length, count and offset from firmware is
//! checked before use (no panics, no overflow, no unbounded loops: tables
//! are at most [`MAX_TABLE_LEN`] bytes and results go into fixed arrays).
//!
//! First draft of the RSDP/root/MADT part by the local model (qwen3.6:27b);
//! reviewed, fixed and extended (HPET, MCFG, FADT, C interface, tests).

/// Physical memory as seen by the parser. The kernel implementation returns
/// None when [pa, pa+len) is not fully inside mapped firmware memory.
/// A returned slice is exactly `len` bytes long.
pub trait PhysMem {
    fn read(&self, pa: u64, len: usize) -> Option<&[u8]>;
}

#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum AcpiError {
    Unmapped,     // PhysMem::read returned None
    BadSignature,
    BadChecksum,
    BadLength,    // too small, too large (> MAX_TABLE_LEN) or not a multiple of the entry size
    BadAlignment, // RSDP not 16-byte aligned, HPET block not 1 KiB aligned
    Overlap,      // a table overlaps the RSDP or the root table (XSDT/RSDT)
    BadValue,     // a field has a value the spec does not allow
    NotFound,
}

impl AcpiError {
    /// Small positive code for the C side (rs_acpi_parse returns -code).
    pub fn code(self) -> i32 {
        self as i32 + 1
    }
}

pub const MAX_TABLE_LEN: u32 = 1 << 20; // 1 MiB
pub const MAX_TABLES: usize = 64;
pub const MAX_CPUS: usize = 256;
pub const MAX_IOAPICS: usize = 16;
pub const MAX_ISOS: usize = 32;
pub const MAX_NMIS: usize = 16;
pub const MAX_MCFG: usize = 8;

fn rd8(b: &[u8], o: usize) -> Option<u8> {
    b.get(o).copied()
}
fn rd16(b: &[u8], o: usize) -> Option<u16> {
    Some(u16::from_le_bytes(b.get(o..o.checked_add(2)?)?.try_into().ok()?))
}
fn rd32(b: &[u8], o: usize) -> Option<u32> {
    Some(u32::from_le_bytes(b.get(o..o.checked_add(4)?)?.try_into().ok()?))
}
fn rd64(b: &[u8], o: usize) -> Option<u64> {
    Some(u64::from_le_bytes(b.get(o..o.checked_add(8)?)?.try_into().ok()?))
}

fn read_exact(m: &dyn PhysMem, pa: u64, len: usize) -> Result<&[u8], AcpiError> {
    pa.checked_add(len as u64).ok_or(AcpiError::BadLength)?;
    match m.read(pa, len) {
        Some(b) if b.len() == len => Ok(b),
        _ => Err(AcpiError::Unmapped),
    }
}

pub fn checksum_ok(b: &[u8]) -> bool {
    b.iter().fold(0u8, |acc, &x| acc.wrapping_add(x)) == 0
}

fn overlaps(a: u64, alen: u64, b: u64, blen: u64) -> bool {
    let ae = a.saturating_add(alen);
    let be = b.saturating_add(blen);
    a < be && b < ae
}

// ---------------------------------------------------------------- RSDP

#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub struct Rsdp {
    pub revision: u8,
    pub rsdt: u32,
    pub xsdt: u64,
    pub pa: u64,
    pub len: u32,
}

/// `bios`: found by the BIOS-area scan, which only looks at 16-byte
/// boundaries (ACPI 6.5 5.2.5.1); under UEFI the RSDP comes from the EFI
/// configuration table and has no alignment rule (OVMF puts it at ...14).
pub fn parse_rsdp(m: &dyn PhysMem, pa: u64, bios: bool) -> Result<Rsdp, AcpiError> {
    if bios && pa % 16 != 0 {
        return Err(AcpiError::BadAlignment);
    }
    let b = read_exact(m, pa, 20)?;
    if b.get(0..8) != Some(b"RSD PTR ") {
        return Err(AcpiError::BadSignature);
    }
    if !checksum_ok(b) {
        return Err(AcpiError::BadChecksum);
    }
    let revision = rd8(b, 15).ok_or(AcpiError::BadLength)?;
    let rsdt = rd32(b, 16).ok_or(AcpiError::BadLength)?;
    if revision < 2 {
        return Ok(Rsdp { revision, rsdt, xsdt: 0, pa, len: 20 });
    }
    let len = rd32(read_exact(m, pa, 36)?, 20).ok_or(AcpiError::BadLength)?;
    if !(36..=4096).contains(&len) {
        return Err(AcpiError::BadLength);
    }
    let full = read_exact(m, pa, len as usize)?;
    if !checksum_ok(full) {
        return Err(AcpiError::BadChecksum);
    }
    let xsdt = rd64(full, 24).ok_or(AcpiError::BadLength)?;
    Ok(Rsdp { revision, rsdt, xsdt, pa, len })
}

// ---------------------------------------------------------------- SDTs

/// Read and validate an SDT: header is 36 bytes, signature at 0..4, length
/// u32 at 4, checksum over `length` bytes. `sig` must match.
pub fn read_table<'a>(m: &'a dyn PhysMem, pa: u64, sig: &[u8; 4]) -> Result<&'a [u8], AcpiError> {
    let header = read_exact(m, pa, 36)?;
    if header.get(0..4) != Some(&sig[..]) {
        return Err(AcpiError::BadSignature);
    }
    let len = rd32(header, 4).ok_or(AcpiError::BadLength)?;
    if !(36..=MAX_TABLE_LEN).contains(&len) {
        return Err(AcpiError::BadLength);
    }
    let table = read_exact(m, pa, len as usize)?;
    if !checksum_ok(table) {
        return Err(AcpiError::BadChecksum);
    }
    Ok(table)
}

/// Root table entries (physical addresses), from the XSDT when the RSDP
/// revision >= 2 and xsdt != 0, else from the RSDT.
pub struct RootTables {
    pub pa: u64,
    pub len: u32,
    pub n: usize,
    pub entries: [u64; MAX_TABLES],
    pub is_xsdt: bool,
    pub truncated: bool,
}

pub fn root_tables(m: &dyn PhysMem, r: &Rsdp) -> Result<RootTables, AcpiError> {
    let is_xsdt = r.revision >= 2 && r.xsdt != 0;
    let (sig, esz, root_pa) = if is_xsdt { (b"XSDT", 8usize, r.xsdt) } else { (b"RSDT", 4usize, r.rsdt as u64) };
    if root_pa == 0 {
        return Err(AcpiError::NotFound);
    }
    if overlaps(root_pa, 36, r.pa, r.len as u64) {
        return Err(AcpiError::Overlap);
    }
    let t = read_table(m, root_pa, sig)?;
    let len = t.len();
    if (len - 36) % esz != 0 {
        return Err(AcpiError::BadLength);
    }
    let mut out = RootTables { pa: root_pa, len: len as u32, n: 0, entries: [0; MAX_TABLES], is_xsdt, truncated: false };
    for off in (36..len).step_by(esz) {
        let e = if esz == 8 { rd64(t, off) } else { rd32(t, off).map(u64::from) }.ok_or(AcpiError::BadLength)?;
        if e == 0 || out.entries[..out.n].contains(&e) {
            continue;
        }
        if overlaps(e, 36, r.pa, r.len as u64) || overlaps(e, 36, root_pa, len as u64) {
            return Err(AcpiError::Overlap);
        }
        if out.n == MAX_TABLES {
            out.truncated = true;
            continue;
        }
        out.entries[out.n] = e;
        out.n += 1;
    }
    Ok(out)
}

/// Find the first table with signature `sig` among the root entries whose
/// header and checksum are valid; tables that fail validation are skipped.
pub fn find_table<'a>(m: &'a dyn PhysMem, root: &RootTables, sig: &[u8; 4]) -> Result<&'a [u8], AcpiError> {
    for &pa in root.entries.iter().take(root.n) {
        match m.read(pa, 4) {
            Some(h) if h.get(0..4) == Some(&sig[..]) => {}
            _ => continue,
        }
        if let Ok(t) = read_table(m, pa, sig) {
            return Ok(t);
        }
    }
    Err(AcpiError::NotFound)
}

fn check_sig(t: &[u8], sig: &[u8; 4], min: usize) -> Result<(), AcpiError> {
    if t.len() < min {
        return Err(AcpiError::BadLength);
    }
    if t.get(0..4) != Some(&sig[..]) {
        return Err(AcpiError::BadSignature);
    }
    Ok(())
}

// ---------------------------------------------------------------- MADT

#[derive(Clone, Copy, Default, Debug, PartialEq, Eq)]
pub struct Cpu {
    pub apic_id: u32,
    pub acpi_uid: u32,
    pub x2apic: bool,
}
#[derive(Clone, Copy, Default, Debug, PartialEq, Eq)]
pub struct Ioapic {
    pub id: u8,
    pub addr: u32,
    pub gsi_base: u32,
}
#[derive(Clone, Copy, Default, Debug, PartialEq, Eq)]
pub struct Iso {
    pub irq: u8,
    pub gsi: u32,
    pub flags: u16,
}
#[derive(Clone, Copy, Default, Debug, PartialEq, Eq)]
pub struct LapicNmi {
    pub acpi_uid: u32,
    pub flags: u16,
    pub lint: u8,
}

pub struct Madt {
    pub lapic_addr: u64,
    pub pcat_compat: bool,
    pub ncpus: usize,
    pub cpus: [Cpu; MAX_CPUS],
    pub cpus_disabled: usize,
    pub nioapics: usize,
    pub ioapics: [Ioapic; MAX_IOAPICS],
    pub nisos: usize,
    pub isos: [Iso; MAX_ISOS],
    pub nnmis: usize,
    pub nmis: [LapicNmi; MAX_NMIS],
    pub bad_entries: usize,
    pub truncated: bool,
}

/// Append to a fixed array, setting `truncated` when it is full.
fn push<T>(arr: &mut [T], n: &mut usize, truncated: &mut bool, v: T) {
    match arr.get_mut(*n) {
        Some(slot) => {
            *slot = v;
            *n += 1;
        }
        None => *truncated = true,
    }
}

pub fn parse_madt(t: &[u8]) -> Result<Madt, AcpiError> {
    check_sig(t, b"APIC", 44)?;
    let mut d = Madt {
        lapic_addr: rd32(t, 36).ok_or(AcpiError::BadLength)? as u64,
        pcat_compat: rd32(t, 40).ok_or(AcpiError::BadLength)? & 1 != 0,
        ncpus: 0,
        cpus: [Cpu::default(); MAX_CPUS],
        cpus_disabled: 0,
        nioapics: 0,
        ioapics: [Ioapic::default(); MAX_IOAPICS],
        nisos: 0,
        isos: [Iso::default(); MAX_ISOS],
        nnmis: 0,
        nmis: [LapicNmi::default(); MAX_NMIS],
        bad_entries: 0,
        truncated: false,
    };
    let mut off = 44usize;
    while off < t.len() {
        let (Some(ty), Some(elen)) = (rd8(t, off), rd8(t, off + 1)) else {
            d.bad_entries += 1;
            break;
        };
        let elen = elen as usize;
        let Some(e) = t.get(off..off + elen).filter(|_| elen >= 2) else {
            d.bad_entries += 1; // zero/one length or running past the end: stop
            break;
        };
        off += elen;
        let min = match ty {
            0 => 8,
            1 | 5 | 0xa => 12,
            2 => 10,
            4 => 6,
            9 => 16,
            _ => continue, // other types are not needed here
        };
        if e.len() < min {
            d.bad_entries += 1;
            continue;
        }
        // all reads below are in bounds: e.len() >= min
        let ok = match ty {
            0 => {
                let (uid, id, fl) = (e[2] as u32, e[3] as u32, rd32(e, 4).unwrap_or(0));
                if fl & 1 != 0 {
                    push(&mut d.cpus, &mut d.ncpus, &mut d.truncated, Cpu { apic_id: id, acpi_uid: uid, x2apic: false });
                } else {
                    d.cpus_disabled += 1;
                }
                true
            }
            1 => {
                let io = Ioapic { id: e[2], addr: rd32(e, 4).unwrap_or(0), gsi_base: rd32(e, 8).unwrap_or(0) };
                if io.addr != 0 {
                    push(&mut d.ioapics, &mut d.nioapics, &mut d.truncated, io);
                }
                io.addr != 0
            }
            2 => {
                let (bus, irq) = (e[2], e[3]);
                let iso = Iso { irq, gsi: rd32(e, 4).unwrap_or(0), flags: rd16(e, 8).unwrap_or(0) };
                if bus == 0 && irq < 16 {
                    push(&mut d.isos, &mut d.nisos, &mut d.truncated, iso);
                }
                bus == 0 && irq < 16
            }
            4 | 0xa => {
                let nmi = if ty == 4 {
                    let uid = if e[2] == 0xff { 0xffff_ffff } else { e[2] as u32 };
                    LapicNmi { acpi_uid: uid, flags: rd16(e, 3).unwrap_or(0), lint: e[5] }
                } else {
                    LapicNmi { acpi_uid: rd32(e, 4).unwrap_or(0), flags: rd16(e, 2).unwrap_or(0), lint: e[8] }
                };
                if nmi.lint <= 1 {
                    push(&mut d.nmis, &mut d.nnmis, &mut d.truncated, nmi);
                }
                nmi.lint <= 1
            }
            5 => {
                let a = rd64(e, 4).unwrap_or(0);
                if a != 0 {
                    d.lapic_addr = a;
                }
                true
            }
            9 => {
                let (id, fl, uid) = (rd32(e, 4).unwrap_or(0), rd32(e, 8).unwrap_or(0), rd32(e, 12).unwrap_or(0));
                if fl & 1 == 0 {
                    d.cpus_disabled += 1;
                } else if !d.cpus[..d.ncpus].iter().any(|c| c.apic_id == id) {
                    push(&mut d.cpus, &mut d.ncpus, &mut d.truncated, Cpu { apic_id: id, acpi_uid: uid, x2apic: true });
                }
                true
            }
            _ => true,
        };
        if !ok {
            d.bad_entries += 1;
        }
    }
    Ok(d)
}

// ---------------------------------------------------------------- HPET

#[derive(Clone, Copy, Default, Debug, PartialEq, Eq)]
pub struct Hpet {
    pub addr: u64,
    pub block_id: u32,
    pub number: u8,
    pub min_tick: u16,
}

pub fn parse_hpet(t: &[u8]) -> Result<Hpet, AcpiError> {
    check_sig(t, b"HPET", 56)?;
    // Generic Address Structure at 40: space id 0 = system memory
    if rd8(t, 40) != Some(0) {
        return Err(AcpiError::BadValue);
    }
    let h = Hpet {
        block_id: rd32(t, 36).ok_or(AcpiError::BadLength)?,
        addr: rd64(t, 44).ok_or(AcpiError::BadLength)?,
        number: rd8(t, 52).ok_or(AcpiError::BadLength)?,
        min_tick: rd16(t, 53).ok_or(AcpiError::BadLength)?,
    };
    if h.addr == 0 {
        return Err(AcpiError::BadValue);
    }
    if h.addr % 1024 != 0 {
        return Err(AcpiError::BadAlignment);
    }
    Ok(h)
}

// ---------------------------------------------------------------- MCFG

#[derive(Clone, Copy, Default, Debug, PartialEq, Eq)]
pub struct McfgEntry {
    pub base: u64,
    pub segment: u16,
    pub start_bus: u8,
    pub end_bus: u8,
}

pub struct Mcfg {
    pub n: usize,
    pub entries: [McfgEntry; MAX_MCFG],
    pub bad_entries: usize,
    pub truncated: bool,
}

pub fn parse_mcfg(t: &[u8]) -> Result<Mcfg, AcpiError> {
    check_sig(t, b"MCFG", 44)?;
    if (t.len() - 44) % 16 != 0 {
        return Err(AcpiError::BadLength);
    }
    let mut d = Mcfg { n: 0, entries: [McfgEntry::default(); MAX_MCFG], bad_entries: 0, truncated: false };
    for e in t[44..].chunks_exact(16) {
        let m = McfgEntry {
            base: rd64(e, 0).unwrap_or(0),
            segment: rd16(e, 8).unwrap_or(0),
            start_bus: e[10],
            end_bus: e[11],
        };
        // ECAM: 1 MiB per bus, the window must be 1 MiB aligned
        if m.base == 0 || m.base % (1 << 20) != 0 || m.end_bus < m.start_bus {
            d.bad_entries += 1;
            continue;
        }
        push(&mut d.entries, &mut d.n, &mut d.truncated, m);
    }
    Ok(d)
}

// ---------------------------------------------------------------- FADT

#[derive(Clone, Copy, Default, Debug, PartialEq, Eq)]
pub struct Fadt {
    pub revision: u8,
    pub sci_irq: u16,
    pub smi_cmd: u32,
    pub pm_tmr_blk: u32,
    pub pm_tmr_len: u8,
    pub iapc_boot_arch: u16,
    pub flags: u32,
    pub dsdt: u64,
    pub has_8042: bool,
    pub hw_reduced: bool,
}

pub const FADT_IAPC_8042: u16 = 1 << 1;
pub const FADT_HW_REDUCED: u32 = 1 << 20;

/// FADT fields up to `flags` (ACPI 1.0 length, 116 bytes) are required;
/// IAPC_BOOT_ARCH is read from revision 3 (ACPI 2.0) on, X_DSDT when the
/// table is long enough. Before ACPI 2.0 a PC always has an 8042.
pub fn parse_fadt(t: &[u8]) -> Result<Fadt, AcpiError> {
    check_sig(t, b"FACP", 116)?;
    let revision = t[8];
    let iapc = if revision >= 3 { rd16(t, 109).unwrap_or(0) } else { FADT_IAPC_8042 };
    let flags = rd32(t, 112).ok_or(AcpiError::BadLength)?;
    let x_dsdt = if t.len() >= 148 { rd64(t, 140).unwrap_or(0) } else { 0 };
    let dsdt = if x_dsdt != 0 { x_dsdt } else { rd32(t, 40).unwrap_or(0) as u64 };
    let pm_tmr_len = t[91];
    Ok(Fadt {
        revision,
        sci_irq: rd16(t, 46).unwrap_or(0),
        smi_cmd: rd32(t, 48).unwrap_or(0),
        pm_tmr_blk: if pm_tmr_len == 4 { rd32(t, 76).unwrap_or(0) } else { 0 },
        pm_tmr_len,
        iapc_boot_arch: iapc,
        flags,
        dsdt,
        has_8042: iapc & FADT_IAPC_8042 != 0,
        hw_reduced: flags & FADT_HW_REDUCED != 0,
    })
}

// ---------------------------------------------------------------- C view

/// Mirror of `struct acpi_info` in include/magia/acpi.h.
#[repr(C)]
#[derive(Clone, Copy)]
pub struct CAcpiInfo {
    pub found: u32, // ACPI_FOUND_* bits
    pub rsdp_revision: u32,
    pub ncpus: u32,
    pub cpus_disabled: u32,
    pub cpu_apic_id: [u32; MAX_CPUS],
    pub lapic_addr: u64,
    pub pcat_compat: u32,
    pub nioapics: u32,
    pub ioapic_id: [u32; MAX_IOAPICS],
    pub ioapic_addr: [u32; MAX_IOAPICS],
    pub ioapic_gsi_base: [u32; MAX_IOAPICS],
    pub nisos: u32,
    pub iso_irq: [u8; MAX_ISOS],
    pub iso_flags: [u16; MAX_ISOS],
    pub iso_gsi: [u32; MAX_ISOS],
    pub nnmis: u32,
    pub nmi_lint: [u8; MAX_NMIS],
    pub nmi_flags: [u16; MAX_NMIS],
    pub nmi_uid: [u32; MAX_NMIS],
    pub madt_bad_entries: u32,
    pub hpet_addr: u64,
    pub hpet_min_tick: u32,
    pub nmcfg: u32,
    pub mcfg_base: [u64; MAX_MCFG],
    pub mcfg_segment: [u16; MAX_MCFG],
    pub mcfg_start_bus: [u8; MAX_MCFG],
    pub mcfg_end_bus: [u8; MAX_MCFG],
    pub fadt_revision: u32,
    pub fadt_flags: u32,
    pub fadt_iapc_boot_arch: u32,
    pub sci_irq: u32,
    pub pm_tmr_blk: u32,
    pub has_8042: u32,
    pub ntables: u32,
    pub table_sig: [[u8; 4]; MAX_TABLES],
}

pub const FOUND_MADT: u32 = 1;
pub const FOUND_HPET: u32 = 2;
pub const FOUND_MCFG: u32 = 4;
pub const FOUND_FADT: u32 = 8;
pub const FOUND_TRUNCATED: u32 = 0x100;

impl CAcpiInfo {
    pub fn zeroed() -> Self {
        // SAFETY: every field is an integer or an array of integers.
        unsafe { core::mem::zeroed() }
    }
}

/// Parse everything the kernel needs. A bad optional table (HPET, MCFG,
/// FADT) is skipped and reported by its missing ACPI_FOUND_* bit; a bad
/// RSDP or root table is an error.
pub fn parse_all(m: &dyn PhysMem, rsdp_pa: u64, bios: bool, out: &mut CAcpiInfo) -> Result<(), AcpiError> {
    *out = CAcpiInfo::zeroed();
    let r = parse_rsdp(m, rsdp_pa, bios)?;
    out.rsdp_revision = r.revision as u32;
    let root = root_tables(m, &r)?;
    if root.truncated {
        out.found |= FOUND_TRUNCATED;
    }
    for (i, &pa) in root.entries.iter().take(root.n).enumerate() {
        if let Some(s) = m.read(pa, 4).and_then(|h| h.get(0..4)) {
            out.table_sig[i].copy_from_slice(s);
            out.ntables += 1;
        }
    }
    if let Ok(d) = find_table(m, &root, b"APIC").and_then(parse_madt) {
        out.found |= FOUND_MADT;
        if d.truncated {
            out.found |= FOUND_TRUNCATED;
        }
        out.ncpus = d.ncpus as u32;
        out.cpus_disabled = d.cpus_disabled as u32;
        for (o, c) in out.cpu_apic_id.iter_mut().zip(&d.cpus[..d.ncpus]) {
            *o = c.apic_id;
        }
        out.lapic_addr = d.lapic_addr;
        out.pcat_compat = d.pcat_compat as u32;
        out.nioapics = d.nioapics as u32;
        for (i, io) in d.ioapics[..d.nioapics].iter().enumerate() {
            out.ioapic_id[i] = io.id as u32;
            out.ioapic_addr[i] = io.addr;
            out.ioapic_gsi_base[i] = io.gsi_base;
        }
        out.nisos = d.nisos as u32;
        for (i, s) in d.isos[..d.nisos].iter().enumerate() {
            out.iso_irq[i] = s.irq;
            out.iso_flags[i] = s.flags;
            out.iso_gsi[i] = s.gsi;
        }
        out.nnmis = d.nnmis as u32;
        for (i, n) in d.nmis[..d.nnmis].iter().enumerate() {
            out.nmi_lint[i] = n.lint;
            out.nmi_flags[i] = n.flags;
            out.nmi_uid[i] = n.acpi_uid;
        }
        out.madt_bad_entries = d.bad_entries as u32;
    }
    if let Ok(h) = find_table(m, &root, b"HPET").and_then(parse_hpet) {
        out.found |= FOUND_HPET;
        out.hpet_addr = h.addr;
        out.hpet_min_tick = h.min_tick as u32;
    }
    if let Ok(c) = find_table(m, &root, b"MCFG").and_then(parse_mcfg) {
        out.found |= FOUND_MCFG;
        out.nmcfg = c.n as u32;
        for (i, e) in c.entries[..c.n].iter().enumerate() {
            out.mcfg_base[i] = e.base;
            out.mcfg_segment[i] = e.segment;
            out.mcfg_start_bus[i] = e.start_bus;
            out.mcfg_end_bus[i] = e.end_bus;
        }
    }
    if let Ok(f) = find_table(m, &root, b"FACP").and_then(parse_fadt) {
        out.found |= FOUND_FADT;
        out.fadt_revision = f.revision as u32;
        out.fadt_flags = f.flags;
        out.fadt_iapc_boot_arch = f.iapc_boot_arch as u32;
        out.sci_irq = f.sci_irq as u32;
        out.pm_tmr_blk = f.pm_tmr_blk;
        out.has_8042 = f.has_8042 as u32;
    }
    Ok(())
}

// ---------------------------------------------------------------- tests

#[cfg(test)]
mod tests {
    use super::*;
    extern crate std;
    use std::vec;
    use std::vec::Vec;

    /// Fake physical memory: `bytes` at physical address `base`.
    struct Mem {
        base: u64,
        bytes: Vec<u8>,
    }
    impl PhysMem for Mem {
        fn read(&self, pa: u64, len: usize) -> Option<&[u8]> {
            let off = usize::try_from(pa.checked_sub(self.base)?).ok()?;
            self.bytes.get(off..off.checked_add(len)?)
        }
    }
    impl Mem {
        fn put(&mut self, pa: u64, b: &[u8]) {
            let o = (pa - self.base) as usize;
            self.bytes[o..o + b.len()].copy_from_slice(b);
        }
    }

    fn sum_fix(t: &mut [u8], at: usize) {
        t[at] = 0;
        let s = t.iter().fold(0u8, |a, &x| a.wrapping_add(x));
        t[at] = s.wrapping_neg();
    }

    fn sdt(sig: &[u8; 4], rev: u8, body: &[u8]) -> Vec<u8> {
        let mut t = vec![0u8; 36];
        t[0..4].copy_from_slice(sig);
        t[4..8].copy_from_slice(&((36 + body.len()) as u32).to_le_bytes());
        t[8] = rev;
        t.extend_from_slice(body);
        sum_fix(&mut t, 9);
        t
    }

    fn rsdp_v2(xsdt: u64) -> Vec<u8> {
        let mut r = vec![0u8; 36];
        r[0..8].copy_from_slice(b"RSD PTR ");
        r[15] = 2;
        r[20..24].copy_from_slice(&36u32.to_le_bytes());
        r[24..32].copy_from_slice(&xsdt.to_le_bytes());
        sum_fix(&mut r[..20], 8);
        sum_fix(&mut r, 32);
        r
    }

    fn ent(ty: u8, body: &[u8]) -> Vec<u8> {
        let mut e = vec![ty, (2 + body.len()) as u8];
        e.extend_from_slice(body);
        e
    }
    fn lapic(uid: u8, id: u8, en: bool) -> Vec<u8> {
        ent(0, &[uid, id, en as u8, 0, 0, 0])
    }
    fn ioapic(id: u8, addr: u32, gsi: u32) -> Vec<u8> {
        let mut b = vec![id, 0];
        b.extend_from_slice(&addr.to_le_bytes());
        b.extend_from_slice(&gsi.to_le_bytes());
        ent(1, &b)
    }
    fn iso(bus: u8, irq: u8, gsi: u32, flags: u16) -> Vec<u8> {
        let mut b = vec![bus, irq];
        b.extend_from_slice(&gsi.to_le_bytes());
        b.extend_from_slice(&flags.to_le_bytes());
        ent(2, &b)
    }

    /// A QEMU-like MADT: 4 enabled CPUs + 1 disabled, 1 IOAPIC, 2 ISOs, 1 NMI.
    fn good_madt() -> Vec<u8> {
        let mut body = vec![];
        body.extend_from_slice(&0xfee0_0000u32.to_le_bytes());
        body.extend_from_slice(&1u32.to_le_bytes());
        for i in 0..4 {
            body.extend(lapic(i, i, true));
        }
        body.extend(lapic(4, 4, false));
        body.extend(ioapic(0, 0xfec0_0000, 0));
        body.extend(iso(0, 0, 2, 0));
        body.extend(iso(0, 9, 9, 0xd));
        body.extend(ent(4, &[0xff, 0, 0, 1]));
        sdt(b"APIC", 1, &body)
    }

    fn good_hpet() -> Vec<u8> {
        let mut b = vec![0u8; 20];
        b[0..4].copy_from_slice(&0x8086_a201u32.to_le_bytes());
        b[4] = 0; // GAS: system memory
        b[8..16].copy_from_slice(&0xfed0_0000u64.to_le_bytes());
        b[17..19].copy_from_slice(&128u16.to_le_bytes());
        sdt(b"HPET", 1, &b)
    }

    fn good_mcfg() -> Vec<u8> {
        let mut b = vec![0u8; 8];
        b.extend_from_slice(&0xb000_0000u64.to_le_bytes());
        b.extend_from_slice(&[0, 0, 0, 255, 0, 0, 0, 0]);
        sdt(b"MCFG", 1, &b)
    }

    fn good_fadt() -> Vec<u8> {
        let mut b = vec![0u8; 244 - 36];
        b[46 - 36..48 - 36].copy_from_slice(&9u16.to_le_bytes());
        b[76 - 36..80 - 36].copy_from_slice(&0x608u32.to_le_bytes());
        b[91 - 36] = 4;
        b[109 - 36..111 - 36].copy_from_slice(&FADT_IAPC_8042.to_le_bytes());
        sdt(b"FACP", 3, &b)
    }

    const RSDP: u64 = 0x1000;
    const XSDT: u64 = 0x1100;

    /// RSDP -> XSDT -> tables at 0x2000, 0x3000, ...
    fn machine(tables: &[Vec<u8>]) -> Mem {
        let mut m = Mem { base: 0x1000, bytes: vec![0u8; 0x10000] };
        m.put(RSDP, &rsdp_v2(XSDT));
        let mut body = vec![];
        for (i, t) in tables.iter().enumerate() {
            let pa = 0x2000 + 0x1000 * i as u64;
            m.put(pa, t);
            body.extend_from_slice(&pa.to_le_bytes());
        }
        m.put(XSDT, &sdt(b"XSDT", 1, &body));
        m
    }

    fn all_tables() -> Vec<Vec<u8>> {
        vec![good_madt(), good_hpet(), good_mcfg(), good_fadt()]
    }

    #[test]
    fn good_machine_parses_completely() {
        let m = machine(&all_tables());
        let r = parse_rsdp(&m, RSDP, true).unwrap();
        assert_eq!((r.revision, r.xsdt), (2, XSDT));
        let root = root_tables(&m, &r).unwrap();
        assert!(root.is_xsdt && root.n == 4 && !root.truncated);
        assert_eq!(&find_table(&m, &root, b"APIC").unwrap()[0..4], b"APIC");
        assert_eq!(find_table(&m, &root, b"SSDT").err(), Some(AcpiError::NotFound));

        let mut i = CAcpiInfo::zeroed();
        parse_all(&m, RSDP, true, &mut i).unwrap();
        assert_eq!(i.found, FOUND_MADT | FOUND_HPET | FOUND_MCFG | FOUND_FADT);
        assert_eq!((i.ncpus, i.cpus_disabled, i.nioapics, i.nisos, i.nnmis), (4, 1, 1, 2, 1));
        assert_eq!(&i.cpu_apic_id[..4], &[0, 1, 2, 3]);
        assert_eq!((i.lapic_addr, i.pcat_compat), (0xfee0_0000, 1));
        assert_eq!((i.ioapic_addr[0], i.ioapic_gsi_base[0]), (0xfec0_0000, 0));
        assert_eq!((i.iso_irq[0], i.iso_gsi[0]), (0, 2));
        assert_eq!((i.iso_irq[1], i.iso_gsi[1], i.iso_flags[1]), (9, 9, 0xd));
        assert_eq!((i.nmi_uid[0], i.nmi_lint[0]), (0xffff_ffff, 1));
        assert_eq!((i.hpet_addr, i.hpet_min_tick), (0xfed0_0000, 128));
        assert_eq!((i.nmcfg, i.mcfg_base[0], i.mcfg_end_bus[0]), (1, 0xb000_0000, 255));
        assert_eq!((i.fadt_revision, i.sci_irq, i.pm_tmr_blk, i.has_8042), (3, 9, 0x608, 1));
        assert_eq!(i.ntables, 4);
        assert_eq!(&i.table_sig[3], b"FACP");
    }

    #[test]
    fn rsdp_v1_uses_the_rsdt() {
        let mut m = Mem { base: 0x1000, bytes: vec![0u8; 0x4000] };
        let mut r = vec![0u8; 20];
        r[0..8].copy_from_slice(b"RSD PTR ");
        r[16..20].copy_from_slice(&0x1100u32.to_le_bytes());
        sum_fix(&mut r, 8);
        m.put(0x1000, &r);
        m.put(0x2000, &good_madt());
        m.put(0x1100, &sdt(b"RSDT", 1, &0x2000u32.to_le_bytes()));
        let rs = parse_rsdp(&m, 0x1000, true).unwrap();
        assert_eq!((rs.revision, rs.xsdt, rs.len), (0, 0, 20));
        let root = root_tables(&m, &rs).unwrap();
        assert!(!root.is_xsdt && root.n == 1);
        assert!(find_table(&m, &root, b"APIC").is_ok());
    }

    #[test]
    fn rsdp_errors() {
        let good = rsdp_v2(XSDT);
        let at = |b: &[u8], pa: u64| {
            let mut m = Mem { base: 0x1000, bytes: vec![0u8; 0x1000] };
            m.put(0x1000, b);
            parse_rsdp(&m, pa, true)
        };
        let mut b = good.clone();
        b[0] = b'X';
        assert_eq!(at(&b, 0x1000), Err(AcpiError::BadSignature));
        let mut b = good.clone();
        b[17] ^= 1; // inside the first 20 bytes: both checksums break
        assert_eq!(at(&b, 0x1000), Err(AcpiError::BadChecksum));
        let mut b = good.clone();
        b[30] ^= 1; // only the extended checksum breaks
        assert_eq!(at(&b, 0x1000), Err(AcpiError::BadChecksum));
        assert_eq!(at(&good, 0x1008), Err(AcpiError::BadAlignment));
        let mut m = Mem { base: 0x1000, bytes: vec![0u8; 0x1000] };
        m.put(0x1014, &good);
        assert!(parse_rsdp(&m, 0x1014, false).is_ok()); // UEFI: no alignment rule
        let mut b = good.clone();
        b[20..24].copy_from_slice(&20u32.to_le_bytes());
        sum_fix(&mut b, 32);
        assert_eq!(at(&b, 0x1000), Err(AcpiError::BadLength));
        let mut b = good.clone();
        b[20..24].copy_from_slice(&0xffff_ffffu32.to_le_bytes());
        sum_fix(&mut b, 32);
        assert_eq!(at(&b, 0x1000), Err(AcpiError::BadLength));
        // truncated: only 16 bytes of memory exist
        let m = Mem { base: 0x1000, bytes: good[..16].to_vec() };
        assert_eq!(parse_rsdp(&m, 0x1000, true), Err(AcpiError::Unmapped));
        // the last valid address: no overflow on pa + len
        let m = Mem { base: 0, bytes: vec![] };
        assert_eq!(parse_rsdp(&m, u64::MAX - 15, false), Err(AcpiError::BadLength));
    }

    #[test]
    fn read_table_errors() {
        let mut m = Mem { base: 0, bytes: vec![0u8; 100] };
        let t = sdt(b"APIC", 1, &[0u8; 64]); // 100 bytes: fits exactly
        m.put(0, &t);
        assert!(read_table(&m, 0, b"APIC").is_ok());
        assert_eq!(read_table(&m, 0, b"HPET"), Err(AcpiError::BadSignature));
        let mut t2 = t.clone();
        t2[4..8].copy_from_slice(&101u32.to_le_bytes()); // truncated
        m.put(0, &t2);
        assert_eq!(read_table(&m, 0, b"APIC"), Err(AcpiError::Unmapped));
        for bad in [0u32, 10, 35, MAX_TABLE_LEN + 1, 0xffff_ffff] {
            t2[4..8].copy_from_slice(&bad.to_le_bytes());
            m.put(0, &t2);
            assert_eq!(read_table(&m, 0, b"APIC"), Err(AcpiError::BadLength), "len {bad}");
        }
        let mut t3 = t.clone();
        t3[50] ^= 0x40;
        m.put(0, &t3);
        assert_eq!(read_table(&m, 0, b"APIC"), Err(AcpiError::BadChecksum));
        assert_eq!(read_table(&m, 4096, b"APIC"), Err(AcpiError::Unmapped));
        assert_eq!(read_table(&m, u64::MAX - 10, b"APIC"), Err(AcpiError::BadLength));
    }

    #[test]
    fn root_table_errors() {
        // entry area not a multiple of 8
        let mut m = machine(&[]);
        m.put(XSDT, &sdt(b"XSDT", 1, &[0u8; 12]));
        let r = parse_rsdp(&m, RSDP, true).unwrap();
        assert_eq!(root_tables(&m, &r).err(), Some(AcpiError::BadLength));
        // an entry pointing at the XSDT itself / into the RSDP
        for bad in [XSDT, XSDT + 8, RSDP + 4] {
            m.put(XSDT, &sdt(b"XSDT", 1, &bad.to_le_bytes()));
            assert_eq!(root_tables(&m, &r).err(), Some(AcpiError::Overlap), "entry {bad:#x}");
        }
        // zero and duplicate entries are skipped
        let mut body = vec![];
        for pa in [0u64, 0x2000, 0x2000, 0, 0x3000] {
            body.extend_from_slice(&pa.to_le_bytes());
        }
        m.put(XSDT, &sdt(b"XSDT", 1, &body));
        let root = root_tables(&m, &r).unwrap();
        assert_eq!(&root.entries[..root.n], &[0x2000, 0x3000]);
        // huge count: 200 distinct entries, kept to MAX_TABLES
        let body: Vec<u8> = (0..200u64).flat_map(|i| (0x10_0000 + i * 64).to_le_bytes()).collect();
        let mut m2 = Mem { base: 0x1000, bytes: vec![0u8; 0x4000] };
        m2.put(RSDP, &rsdp_v2(XSDT));
        m2.put(XSDT, &sdt(b"XSDT", 1, &body));
        let root = root_tables(&m2, &r).unwrap();
        assert!(root.n == MAX_TABLES && root.truncated);
        // unmapped table entries are simply not found
        assert_eq!(find_table(&m2, &root, b"APIC").err(), Some(AcpiError::NotFound));
        // the root table must not overlap the RSDP
        let r2 = Rsdp { xsdt: RSDP + 16, ..r };
        assert_eq!(root_tables(&m, &r2).err(), Some(AcpiError::Overlap));
    }

    #[test]
    fn find_table_skips_corrupt_copies() {
        let mut bad = good_madt();
        bad[40] ^= 1; // checksum now wrong
        let m = machine(&[bad, good_madt()]);
        let root = root_tables(&m, &parse_rsdp(&m, RSDP, true).unwrap()).unwrap();
        let t = find_table(&m, &root, b"APIC").unwrap();
        assert!(checksum_ok(t));
    }

    #[test]
    fn madt_bad_entries_are_skipped_or_stop_the_walk() {
        let hdr = |extra: &[u8]| {
            let mut b = vec![0u8; 8];
            b.extend_from_slice(extra);
            sdt(b"APIC", 1, &b)
        };
        // zero length: stop
        let d = parse_madt(&hdr(&[0, 0, 1, 2])).unwrap();
        assert_eq!((d.ncpus, d.bad_entries), (0, 1));
        // length 1: stop
        let d = parse_madt(&hdr(&[0, 1])).unwrap();
        assert_eq!(d.bad_entries, 1);
        // runs past the end: stop after the good entry
        let mut b = lapic(0, 0, true);
        b.extend_from_slice(&[0, 40, 0, 0]);
        let d = parse_madt(&hdr(&b)).unwrap();
        assert_eq!((d.ncpus, d.bad_entries), (1, 1));
        // a lone trailing byte
        let mut b = lapic(0, 0, true);
        b.push(0);
        assert_eq!(parse_madt(&hdr(&b)).unwrap().bad_entries, 1);
        // too short for the type: skipped, walk continues
        let mut b = ent(0, &[0, 0, 1, 0]); // LAPIC of length 6
        b.extend(lapic(1, 1, true));
        b.extend(ent(9, &[0u8; 6])); // x2APIC of length 8
        let d = parse_madt(&hdr(&b)).unwrap();
        assert_eq!((d.ncpus, d.bad_entries), (1, 2));
        // ISO on a non-ISA bus or with irq >= 16, IOAPIC at 0, NMI on LINT2
        let mut b = iso(1, 4, 4, 0);
        b.extend(iso(0, 16, 16, 0));
        b.extend(ioapic(1, 0, 0));
        b.extend(ent(4, &[0, 0, 0, 2]));
        let d = parse_madt(&hdr(&b)).unwrap();
        assert_eq!((d.nisos, d.nioapics, d.nnmis, d.bad_entries), (0, 0, 0, 4));
        // header too short, wrong signature
        assert_eq!(parse_madt(&sdt(b"APIC", 1, &[0u8; 4])).err(), Some(AcpiError::BadLength));
        assert_eq!(parse_madt(&sdt(b"APIX", 1, &[0u8; 8])).err(), Some(AcpiError::BadSignature));
    }

    #[test]
    fn madt_x2apic_and_override() {
        let mut b = vec![0u8; 8];
        b.extend(lapic(0, 0, true));
        let mut x = vec![0, 0];
        x.extend_from_slice(&0u32.to_le_bytes()); // same id as the LAPIC entry: duplicate
        x.extend_from_slice(&1u32.to_le_bytes());
        x.extend_from_slice(&0u32.to_le_bytes());
        b.extend(ent(9, &x));
        let mut x = vec![0, 0];
        x.extend_from_slice(&300u32.to_le_bytes());
        x.extend_from_slice(&1u32.to_le_bytes());
        x.extend_from_slice(&7u32.to_le_bytes());
        b.extend(ent(9, &x));
        let mut o = vec![0, 0];
        o.extend_from_slice(&0x1_fee0_0000u64.to_le_bytes());
        b.extend(ent(5, &o));
        let d = parse_madt(&sdt(b"APIC", 1, &b)).unwrap();
        assert_eq!(d.ncpus, 2);
        assert_eq!(d.cpus[1], Cpu { apic_id: 300, acpi_uid: 7, x2apic: true });
        assert_eq!(d.lapic_addr, 0x1_fee0_0000);
    }

    #[test]
    fn madt_huge_counts_are_truncated() {
        let mut b = vec![0u8; 8];
        for i in 0..300u32 {
            b.extend(lapic(i as u8, i as u8, true));
        }
        for i in 0..40u32 {
            b.extend(ioapic(i as u8, 0xfec0_0000 + i * 0x1000, i * 24));
        }
        let d = parse_madt(&sdt(b"APIC", 1, &b)).unwrap();
        assert_eq!((d.ncpus, d.nioapics), (MAX_CPUS, MAX_IOAPICS));
        assert!(d.truncated);
    }

    #[test]
    fn hpet_mcfg_fadt_errors() {
        assert!(parse_hpet(&good_hpet()).is_ok());
        let mut h = good_hpet();
        h[40] = 1; // I/O space
        assert_eq!(parse_hpet(&h).err(), Some(AcpiError::BadValue));
        let mut h = good_hpet();
        h[44..52].copy_from_slice(&0xfed0_0010u64.to_le_bytes());
        assert_eq!(parse_hpet(&h).err(), Some(AcpiError::BadAlignment));
        let mut h = good_hpet();
        h[44..52].copy_from_slice(&0u64.to_le_bytes());
        assert_eq!(parse_hpet(&h).err(), Some(AcpiError::BadValue));
        assert_eq!(parse_hpet(&good_hpet()[..50]).err(), Some(AcpiError::BadLength));

        let m = parse_mcfg(&good_mcfg()).unwrap();
        assert_eq!((m.n, m.entries[0].start_bus, m.entries[0].end_bus), (1, 0, 255));
        assert_eq!(parse_mcfg(&sdt(b"MCFG", 1, &[0u8; 8 + 15])).err(), Some(AcpiError::BadLength));
        let mut b = vec![0u8; 8];
        b.extend_from_slice(&0xb000_1000u64.to_le_bytes()); // not 1 MiB aligned
        b.extend_from_slice(&[0, 0, 0, 255, 0, 0, 0, 0]);
        b.extend_from_slice(&0xc000_0000u64.to_le_bytes()); // end < start
        b.extend_from_slice(&[0, 0, 9, 3, 0, 0, 0, 0]);
        let m = parse_mcfg(&sdt(b"MCFG", 1, &b)).unwrap();
        assert_eq!((m.n, m.bad_entries), (0, 2));
        let b: Vec<u8> = vec![0u8; 8].into_iter().chain((0..20u64).flat_map(|i| {
            let mut e = ((i + 1) << 28).to_le_bytes().to_vec();
            e.extend_from_slice(&[0, 0, 0, 255, 0, 0, 0, 0]);
            e
        })).collect();
        let m = parse_mcfg(&sdt(b"MCFG", 1, &b)).unwrap();
        assert!(m.n == MAX_MCFG && m.truncated);

        let f = parse_fadt(&good_fadt()).unwrap();
        assert!(f.has_8042 && !f.hw_reduced);
        let mut f1 = good_fadt()[..116].to_vec(); // ACPI 1.0 length
        f1[4..8].copy_from_slice(&116u32.to_le_bytes());
        f1[8] = 1;
        sum_fix(&mut f1, 9);
        assert!(parse_fadt(&f1).unwrap().has_8042);
        let mut f3 = good_fadt();
        f3[109..111].copy_from_slice(&0u16.to_le_bytes()); // no 8042
        f3[112..116].copy_from_slice(&FADT_HW_REDUCED.to_le_bytes());
        let f = parse_fadt(&f3).unwrap();
        assert!(!f.has_8042 && f.hw_reduced);
        assert_eq!(parse_fadt(&good_fadt()[..100]).err(), Some(AcpiError::BadLength));
    }

    #[test]
    fn bad_optional_tables_are_skipped_not_fatal() {
        let mut h = good_hpet();
        h[40] = 1;
        sum_fix(&mut h, 9);
        let m = machine(&[good_madt(), h]);
        let mut i = CAcpiInfo::zeroed();
        parse_all(&m, RSDP, true, &mut i).unwrap();
        assert_eq!(i.found, FOUND_MADT);
        let m = Mem { base: 0, bytes: vec![0u8; 64] };
        assert_eq!(parse_all(&m, 0, true, &mut i), Err(AcpiError::BadSignature));
    }

    fn xorshift(s: &mut u64) -> u64 {
        *s ^= *s << 13;
        *s ^= *s >> 7;
        *s ^= *s << 17;
        *s
    }

    /// Random mutations of a whole machine (RSDP, XSDT and every table),
    /// with and without fixed-up checksums so mutations get past them:
    /// the parser must never panic, loop or read out of bounds.
    #[test]
    fn random_mutations_never_panic() {
        let base = machine(&all_tables());
        let mut s = 0x9e37_79b9_7f4a_7c15u64;
        for round in 0..4000 {
            let mut m = Mem { base: base.base, bytes: base.bytes.clone() };
            let region = (xorshift(&mut s) % 6) as u64; // which structure to hit
            let (start, len) = match region {
                0 => (RSDP, 36),
                1 => (XSDT, 36 + 8 * 4),
                r => (0x2000 + 0x1000 * (r - 2), 244),
            };
            for _ in 0..1 + xorshift(&mut s) % 4 {
                let pa = start + xorshift(&mut s) % len;
                let o = (pa - m.base) as usize;
                m.bytes[o] ^= (1 + xorshift(&mut s) % 255) as u8;
            }
            if round % 2 == 0 && region >= 1 {
                // repair the SDT checksum so the mutation reaches the parser
                let o = (start - m.base) as usize;
                let l = u32::from_le_bytes(m.bytes[o + 4..o + 8].try_into().unwrap()) as usize;
                if (36..=0x1000).contains(&l) && o + l <= m.bytes.len() {
                    sum_fix(&mut m.bytes[o..o + l], 9);
                }
            }
            let mut i = CAcpiInfo::zeroed();
            let _ = parse_all(&m, RSDP, true, &mut i);
            assert!(i.ncpus as usize <= MAX_CPUS && i.nioapics as usize <= MAX_IOAPICS);
        }
        // and every truncation of every table
        for t in all_tables() {
            for n in 0..t.len() {
                let _ = parse_madt(&t[..n]);
                let _ = parse_hpet(&t[..n]);
                let _ = parse_mcfg(&t[..n]);
                let _ = parse_fadt(&t[..n]);
            }
        }
    }
}
