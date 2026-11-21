// SPDX-License-Identifier: BSD-2-Clause
// Copyright (c) 2026 zenmastercat and Magia contributors
//! USB hub data (USB 2.0 ch. 11, USB 3.2 ch. 10) and the HID boot mouse
//! (HID 1.11 appendix B.2), parsed as untrusted input: the hub interface of
//! a configuration, the USB 2 and SuperSpeed hub descriptors, GET_STATUS
//! port status, the boot mouse interface and its reports. No heap, no
//! unsafe, no panics. The C side is kernel/xhci.c (OpenBSD uhub.c order).
//!
//! First draft by the local model (qwen3.6:27b); reviewed and fixed: the
//! hub endpoint must belong to the hub interface itself (the draft took an
//! interrupt-IN endpoint of any later interface), and the first hub
//! interface wins.

use crate::usb::{config_total_length, UsbError};

/// Hub interface descriptor info.
#[derive(Debug, Clone, Copy, PartialEq, Eq, Default)]
pub struct HubIf {
    pub config_value: u8,
    pub interface: u8,
    pub ep_addr: u8,
    pub ep_max_packet: u16,
    pub ep_interval: u8,
    pub mtt_alt: u8,
}

/// Walk a configuration descriptor to find a USB Hub interface (Class 9).
/// Finds the first interface with Class 9, AltSetting 0, and an Interrupt IN endpoint.
/// Also scans for Multi-TT alternate settings (Protocol 2, AltSetting != 0) for the same interface number.
pub fn find_hub(b: &[u8]) -> Result<HubIf, UsbError> {
    let total_len = config_total_length(b)? as usize;
    let limit = total_len.min(b.len());
    let mut offset = 0;
    let mut config_value = 0u8;
    let mut hub_interface_num: Option<u8> = None;
    let mut in_hub = false; // the current interface is the hub interface, alt 0
    let mut found_ep = false;
    let mut result_ep_addr = 0u8;
    let mut result_ep_max = 0u16;
    let mut result_ep_int = 0u8;
    let mut mtt_alt = 0xffu8;

    while offset < limit {
        // Check bounds for header
        if offset + 2 > limit {
            break;
        }
        let b_length = b[offset] as usize;
        let b_type = b[offset + 1];

        // Validate descriptor length
        if b_length < 2 {
            break;
        }
        if offset + b_length > limit {
            break;
        }

        match b_type {
            2 => {
                // Configuration descriptor
                if b_length >= 9 {
                    config_value = b[offset + 5];
                }
            }
            4 => {
                // Interface descriptor
                in_hub = false;
                if b_length >= 9 {
                    let iface_num = b[offset + 2];
                    let alt_setting = b[offset + 3];
                    let class = b[offset + 5];
                    let protocol = b[offset + 7];

                    // Check for Multi-TT alternate setting for the hub interface
                    if let Some(hub_num) = hub_interface_num {
                        if class == 9 && iface_num == hub_num && protocol == 2 && alt_setting != 0 {
                            mtt_alt = alt_setting;
                        }
                    }

                    // The first hub interface (class 9, alt 0) wins
                    if class == 9 && alt_setting == 0 && hub_interface_num.is_none() {
                        hub_interface_num = Some(iface_num);
                        in_hub = true;
                    }
                }
            }
            5 => {
                // Endpoint descriptor
                if b_length >= 7 {
                    let ep_addr = b[offset + 2];
                    let bm_attr = b[offset + 3];
                    let w_max = u16::from_le_bytes([b[offset + 4], b[offset + 5]]);
                    let interval = b[offset + 6];

                    // Endpoints belong to the interface descriptor before them.
                    if !found_ep {
                        if in_hub
                            && (ep_addr & 0x80) != 0 // IN
                            && (bm_attr & 3) == 3    // Interrupt
                            && (w_max & 0x7ff) >= 1
                        {
                            found_ep = true;
                            result_ep_addr = ep_addr;
                            result_ep_max = w_max & 0x7ff;
                            result_ep_int = interval;
                        }
                    }
                }
            }
            _ => {}
        }
        offset += b_length;
    }

    if found_ep {
        Ok(HubIf {
            config_value,
            interface: hub_interface_num.unwrap_or(0),
            ep_addr: result_ep_addr,
            ep_max_packet: result_ep_max,
            ep_interval: result_ep_int,
            mtt_alt,
        })
    } else {
        Err(UsbError::NotFound)
    }
}

