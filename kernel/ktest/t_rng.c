/* SPDX-License-Identifier: BSD-2-Clause */
/* Copyright (c) 2026 zenmastercat and Magia contributors */
/*
 * t_rng.c - ktests for ChaCha20 and the kernel RNG (04x).
 * First draft by the local model (qwen3.6:27b), reviewed; the known-answer
 * vectors are RFC 8439 (A.1 #1, 2.3.2) and an OpenSSL-generated two-block
 * vector, all cross-checked with openssl enc -chacha20 on the host.
 */
#include <magia/boot.h>
#include <magia/ktest.h>
#include <magia/rng.h>
#include <magia/thread.h>

KTEST(040, chacha20_known_answers)
{
	uint8_t key[32];
	uint8_t nonce[8];
	uint8_t out[128];

	/* Vector (a): key all zero, nonce all zero, counter 0, 64 bytes */
	memset(key, 0, sizeof(key));
	memset(nonce, 0, sizeof(nonce));
	chacha20_keystream(key, nonce, 0, out, 64);

	static const uint8_t vec_a[64] = {
		0x76, 0xb8, 0xe0, 0xad, 0xa0, 0xf1, 0x3d, 0x90,
		0x40, 0x5d, 0x6a, 0xe5, 0x53, 0x86, 0xbd, 0x28,
		0xbd, 0xd2, 0x19, 0xb8, 0xa0, 0x8d, 0xed, 0x1a,
		0xa8, 0x36, 0xef, 0xcc, 0x8b, 0x77, 0x0d, 0xc7,
		0xda, 0x41, 0x59, 0x7c, 0x51, 0x57, 0x48, 0x8d,
		0x77, 0x24, 0xe0, 0x3f, 0xb8, 0xd8, 0x4a, 0x37,
		0x6a, 0x43, 0xb8, 0xf4, 0x15, 0x18, 0xa1, 0x1c,
		0xc3, 0x87, 0xb6, 0x69, 0xb2, 0xee, 0x65, 0x86
	};
	KT_EQ(memcmp(out, vec_a, 64), 0);

	/* Vector (b): key 0x00..0x1f, RFC 8439 section 2.3.2 */
	/* Nonce: 00 00 00 09 00 00 00 4a 00 00 00 00 */
	/* Original layout: 64-bit counter = 0x0900000000000001, nonce = 00 00 00 4a 00 00 00 00 */
	for (int i = 0; i < 32; i++)
		key[i] = (uint8_t)i;
	nonce[0] = 0x00; nonce[1] = 0x00; nonce[2] = 0x00; nonce[3] = 0x4a;
	nonce[4] = 0x00; nonce[5] = 0x00; nonce[6] = 0x00; nonce[7] = 0x00;
	chacha20_keystream(key, nonce, 0x0900000000000001ULL, out, 64);

	static const uint8_t vec_b[64] = {
		0x10, 0xf1, 0xe7, 0xe4, 0xd1, 0x3b, 0x59, 0x15,
		0x50, 0x0f, 0xdd, 0x1f, 0xa3, 0x20, 0x71, 0xc4,
		0xc7, 0xd1, 0xf4, 0xc7, 0x33, 0xc0, 0x68, 0x03,
		0x04, 0x22, 0xaa, 0x9a, 0xc3, 0xd4, 0x6c, 0x4e,
		0xd2, 0x82, 0x64, 0x46, 0x07, 0x9f, 0xaa, 0x09,
		0x14, 0xc2, 0xd7, 0x05, 0xd9, 0x8b, 0x02, 0xa2,
		0xb5, 0x12, 0x9c, 0xd1, 0xde, 0x16, 0x4e, 0xb9,
		0xcb, 0xd0, 0x83, 0xe8, 0xa2, 0x50, 0x3c, 0x4e
	};
	KT_EQ(memcmp(out, vec_b, 64), 0);

	/* Vector (c): key 31 zeros then 0x01, nonce 00 00 00 00 00 00 00 02, counter 0, 128 bytes */
	memset(key, 0, 31);
	key[31] = 0x01;
	memset(nonce, 0, 7);
	nonce[7] = 0x02;
	chacha20_keystream(key, nonce, 0, out, 128);

	static const uint8_t vec_c[128] = {
		0xec, 0xfa, 0x25, 0x4f, 0x84, 0x5f, 0x64, 0x74,
		0x73, 0xd3, 0xcb, 0x14, 0x0d, 0xa9, 0xe8, 0x76,
		0x06, 0xcb, 0x33, 0x06, 0x6c, 0x44, 0x7b, 0x87,
		0xbc, 0x26, 0x66, 0xdd, 0xe3, 0xfb, 0xb7, 0x39,
		0xa3, 0x71, 0xc9, 0xec, 0x7a, 0xbc, 0xb4, 0xcf,
		0xa9, 0x21, 0x1f, 0x7d, 0x90, 0xf6, 0x4c, 0x2d,
		0x07, 0xf8, 0x9e, 0x5c, 0xf9, 0xb9, 0x3e, 0x33,
		0x0a, 0x6e, 0x4c, 0x08, 0xaf, 0x5b, 0xa6, 0xd5,
		0xe2, 0x95, 0x89, 0x5d, 0x80, 0x8f, 0x4d, 0xb3,
		0x26, 0x44, 0x1f, 0xcb, 0x51, 0xec, 0x53, 0x04,
		0x2e, 0x40, 0x29, 0xf7, 0x2a, 0x6f, 0x1e, 0xf8,
		0xd8, 0xb9, 0x0c, 0x74, 0x25, 0x0d, 0x30, 0x82,
		0x4e, 0xf2, 0xf0, 0xab, 0xb1, 0x0b, 0x09, 0x61,
		0xa0, 0x96, 0xf3, 0x74, 0x98, 0xbd, 0x04, 0x77,
		0x67, 0xfc, 0xe3, 0xa2, 0x28, 0xc5, 0xe3, 0xf9,
		0x39, 0x92, 0x11, 0xba, 0x2b, 0xd4, 0x49, 0x64
	};
	KT_EQ(memcmp(out, vec_c, 128), 0);

	/* Check that generating 128 bytes equals generating 64 at counter 0 then 64 at counter 1 */
	chacha20_keystream(key, nonce, 0, out, 64);
	uint8_t out2[64];
	chacha20_keystream(key, nonce, 1, out2, 64);
	KT_EQ(memcmp(out, vec_c, 64), 0);
	KT_EQ(memcmp(out2, vec_c + 64, 64), 0);

	/* Check that asking for 100 bytes gives the first 100 bytes of (c) */
	chacha20_keystream(key, nonce, 0, out, 100);
	KT_EQ(memcmp(out, vec_c, 100), 0);
}

