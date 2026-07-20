/*
 * Routines to provide a memory-efficient hashtable.
 *
 * Copyright (C) 2007-2022 Wayne Davison
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 3 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License along
 * with this program; if not, visit the http://fsf.org website.
 */

#include "rsync.h"

#define HASH_LOAD_LIMIT(size) ((size)*3/4)

#define hashsize(n) ((uint32_t)1<<(n))
#define hashmask(n) (hashsize(n)-1)
#define rot(x,k) (((x)<<(k)) | ((x)>>(32-(k))))

struct hashtable *hashtable_create(int size)
{
	int req = size;
	struct hashtable *tbl;
	uint32 node_size = sizeof(struct ht_int64_node);

	if ((uint32)size > (INT_MAX / node_size)) {
		rprintf(FERROR, "[%s] called hashtable_create with invalid parameter\n", who_am_i());
		exit_cleanup(RERR_PROTOCOL);
    }

	/* Pick a power of 2 that can hold the requested size. */
#ifdef HAVE_STDC_BIT_CEIL
	size = stdc_bit_ceil(req < 16 ? (size_t)16 : (size_t)req);
#else
	size = 16;
	while (size < req)
		size *= 2;
#endif

	tbl = new(struct hashtable);
	tbl->nodes = new_array0(char, size * node_size);
	tbl->size = size;
	tbl->entries = 0;
	tbl->node_size = node_size;

	if (DEBUG_GTE(HASH, 1)) {
		char buf[32];
		if (req != size)
			snprintf(buf, sizeof buf, "req: %d, ", req);
		else
			*buf = '\0';
		rprintf(FINFO, "[%s] created hashtable %lx (%ssize: %d, keys: 64-bit)\n",
			who_am_i(), (long)tbl, buf, size);
	}

	return tbl;
}

void hashtable_destroy(struct hashtable *tbl)
{
	if (DEBUG_GTE(HASH, 1)) {
		rprintf(FINFO, "[%s] destroyed hashtable %lx (size: %d, keys: 64-bit)\n",
			who_am_i(), (long)tbl, tbl->size);
	}
	free(tbl->nodes);
	free(tbl);
}

/* Returns the node that holds the indicated key if it exists. When it does not
 * exist, it returns either NULL (when data_when_new is NULL), or it returns a
 * new node with its node->data set to the indicated value.
 *
 * If your code doesn't know the data value for a new node in advance (usually
 * because it doesn't know if a node is new or not) you should pass in a unique
 * (non-0) value that you can use to check if the returned node is new. You can
 * then overwrite the data with any value you want (even 0) since it only needs
 * to be different than whatever data_when_new value you use later on. */
struct ht_int64_node *hashtable_find(struct hashtable *tbl, int64 key, void *data_when_new)
{
	struct ht_int64_node *node;
	uint32 ndx;

	if (key == 0) {
		rprintf(FERROR, "Internal hashtable error: illegal key supplied!\n");
		exit_cleanup(RERR_MESSAGEIO);
	}

	if (data_when_new && tbl->entries > HASH_LOAD_LIMIT(tbl->size)) {
		void *old_nodes = tbl->nodes;
		int size = tbl->size * 2;
		int i;

		tbl->nodes = new_array0(char, size * tbl->node_size);
		tbl->size = size;
		tbl->entries = 0;

		if (DEBUG_GTE(HASH, 1)) {
			rprintf(FINFO, "[%s] growing hashtable %lx (size: %d, keys: 64-bit)\n",
				who_am_i(), (long)tbl, size);
		}

		for (i = size / 2; i-- > 0; ) {
			struct ht_int64_node *move_node = ht_node(tbl, old_nodes, i);
			int64 move_key = ht_key(move_node);
			if (move_key == 0)
				continue;
			if (move_node->data)
				hashtable_find(tbl, move_key, move_node->data);
			else {
				node = hashtable_find(tbl, move_key, "");
				node->data = 0;
			}
		}

		free(old_nodes);
	}

#if SIZEOF_INT64 >= 8
	/* Pelle Evensen's Moremur */
	uint64_t h = (uint64_t)key;
	h ^= h >> 27;
	h *= 0x3C79AC492BA7B653ULL;
	h ^= h >> 33;
	h *= 0x1C69B3F74AC4AE35ULL;
	h ^= h >> 27;
	ndx = (uint32_t)h;
#else
	uint32_t h = (uint32_t)key;
	h ^= h >> 16;
	h *= 0x21f0aaad;
	h ^= h >> 15;
	h *= 0xf35a2d97;
	h ^= h >> 15;
	ndx = h;
#endif

	/* If it already exists, return the node.  If we're not
	 * allocating, return NULL if the key is not found. */
	while (1) {
		int64 nkey;

		ndx &= tbl->size - 1;
		node = ht_node(tbl, tbl->nodes, ndx);
		nkey = ht_key(node);

		if (nkey == key)
			return node;
		if (nkey == 0) {
			if (!data_when_new)
				return NULL;
			break;
		}
		ndx++;
	}

	node->key = key;
	node->data = data_when_new;
	tbl->entries++;
	return node;
}

