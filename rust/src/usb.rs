// SPDX-License-Identifier: BSD-2-Clause
// Copyright (c) 2026 zenmastercat and Magia contributors
//! USB data from devices, parsed as untrusted input: device and configuration
//! descriptors (USB 2.0 spec ch. 9), the HID boot keyboard (HID 1.11 appendix B)
//! and its 8-byte input reports, translated to ASCII (US layout, HID usage
//! tables 1.12 ch. 10).
//!
//! First draft by the local model (qwen3.6:27b); reviewed and fixed (the
//! "new key" rule compares against the whole previous report, a duplicated
//! function removed), plus the C interface.

#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum UsbError { Short, BadLength, BadType, NotFound }

#[derive(Debug, Clone, Copy, PartialEq, Eq, Default)]
pub struct DeviceDesc {
    pub usb_version: u16, pub class: u8, pub subclass: u8, pub protocol: u8,
    pub max_packet0: u8, pub vendor: u16, pub product: u16, pub num_configs: u8,
}

/// Device descriptor: bLength (byte 0) must be >= 18 and <= b.len(), bDescriptorType (byte 1) == 1.
/// Fields: bcdUSB u16 LE @2, class @4, subclass @5, protocol @6, bMaxPacketSize0 @7, idVendor u16 @8,
/// idProduct u16 @10, bNumConfigurations @17. bMaxPacketSize0 must be 8, 16, 32, 64 or 9 (USB 3: 2^9), else BadLength.
pub fn parse_device_desc(b: &[u8]) -> Result<DeviceDesc, UsbError> {
    if b.len() < 2 { return Err(UsbError::Short); }
    let b_length = b[0] as usize;
    if b_length < 18 || b_length > b.len() { return Err(UsbError::BadLength); }
    if b[1] != 1 { return Err(UsbError::BadType); }
    let usb_version = u16::from_le_bytes([b[2], b[3]]);
    let class = b[4];
    let subclass = b[5];
    let protocol = b[6];
    let max_packet0 = b[7];
    if ![8, 16, 32, 64, 9].contains(&max_packet0) { return Err(UsbError::BadLength); }
    let vendor = u16::from_le_bytes([b[8], b[9]]);
    let product = u16::from_le_bytes([b[10], b[11]]);
    let num_configs = b[17];
    Ok(DeviceDesc { usb_version, class, subclass, protocol, max_packet0, vendor, product, num_configs })
}

/// Only the first 8 bytes may have been read so far: returns bMaxPacketSize0 (byte 7)
/// if b.len() >= 8, b[1] == 1 and the value is one of 8, 16, 32, 64, 9; else Err.
pub fn max_packet0_from_prefix(b: &[u8]) -> Result<u8, UsbError> {
    if b.len() < 8 { return Err(UsbError::Short); }
    if b[1] != 1 { return Err(UsbError::BadType); }
    let mp = b[7];
    if ![8, 16, 32, 64, 9].contains(&mp) { return Err(UsbError::BadLength); }
    Ok(mp)
}

/// Configuration descriptor header (9 bytes, type 2): returns wTotalLength (u16 @2).
/// Requires b.len() >= 9, b[0] >= 9, b[1] == 2, wTotalLength >= 9.
pub fn config_total_length(b: &[u8]) -> Result<u16, UsbError> {
    if b.len() < 9 { return Err(UsbError::Short); }
    if b[0] < 9 { return Err(UsbError::BadLength); }
    if b[1] != 2 { return Err(UsbError::BadType); }
    let w_total = u16::from_le_bytes([b[2], b[3]]);
    if w_total < 9 { return Err(UsbError::BadLength); }
    Ok(w_total)
}

#[derive(Debug, Clone, Copy, PartialEq, Eq, Default)]
pub struct BootKeyboard {
    pub config_value: u8,   // bConfigurationValue (config byte 5)
    pub interface: u8,      // bInterfaceNumber
    pub alt_setting: u8,
    pub ep_addr: u8,        // endpoint address, bit 7 set (IN)
    pub ep_max_packet: u16, // wMaxPacketSize & 0x7ff
    pub ep_interval: u8,    // bInterval
}

