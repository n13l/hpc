/*
 * The MIT License (MIT)         Copyright (c) 2026 Daniel Kubec <niel@rtfm.cz> 
 *
 * Permission is hereby granted, free of charge, to any person obtaining a copy 
 * of this software and associated documentation files (the "Software"),to deal
 * in the Software without restriction, including without limitation the rights 
 * to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
 * copies of the Software, and to permit persons to whom the Software is
 * furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in
 * all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 * AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 * OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
 * THE SOFTWARE.
 */

#ifndef __HPC_INDEX_H__
#define __HPC_INDEX_H__

#include <hpc/compiler.h>

/*
 * Constant-Time Power-of-2 Index Verification
 *
 * Branchless bounds checking for array indices, designed for side-channel
 * resistant code. All implementations take the same number of cycles
 * regardless of whether the index is valid, preventing timing attacks.
 *
 * Comparison: generic branch vs. branchless implementations
 *
 *   A generic bounds-checked array access uses a conditional branch:
 *
 *     if (index < size) return arr[index];
 *     return arr[size - 1];
 *
 *                            | Predicted OK  | Mispredicted
 *   -------------------------+---------------+------------------
 *   Generic branch (cmp+jcc) | ~1 cycle      | ~12-20 cycles
 *   index_pow2_0b_arm64      |  2 cycles     |  2 cycles (always)
 *   index_pow2_0b_x86_modern |  2 cycles     |  2 cycles (always)
 *   index_pow2_0b_x86        |  7 cycles     |  7 cycles (always)
 *   index_pow2_0b_bw (C)     |  7 cycles     |  7 cycles (always)
 *
 *   Misprediction penalties: Intel ~12-20 cycles, AMD Zen ~10-15 cycles.
 *
 * When branches win:
 *
 *   If the branch is highly predictable (e.g., index is almost always valid),
 *   the branch version is faster (~1 cycle vs ~2 cycles) because modern
 *   predictors achieve >99% accuracy on repetitive patterns.
 *
 * When branchless wins:
 *
 *   - Random or adversarial indices: misprediction rate rises, each costing
 *     12-20 cycles. Even 5% misprediction rate makes branchless faster.
 *   - Constant-time requirement: branches leak the condition via timing
 *     side-channels. For crypto, this is the primary motivation — branchless
 *     versions take the same time regardless of whether the index is valid,
 *     preventing an attacker from inferring array access patterns.
 *   - Out-of-order execution: branches can stall the entire pipeline due to
 *     speculative execution rollback. Branchless code has a fixed, predictable
 *     cost that the OoO engine can schedule around.
 *
 * CMOV considerations (pre-Broadwell / pre-Zen):
 *
 *   On older x86-64 microarchitectures, CMOV is decoded as 2 uops with
 *   higher latency (~2+ cycles) and lower throughput compared to regular MOV.
 *   It can also increase dependency chains and cause pipeline stalls.
 *   The bitwise variants (_bw, _x86) avoid CMOV entirely, using only basic
 *   ALU operations (AND, OR, XOR, NEG) which have lower latencies, higher
 *   throughput, and are more easily scheduled by the OoO engine.
 *
 *   On modern CPUs (Broadwell+, Zen+), CMOV is single uop with 1-cycle
 *   latency, making the _modern variants strictly superior.
 *
 * Sustained throughput (1000 sequential accesses, ~15 cycle mispredict):
 *
 *   Misprediction rate | Branch (cmp+jcc) | Branchless (2 cyc)
 *   -------------------+------------------+--------------------
 *   0%  (always valid) | ~1000 cycles     | ~2000 cycles
 *   1%                 | ~1150 cycles     | ~2000 cycles
 *   5%                 | ~1750 cycles     | ~2000 cycles
 *   7%  (break-even)   | ~2050 cycles     | ~2000 cycles
 *   10%                | ~2500 cycles     | ~2000 cycles
 *   50% (random)       | ~8500 cycles     | ~2000 cycles
 *
 *   Break-even point is ~7% misprediction rate. Below that, branches win.
 *   Above that, branchless is faster. For crypto, predictability is
 *   irrelevant — even a single observable timing variation can leak a key bit.
 */

/*
 * index_pow2_0b_bw() - Portable C bitwise, ~7 CPU cycles, no branches.
 *
 * Branchless and constant-time. Both paths are always computed and the
 * result is selected via bitwise masking. Safe for side-channel resistant
 * code on all architectures.
 *
 * Expected latency: ~7 cycles.
 *
 * xor     edx, edx
 * cmp     esi, edi
 * setb    dl
 * lea     eax, [rdx-1]
 * neg     edx
 * and     eax, edi
 * and     edx, esi
 * or      eax, edx
 */

static inline u32
index_pow2_0b_bw(u32 index, u32 mask)
{
	unsigned cond = -((index) > (mask));
	return (((index) & ~cond) | ((mask) & cond));
}

/*
 * index_pow2_0b_x86() - x86_64 inline asm, 8 instructions, no branches.
 *
 * Compatible with all x86_64 CPUs. Uses only basic ALU operations to avoid
 * cmov latency on older microarchitectures (pre-Broadwell / pre-Zen).
 *
 * xorl    %1, %1
 * cmpl    %3, %2
 * seta    %b1
 * leal    -1(%1), %0
 * negl    %1
 * andl    %2, %0
 * andl    %3, %1
 * orl     %1, %0
 */

