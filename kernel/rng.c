/* SPDX-License-Identifier: BSD-2-Clause */
/* Copyright (c) 2026 zenmastercat and Magia contributors */
/*
 * rng.c - kernel random numbers: a ChaCha20 keystream generator in the
 * style of OpenBSD's arc4random(3), with fast key erasure.
 *
 * The ChaCha20 core is D. J. Bernstein's public-domain chacha-merged.c as
 * carried by OpenBSD (kernel/crypto/chacha_private.h). The generator is
 * written for Magia after the arc4random design:
 *   - output comes from a 1 KiB buffer of keystream; every refill first
 *     re-keys the cipher from the first 40 bytes of fresh keystream and
 *     wipes them ("fast key erasure"), so a later state compromise does
 *     not reveal earlier output, and bytes handed out are wiped from the
 *     buffer;
 *   - reseeding mixes each entropy source in through a separate re-key
 *     (new key = keystream XOR source), so no source can cancel another
 *     without breaking ChaCha20: Limine's boot entropy (once), RDSEED,
 *     RDRAND, and TSC jitter; it happens at boot, after RNG_RESEED_BYTES
 *     of output, and on rng_reseed().
 * The stack-protector guard and the per-boot thread-stack canary are drawn
 * from it before kmain() runs (rng_early_init, called from _start).
 */
#include <magia/boot.h>
#include <magia/linker_set.h>
#include <magia/rng.h>
#include <magia/spinlock.h>
#include <magia/sysctl.h>
#include <magia/x86.h>

typedef unsigned int u_int;
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wunused-function"
#pragma GCC diagnostic ignored "-Wunused-variable"
#pragma GCC diagnostic ignored "-Wunused-const-variable"
#pragma GCC diagnostic ignored "-Wunterminated-string-initialization"
#include "crypto/chacha_private.h"
#pragma GCC diagnostic pop

#define KEYSZ 32
#define IVSZ 8
#define RSBUFSZ (16 * 64)

static chacha_ctx rs;
static uint8_t rs_buf[RSBUFSZ];
static size_t rs_have;           /* unused bytes at the end of rs_buf */
static size_t rs_count;          /* bytes left until the next reseed */
static spinlock_t rng_lock = SPINLOCK_INIT("rng");
volatile uint64_t rng_reseeds;
unsigned rng_sources;
uint64_t stack_canary = 0x4d61676961537421ULL; /* replaced at boot */
SYSCTL_U64_RO(kern_rng_reseeds, "kern.random.reseeds", (const uint64_t *)&rng_reseeds,
	      "kernel RNG reseeds since boot");

/* memset that the compiler may not drop */
static void wipe(void *p, size_t n)
{
	memset(p, 0, n);
	__asm__ volatile("" ::"r"(p) : "memory");
}

static void rs_init(const uint8_t *buf)
{
	chacha_keysetup(&rs, buf, KEYSZ * 8);
	chacha_ivsetup(&rs, buf + KEYSZ, NULL);
}

/* Refill rs_buf; XOR in up to 40 bytes of `dat`; re-key from the first
 * 40 bytes and wipe them. */
static void rs_rekey(const uint8_t *dat, size_t datlen)
{
	memset(rs_buf, 0, RSBUFSZ);
	chacha_encrypt_bytes(&rs, rs_buf, rs_buf, RSBUFSZ);
	for (size_t i = 0; dat && i < MIN(datlen, (size_t)(KEYSZ + IVSZ)); i++)
		rs_buf[i] ^= dat[i];
	rs_init(rs_buf);
	wipe(rs_buf, KEYSZ + IVSZ);
	rs_have = RSBUFSZ - KEYSZ - IVSZ;
}

static bool rdseed64(uint64_t *v)
{
	for (int i = 0; i < 32; i++) {
		uint8_t ok;
		__asm__ volatile("rdseed %0; setc %1" : "=r"(*v), "=qm"(ok));
		if (ok)
			return true;
		cpu_relax();
	}
	return false;
}

static bool rdrand64(uint64_t *v)
{
	for (int i = 0; i < 16; i++) {
		uint8_t ok;
		__asm__ volatile("rdrand %0; setc %1" : "=r"(*v), "=qm"(ok));
		if (ok)
			return true;
	}
	return false;
}

/* TSC jitter: timing of a data-dependent loop, a few bits per sample at
 * best; 40 bytes of samples per reseed. shortcut: no entropy estimate,
 * it is one source among four; add a health test (SP 800-90B) for stage 4. */
static void tsc_jitter(uint8_t *out, size_t n)
{
	uint64_t prev = rdtsc();
	volatile uint64_t sink = prev;
	for (size_t i = 0; i < n; i++) {
		for (int k = 0; k < 64 + (int)(prev & 63); k++)
			sink = sink * 6364136223846793005ULL + k;
		uint64_t t = rdtsc();
		out[i] = (uint8_t)((t - prev) ^ (t >> 8));
		prev = t;
	}
}