uint64_t hashlittle2(const void *key, size_t length)
{
	static int rndinit;
	static uint64_t sipkey[2];
	uint64_t ret;

	if (!rndinit) {
		rand_bytes(sipkey, sizeof(sipkey));
		rndinit = 1;
	}
	ret = siphash13(key, length, sipkey[0], sipkey[1]);
	return ret ?: 1;
}

#if defined(_MSC_VER)
#  include <stdlib.h>
#  define SIP_INLINE __forceinline
#  define sip_bswap64 _byteswap_uint64
#else
#  define SIP_INLINE inline __attribute__((always_inline))
#  define sip_bswap64 __builtin_bswap64
#endif

#define ROTL64(x, b) (uint64_t)(((x) << (b)) | ((x) >> (64 - (b))))

/* One SipRound, operating on the four state words. */
#define SIPROUND(v0, v1, v2, v3)                                              \
	do {                                                                      \
		(v0) += (v1); (v1) = ROTL64((v1), 13); (v1) ^= (v0);                  \
		(v0) = ROTL64((v0), 32);                                              \
		(v2) += (v3); (v3) = ROTL64((v3), 16); (v3) ^= (v2);                  \
		(v0) += (v3); (v3) = ROTL64((v3), 21); (v3) ^= (v0);                  \
		(v2) += (v1); (v1) = ROTL64((v1), 17); (v1) ^= (v2);                  \
		(v2) = ROTL64((v2), 32);                                              \
	} while (0)

/* Unaligned little-endian 64-bit load. */
static SIP_INLINE uint64_t sip_load64_le(const unsigned char *p)
{
	uint64_t v;
	memcpy(&v, p, sizeof v);
#if defined(__BYTE_ORDER__) && (__BYTE_ORDER__ == __ORDER_BIG_ENDIAN__)
	v = sip_bswap64(v);
#endif
	return v;
}

/*
 * Branch-free ASCII lowercasing of 8 bytes at once (SWAR).
 *
 * For every byte b: if 'A' <= b <= 'Z' then set bit 0x20, else leave b as-is.
 * Bytes with the high bit set (non-ASCII) are never touched, and zero padding
 * bytes in the final partial word are unaffected ('\0' is not uppercase).
 */
static SIP_INLINE uint64_t sip_tolower64(uint64_t x)
{
	const uint64_t hi   = 0x8080808080808080ULL;
	uint64_t seven      = x & 0x7F7F7F7F7F7F7F7FULL;  /* low 7 bits of each byte */
	uint64_t ge_A       = seven + 0x3F3F3F3F3F3F3F3FULL; /* bit7 set iff byte >= 'A' (0x41) */
	uint64_t gt_Z       = seven + 0x2525252525252525ULL; /* bit7 set iff byte >  'Z' (0x5A) */
	uint64_t is_upper   = (ge_A & ~gt_Z & ~x) & hi;      /* ~x: reject non-ASCII bytes   */
	return x | (is_upper >> 2);                          /* 0x80 >> 2 == 0x20            */
}

/*
 * Core. `fold_case` is a compile-time constant at each call site, so the
 * compiler removes the untaken branches entirely.
 */
static SIP_INLINE uint64_t
siphash13_core(const void *data, size_t len, uint64_t k0, uint64_t k1,
			   int fold_case)
{
	const unsigned char *p   = (const unsigned char *)data;
	const unsigned char *end = p + (len & ~(size_t)7);
	size_t left = len & 7;

	uint64_t v0 = 0x736f6d6570736575ULL ^ k0;
	uint64_t v1 = 0x646f72616e646f6dULL ^ k1;
	uint64_t v2 = 0x6c7967656e657261ULL ^ k0;
	uint64_t v3 = 0x7465646279746573ULL ^ k1;
	uint64_t m;

	for (; p != end; p += 8) {
		m = sip_load64_le(p);
		if (fold_case)
			m = sip_tolower64(m);
		v3 ^= m;
		SIPROUND(v0, v1, v2, v3);
		v0 ^= m;
	}

	/* Final block: remaining 0..7 bytes, with (len & 0xff) in the top byte. */
	m = 0;
	if (left)
		memcpy(&m, p, left); /* little-endian layout of the tail bytes */
#if defined(__BYTE_ORDER__) && (__BYTE_ORDER__ == __ORDER_BIG_ENDIAN__)
	m = sip_bswap64(m) >> (8 * (8 - left));
#endif
	if (fold_case)
		m = sip_tolower64(m); /* padding zeros and length byte area are 0 here */
	m |= (uint64_t)(len & 0xff) << 56;

	v3 ^= m;
	SIPROUND(v0, v1, v2, v3);
	v0 ^= m;

	v2 ^= 0xff;
	SIPROUND(v0, v1, v2, v3);
	SIPROUND(v0, v1, v2, v3);
	SIPROUND(v0, v1, v2, v3);

	return v0 ^ v1 ^ v2 ^ v3;
}

uint64_t siphash13(const void *data, size_t len, uint64_t k0, uint64_t k1)
{
	return siphash13_core(data, len, k0, k1, 0);
}

uint64_t siphash13_ci(const void *data, size_t len, uint64_t k0, uint64_t k1)
{
	return siphash13_core(data, len, k0, k1, 1);
}
