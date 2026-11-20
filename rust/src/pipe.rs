// SPDX-License-Identifier: BSD-2-Clause
// Copyright (c) 2026 zenmastercat and Magia contributors
//! Pipes: a bounded byte queue with reader/writer counts (POSIX semantics).
//! Blocking and wakeups are done by the callers in fs.rs.

use alloc::collections::VecDeque;

pub const PIPE_BUF_SIZE: usize = 4096;

pub struct Pipe {
    pub buf: VecDeque<u8>,
    pub readers: u32,
    pub writers: u32,
}

impl Default for Pipe {
    fn default() -> Self {
        Self::new()
    }
}

impl Pipe {
    pub fn new() -> Self {
        Pipe { buf: VecDeque::with_capacity(PIPE_BUF_SIZE), readers: 1, writers: 1 }
    }
    pub fn avail(&self) -> usize {
        self.buf.len()
    }
    pub fn space(&self) -> usize {
        PIPE_BUF_SIZE - self.buf.len()
    }
}