/// Walk a whole configuration descriptor (b = all wTotalLength bytes) and find the first
/// interface with bInterfaceClass 3, bInterfaceSubClass 1, bInterfaceProtocol 1 (HID boot
/// keyboard) and, inside that interface (before the next interface descriptor), its first
/// endpoint descriptor (type 5, bLength >= 7) whose address has bit 7 set (IN) and whose
/// bmAttributes & 3 == 3 (interrupt). Rules: header as in config_total_length; the walk
/// uses wTotalLength but never goes past b.len(); each descriptor is bLength (byte 0),
/// bDescriptorType (byte 1); bLength < 2 or running past the end stops the walk (return
/// NotFound if nothing found yet); interface descriptors (type 4) need bLength >= 9:
/// bInterfaceNumber @2, bAlternateSetting @3, bNumEndpoints @4, class @5, subclass @6,
/// protocol @7; endpoint descriptors (type 5) need bLength >= 7: address @2, attributes @3,
/// wMaxPacketSize u16 LE @4, bInterval @6. Shorter descriptors of those types are skipped.
/// wMaxPacketSize & 0x7ff must be >= 8 for the keyboard endpoint, else keep searching.
/// Other descriptor types (HID descriptor 0x21, etc.) are skipped.
pub fn find_boot_keyboard(b: &[u8]) -> Result<BootKeyboard, UsbError> {
    let total_len = config_total_length(b)? as usize;
    let limit = total_len.min(b.len());
    let mut offset = 0;
    let mut config_value = 0u8;
    let mut in_keyboard_interface = false;
    let mut kbd_interface_num = 0u8;
    let mut kbd_alt_setting = 0u8;

    while offset < limit {
        if offset + 2 > limit { return Err(UsbError::NotFound); }
        let b_length = b[offset] as usize;
        let b_type = b[offset + 1];
        if b_length < 2 { return Err(UsbError::NotFound); }
        if offset + b_length > limit { return Err(UsbError::NotFound); }

        match b_type {
            2 => {
                if b_length >= 9 {
                    config_value = b[offset + 5];
                }
            }
            4 => {
                in_keyboard_interface = false;
                if b_length >= 9 {
                    let class = b[offset + 5];
                    let subclass = b[offset + 6];
                    let protocol = b[offset + 7];
                    if class == 3 && subclass == 1 && protocol == 1 {
                        in_keyboard_interface = true;
                        kbd_interface_num = b[offset + 2];
                        kbd_alt_setting = b[offset + 3];
                    }
                }
            }
            5 => {
                if in_keyboard_interface && b_length >= 7 {
                    let ep_addr = b[offset + 2];
                    let bm_attr = b[offset + 3];
                    let w_max = u16::from_le_bytes([b[offset + 4], b[offset + 5]]);
                    let interval = b[offset + 6];
                    if (ep_addr & 0x80) != 0 && (bm_attr & 3) == 3 && (w_max & 0x7ff) >= 8 {
                        return Ok(BootKeyboard {
                            config_value,
                            interface: kbd_interface_num,
                            alt_setting: kbd_alt_setting,
                            ep_addr,
                            ep_max_packet: w_max & 0x7ff,
                            ep_interval: interval,
                        });
                    }
                }
            }
            _ => {}
        }
        offset += b_length;
    }
    Err(UsbError::NotFound)
}

/// HID boot keyboard state: turns 8-byte reports (byte 0 modifiers: bit0 LCtrl, bit1 LShift,
/// bit2 LAlt, bit4 RCtrl, bit5 RShift, bit6 RAlt; byte 1 reserved; bytes 2..8 up to six
/// pressed key usage codes, 0 = none, 1 = ErrorRollOver) into ASCII for keys that are newly
/// pressed compared with the previous report.
pub struct BootKbd { prev: [u8; 6], pub caps: bool }

impl BootKbd {
    pub const fn new() -> Self {
        BootKbd { prev: [0; 6], caps: false }
    }

    /// Returns the number of bytes written to `out` (at most out.len()).
    /// Reports shorter than 3 bytes are ignored (return 0, state unchanged).
    /// A report whose key bytes contain 1 (ErrorRollOver, phantom state) is ignored
    /// entirely (return 0, state unchanged). Only bytes 2..min(len,8) are keys.
    /// Caps Lock (usage 0x39) newly pressed toggles caps and produces nothing.
    pub fn report(&mut self, r: &[u8], out: &mut [u8]) -> usize {
        let Some(&modifiers) = r.first() else { return 0 };
        if r.len() < 3 {
            return 0;
        }
        let mut keys = [0u8; 6];
        for (k, &v) in keys.iter_mut().zip(r.iter().skip(2).take(6)) {
            *k = v;
        }
        if keys.contains(&1) {
            return 0; // ErrorRollOver: too many keys, the report is meaningless
        }
        let shift = modifiers & 0x22 != 0;
        let ctrl = modifiers & 0x11 != 0;
        let mut n = 0;
        // A key is new if it was not anywhere in the previous report
        // (keys may move between slots when others are released).
        for &k in keys.iter() {
            if k == 0 || self.prev.contains(&k) {
                continue;
            }
            if k == 0x39 {
                self.caps = !self.caps;
                continue;
            }
            if let (Some(c), Some(slot)) = (usage_to_ascii(k, shift, ctrl, self.caps), out.get_mut(n)) {
                *slot = c;
                n += 1;
            }
        }
        self.prev = keys;
        n
    }
}