/// USB Hub Descriptor.
#[derive(Debug, Clone, Copy, PartialEq, Eq, Default)]
pub struct HubDesc {
    pub nports: u8,
    pub characteristics: u16,
    pub tt_think: u8,
    pub power_on_ms: u16,
    pub ss: bool,
}

/// Parse a Hub Descriptor (USB 2.0 or SuperSpeed).
pub fn parse_hub_desc(b: &[u8], ss: bool) -> Result<HubDesc, UsbError> {
    if b.len() < 2 {
        return Err(UsbError::Short);
    }

    let b_length = b[0] as usize;
    let b_type = b[1];

    if ss {
        // SuperSpeed Hub Descriptor
        if b_type != 0x2a {
            return Err(UsbError::BadType);
        }
        if b_length < 12 || b_length > b.len() {
            return Err(UsbError::BadLength);
        }
        let nports = b[2];
        if nports < 1 || nports > 15 {
            return Err(UsbError::BadLength);
        }
        let characteristics = u16::from_le_bytes([b[3], b[4]]);
        let pwr_on = b[5] as u16;
        Ok(HubDesc {
            nports,
            characteristics,
            tt_think: 0,
            power_on_ms: pwr_on * 2,
            ss: true,
        })
    } else {
        // USB 2.0 Hub Descriptor
        if b_type != 0x29 {
            return Err(UsbError::BadType);
        }
        if b_length < 7 || b_length > b.len() {
            return Err(UsbError::BadLength);
        }
        let nports = b[2];
        if nports < 1 {
            return Err(UsbError::BadLength);
        }
        let characteristics = u16::from_le_bytes([b[3], b[4]]);
        let pwr_on = b[5] as u16;
        let tt_think = ((characteristics >> 5) & 3) as u8;
        Ok(HubDesc {
            nports,
            characteristics,
            tt_think,
            power_on_ms: pwr_on * 2,
            ss: false,
        })
    }
}

/// Port Status response.
#[derive(Debug, Clone, Copy, PartialEq, Eq, Default)]
pub struct PortStatus {
    pub status: u16,
    pub change: u16,
}

/// Parse GET_STATUS(port) response.
pub fn parse_port_status(b: &[u8]) -> Result<PortStatus, UsbError> {
    if b.len() < 4 {
        return Err(UsbError::Short);
    }
    let status = u16::from_le_bytes([b[0], b[1]]);
    let change = u16::from_le_bytes([b[2], b[3]]);
    Ok(PortStatus { status, change })
}

/// HID Boot Mouse interface info.
#[derive(Debug, Clone, Copy, PartialEq, Eq, Default)]
pub struct BootMouse {
    pub config_value: u8,
    pub interface: u8,
    pub ep_addr: u8,
    pub ep_max_packet: u16,
    pub ep_interval: u8,
}

