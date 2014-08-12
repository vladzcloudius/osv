/*
 * Copyright (C) 2013 Cloudius Systems, Ltd.
 *
 * This work is open source software, licensed under the terms of the
 * BSD license as described in the LICENSE file in the top-level directory.
 */


#include <string.h>
#include <stdint.h>
#include "cpuid.hh"
#include <osv/string.h>
#include <osv/prio.hh>
#include "memcpy_decode.hh"
#include <assert.h>
#include <x86intrin.h>
#include <osv/trace.hh>

TRACEPOINT(trace_memcpy_bad_pointers, "src %p, dest %p n %d", const void*, void*, size_t);

extern "C"
void *memcpy_base(void *__restrict dest, const void *__restrict src, size_t n);
extern "C"
void *memset_base(void *__restrict dest, int c, size_t n);

extern "C" void memcpy_fixup_byte(exception_frame *ef, size_t fixup)
{
    assert(fixup <= ef->rcx);
    ef->rdi += fixup;
    ef->rsi += fixup;
    ef->rcx -= fixup;
}

extern "C" void memcpy_fixup_long(exception_frame *ef, size_t fixup)
{
    assert(fixup/sizeof(long) <= ef->rcx);
    ef->rdi += fixup;
    ef->rsi += fixup;
    ef->rcx -= fixup / sizeof(long);
}

// Please note that the arguments to those functions are passed by reference. That
// allow us to reuse the fact that the updates to src and dest will be held in the
// registers themselves, and avoid storing that temporarily.
//
// The generated code is significantly different:
//
// 0000000000330d40 <memcpy_repmov_old>:
// 330d40:       55                      push   %rbp
// 330d41:       48 89 d1                mov    %rdx,%rcx
// 330d44:       48 89 f8                mov    %rdi,%rax
// 330d47:       48 c1 e9 03             shr    $0x3,%rcx
// 330d4b:       48 89 e5                mov    %rsp,%rbp
// 330d4e:       f3 48 a5                rep movsq %ds:(%rsi),%es:(%rdi)
// 330d51:       83 e2 07                and    $0x7,%edx
// 330d54:       48 89 d1                mov    %rdx,%rcx
// 330d57:       f3 a4                   rep movsb %ds:(%rsi),%es:(%rdi)
// 330d59:       5d                      pop    %rbp
//
// Versus:
//
// 0000000000330d40 <memcpy_repmov_old>:
// 330d40:       55                      push   %rbp
// 330d41:       48 89 d1                mov    %rdx,%rcx
// 330d44:       48 89 f8                mov    %rdi,%rax
// 330d47:       49 89 f1                mov    %rsi,%r9
// 330d4a:       48 c1 e9 03             shr    $0x3,%rcx
// 330d4e:       48 89 e5                mov    %rsp,%rbp
// 330d51:       f3 48 a5                rep movsq %ds:(%rsi),%es:(%rdi)
// 330d54:       48 c1 e1 03             shl    $0x3,%rcx
// 330d58:       83 e2 07                and    $0x7,%edx
// 330d5b:       48 8d 3c 08             lea    (%rax,%rcx,1),%rdi
// 330d5f:       49 8d 34 09             lea    (%r9,%rcx,1),%rsi
// 330d63:       48 89 d1                mov    %rdx,%rcx
// 330d66:       f3 a4                   rep movsb %ds:(%rsi),%es:(%rdi)
// 330d68:       5d                      pop    %rbp
//
//
// Note how in the second version the arguments need to be updated to reflect
// the displacement after movsq, while in the first version, it just happens
// naturally.
static inline __always_inline void
repmovsq(void *__restrict &dest, const void *__restrict &src, size_t &n)
{
    asm volatile
       ("1: \n\t"
        "rep movsq\n\t"
        ".pushsection .memcpy_decode, \"ax\" \n\t"
        ".quad 1b, 8, memcpy_fixup_long\n\t"
        ".popsection\n"
            : "+D"(dest), "+S"(src), "+c"(n) : : "memory");
}