/// ASCII for one HID usage code (US layout), or None:
///  0x04..=0x1d letters a..z (shift or caps -> uppercase; shift and caps together -> lowercase);
///  0x1e..=0x27 "1234567890", shifted "!@#$%^&*()";
///  0x28 Enter -> b'\r', 0x29 Escape -> 27, 0x2a Backspace -> 8, 0x2b Tab -> b'\t', 0x2c space,
///  0x2d '-' '_', 0x2e '=' '+', 0x2f '[' '{', 0x30 ']' '}', 0x31 '\\' '|', 0x33 ';' ':',
///  0x34 '\'' '"', 0x35 '`' '~', 0x36 ',' '<', 0x37 '.' '>', 0x38 '/' '?',
///  keypad 0x54 '/', 0x55 '*', 0x56 '-', 0x57 '+', 0x58 Enter b'\r', 0x59..=0x61 '1'..'9', 0x62 '0', 0x63 '.'.
///  With ctrl held, a letter gives its control code (letter & 0x1f), e.g. Ctrl-C = 3.
pub fn usage_to_ascii(usage: u8, shift: bool, ctrl: bool, caps: bool) -> Option<u8> {
    // Letters a-z (0x04 to 0x1d)
    if (0x04..=0x1d).contains(&usage) {
        let base = (usage - 0x04) + b'a';
        if ctrl {
            return Some(base & 0x1f);
        }
        let upper = shift ^ caps;
        if upper {
            return Some(base - 32); // To uppercase
        }
        return Some(base);
    }

    // Numbers 1-0 (0x1e to 0x27)
    if (0x1e..=0x27).contains(&usage) {
        let idx = usage - 0x1e;
        let nums = b"1234567890";
        let shifted = b"!@#$%^&*()";
        if shift {
            return Some(shifted[idx as usize]);
        }
        return Some(nums[idx as usize]);
    }

    // Special keys
    match usage {
        0x28 => Some(b'\r'), // Enter
        0x29 => Some(27),    // Escape
        0x2a => Some(8),     // Backspace
        0x2b => Some(b'\t'), // Tab
        0x2c => Some(b' '),  // Space
        0x2d => if shift { Some(b'_') } else { Some(b'-') },
        0x2e => if shift { Some(b'+') } else { Some(b'=') },
        0x2f => if shift { Some(b'{') } else { Some(b'[') },
        0x30 => if shift { Some(b'}') } else { Some(b']') },
        0x31 => if shift { Some(b'|') } else { Some(b'\\') },
        0x33 => if shift { Some(b':') } else { Some(b';') },
        0x34 => if shift { Some(b'"') } else { Some(b'\'') },
        0x35 => if shift { Some(b'~') } else { Some(b'`') },
        0x36 => if shift { Some(b'<') } else { Some(b',') },
        0x37 => if shift { Some(b'>') } else { Some(b'.') },
        0x38 => if shift { Some(b'?') } else { Some(b'/') },
        
        // Keypad
        0x54 => Some(b'/'),
        0x55 => Some(b'*'),
        0x56 => Some(b'-'),
        0x57 => Some(b'+'),
        0x58 => Some(b'\r'),
        0x59..=0x61 => Some(usage - 0x59 + b'1'), // 1-9
        0x62 => Some(b'0'),
        0x63 => Some(b'.'),
        
        _ => None,
    }
}

#[cfg(test)]
mod tests {
    extern crate std;
    use super::*;
    use std::vec;
    use std::vec::Vec;

    #[test]
    fn test_device_desc_qemu() {
        let b = [18,1,0x00,0x02,0,0,0,8,0x27,0x06,0x01,0x00,0,0,1,4,11,1];
        let desc = parse_device_desc(&b).unwrap();
        assert_eq!(desc.vendor, 0x0627);
        assert_eq!(desc.product, 1);
        assert_eq!(desc.max_packet0, 8);
        
        let mp = max_packet0_from_prefix(&b[..8]).unwrap();
        assert_eq!(mp, 8);
    }

