/*
 * The MIT License (MIT)         Copyright (c) 2017 Daniel Kubec <niel@rtfm.cz> 
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

#ifndef __HPC_BITS_H__
#define __HPC_BITS_H__

#include <hpc/compiler.h>

/* Next Largest Power of 2 */
static inline unsigned int
bits_nlpo2(register unsigned int x)
{
        x |= (x >> 1);
        x |= (x >> 2);
        x |= (x >> 4);
        x |= (x >> 8);
        x |= (x >> 16);
        return(x+1);
}

/* Most Significant 1 Bit */
static inline unsigned int
bits_msb32(register unsigned int x)
{
        x |= (x >> 1);
        x |= (x >> 2);
        x |= (x >> 4);
        x |= (x >> 8);
        x |= (x >> 16);
        return(x & ~(x >> 1));
}

/* Trailing Zero Count */
static inline int unsigned int
bits_tzc(register int x)
{
        return(ones((x & -x) - 1));
}

static inline int
bits_select(int cond, int a, int b)
{
    return (a& -cond) | (b & ~(-cond)); 
}

static inline u32
bits_u32_is_pow2(u32 x)
{
	return (x && !(x & (x - 1)));
}

static inline int
bits_u32_has_zerobyte(u32 u32)
{
	return ~((((u32 & 0x7F7F7F7F) + 0x7F7F7F7F) | u32) | 0x7F7F7F7F);
}

static inline int
bits_int_cond_inc(const int a, const int b, const int c, int x)
{
	x += (( b - a ) >> 31) & C;
	return x;
}

static inline int
bits_int_cond_inc2(const int a, const int b, const int c, int x)
{
	x += -( a > b ) & c;
	return x;
}

#define xor_given_num_tobits(num1, num, tobit) \
	num1 = num ^ ((1 << tobit) - 1)

#define xor_with_only_firstbits(num, first) \
	num = num ^ ((1 << first) - 1)

/*
 * A 2n-bits Dyck word is a string containing exactly n 1s and n 0s and such 
 * that each of its prefix substrings contains no more 0s than 1s.
 * Dyck words appear in a vast number of problems [5]. Consequently, generating
 * them has many applications.
 *
 * For instance, if 1s and 0s are replaced with
 * openning and closing parentheses, then a Dyck word is a combination of n 
 * properly balanced pairs of parentheses. When 1 denotes a move rightwards and
 * 0 denotes a move upwards, a Dyck word represents a monotonic path along the
 * edges of an n × n grid that starts at the lower left corner, finishes at the
 * upper right corner and stays below diagonal. Figure 1 shows all 14 such paths
 * on a 4 × 4 grid.
 *
 * For the sake of clarity, another implementation of essentially the same 
 * algorithm is presented. This one works on strings of two different
 * arbitrarily chosen symbols (e.g., opening and closing parentheses).
 *
 * Contrarily to the implementation in Listing 1, the second one has explicit
 * loops and branches and has O(n) time complexity.
 *
 * The apparent contradiction between being two implementations of “essentially
 * the same” algorithm but one being loopless, brancheless and O(1) and the
 * other having loops, branches and being O(n) can be explanained as follows.
 *
 * https://arxiv.org/pdf/1602.06426.pdf
 */

static inline u64
bits_next_dyck_word(u64 w) {
	u64 a = w & −w;
	u64 b = w + a;
	u64 c = w ^ b;
	c = (c / a >> 2) + 1 ;
	c = ((c * c − 1) & 0xaaaaaaaaaaaaaaaa ) | b ;
	return c;
}

#endif