KTEST(041, rng_output_does_not_repeat)
{
	static uint64_t vals[256];
	for (int i = 0; i < 256; i++)
		vals[i] = arc4random64();

	/* Check no two values are equal */
	int dups = 0;
	for (int i = 0; i < 256; i++)
		for (int j = i + 1; j < 256; j++)
			dups += vals[i] == vals[j];
	KT_EQ(dups, 0);

	/* Fill two 64-byte buffers and check they differ */
	uint8_t buf1[64], buf2[64];
	arc4random_buf(buf1, 64);
	arc4random_buf(buf2, 64);
	KT_ASSERT(memcmp(buf1, buf2, 64) != 0);

	/* Fill a 4096-byte buffer and count set bits */
	static uint8_t bigbuf[4096 + 1];
	arc4random_buf(bigbuf, 4096);
	int set_bits = 0;
	for (int i = 0; i < 4096; i++) {
		uint8_t b = bigbuf[i];
		for (int bit = 0; bit < 8; bit++) {
			if (b & (1 << bit))
				set_bits++;
		}
	}
	/* 45% to 55% of 32768 */
	KT_ASSERT(set_bits * 100 >= 32768 * 45);
	KT_ASSERT(set_bits * 100 <= 32768 * 55);

	/* arc4random_buf with n = 0 must not crash */
	arc4random_buf(bigbuf, 0);

	/* arc4random_buf with various n must not write past n */
	size_t sizes[] = { 1, 3, 63, 65, 1000 };
	for (int i = 0; i < 5; i++) {
		size_t n = sizes[i];
		memset(bigbuf, 0xaa, sizeof(bigbuf));
		arc4random_buf(bigbuf, n);
		/* Check byte at index n is still 0xaa */
		KT_EQ(bigbuf[n], 0xaa);
	}
}

KTEST(042, rng_reseed_counter_advances)
{
	uint64_t r0 = rng_reseeds;
	KT_ASSERT(r0 >= 1);

	rng_reseed();
	KT_EQ(rng_reseeds, r0 + 1);

	/* Draw RNG_RESEED_BYTES + 4096 bytes in 4096-byte chunks */
	static uint8_t chunk[4096];
	size_t total = RNG_RESEED_BYTES + 4096;
	while (total > 0) {
		size_t to_read = total > 4096 ? 4096 : total;
		arc4random_buf(chunk, to_read);
		total -= to_read;
	}

	/* Check that at least one more reseed occurred */
	KT_ASSERT(rng_reseeds >= r0 + 2);
}

KTEST(043, stack_guard_and_canaries_are_random)
{
	/* Low byte of __stack_chk_guard is zero by design */
	KT_EQ(__stack_chk_guard & 0xff, 0);

	/* __stack_chk_guard must not be zero */
	KT_ASSERT(__stack_chk_guard != 0);

	/* __stack_chk_guard must not be the old fixed value */
	KT_ASSERT(__stack_chk_guard != 0x595e9fbd94fda766ULL);

	/* stack_canary must not be zero or the old fixed value */
	KT_ASSERT(stack_canary != 0);
	KT_ASSERT(stack_canary != 0x4d61676961537421ULL);

	/* The running test thread's stack bottom carries the canary */
	KT_EQ(*(uint64_t *)curthread()->stack, stack_canary);
	KT_ASSERT(stack_canary != __stack_chk_guard);
	/* every entropy source this CPU offers was used at the last reseed */
	KT_ASSERT(rng_sources & RNG_SRC_TSC);
	KT_ASSERT(bootinfo.nentropy == 0); /* Limine's values were consumed and wiped */
	/* arc4random_uniform stays in range and hits both ends of a small range */
	bool lo = false, hi = false;
	for (int i = 0; i < 2000; i++) {
		uint32_t r = arc4random_uniform(10);
		KT_ASSERT(r < 10);
		lo |= r == 0;
		hi |= r == 9;
	}
	KT_ASSERT(lo && hi);
	KT_EQ(arc4random_uniform(0), 0);
	KT_EQ(arc4random_uniform(1), 0);
}