    #[test]
    fn test_device_desc_errors() {
        assert_eq!(parse_device_desc(&[]), Err(UsbError::Short));
        let mut b17 = [0u8; 17];
        b17[0] = 17; b17[1] = 1;
        assert_eq!(parse_device_desc(&b17), Err(UsbError::BadLength));
        
        let mut b10 = [0u8; 18];
        b10[0] = 10; b10[1] = 1;
        assert_eq!(parse_device_desc(&b10), Err(UsbError::BadLength));
        
        let mut b_type = [0u8; 18];
        b_type[0] = 18; b_type[1] = 2;
        assert_eq!(parse_device_desc(&b_type), Err(UsbError::BadType));
        
        let mut b_mp = [0u8; 18];
        b_mp[0] = 18; b_mp[1] = 1; b_mp[7] = 7;
        assert_eq!(parse_device_desc(&b_mp), Err(UsbError::BadLength));
    }

    #[test]
    fn test_config_qemu() {
        // Config header
        let config = [9,2,34,0,1,1,0,0xa0,50];
        assert_eq!(config_total_length(&config), Ok(34));
        
        // Full config
        let mut b = Vec::new();
        b.extend_from_slice(&config);
        // Interface
        b.extend_from_slice(&[9,4,0,0,1,3,1,1,0]);
        // HID Descriptor
        b.extend_from_slice(&[9,0x21,0x11,0x01,0,1,0x22,0x3f,0]);
        // Endpoint
        b.extend_from_slice(&[7,5,0x81,3,8,0,7]);
        
        let kbd = find_boot_keyboard(&b).unwrap();
        assert_eq!(kbd.config_value, 1);
        assert_eq!(kbd.interface, 0);
        assert_eq!(kbd.alt_setting, 0);
        assert_eq!(kbd.ep_addr, 0x81);
        assert_eq!(kbd.ep_max_packet, 8);
        assert_eq!(kbd.ep_interval, 7);
    }

    #[test]
    fn test_config_errors() {
        // Zero length descriptor inside
        let mut b = Vec::new();
        b.extend_from_slice(&[9,2,20,0,1,1,0,0xa0,50]); // Total 20
        b.extend_from_slice(&[9,4,0,0,1,3,1,1,0]); // Interface
        b.extend_from_slice(&[0,0]); // Zero length
        assert_eq!(find_boot_keyboard(&b), Err(UsbError::NotFound));
        
        // Descriptor running past end
        let mut b2 = Vec::new();
        b2.extend_from_slice(&[9,2,100,0,1,1,0,0xa0,50]);
        b2.extend_from_slice(&[9,4,0,0,1,3,1,1,0]);
        assert_eq!(find_boot_keyboard(&b2), Err(UsbError::NotFound));
        
        // wTotalLength larger than buffer
        let mut b3 = Vec::new();
        b3.extend_from_slice(&[9,2,100,0,1,1,0,0xa0,50]);
        assert_eq!(find_boot_keyboard(&b3), Err(UsbError::NotFound));
        
        // wTotalLength 3 (too small for header)
        let b4 = [3,2,0,0,0,0,0,0,0];
        assert_eq!(config_total_length(&b4), Err(UsbError::BadLength));
        
        // OUT endpoint only
        let mut b5 = Vec::new();
        b5.extend_from_slice(&[9,2,25,0,1,1,0,0xa0,50]);
        b5.extend_from_slice(&[9,4,0,0,1,3,1,1,0]);
        b5.extend_from_slice(&[7,5,0x01,3,8,0,7]); // OUT
        assert_eq!(find_boot_keyboard(&b5), Err(UsbError::NotFound));
        
        // Bulk endpoint only
        let mut b6 = Vec::new();
        b6.extend_from_slice(&[9,2,25,0,1,1,0,0xa0,50]);
        b6.extend_from_slice(&[9,4,0,0,1,3,1,1,0]);
        b6.extend_from_slice(&[7,5,0x81,2,8,0,7]); // Bulk
        assert_eq!(find_boot_keyboard(&b6), Err(UsbError::NotFound));
        
        // Mouse interface
        let mut b7 = Vec::new();
        b7.extend_from_slice(&[9,2,25,0,1,1,0,0xa0,50]);
        b7.extend_from_slice(&[9,4,0,0,1,3,1,2,0]); // Protocol 2
        b7.extend_from_slice(&[7,5,0x81,3,8,0,7]);
        assert_eq!(find_boot_keyboard(&b7), Err(UsbError::NotFound));
        
        // Endpoint before keyboard interface
        let mut b8 = Vec::new();
        b8.extend_from_slice(&[9,2,35,0,1,1,0,0xa0,50]);
        b8.extend_from_slice(&[7,5,0x81,3,8,0,7]); // EP first
        b8.extend_from_slice(&[9,4,0,0,1,3,1,1,0]); // Interface
        assert_eq!(find_boot_keyboard(&b8), Err(UsbError::NotFound));
    }

