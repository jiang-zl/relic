/*
 * RELIC is an Efficient LIbrary for Cryptography
 * Copyright (C) 2007-2013 RELIC Authors
 *
 * This file is part of RELIC. RELIC is legal property of its developers,
 * whose names are not listed here. Please refer to the COPYRIGHT file
 * for contact information.
 *
 * RELIC is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 *
 * RELIC is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public License
 * along with RELIC. If not, see <http://www.gnu.org/licenses/>.
 */

/**
 * @file
 *
 * Implementation of the hash-drbg pseudo-random number generator.
 *
 * @version $Id: relic_rand.c 1522 2013-08-27 15:55:54Z dfaranha $
 * @ingroup rand
 */

#include <stdlib.h>
#include <stdint.h>

#include "relic_conf.h"
#include "relic_core.h"
#include "relic_label.h"
#include "relic_rand.h"
#include "relic_md.h"
#include "relic_err.h"

/*============================================================================*/
/* Private definitions                                                        */
/*============================================================================*/

/*
 * Computes the hash derivation function.
 *
 * param[out] out       - the result.
 * param[in] out_len    - the number of bytes to return.
 * param[in] in         - the input string.
 * param[in] in_len     - the number of bytes in the input.
 */
static void rand_hash(unsigned char *out, int out_len, unsigned char *in,
		int in_len) {
	uint32_t j = util_conv_big(8 * out_len);
	int len = CEIL(out_len, MD_LEN);
	unsigned char buf[1 + sizeof(uint32_t) + in_len], hash[MD_LEN];

	buf[0] = 1;
	memcpy(buf + 1, &j, sizeof(uint32_t));
	memcpy(buf + 1 + sizeof(uint32_t), in, in_len);

	for (int i = 0; i < len; i++) {
		/* h = Hash(counter || bits_to_return || input_string) */
		md_map(hash, buf, 1 + sizeof(uint32_t) + in_len);
		/* temp = temp || h */
		memcpy(out, hash, MIN(MD_LEN, out_len));
		out += MD_LEN;
		out_len -= MD_LEN;
		/* counter = counter + 1 */
		buf[0]++;
	}
}

/**
 * Accumulates a small integer in the internal state.
 * 
 * @param[in,out] state		- the internal state.
 * @param[in] digit			- the small integer.
 */
static int rand_inc(unsigned char *data, int size, int digit) {
	int carry = digit;
	for (int i = size - 1; i >= 0; i--) {
		int16_t s;
		s = (data[i] + carry);
		data[i] = s & 0xFF;
		carry = s >> 8;
	}
	return carry;
}

/**
 * Accumulates the hash value in the internal state.
 * 
 * @param[in,out] state		- the internal state.
 * @param[in] hash			- the hash value.
 */
static int rand_add(unsigned char *state, unsigned char *hash, int size) {
	int carry = 0;
	for (int i = size - 1; i >= 0; i--) {
		/* Make sure carries are detected. */
		int16_t s;
		s = (state[i] + hash[i] + carry);
		state[i] = s & 0xFF;
		carry = s >> 8;
	}
	return carry;
}

/**
 * Generates pseudo-random bytes by iterating the hash function.
 *
 * @param[out] out 			- the buffer to write.
 * @param[in] out_len		- the number of bytes to write.
 */
static void rand_gen(unsigned char *out, int out_len) {
	int m = CEIL(out_len, MD_LEN);
	unsigned char hash[MD_LEN], data[(RAND_SIZE - 1)/2];
	ctx_t *ctx = core_get();

	/* data = V */
	memcpy(data, ctx->rand + 1, (RAND_SIZE - 1)/2);
	for (int i = 0; i < m; i++) {
		/* w_i = Hash(data) */
		md_map(hash, data, sizeof(data));
		/* W = W || w_i */
		memcpy(out, hash, MIN(MD_LEN, out_len));
		out += MD_LEN;
		out_len -= MD_LEN;
		/* data = data + 1 mod 2^b. */
		rand_inc(data, (RAND_SIZE - 1)/2, 1);
	}
}

/*============================================================================*/
/* Public definitions                                                         */
/*============================================================================*/

void rand_bytes(unsigned char *buf, int size) {
	unsigned char hash[MD_LEN];
	int carry, len  = (RAND_SIZE - 1)/2;
	ctx_t *ctx = core_get();

	if (sizeof(int) > 2 && size > (1 << 16)) {
		THROW(ERR_NO_VALID);
	}

	/* buf = hash_gen(size) */
	rand_gen(buf, size);
	/* H = hash(03 || V) */
	ctx->rand[0] = 0x3;
	md_map(hash, ctx->rand, 1 + len);
	/* V = V + H + C  + reseed_counter. */
	rand_add(ctx->rand + 1, ctx->rand + 1 + len, len);
	carry = rand_add(ctx->rand + 1 + (len - MD_LEN), hash, MD_LEN);
	rand_inc(ctx->rand, len - MD_LEN + 1, carry);
	rand_inc(ctx->rand, len + 1, ctx->counter);
	ctx->counter = ctx->counter + 1;
}

void rand_seed(unsigned char *buf, int size) {
	ctx_t *ctx = core_get();
	int len = (RAND_SIZE - 1) / 2;

	if (size <= 0) {
		THROW(ERR_NO_VALID);
	}

	if (sizeof(int) > 4 && size > (1 << 32)) {
		THROW(ERR_NO_VALID);
	}

	ctx->rand[0] = 0x0;
	if (ctx->seeded == 0) {
		/* V = hash_df(seed). */
		rand_hash(ctx->rand + 1, len, buf, size);
		/* C = hash_df(00 || V). */
		rand_hash(ctx->rand + 1 + len, len, ctx->rand, len + 1);
	} else {
		/* V = hash_df(01 || V || seed). */
		unsigned char tmp[1 + len + size];
		tmp[0] = 1;
		memcpy(tmp + 1, ctx->rand + 1, len);
		memcpy(tmp + 1 + len, buf, size);
		rand_hash(ctx->rand + 1, len, tmp, sizeof(tmp));
		/* C = hash_df(00 || V). */
		rand_hash(ctx->rand + 1 + len, len, ctx->rand, len + 1);
	}
	ctx->counter = ctx->seeded = 1;
}