#define index_pow2_0b_x86(index, mask) ({ \
	u32 __res; \
	u32 __tmp; \
	__asm__ ( \
		"xorl   %1, %1\n\t" \
		"cmpl   %3, %2\n\t" \
		"seta   %b1\n\t" \
		"leal   -1(%1), %0\n\t" \
		"negl   %1\n\t" \
		"andl   %2, %0\n\t" \
		"andl   %3, %1\n\t" \
		"orl    %1, %0" \
		: "=&r" (__res), "=&r" (__tmp) \
		: "r" ((u32)(index)), "r" ((u32)(mask)) \
		: "cc" \
	); \
	__res; \
})

/*
 * index_pow2_0b_x86_modern() - x86_64 inline asm, 2 instructions.
 *
 * Branchless and constant-time. CMOV always reads both operands and takes
 * the same number of cycles regardless of the condition. No branch prediction
 * or speculative execution is involved — the condition only controls which
 * value is written. Safe for side-channel resistant code.
 *
 * Optimized for modern Intel (Broadwell+) and AMD (Zen+) where CMOV executes
 * as single uop with 1-cycle latency. The operation is unsigned min(index, mask).
 *
 * The MOV for initialization is eliminated via register renaming on
 * Intel Ivy Bridge+ and AMD Zen+ (zero latency, zero throughput cost).
 *
 * Expected latency: ~2 cycles (1 cycle CMP + 1 cycle CMOV).
 * Expected throughput: 1 per cycle on modern cores.
 *
 * cmpl    %0, %1
 * cmovbel %1, %0
 */

#define index_pow2_0b_x86_modern(index, mask) ({ \
	u32 __res = (u32)(mask); \
	__asm__ ( \
		"cmpl   %0, %1\n\t" \
		"cmovbel %1, %0" \
		: "+r" (__res) \
		: "r" ((u32)(index)) \
		: "cc" \
	); \
	__res; \
})

/*
 * index_pow2_0b_arm64() - AArch64 inline asm, 2 instructions, no branches.
 *
 * cmp     w1, w2
 * csel    w0, w2, w1, hi
 */

#define index_pow2_0b_arm64(index, mask) ({ \
	u32 __res; \
	__asm__ ( \
		"cmp    %w1, %w2\n\t" \
		"csel   %w0, %w2, %w1, hi" \
		: "=r" (__res) \
		: "r" ((u32)(index)), "r" ((u32)(mask)) \
		: "cc" \
	); \
	__res; \
})

/*
 * index_pow2_1b_bw() - Portable C bitwise, ~5 CPU cycles, no branches.
 *
 * Branchless and constant-time. Tests if index exceeds the power-of-2
 * boundary (msb bit set) and masks the result via bitwise operations.
 *
 * Expected latency: ~5 cycles.
 *
 * xor     eax, eax
 * test    edi, edx
 * sete    al
 * neg     eax
 * and     eax, edi
 */

#define index_pow2_1b_bw(index, mask, msb) ({ \
	(index & (-(!(index & msb)))); \
})

/*
 * Branch-based fast path for 0-based index. Uses the branch predictor for
 * maximum throughput when indices are almost always valid (~1 cycle).
 */

static inline u32
index_pow2_0b_branch(u32 index, u32 mask)
{
	if (__builtin_expect(index <= mask, 1))
		return index;
	return mask;
}

#if defined(__x86_64__) || defined(_M_X64)
#define index_pow2_0b(index, mask) ({ index_pow2_0b_x86(index, mask); })
#elif defined(__aarch64__) || defined(__arm64__)
#define index_pow2_0b(index, mask) ({ index_pow2_0b_arm64(index, mask); })
#endif

/*
 * index_pow2_1b_x86() - x86_64 inline asm, 5 instructions, no branches.
 *
 * xorl    %0, %0
 * testl   %2, %1
 * sete    %b0
 * negl    %0
 * andl    %1, %0
 */

#define index_pow2_1b_x86(index, mask, msb) ({ \
	u32 __res; \
	__asm__ ( \
		"xorl   %0, %0\n\t" \
		"testl  %2, %1\n\t" \
		"sete   %b0\n\t" \
		"negl   %0\n\t" \
		"andl   %1, %0" \
		: "=&r" (__res) \
		: "r" ((u32)(index)), "r" ((u32)(msb)) \
		: "cc" \
	); \
	__res; \
})

/*
 * index_pow2_1b_arm64() - AArch64 inline asm, 2 instructions, no branches.
 *
 * tst     w1, w2
 * csel    w0, w1, wzr, eq
 */

#define index_pow2_1b_arm64(index, mask, msb) ({ \
	u32 __res; \
	__asm__ ( \
		"tst    %w1, %w2\n\t" \
		"csel   %w0, %w1, wzr, eq" \
		: "=r" (__res) \
		: "r" ((u32)(index)), "r" ((u32)(msb)) \
		: "cc" \
	); \
	__res; \
})

/*
 * Branch-based fast path for 1-based index. Uses the branch predictor for
 * maximum throughput when indices are almost always valid (~1 cycle).
 */

static inline u32
index_pow2_1b_branch(u32 index, u32 mask, u32 msb)
{
	if (__builtin_expect(!(index & msb), 1))
		return index;
	return 0;
}

#if defined(__x86_64__) || defined(_M_X64)
#define index_pow2_1b(index, mask, msb) ({ index_pow2_1b_x86(index, mask, msb); })
#elif defined(__aarch64__) || defined(__arm64__)
#define index_pow2_1b(index, mask, msb) ({ index_pow2_1b_arm64(index, mask, msb); })
#endif

#endif
