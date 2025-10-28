#ifndef _PTX_H
#define _PTX_H

#include<cuda_runtime.h>

#define madc_hi(dest, a, x, b) asm volatile("madc.hi.u32 %0, %1, %2, %3;\n\t" : "=r"(dest) : "r"(a), "r"(x), "r"(b))
#define madc_hi_cc(dest, a, x, b) asm volatile("madc.hi.cc.u32 %0, %1, %2, %3;\n\t" : "=r"(dest) : "r"(a), "r"(x), "r"(b))
#define mad_hi_cc(dest, a, x, b) asm volatile("mad.hi.cc.u32 %0, %1, %2, %3;\n\t" : "=r"(dest) : "r"(a), "r"(x), "r"(b))

#define mad_lo_cc(dest, a, x, b) asm volatile("mad.lo.cc.u32 %0, %1, %2, %3;\n\t" : "=r"(dest) : "r"(a), "r"(x), "r"(b))
#define madc_lo(dest, a, x, b) asm volatile("madc.lo.u32 %0, %1, %2, %3;\n\t" : "=r"(dest) : "r"(a), "r"(x), "r"(b))
#define madc_lo_cc(dest, a, x, b) asm volatile("madc.lo.cc.u32 %0, %1, %2, %3;\n\t" : "=r"(dest) : "r"(a), "r"(x),"r"(b))

#define addc(dest, a, b) asm volatile("addc.u32 %0, %1, %2;\n\t" : "=r"(dest) : "r"(a), "r"(b))
#define add_cc(dest, a, b) asm volatile("add.cc.u32 %0, %1, %2;\n\t" : "=r"(dest) : "r"(a), "r"(b))
#define addc_cc(dest, a, b) asm volatile("addc.cc.u32 %0, %1, %2;\n\t" : "=r"(dest) : "r"(a), "r"(b))

#define sub_cc(dest, a, b) asm volatile("sub.cc.u32 %0, %1, %2;\n\t" : "=r"(dest) : "r"(a), "r"(b))
#define subc_cc(dest, a, b) asm volatile("subc.cc.u32 %0, %1, %2;\n\t" : "=r"(dest) : "r"(a), "r"(b))
#define subc(dest, a, b) asm volatile("subc.u32 %0, %1, %2;\n\t" : "=r"(dest) : "r"(a), "r"(b))

#define set_eq(dest,a,b) asm volatile("set.eq.u32.u32 %0, %1, %2;\n\t" : "=r"(dest) : "r"(a), "r"(b))

#define lsbpos(x) (__ffs((x)))

__device__ __forceinline__ unsigned int endian(unsigned int x)
{
	return (x << 24) | ((x << 8) & 0x00ff0000) | ((x >> 8) & 0x0000ff00) | (x >> 24);
}

/**
 * Phase 3 Optimizations: Modern PTX instructions for RTX 4090
 * These leverage funnel shifts and other advanced instructions
 */

// Funnel shift right - much faster than manual rotation on modern GPUs
__device__ __forceinline__ unsigned int rotr_ptx(unsigned int x, unsigned int n)
{
	unsigned int result;
	asm("shf.r.wrap.b32 %0, %1, %2, %3;" : "=r"(result) : "r"(x), "r"(x), "r"(n));
	return result;
}

// Funnel shift left - for completeness
__device__ __forceinline__ unsigned int rotl_ptx(unsigned int x, unsigned int n)
{
	unsigned int result;
	asm("shf.l.wrap.b32 %0, %1, %2, %3;" : "=r"(result) : "r"(x), "r"(x), "r"(n));
	return result;
}

// Bit reverse for endian swaps (single instruction on modern GPUs)
__device__ __forceinline__ unsigned int brev(unsigned int x)
{
	unsigned int result;
	asm("brev.b32 %0, %1;" : "=r"(result) : "r"(x));
	return result;
}

// Fast byte permutation for endian operations
__device__ __forceinline__ unsigned int prmt_endian(unsigned int x)
{
	unsigned int result;
	// Permute bytes: 0x0123 -> 0x3210
	asm("prmt.b32 %0, %1, 0, 0x0123;" : "=r"(result) : "r"(x));
	return result;
}

// Warp-level shuffle for parallel reduction (useful for big integer operations)
__device__ __forceinline__ unsigned int warp_shuffle_down(unsigned int val, int offset)
{
	return __shfl_down_sync(0xffffffff, val, offset);
}

__device__ __forceinline__ unsigned int warp_shuffle_xor(unsigned int val, int mask)
{
	return __shfl_xor_sync(0xffffffff, val, mask);
}

// Video instructions for parallel bitwise operations (Turing+)
__device__ __forceinline__ unsigned int lop3_maj(unsigned int a, unsigned int b, unsigned int c)
{
	unsigned int result;
	// LUT: 0xE8 = MAJ(a,b,c) = (a&b)|(a&c)|(b&c)
	asm("lop3.b32 %0, %1, %2, %3, 0xE8;" : "=r"(result) : "r"(a), "r"(b), "r"(c));
	return result;
}

__device__ __forceinline__ unsigned int lop3_ch(unsigned int e, unsigned int f, unsigned int g)
{
	unsigned int result;
	// LUT: 0xCA = CH(e,f,g) = (e&f)^(~e&g)
	asm("lop3.b32 %0, %1, %2, %3, 0xCA;" : "=r"(result) : "r"(e), "r"(f), "r"(g));
	return result;
}

/**
 * Phase 4 Optimizations: Warp-level voting and reduction primitives
 * For parallel boolean operations across warps
 */

// Warp-level ballot: returns bitmask of predicate across warp
__device__ __forceinline__ unsigned int warp_ballot(bool predicate)
{
	return __ballot_sync(0xffffffff, predicate);
}

// Warp-level all: returns true if predicate is true for all threads in warp
__device__ __forceinline__ bool warp_all(bool predicate)
{
	return __all_sync(0xffffffff, predicate);
}

// Warp-level any: returns true if predicate is true for any thread in warp
__device__ __forceinline__ bool warp_any(bool predicate)
{
	return __any_sync(0xffffffff, predicate);
}

// Warp-level uniform integer shuffle for broadcasting values
__device__ __forceinline__ unsigned int warp_broadcast(unsigned int val, int srcLane)
{
	return __shfl_sync(0xffffffff, val, srcLane);
}

#endif