static void rs_stir(void)
{
	uint8_t seed[KEYSZ + IVSZ];
	uint32_t a, b, c, d;
	cpuid(1, 0, &a, &b, &c, &d);
	bool has_rdrand = (c >> 30) & 1;
	cpuid(0, 0, &a, &b, &c, &d);
	bool has_rdseed = false;
	if (a >= 7) {
		cpuid(7, 0, &a, &b, &c, &d);
		has_rdseed = (b >> 18) & 1;
	}
	unsigned src = 0;

	if (bootinfo.nentropy) { /* Limine's values: used once, then wiped */
		memset(seed, 0, sizeof(seed));
		memcpy(seed, bootinfo.entropy, MIN(sizeof(seed), bootinfo.nentropy * 8));
		rs_rekey(seed, sizeof(seed));
		wipe(bootinfo.entropy, sizeof(bootinfo.entropy));
		bootinfo.nentropy = 0;
		src |= RNG_SRC_LIMINE;
	}
	if (has_rdseed) {
		uint64_t v[5];
		bool ok = true;
		for (int i = 0; i < 5; i++)
			ok &= rdseed64(&v[i]);
		if (ok) {
			rs_rekey((const uint8_t *)v, sizeof(v));
			src |= RNG_SRC_RDSEED;
		}
		wipe(v, sizeof(v));
	}
	if (has_rdrand) {
		uint64_t v[5];
		bool ok = true;
		for (int i = 0; i < 5; i++)
			ok &= rdrand64(&v[i]);
		if (ok) {
			rs_rekey((const uint8_t *)v, sizeof(v));
			src |= RNG_SRC_RDRAND;
		}
		wipe(v, sizeof(v));
	}
	tsc_jitter(seed, sizeof(seed));
	rs_rekey(seed, sizeof(seed));
	src |= RNG_SRC_TSC;
	wipe(seed, sizeof(seed));

	/* discard the buffer filled under the previous key */
	rs_have = 0;
	memset(rs_buf, 0, RSBUFSZ);
	rs_count = RNG_RESEED_BYTES;
	rng_sources = src;
	rng_reseeds++;
}

static void random_buf_locked(uint8_t *out, size_t n)
{
	if (rs_count <= n)
		rs_stir();
	rs_count = rs_count > n ? rs_count - n : 0;
	while (n > 0) {
		if (rs_have == 0)
			rs_rekey(NULL, 0);
		size_t m = MIN(n, rs_have);
		uint8_t *keystream = rs_buf + RSBUFSZ - rs_have;
		memcpy(out, keystream, m);
		wipe(keystream, m);
		out += m;
		n -= m;
		rs_have -= m;
	}
}

void arc4random_buf(void *buf, size_t n)
{
	spin_lock(&rng_lock);
	random_buf_locked(buf, n);
	spin_unlock(&rng_lock);
}

uint32_t arc4random(void)
{
	uint32_t v;
	arc4random_buf(&v, sizeof(v));
	return v;
}

uint64_t arc4random64(void)
{
	uint64_t v;
	arc4random_buf(&v, sizeof(v));
	return v;
}

/* Uniform in [0, upper_bound): reject the low values that would bias the
 * modulo (the arc4random_uniform(3) method). */
uint32_t arc4random_uniform(uint32_t upper_bound)
{
	if (upper_bound < 2)
		return 0;
	uint32_t min = -upper_bound % upper_bound, r;
	do
		r = arc4random();
	while (r < min);
	return r % upper_bound;
}

void rng_reseed(void)
{
	spin_lock(&rng_lock);
	rs_stir();
	spin_unlock(&rng_lock);
}

void chacha20_keystream(const uint8_t key[32], const uint8_t nonce[8], uint64_t counter, uint8_t *out, size_t n)
{
	chacha_ctx x;
	uint8_t ctr[8];
	for (int i = 0; i < 8; i++)
		ctr[i] = (uint8_t)(counter >> (8 * i));
	chacha_keysetup(&x, key, 256);
	chacha_ivsetup(&x, nonce, ctr);
	memset(out, 0, n);
	while (n > 0) { /* chacha_encrypt_bytes takes a u32 length */
		uint32_t m = (uint32_t)MIN(n, (size_t)1 << 30);
		chacha_encrypt_bytes(&x, out, out, m);
		out += m;
		n -= m;
	}
	wipe(&x, sizeof(x));
}

/*
 * Called from _start after boot_early() (Limine's entropy is in bootinfo),
 * before kmain(): no stack-protected frame is live yet, so replacing the
 * guard here cannot trip a canary check. This function itself must not be
 * instrumented, since it returns after the guard changed.
 */
__attribute__((no_stack_protector)) void rng_early_init(void)
{
	uint8_t zero[KEYSZ + IVSZ] = { 0 };
	rs_init(zero);
	rng_reseed();
	uint64_t g;
	do
		g = arc4random64() & ~0xffULL; /* low byte 0: string overreads stop at the canary */
	while (g == 0);
	__stack_chk_guard = g;
	do
		stack_canary = arc4random64();
	while (stack_canary == 0);
}

static void rng_report(void)
{
	kprintf("rng: ChaCha20 (fast key erasure), %lu reseed(s), sources:%s%s%s%s\n", (uint64_t)rng_reseeds,
		rng_sources & RNG_SRC_LIMINE ? " Limine" : "", rng_sources & RNG_SRC_RDSEED ? " RDSEED" : "",
		rng_sources & RNG_SRC_RDRAND ? " RDRAND" : "", rng_sources & RNG_SRC_TSC ? " TSC-jitter" : "");
}
SYSINIT(rng_report, 10001, rng_report);