    #[test]
    fn test_boot_kbd() {
        let mut kbd = BootKbd::new();
        let mut out = [0u8; 16];
        
        // 'a' pressed
        let r = [0,0,0x04,0,0,0,0,0];
        let n = kbd.report(&r, &mut out);
        assert_eq!(n, 1);
        assert_eq!(out[0], b'a');
        
        // Same report, no new keys
        let n = kbd.report(&r, &mut out);
        assert_eq!(n, 0);
        
        // Shift + 'b' (0x05) pressed, 'a' still held
        let r2 = [2,0,0x04,0x05,0,0,0,0];
        let n = kbd.report(&r2, &mut out);
        assert_eq!(n, 1);
        assert_eq!(out[0], b'B');
        
        // Enter
        let r3 = [0,0,0x28,0,0,0,0,0];
        let n = kbd.report(&r3, &mut out);
        assert_eq!(n, 1);
        assert_eq!(out[0], b'\r');
        
        // Ctrl-C (0x06 is 'c')
        let r4 = [1,0,0x06,0,0,0,0,0];
        let n = kbd.report(&r4, &mut out);
        assert_eq!(n, 1);
        assert_eq!(out[0], 3); // Ctrl-C
        
        // Caps Lock toggle
        let r5 = [0,0,0x39,0,0,0,0,0];
        let n = kbd.report(&r5, &mut out);
        assert_eq!(n, 0);
        assert!(kbd.caps);
        
        // 'a' with caps
        let r6 = [0,0,0x04,0,0,0,0,0];
        let n = kbd.report(&r6, &mut out);
        assert_eq!(n, 1);
        assert_eq!(out[0], b'A');
        
        // ErrorRollOver
        let r7 = [0,0,1,1,1,1,1,1];
        let n = kbd.report(&r7, &mut out);
        assert_eq!(n, 0);
        
        // Short report
        let r8 = [0,0];
        let n = kbd.report(&r8, &mut out);
        assert_eq!(n, 0);
        
        // Long report
        let r9 = [0u8; 64];
        let n = kbd.report(&r9, &mut out);
        assert_eq!(n, 0);
        
        // Keys moving between slots are not new: release 'a', 'b' moves to slot 0
        let mut k2 = BootKbd::new();
        assert_eq!(k2.report(&[0, 0, 0x04, 0x05, 0, 0, 0, 0], &mut out), 2);
        assert_eq!(k2.report(&[0, 0, 0x05, 0, 0, 0, 0, 0], &mut out), 0);
        // ErrorRollOver did not change state: the held key is still not new
        assert_eq!(k2.report(&[0, 0, 1, 1, 1, 1, 1, 1], &mut out), 0);
        assert_eq!(k2.report(&[0, 0, 0x05, 0, 0, 0, 0, 0], &mut out), 0);
        // Out buffer limit
        let mut out_small = [0u8; 1];
        let r10 = [0,0,0x04,0x05,0,0,0,0];
        let n = kbd.report(&r10, &mut out_small);
        assert_eq!(n, 1);
    }

    #[test]
    fn test_fuzz() {
        let mut state = 0x123456789ABCDEF0u64;
        for _ in 0..5000 {
            state ^= state << 13;
            state ^= state >> 7;
            state ^= state << 17;
            let len = (state % 300) as usize;
            let mut b = vec![0u8; len];
            for x in b.iter_mut() {
                state ^= state << 13;
                state ^= state >> 7;
                state ^= state << 17;
                *x = state as u8;
            }
            if len > 3 && state & 1 == 1 {
                b[1] = 2; // often a plausible config header, so the walk runs
                b[0] = 9;
            }
            
            let _ = parse_device_desc(&b);
            let _ = config_total_length(&b);
            let _ = find_boot_keyboard(&b);
            
            let mut kbd = BootKbd::new();
            let mut out = [0u8; 16];
            let _ = kbd.report(&b, &mut out);
        }
    }
}
