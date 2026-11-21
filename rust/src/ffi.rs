// SPDX-License-Identifier: BSD-2-Clause
// Copyright (c) 2026 zenmastercat and Magia contributors
//! The C -> Rust boundary (include/magia/rust.h). This is the only file
//! with global kernel state: GIANT guards the `Kernel`; the scheduler
//! (SCHED) and jail memory accounting (ACCT) have their own locks in
//! kenv.rs.
//!
//! Lock order: GIANT -> SCHED, GIANT -> ACCT. SCHED and ACCT never take
//! another lock. No Rust lock is held across magia_sleep_commit or
//! magia_yield.

use crate::abi::*;
use crate::kenv::{self, ACCT, SCHED};
use crate::kernel::Kernel;
use crate::klock::KLock;
use crate::syscall::{Ctx, Ret};

static GIANT: KLock<Option<Kernel>> = KLock::new("GIANT", None);

fn bytes<'a>(p: *const u8, n: usize) -> &'a [u8] {
    if p.is_null() || n == 0 {
        &[]
    } else {
        // SAFETY: C passes (pointer, length) pairs of kernel memory.
        unsafe { core::slice::from_raw_parts(p, n) }
    }
}

fn text<'a>(p: *const u8, n: usize) -> &'a str {
    core::str::from_utf8(bytes(p, n)).unwrap_or("")
}

#[unsafe(no_mangle)]
pub extern "C" fn rs_init(bi: *const RsBootInfo) {
    // SAFETY: C passes a valid boot info struct.
    let bi = unsafe { &*bi };
    for (i, &(name, sz)) in ABI_SIZES.iter().enumerate() {
        if bi.abi_sizes[i] != sz as u64 {
            panic!("ABI mismatch: sizeof(struct {}) is {} in C, {} in Rust", name, bi.abi_sizes[i], sz);
        }
    }
    let tid0 = kenv::current_tid();
    let k = Kernel::new(if tid0 > 0 { tid0 } else { 1 });
    let nsys = crate::syscall::SYSCALLS.len();
    let nsysctl = k.sysctl.leaves().len();
    *GIANT.lock() = Some(k);
    crate::kprint!(
        "rust: {} syscalls, {} sysctl nodes, ramfs + devfs, jail 0 ({} pages, NX={} SMEP={} SMAP={})\n",
        nsys,
        nsysctl,
        bi.total_pages,
        bi.cpu_flags & RS_CPU_NX != 0,
        bi.cpu_flags & RS_CPU_SMEP != 0,
        bi.cpu_flags & RS_CPU_SMAP != 0
    );
}

/// System call entry for both ring 3 and kernel-thread processes.
#[unsafe(no_mangle)]
pub extern "C" fn rs_syscall(pid: i32, nr: u64, args: *const u64) -> i64 {
    let mut a = [0u64; 6];
    if !args.is_null() {
        // SAFETY: C passes an array of six u64 in kernel memory.
        unsafe { core::ptr::copy_nonoverlapping(args, a.as_mut_ptr(), 6) };
    }
    let mut ctx = Ctx::new(pid, kenv::current_tid());
    loop {
        let r = {
            let mut g = GIANT.lock();
            let Some(k) = g.as_mut() else { return -(ENOSYS as i64) };
            let r = k.syscall(&mut ctx, nr, &a);
            if let Ret::Block(t) = r {
                // still under GIANT: a waker needs GIANT, so no wakeup
                // can slip in between registration and prepare
                kenv::sleep_prepare(t);
            }
            r
        };
        match r {
            Ret::Val(v) => {
                if ctx.yield_after {
                    kenv::yield_now();
                }
                return v;
            }
            Ret::Block(_) => {
                ctx.timed_out = kenv::sleep_commit();
                ctx.retry += 1;
            }
        }
    }
}

#[unsafe(no_mangle)]
pub extern "C" fn rs_proc_spawn(ppid: i32, tid: i32, name: *const u8, namelen: usize) -> i32 {
    let mut g = GIANT.lock();
    let Some(k) = g.as_mut() else { return -ENOSYS };
    match k.spawn(ppid, tid, bytes(name, namelen)) {
        Ok(pid) => pid,
        Err(e) => -e,
    }
}