static inline __always_inline void
repmovsb(void *__restrict &dest, const void *__restrict &src, size_t &n)
{
    asm volatile
       ("1: \n\t"
        "rep movsb\n\t"
        ".pushsection .memcpy_decode, \"ax\" \n\t"
        ".quad 1b, 1, memcpy_fixup_byte\n\t"
        ".popsection\n"
            : "+D"(dest), "+S"(src), "+c"(n) : : "memory");
}

template <size_t N>
__attribute__((optimize("omit-frame-pointer")))
void* do_small_memcpy(void *dest, const void *src)
{
    struct [[gnu::packed]] data {
        char x[N];
    };
    *static_cast<data*>(dest) = *static_cast<const data*>(src);
    return dest;
}

static void* (* const small_memcpy_table[16])(void*, const void*) = {
    do_small_memcpy<0>,
    do_small_memcpy<1>,
    do_small_memcpy<2>,
    do_small_memcpy<3>,
    do_small_memcpy<4>,
    do_small_memcpy<5>,
    do_small_memcpy<6>,
    do_small_memcpy<7>,
    do_small_memcpy<8>,
    do_small_memcpy<9>,
    do_small_memcpy<10>,
    do_small_memcpy<11>,
    do_small_memcpy<12>,
    do_small_memcpy<13>,
    do_small_memcpy<14>,
    do_small_memcpy<15>,
};

static inline void* small_memcpy(void *dest, const void *src, size_t n)
{
    return small_memcpy_table[n](dest, src);
}

template <unsigned N>
struct sse_register_file;

template <>
struct sse_register_file<0> {
    void load(const __m128i* p) {}
    void store(__m128i* p) {}
};

template <unsigned N>
struct sse_register_file : sse_register_file<N-1> {
    __m128i reg;
    void load(const __m128i* p) {
        sse_register_file<N-1>::load(p);
        reg = _mm_loadu_si128(&p[N-1]);
    }
    void store(__m128i* p) {
        sse_register_file<N-1>::store(p);
        _mm_storeu_si128(&p[N-1], reg);
    }
};

template <unsigned N>
__attribute__((optimize("unroll-loops"), optimize("omit-frame-pointer")))
void do_sse_memcpy(void* dest, const void* src)
{
    auto sse_dest = static_cast<__m128i*>(dest);
    auto sse_src = static_cast<const __m128i*>(src);
    sse_register_file<N> regs;
    regs.load(sse_src);
    regs.store(sse_dest);
}

static void (* const sse_memcpy_table[16])(void*, const void*) = {
    do_sse_memcpy<0>,
    do_sse_memcpy<1>,
    do_sse_memcpy<2>,
    do_sse_memcpy<3>,
    do_sse_memcpy<4>,
    do_sse_memcpy<5>,
    do_sse_memcpy<6>,
    do_sse_memcpy<7>,
    do_sse_memcpy<8>,
    do_sse_memcpy<9>,
    do_sse_memcpy<10>,
    do_sse_memcpy<11>,
    do_sse_memcpy<12>,
    do_sse_memcpy<13>,
    do_sse_memcpy<14>,
    do_sse_memcpy<15>,
};

static inline void* sse_memcpy(void* dest, const void* src, size_t n)
{
    sse_memcpy_table[n/16](dest, src);
    small_memcpy(dest + (n & ~15), src + (n & ~15), n & 15);
    return dest;
}

static inline void *memcpy_vlad(void *__restrict dest,
				const void *__restrict src, size_t n)
{
	size_t i, nw = n / 8;
        size_t nb = n & 7;
	u64 *dest64 = (u64*)dest;
	u64 *src64 = (u64*)src;

	if (((u64)src & 7) || ((u64)dest & 7)) {
		nw = 0;
		nb = n;
		trace_memcpy_bad_pointers(src, dest, n);
	}

	for (i = 0; i < nw; i++) {
		register u64 val = *src64++;
		*dest64++ = val;
	}

	u8 *dest8 = (u8*)(dest + 8 * nw);
	u8 *src8 = (u8*)(src + 8 * nw);

	for (i = 0; i < nb; i++) {
		register u8 val = *src8++;
		*dest8++ = val;
	}

	return dest;
}

