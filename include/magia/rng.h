/* SPDX-License-Identifier: BSD-2-Clause */
/* Copyright (c) 2026 zenmastercat and Magia contributors */
/* rng.h - the kernel random number generator (kernel/rng.c). */
#ifndef MAGIA_RNG_H
#define MAGIA_RNG_H

#include <magia/kernel.h>

/* n bytes of raw ChaCha20 keystream (D. J. Bernstein's original layout:
 * 64-bit block counter in state words 12-13, 64-bit nonce in words 14-15)
 * for a 256-bit key, starting at block `counter`. For known-answer tests. */
void chacha20_keystream(const uint8_t key[32], const uint8_t nonce[8], uint64_t counter, uint8_t *out,
			size_t n);

void arc4random_buf(void *buf, size_t n);
uint32_t arc4random(void);
uint64_t arc4random64(void);
uint32_t arc4random_uniform(uint32_t upper_bound); /* uniform in [0, upper_bound) */
void rng_reseed(void);              /* force a reseed from the entropy sources now */
void rng_early_init(void);          /* from _start: seed, then set the stack guard */

extern volatile uint64_t rng_reseeds; /* reseeds done since boot (>= 1 after boot) */
#define RNG_RESEED_BYTES 1600000      /* output bytes after which the generator reseeds */
extern uint64_t stack_canary;         /* random per boot: bottom-of-stack marker of every kernel thread */
extern uint64_t __stack_chk_guard;
extern unsigned rng_sources;          /* RNG_SRC_* bits seen at the last reseed */
#define RNG_SRC_RDSEED 1
#define RNG_SRC_RDRAND 2
#define RNG_SRC_TSC    4
#define RNG_SRC_LIMINE 8

#endif