// ---- EEVDF scheduler (usable before rs_init) ----

#[unsafe(no_mangle)]
pub extern "C" fn rs_sched_add(tid: i32, weight: u32) {
    SCHED.lock().add(tid, weight)
}

#[unsafe(no_mangle)]
pub extern "C" fn rs_sched_remove(tid: i32) {
    SCHED.lock().remove(tid)
}

#[unsafe(no_mangle)]
pub extern "C" fn rs_sched_wakeup(tid: i32, now_ns: u64) -> i32 {
    SCHED.lock().wakeup(tid, now_ns) as i32
}

#[unsafe(no_mangle)]
pub extern "C" fn rs_sched_tick(now_ns: u64) -> i32 {
    SCHED.lock().tick(now_ns) as i32
}

#[unsafe(no_mangle)]
pub extern "C" fn rs_sched_pick(prev_tid: i32, prev_runnable: i32, now_ns: u64) -> i32 {
    SCHED.lock().pick(prev_tid, prev_runnable != 0, now_ns)
}

#[unsafe(no_mangle)]
pub extern "C" fn rs_sched_yield(tid: i32) {
    SCHED.lock().yield_(tid)
}

/// Timer IRQ, every ms. Skips the tick if GIANT is busy (cannot happen on
/// a uniprocessor, since GIANT disables IRQs) or before rs_init.
#[unsafe(no_mangle)]
pub extern "C" fn rs_timer_tick(now_ms: u64) {
    if let Some(mut g) = GIANT.try_lock() {
        if let Some(k) = g.as_mut() {
            k.timer_tick(now_ms);
        }
    }
}

// ---- jail memory accounting (ACCT only; called from C uvm_alloc) ----

#[unsafe(no_mangle)]
pub extern "C" fn rs_jail_mem_charge(jid: i32, bytes: u64) -> i32 {
    match ACCT.lock().charge(jid, bytes) {
        Ok(()) => 0,
        Err(_) => -ENOMEM,
    }
}

#[unsafe(no_mangle)]
pub extern "C" fn rs_jail_mem_uncharge(jid: i32, bytes: u64) {
    ACCT.lock().uncharge(jid, bytes)
}

// ---- debug monitor ----

fn with_kernel(f: impl FnOnce(&mut Kernel)) {
    let mut g = GIANT.lock();
    match g.as_mut() {
        Some(k) => f(k),
        None => crate::kprint!("rust subsystems not initialised\n"),
    }
}

#[unsafe(no_mangle)]
pub extern "C" fn rs_dbg_ps() {
    with_kernel(|k| k.dbg_ps())
}

#[unsafe(no_mangle)]
pub extern "C" fn rs_dbg_jails() {
    with_kernel(|k| k.dbg_jails())
}

#[unsafe(no_mangle)]
pub extern "C" fn rs_dbg_sysctl(name: *const u8, namelen: usize, value: *const u8, valuelen: usize) {
    with_kernel(|k| k.dbg_sysctl(text(name, namelen), text(value, valuelen)))
}

#[unsafe(no_mangle)]
pub extern "C" fn rs_dbg_ls(path: *const u8, len: usize) {
    with_kernel(|k| k.dbg_ls(bytes(path, len)))
}

#[unsafe(no_mangle)]
pub extern "C" fn rs_dbg_cat(path: *const u8, len: usize) {
    with_kernel(|k| k.dbg_cat(bytes(path, len)))
}

#[unsafe(no_mangle)]
pub extern "C" fn rs_dbg_uname() {
    with_kernel(|k| k.dbg_uname())
}

// ---- ACPI (rust/src/acpi.rs; C side kernel/acpi.c) ----

unsafe extern "C" {
    fn magia_acpi_map(pa: u64, len: usize) -> *const u8;
}

