/*
 * Copyright (C) 2018 Marvell International Ltd.
 *
 * SPDX-License-Identifier:    GPL-2.0
 * https://spdx.org/licenses
 */


/**
 * Atomically adds a signed value to a 64 bit (aligned) memory location,
 * and returns previous value.
 *
 * This version does not perform 'sync' operations to enforce memory
 * operations.  This should only be used when there are no memory operation
 * ordering constraints.  (This should NOT be used for reference counting -
 * use the standard version instead.)
 *
 * @param ptr    address in memory to add incr to
 * @param incr   amount to increment memory location by (signed)
 *
 * @return Value of memory location before increment
 */
static inline s64 cavm_atomic_fetch_and_add64_nosync(s64 *ptr, s64 incr)
{
	s64 result;
	/* Atomic add with no ordering */
	asm volatile("ldadd %x[i], %x[r], [%[b]]"
		     : [r] "=r" (result), "+m" (*ptr)
		     : [i] "r" (incr), [b] "r" (ptr)
		     : "memory");
	return result;
}

static inline void cavm_lmt_cancel(const struct nix *nix)
{
	writeq(0, nix->lmt_base + CAVM_LMT_LF_LMTCANCEL());
}

static inline volatile u64 *cavm_lmt_store_ptr(struct nix *nix)
{
	return (volatile u64 *)((u8 *)(nix->lmt_base) +
				       CAVM_LMT_LF_LMTLINEX(0));
}

static inline s64 cavm_lmt_submit(u64 io_address)
{
	s64 result = 0;

	asm volatile("ldeor xzr, %x[rf],[%[rs]]"
			: [rf] "=r"(result) : [rs] "r"(io_address));
	return result;
}