extern "C"
void *memcpy_repmov_old(void *__restrict dest, const void *__restrict src, size_t n)
{
    if (n <= 15) {
        return small_memcpy(dest, src, n);
    } else if (n < 256) {
        return sse_memcpy(dest, src, n);
    } else {
#if 0
        auto ret = dest;
        auto nw = n / 8;
        auto nb = n & 7;

        repmovsq(dest, src, nw);
        repmovsb(dest, src, nb);

	return ret;
#else
	return memcpy_vlad(dest, src, n);
#endif
    }
}

extern "C"
void *memcpy_repmov(void *__restrict dest, const void *__restrict src, size_t n)
{
    if (n <= 15) {
        return small_memcpy(dest, src, n);
    } else if (n < 256) {
        return sse_memcpy(dest, src, n);
    } else {
        auto ret = dest;
        repmovsb(dest, src, n);
        return ret;
    }
}

extern "C"
void *(*resolve_memcpy())(void *__restrict dest, const void *__restrict src, size_t n)
{
    if (false && processor::features().repmovsb) {
        return memcpy_repmov;
    }
    return memcpy_repmov_old;
}

void *memcpy(void *__restrict dest, const void *__restrict src, size_t n)
    __attribute__((ifunc("resolve_memcpy")));

// Since we are using explicit loops, and not the rep instruction
// (that requires a very specific rcx layout), we can use the same
// fixup for both versions here.
extern "C" void backwards_fixup(exception_frame *ef, size_t fixup)
{
    assert(fixup <= ef->rcx);
    ef->rdi -= fixup;
    ef->rsi -= fixup;
    ef->rcx -= fixup;
}

// In both the function below, we'll write the loop in C and the actual
// assignment in assembly, because it is a lot easier. But that means that the
// loop counter may be in some register other than rcx. Because we have listed
// it as an input operand, *at the time* of copy it will be in the correct
// position, which means the compiler will have to generate an extra operation
// in that scenarion. We will trust the compiler to do the right thing and keep
// the counter in rcx since it knows it has to be there eventually. And if it
// can't, it can't. That's probably better than to code the whole thing ourselves.
static inline __always_inline void
byte_backwards(char * &d, const char * &s, size_t& n)
{
    // We could force a register and avoid declaring it, but it is better to leave
    // the compiler free.
    char tmp = 0;
    while (n--) {
        --d;
        --s;
        asm volatile
        (
         "1:\n\t"
         "mov (%1), %3\n\t"
         "mov %3, (%0)\n\t"
         ".pushsection .memcpy_decode, \"ax\" \n\t"
         ".quad 1b, 1, backwards_fixup\n\t"
         ".popsection\n"
            : "+D"(d), "+S"(s), "+c"(n) : "r"(tmp) : "memory");
    }
}

static inline __always_inline void
long_backwards(char * &d, const char * &s, size_t& n)
{
    unsigned long tmp = 0;
    for (; n >= sizeof(long); n -= sizeof(long)) {
        d -= sizeof(long);
        s -= sizeof(long);
        asm volatile
            (
             "1:\n\t"
             "mov    (%1), %3\n\t"
             "mov    %3, (%0)\n\t"
             ".pushsection .memcpy_decode, \"ax\" \n\t"
             ".quad 1b, 8, backwards_fixup\n\t"
             ".popsection\n"
                : "+D"(d), "+S"(s), "+c"(n) : "r"(tmp) : "memory");
    }
}