struct KernelPhys;
impl crate::acpi::PhysMem for KernelPhys {
    fn read(&self, pa: u64, len: usize) -> Option<&[u8]> {
        // SAFETY: magia_acpi_map returns NULL or a direct-map pointer to
        // `len` bytes of mapped, read-only-in-practice firmware memory.
        let p = unsafe { magia_acpi_map(pa, len) };
        if p.is_null() { None } else { Some(unsafe { core::slice::from_raw_parts(p, len) }) }
    }
}

/// Parse the ACPI tables from the RSDP; 0 or -code (size mismatch: -99).
#[unsafe(no_mangle)]
pub extern "C" fn rs_acpi_parse(rsdp_pa: u64, bios: i32, out: *mut crate::acpi::CAcpiInfo, size: usize) -> i32 {
    if out.is_null() || size != core::mem::size_of::<crate::acpi::CAcpiInfo>() {
        return -99;
    }
    // SAFETY: C passes a pointer to its `struct acpi_info` of `size` bytes.
    let out = unsafe { &mut *out };
    match crate::acpi::parse_all(&KernelPhys, rsdp_pa, bios != 0, out) {
        Ok(()) => 0,
        Err(e) => -e.code(),
    }
}

// ---- USB (rust/src/usb.rs; C side kernel/xhci.c, include/magia/usb.h) ----

#[repr(C)]
pub struct CUsbDev {
    usb_version: u16,
    vendor: u16,
    product: u16,
    class: u8,
    subclass: u8,
    protocol: u8,
    max_packet0: u8,
    num_configs: u8,
    _pad: [u8; 3],
}

#[repr(C)]
pub struct CUsbBootKbd {
    config_value: u8,
    interface: u8,
    alt_setting: u8,
    ep_addr: u8,
    ep_max_packet: u16,
    ep_interval: u8,
    _pad: u8,
}

/// Report state of each USB keyboard (C hands out the index); the xHCI
/// interrupt handler is its only user.
pub const USB_MAX_KBDS: usize = 8;
static USB_KBD: KLock<[crate::usb::BootKbd; USB_MAX_KBDS]> =
    KLock::new("usbkbd", [const { crate::usb::BootKbd::new() }; USB_MAX_KBDS]);

#[unsafe(no_mangle)]
pub extern "C" fn rs_usb_max_packet0(b: *const u8, n: usize) -> i32 {
    crate::usb::max_packet0_from_prefix(bytes(b, n)).map_or(-1, i32::from)
}

#[unsafe(no_mangle)]
pub extern "C" fn rs_usb_device_desc(b: *const u8, n: usize, out: *mut CUsbDev) -> i32 {
    let Ok(d) = crate::usb::parse_device_desc(bytes(b, n)) else { return -1 };
    if out.is_null() {
        return -1;
    }
    // SAFETY: C passes a pointer to its struct rs_usb_dev.
    unsafe {
        *out = CUsbDev {
            usb_version: d.usb_version,
            vendor: d.vendor,
            product: d.product,
            class: d.class,
            subclass: d.subclass,
            protocol: d.protocol,
            max_packet0: d.max_packet0,
            num_configs: d.num_configs,
            _pad: [0; 3],
        };
    }
    0
}

#[unsafe(no_mangle)]
pub extern "C" fn rs_usb_config_total(b: *const u8, n: usize) -> i32 {
    crate::usb::config_total_length(bytes(b, n)).map_or(-1, i32::from)
}

#[unsafe(no_mangle)]
pub extern "C" fn rs_usb_find_boot_kbd(b: *const u8, n: usize, out: *mut CUsbBootKbd) -> i32 {
    let Ok(k) = crate::usb::find_boot_keyboard(bytes(b, n)) else { return -1 };
    if out.is_null() {
        return -1;
    }
    // SAFETY: C passes a pointer to its struct rs_usb_bootkbd.
    unsafe {
        *out = CUsbBootKbd {
            config_value: k.config_value,
            interface: k.interface,
            alt_setting: k.alt_setting,
            ep_addr: k.ep_addr,
            ep_max_packet: k.ep_max_packet,
            ep_interval: k.ep_interval,
            _pad: 0,
        };
    }
    0
}