/// Find HID Boot Mouse interface (Class 3, Subclass 1, Protocol 2).
pub fn find_boot_mouse(b: &[u8]) -> Result<BootMouse, UsbError> {
    let total_len = config_total_length(b)? as usize;
    let limit = total_len.min(b.len());
    let mut offset = 0;
    let mut config_value = 0u8;
    let mut in_mouse_interface = false;
    let mut mouse_iface_num = 0u8;

    while offset < limit {
        if offset + 2 > limit {
            return Err(UsbError::NotFound);
        }
        let b_length = b[offset] as usize;
        let b_type = b[offset + 1];

        if b_length < 2 {
            return Err(UsbError::NotFound);
        }
        if offset + b_length > limit {
            return Err(UsbError::NotFound);
        }

        match b_type {
            2 => {
                if b_length >= 9 {
                    config_value = b[offset + 5];
                }
            }
            4 => {
                in_mouse_interface = false;
                if b_length >= 9 {
                    let class = b[offset + 5];
                    let subclass = b[offset + 6];
                    let protocol = b[offset + 7];
                    let alt = b[offset + 3];
                    // Boot Mouse: Class 3, Subclass 1, Protocol 2, Alt 0
                    if class == 3 && subclass == 1 && protocol == 2 && alt == 0 {
                        in_mouse_interface = true;
                        mouse_iface_num = b[offset + 2];
                    }
                }
            }
            5 => {
                if in_mouse_interface && b_length >= 7 {
                    let ep_addr = b[offset + 2];
                    let bm_attr = b[offset + 3];
                    let w_max = u16::from_le_bytes([b[offset + 4], b[offset + 5]]);
                    let interval = b[offset + 6];
                    // Interrupt IN, wMaxPacketSize >= 3
                    if (ep_addr & 0x80) != 0 && (bm_attr & 3) == 3 && (w_max & 0x7ff) >= 3 {
                        return Ok(BootMouse {
                            config_value,
                            interface: mouse_iface_num,
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

/// Parsed Mouse Report.
#[derive(Debug, Clone, Copy, PartialEq, Eq, Default)]
pub struct MouseReport {
    pub buttons: u8,
    pub dx: i8,
    pub dy: i8,
    pub wheel: i8,
}

/// Parse a HID Boot Mouse report.
pub fn parse_mouse_report(r: &[u8]) -> Option<MouseReport> {
    if r.len() < 3 {
        return None;
    }
    let buttons = r[0];
    let dx = r[1] as i8;
    let dy = r[2] as i8;
    let wheel = if r.len() >= 4 { r[3] as i8 } else { 0 };
    Some(MouseReport { buttons, dx, dy, wheel })
}

#[cfg(test)]
mod tests {
    extern crate std;
    use super::*;
    use std::vec;
    use std::vec::Vec;

    #[test]
    fn test_hub_qemu() {
        // Config header: Total 25
        let config = [9, 2, 25, 0, 1, 1, 0, 0xa0, 50];
        // Interface: Class 9 (Hub), Subclass 0, Protocol 0
        let iface = [9, 4, 0, 0, 1, 9, 0, 0, 0];
        // Endpoint: IN, Interrupt, MaxPacket 1, Interval 0xff
        let ep = [7, 5, 0x81, 3, 1, 0, 0xff];

        let mut b = Vec::new();
        b.extend_from_slice(&config);
        b.extend_from_slice(&iface);
        b.extend_from_slice(&ep);

        let hub = find_hub(&b).unwrap();
        assert_eq!(hub.config_value, 1);
        assert_eq!(hub.interface, 0);
        assert_eq!(hub.ep_addr, 0x81);
        assert_eq!(hub.ep_max_packet, 1);
        assert_eq!(hub.ep_interval, 0xff);
        assert_eq!(hub.mtt_alt, 0xff); // No multi-TT alt setting
    }

    #[test]
    fn test_hub_multi_tt() {
        // Config header: Total 50
        let config = [9, 2, 50, 0, 1, 1, 0, 0xa0, 50];
        
        // Interface 0, Alt 0, Protocol 1 (TT Think Time relevant)
        let iface0 = [9, 4, 0, 0, 1, 9, 0, 1, 0];
        let ep0 = [7, 5, 0x81, 3, 1, 0, 0xff];
        
        // Interface 0, Alt 1, Protocol 2 (Multi-TT)
        let iface1 = [9, 4, 0, 1, 1, 9, 0, 2, 0];
        let ep1 = [7, 5, 0x81, 3, 1, 0, 0xff];

        let mut b = Vec::new();
        b.extend_from_slice(&config);
        b.extend_from_slice(&iface0);
        b.extend_from_slice(&ep0);
        b.extend_from_slice(&iface1);
        b.extend_from_slice(&ep1);

        let hub = find_hub(&b).unwrap();
        assert_eq!(hub.mtt_alt, 1);
    }

    #[test]
    fn test_hub_errors() {
        // Hub without endpoint
        let mut b1 = Vec::new();
        b1.extend_from_slice(&[9, 2, 18, 0, 1, 1, 0, 0xa0, 50]);
        b1.extend_from_slice(&[9, 4, 0, 0, 1, 9, 0, 0, 0]);
        assert_eq!(find_hub(&b1), Err(UsbError::NotFound));

        // Endpoint OUT only
        let mut b2 = Vec::new();
        b2.extend_from_slice(&[9, 2, 25, 0, 1, 1, 0, 0xa0, 50]);
        b2.extend_from_slice(&[9, 4, 0, 0, 1, 9, 0, 0, 0]);
        b2.extend_from_slice(&[7, 5, 0x01, 3, 1, 0, 0xff]); // OUT
        assert_eq!(find_hub(&b2), Err(UsbError::NotFound));

        // Zero-length descriptor
        let mut b3 = Vec::new();
        b3.extend_from_slice(&[9, 2, 20, 0, 1, 1, 0, 0xa0, 50]);
        b3.extend_from_slice(&[9, 4, 0, 0, 1, 9, 0, 0, 0]);
        b3.extend_from_slice(&[0, 0]);
        assert_eq!(find_hub(&b3), Err(UsbError::NotFound));

        // wTotalLength past buffer
        let mut b4 = Vec::new();
        b4.extend_from_slice(&[9, 2, 100, 0, 1, 1, 0, 0xa0, 50]);
        b4.extend_from_slice(&[9, 4, 0, 0, 1, 9, 0, 0, 0]);
        assert_eq!(find_hub(&b4), Err(UsbError::NotFound));

        // A hub interface without an endpoint, then a HID interface with
        // one: that endpoint is not the hub's (review fix)
        let mut b5 = Vec::new();
        b5.extend_from_slice(&[9, 2, 34, 0, 2, 1, 0, 0xa0, 50]);
        b5.extend_from_slice(&[9, 4, 0, 0, 0, 9, 0, 0, 0]);
        b5.extend_from_slice(&[9, 4, 1, 0, 1, 3, 1, 1, 0]);
        b5.extend_from_slice(&[7, 5, 0x81, 3, 8, 0, 10]);
        assert_eq!(find_hub(&b5), Err(UsbError::NotFound));

        // Two hub interfaces: the first one and its endpoint win
        let mut b6 = Vec::new();
        b6.extend_from_slice(&[9, 2, 41, 0, 2, 1, 0, 0xa0, 50]);
        b6.extend_from_slice(&[9, 4, 3, 0, 1, 9, 0, 0, 0]);
        b6.extend_from_slice(&[7, 5, 0x82, 3, 2, 0, 12]);
        b6.extend_from_slice(&[9, 4, 5, 0, 1, 9, 0, 0, 0]);
        b6.extend_from_slice(&[7, 5, 0x83, 3, 2, 0, 12]);
        let h = find_hub(&b6).unwrap();
        assert_eq!((h.interface, h.ep_addr, h.mtt_alt), (3, 0x82, 0xff));

        // A bad descriptor after the endpoint still returns the hub
        let mut b7 = Vec::new();
        b7.extend_from_slice(&[9, 2, 27, 0, 1, 1, 0, 0xa0, 50]);
        b7.extend_from_slice(&[9, 4, 0, 0, 1, 9, 0, 0, 0]);
        b7.extend_from_slice(&[7, 5, 0x81, 3, 1, 0, 0xff]);
        b7.extend_from_slice(&[1, 4]);
        assert!(find_hub(&b7).is_ok());
    }

    #[test]
    fn test_hub_desc() {
        // QEMU 8-port hub
        let b = [9, 0x29, 8, 0x0a, 0x00, 0x01, 0x00, 0x00, 0xff];
        let desc = parse_hub_desc(&b, false).unwrap();
        assert_eq!(desc.nports, 8);
        assert_eq!(desc.characteristics, 0x000a);
        assert_eq!(desc.tt_think, 0); // (0x000a >> 5) & 3 = 0
        assert_eq!(desc.power_on_ms, 2); // 1 * 2
        assert!(!desc.ss);

        // Genesys-like with TT think bits
        // Characteristics 0x00e9 -> binary ...11101001. Bits 5-6 are 11 (3).
        let b2 = [9, 0x29, 4, 0xe9, 0x00, 0x02, 0x00, 0x00, 0xff];
        let desc2 = parse_hub_desc(&b2, false).unwrap();
        assert_eq!(desc2.tt_think, 3);

        // SS Descriptor
        let b3 = [12, 0x2a, 4, 0x09, 0, 50, 0, 0, 0, 0, 0, 0];
        let desc3 = parse_hub_desc(&b3, true).unwrap();
        assert!(desc3.ss);
        assert_eq!(desc3.nports, 4);
        assert_eq!(desc3.power_on_ms, 100); // 50 * 2

        // Errors
        assert_eq!(parse_hub_desc(&[9, 0x29, 0, 0, 0, 0, 0, 0, 0], false), Err(UsbError::BadLength)); // nports 0
        assert_eq!(parse_hub_desc(&[10, 0x29, 1, 0, 0, 0, 0, 0, 0], false), Err(UsbError::BadLength)); // bDescLength > len
        assert_eq!(parse_hub_desc(&[9], false), Err(UsbError::Short));
        assert_eq!(parse_hub_desc(&[12, 0x2a, 16, 0, 0, 0, 0, 0, 0, 0, 0, 0], true), Err(UsbError::BadLength)); // nports 16
        assert_eq!(parse_hub_desc(&[6, 0x29, 1, 0, 0, 0, 0, 0, 0], false), Err(UsbError::BadLength)); // len < 7
        assert_eq!(parse_hub_desc(&[9, 0x20, 1, 0, 0, 0, 0, 0, 0], false), Err(UsbError::BadType)); // wrong type
    }

    #[test]
    fn test_port_status() {
        let b = [0x03, 0x01, 0x01, 0x00];
        let ps = parse_port_status(&b).unwrap();
        assert_eq!(ps.status, 0x0103);
        assert_eq!(ps.change, 1);

        assert_eq!(parse_port_status(&[0, 0, 0]), Err(UsbError::Short));
    }

    #[test]
    fn test_mouse() {
        // Config
        let config = [9, 2, 25, 0, 1, 1, 0, 0xa0, 50];
        let iface = [9, 4, 0, 0, 1, 3, 1, 2, 0]; // Class 3, Sub 1, Proto 2
        let ep = [7, 5, 0x81, 3, 4, 0, 0x0a];

        let mut b = Vec::new();
        b.extend_from_slice(&config);
        b.extend_from_slice(&iface);
        b.extend_from_slice(&ep);

        let mouse = find_boot_mouse(&b).unwrap();
        assert_eq!(mouse.interface, 0);
        assert_eq!(mouse.ep_addr, 0x81);
        assert_eq!(mouse.ep_max_packet, 4);

        // Report
        let r = [1, 0xfe, 5, 0xff];
        let mr = parse_mouse_report(&r).unwrap();
        assert_eq!(mr.buttons, 1);
        assert_eq!(mr.dx, -2);
        assert_eq!(mr.dy, 5);
        assert_eq!(mr.wheel, -1);

        // 3-byte report
        let r2 = [1, 0, 0];
        let mr2 = parse_mouse_report(&r2).unwrap();
        assert_eq!(mr2.wheel, 0);

        // 2-byte report
        assert!(parse_mouse_report(&[1, 0]).is_none());
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
                b[1] = 2;
                b[0] = 9;
            }

            let _ = find_hub(&b);
            let _ = find_boot_mouse(&b);
            let _ = parse_hub_desc(&b, false);
            let _ = parse_hub_desc(&b, true);
            let _ = parse_port_status(&b);
            let _ = parse_mouse_report(&b);
        }
    }
}