// According to Avi, this is likely to be faster than repmov with the direction
// flag set. Still, although always copying it byte by byte would be a lot simpler,
// it is faster to copy 8-byte aligned regions if we can. We'll go through the pain
// of doing that.
void *memcpy_backwards(void *dst, const void *src, size_t n)
{
    char *d = reinterpret_cast<char *>(dst);
    const char *s = reinterpret_cast<const char *>(src);

    // There are two fixup scenarios here to consider:
    // 1) If the addresses have the same alignment, in which case word-aligning one will
    // naturally word align the other
    // 2) mismatching alignments, in which we are no better if we copy words to the destination
    // since the source operand won't be word aligned.
    //
    // In general, we don't need to worry about fixups if the addresses are not aligned.
    // Since in the second case we will copy byte by byte anyway, we will do the whole operation
    // in one asm block.
    //
    // But for the first case, we can code it in C (without any fixups) until we reach the aligned
    // part, and only then introduce a fixup block.
    d += n;
    s += n;
    if ((uintptr_t)s % sizeof(unsigned long) == (uintptr_t)d % sizeof(unsigned long)) {
        while ((uintptr_t)(d) % sizeof(unsigned long)) {
            if (!n--) {
                return dst;
            }
            *d-- = *s--;
        }

        long_backwards(d, s, n);
    }

    byte_backwards(d, s, n);

    return dst;
}

extern memcpy_decoder memcpy_decode_start[], memcpy_decode_end[];

static void sort_memcpy_decoder() __attribute__((constructor(init_prio::sort)));

static void sort_memcpy_decoder()
{
    std::sort(memcpy_decode_start, memcpy_decode_end);
}

unsigned char *memcpy_decoder::dest(exception_frame *ef)
{
    return reinterpret_cast<unsigned char *>(ef->rdi);
}

unsigned char *memcpy_decoder::src(exception_frame *ef)
{
    return reinterpret_cast<unsigned char *>(ef->rsi);
}

size_t memcpy_decoder::size(exception_frame *ef)
{
    return ef->rcx * _size;
}

memcpy_decoder::memcpy_decoder(ulong pc, fixup_function fn)
    : _pc(pc), _fixup_fn(fn)
{
}

memcpy_decoder *memcpy_find_decoder(exception_frame *ef)
{
    memcpy_decoder v{ef->rip, 0};
    auto dec = std::lower_bound(memcpy_decode_start, memcpy_decode_end, v);
    if (dec != memcpy_decode_end && ((*dec) == ef->rip)) {
        return &*dec;
    }
    return nullptr;
}

static inline void small_memset(void *dest, int c, size_t n)
{
    size_t qty = n / 8;
    unsigned long *to_8 = (unsigned long *)dest;

    while (qty--) {
        *to_8++ = (uint8_t)c * 0x0101010101010101ull;
    }

    qty = n % 8;
    unsigned int *to_4 = (unsigned int *)to_8;

    if (qty / 4) {
        *to_4++ = (uint8_t)c * 0x01010101ul;
    }

    qty = qty % 4;
    unsigned short *to_2 = (unsigned short *)to_4;
    if (qty / 2) {
        *to_2++ = (uint8_t)c * 0x0101ul;
    }

    unsigned char *to = (unsigned char *)to_2;
    if (qty % 2) {
        *to++ = (uint8_t)c;
    }
}

extern "C"
void *memset_repstos_old(void *__restrict dest, int c, size_t n)
{
    auto ret = dest;
    if (n <= 64) {
        small_memset(dest, c, n);
    }
    else {
        auto nw = n / 8;
        auto nb = n & 7;
        auto cw = (uint8_t)c * 0x0101010101010101ull;
        asm volatile("rep stosq" : "+D"(dest), "+c"(nw) : "a"(cw) : "memory");
        asm volatile("rep stosb" : "+D"(dest), "+c"(nb) : "a"(cw) : "memory");
    }
    return ret;
}

extern "C"
void *memset_repstosb(void *__restrict dest, int c, size_t n)
{
    auto ret = dest;
    if (n <= 64) {
        small_memset(dest, c, n);
    } else {
        asm volatile("rep stosb" : "+D"(dest), "+c"(n) : "a"(c) : "memory");
    }
    return ret;
}

extern "C"
void *(*resolve_memset())(void *__restrict dest, int c, size_t n)
{
    if (processor::features().repmovsb) {
        return memset_repstosb;
    }
    return memset_repstos_old;
}

void *memset(void *__restrict dest, int c, size_t n)
    __attribute__((ifunc("resolve_memset")));