/// Keyboard `id`'s report -> ASCII (and VT220 sequences); 0 for a bad id.
#[unsafe(no_mangle)]
pub extern "C" fn rs_usb_kbd_report(id: u32, r: *const u8, n: usize, out: *mut u8, outlen: usize) -> usize {
    if out.is_null() || outlen == 0 {
        return 0;
    }
    // SAFETY: C passes (pointer, length) of its output buffer.
    let out = unsafe { core::slice::from_raw_parts_mut(out, outlen) };
    match USB_KBD.lock().get_mut(id as usize) {
        Some(k) => k.report(bytes(r, n), out),
        None => 0,
    }
}

/// A new keyboard takes slot `id`: forget the previous keyboard's state.
#[unsafe(no_mangle)]
pub extern "C" fn rs_usb_kbd_reset(id: u32) {
    if let Some(k) = USB_KBD.lock().get_mut(id as usize) {
        *k = crate::usb::BootKbd::new();
    }
}

fn put_iface(out: *mut CUsbBootKbd, i: CUsbBootKbd) -> i32 {
    if out.is_null() {
        return -1;
    }
    // SAFETY: C passes a pointer to its struct rs_usb_iface.
    unsafe { *out = i };
    0
}

/// The HID boot mouse interface (same C struct as the keyboard's).
#[unsafe(no_mangle)]
pub extern "C" fn rs_usb_find_boot_mouse(b: *const u8, n: usize, out: *mut CUsbBootKbd) -> i32 {
    let Ok(m) = crate::usbhub::find_boot_mouse(bytes(b, n)) else { return -1 };
    put_iface(out, CUsbBootKbd {
        config_value: m.config_value,
        interface: m.interface,
        alt_setting: 0,
        ep_addr: m.ep_addr,
        ep_max_packet: m.ep_max_packet,
        ep_interval: m.ep_interval,
        _pad: 0,
    })
}

/// The hub interface; the last byte is the multi-TT alternate setting (0xff: none).
#[unsafe(no_mangle)]
pub extern "C" fn rs_usb_find_hub(b: *const u8, n: usize, out: *mut CUsbBootKbd) -> i32 {
    let Ok(h) = crate::usbhub::find_hub(bytes(b, n)) else { return -1 };
    put_iface(out, CUsbBootKbd {
        config_value: h.config_value,
        interface: h.interface,
        alt_setting: 0,
        ep_addr: h.ep_addr,
        ep_max_packet: h.ep_max_packet,
        ep_interval: h.ep_interval,
        _pad: h.mtt_alt,
    })
}

/// Hub descriptor -> nports | tt_think << 8 | ss << 10 | power_on_ms << 16, or -1.
#[unsafe(no_mangle)]
pub extern "C" fn rs_usb_hub_desc(b: *const u8, n: usize, ss: i32) -> i64 {
    match crate::usbhub::parse_hub_desc(bytes(b, n), ss != 0) {
        Ok(d) => d.nports as i64 | (d.tt_think as i64) << 8 | (d.ss as i64) << 10 | (d.power_on_ms as i64) << 16,
        Err(_) => -1,
    }
}

/// GET_STATUS(port) answer -> wPortStatus | wPortChange << 16, or -1.
#[unsafe(no_mangle)]
pub extern "C" fn rs_usb_port_status(b: *const u8, n: usize) -> i64 {
    match crate::usbhub::parse_port_status(bytes(b, n)) {
        Ok(s) => s.status as i64 | (s.change as i64) << 16,
        Err(_) => -1,
    }
}

/// Boot mouse report -> buttons | dx << 8 | dy << 16 | wheel << 24 (bytes), or -1.
#[unsafe(no_mangle)]
pub extern "C" fn rs_usb_mouse_report(r: *const u8, n: usize) -> i64 {
    match crate::usbhub::parse_mouse_report(bytes(r, n)) {
        Some(m) => {
            m.buttons as i64 | (m.dx as u8 as i64) << 8 | (m.dy as u8 as i64) << 16 | (m.wheel as u8 as i64) << 24
        }
        None => -1,
    }
}
