/*
 * Copyright 2024 Benjamin Byholm
 *
 * Permission is hereby granted, free of charge, to any person obtaining a copy
 * of this software and associated documentation files (the “Software”), to deal
 * in the Software without restriction, including without limitation the rights
 * to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
 * copies of the Software, and to permit persons to whom the Software is
 * furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in
 * all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED “AS IS”, WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 * AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 * OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
 * SOFTWARE.
 */

#ifndef _POSIX_C_SOURCE
#define _POSIX_C_SOURCE 200112L
#endif

#include <assert.h>
#include <errno.h>
#include <limits.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#ifndef __has_builtin
#define __has_builtin(x) 0
#endif

#if !(__has_builtin(__builtin_expect) || defined(__builtin_expect))
#define __builtin_expect(exp, c) (exp)
#endif

#if !(defined(LIKELY) || defined(UNLIKELY))
#define LIKELY(exp) __builtin_expect(!!(exp), 1)
#define UNLIKELY(exp) __builtin_expect(!!(exp), 0)
#endif

#ifdef __clang__
#define CLANG_WI_PS                             \
	_Pragma("GCC diagnostic push") _Pragma( \
		"GCC diagnostic ignored \"-Wsometimes-uninitialized\"")
#define CLANG_WI_PP _Pragma("GCC diagnostic pop")
#define CLANG_WI(x) CLANG_WI_PS(x) CLANG_WI_PP
#else
#define CLANG_WI_PS
#define CLANG_WI_PP
#define CLANG_WI(x) (x)
#endif

#ifndef __clang__
#define GCC_WI_PS                      \
	_Pragma("GCC diagnostic push") \
		_Pragma("GCC diagnostic ignored \"-Wmaybe-uninitialized\"")
#define GCC_WI_PP _Pragma("GCC diagnostic pop")
#define GCC_WI(x)                    \
	__extension__({              \
		GCC_WI_PS;           \
		typeof(x) res = (x); \
		GCC_WI_PP;           \
		res;                 \
	})
#else
#define GCC_WI_PS
#define GCC_WI_PP
#define GCC_WI(x) (x)
#endif

#ifdef __INTEL_COMPILER
#define ICX_WI_PS _Pragma("warning push") _Pragma("warning disable 3656")
#define ICX_WI_PP _Pragma("warning pop")
#define ICX_WI(x) ICX_WI_PS(x) ICX_WI_PP
#else
#define ICX_WI_PS
#define ICX_WI_PP
#define ICX_WI(x) (x)
#endif

#define ASIZE(x) (sizeof(x) / sizeof *(x))

/*
 * the size of a ring must be a power of 2 less than SIZE_MAX & (SIZE_MAX >> 1)
 */
#define RING_SIZE ((size_t)1 << 8)
static_assert(RING_SIZE <= (SIZE_MAX >> 1), "too large RING_SIZE");
static_assert(!(RING_SIZE & (RING_SIZE - 1)), "invalid RING_SIZE");

/*
 * rotate left one bit
 */
static inline uint32_t rol(uint32_t v)
{
	return (v << 1) | (v >> 31);
}

/*
 * a ring buffer defined by a head hd and a tail tl
 */
struct ring {
	size_t hd;
	size_t tl;
};

/*
 * used space
 */
static inline size_t ring_size(const struct ring *r)
{
	return r->hd - r->tl;
}

/*
 * free space
 */
static inline size_t ring_capacity(const struct ring *r)
{
	return RING_SIZE - ring_size(r);
}

/*
 * mask an index for a ring
 */
static inline size_t ring_mask(size_t val)
{
	return val & ((RING_SIZE << 1) - 1);
}

/*
 * contiguous free space
 */
static inline size_t ring_run(const struct ring *r)
{
	return (RING_SIZE << 1) - ring_mask(r->hd);
}

/*
 * lz77 sliding window implemented as two consecutive ring buffers,
 * dictionary and lookahead, which enables searching across both
 * buffers and efficient window maintenance with minimal copying
 */
struct wnd {
	struct ring dictionary;
	struct ring lookahead;
	uint8_t bf[RING_SIZE << 1];
};

/*
 * initialize lz77 sliding window w
 */
static inline void wnd_init(struct wnd *w)
{
	w->dictionary = (struct ring){ 0 };
	w->lookahead = (struct ring){ 0 };
}

/*
 * shifts data from the lookahead buffer to the dictionary buffer,
 * overwriting old data in the dictionary buffer as needed
 */
static inline void wnd_shift(struct wnd *w, size_t n)
{
	const size_t c = ring_capacity(&w->dictionary);

	w->dictionary.hd += n;
	w->dictionary.tl += n > c ? n - c : 0;
	w->lookahead.tl += n;
}

/*
 * reads data from i into the lookahead buffer up to its capacity
 */
static int wnd_read(struct wnd *w, FILE *i)
{
	for (;;) {
		const size_t r = ring_run(&w->lookahead);
		const size_t c = ring_capacity(&w->lookahead);
		const size_t u = c > r ? r : c;
		size_t n;

		n = fread(w->bf + ring_mask(w->lookahead.hd), 1, u, i);
		w->lookahead.hd += n;
		if (UNLIKELY(n != u)) {
			if (LIKELY(feof(i)))
				return EOF;
			if (UNLIKELY(ferror(i)))
				return errno;
			continue;
		}
		if (LIKELY(c == u))
			break;
	};

	return 0;
}

/*
 * initialize t[i] with the longest prefix of the lookahead buffer bf[0:i]
 * that is also a suffix of bf[0:i]
 */
static void kmp_init(uint8_t *restrict t, const struct wnd *w)
{
	size_t i = w->lookahead.tl;
	size_t j = i + 1;

	if (UNLIKELY(ring_size(&w->lookahead) < 2))
		return;

	t[0] = 0;

	do {
		if (UNLIKELY(w->bf[ring_mask(i)] == w->bf[ring_mask(j)]))
			t[j++ - w->lookahead.tl] = ++i - w->lookahead.tl;
		else if (LIKELY(i == w->lookahead.tl))
			t[j++ - w->lookahead.tl] = 0;
		else
			i = w->lookahead.tl + t[i - w->lookahead.tl - 1];
	} while (LIKELY(j != w->lookahead.hd));
}

/*
 * a pair of offset o and length l for a result of kmp_search
 */
struct pair {
	size_t o;
	size_t l;
};

/*
 * search for the longest partial match of the lookahead buffer found in the
 * dictionary buffer, allowing overlap of matches into the lookahead buffer
 */
static struct pair kmp_search(const struct wnd *w)
{
	uint8_t a[RING_SIZE];
	struct pair p = { 0 };

	kmp_init(a, w);

	size_t i = w->lookahead.tl;
	size_t j = w->dictionary.tl;

	while (LIKELY(j != w->lookahead.hd)) {
		const size_t l = i - w->lookahead.tl;
		const size_t o = j - w->dictionary.tl - l;

		if (UNLIKELY(o == ring_size(&w->dictionary)))
			break;
		if (UNLIKELY(w->bf[ring_mask(i)] == w->bf[ring_mask(j)])) {
			if (++j, UNLIKELY(++i == w->lookahead.hd))
				return (struct pair){ o, l + 1 };
		} else if (LIKELY(i == w->lookahead.tl))
			++j;
		else if (i = w->lookahead.tl + a[l - 1], UNLIKELY(l > p.l))
			p = (struct pair){ o, l };
	}

	return p;
}

/*
 * a match in the dictionary buffer with offset o and length l,
 * or the raw byte v if l == 0
 */
struct match {
	union {
		uint8_t o;
		uint8_t v;
	};
	uint8_t l;
};

/*
 * match the lookahead buffer to the dictionary buffer
 */
static struct match match(struct wnd *w)
{
	struct match m;
	struct pair p = kmp_search(w);
	const size_t tl = w->lookahead.tl;

	/* not worth encoding */
	if (UNLIKELY(
		    p.l < 2 ||
		    p.l == 2 && ring_size(&w->lookahead) > 3 &&
			    w->bf[ring_mask(tl + 2)] == w->bf[ring_mask(tl)] &&
			    (w->bf[ring_mask(tl + 3)] == w->bf[ring_mask(tl)] ||
			     w->bf[ring_mask(tl + 3)] ==
				     w->bf[ring_mask(w->dictionary.tl + p.l)]))) {
		m.v = w->bf[ring_mask(tl)];
		m.l = 0;

		wnd_shift(w, 1);

		return m;
	}

	m.o = (uint8_t)(ring_size(&w->dictionary) - p.o - 1);
	m.l = (uint8_t)p.l - 1;

	wnd_shift(w, p.l);

	return m;
}

/*
 * encode n matches with control byte c to file o
 */
static int encode(struct match *restrict m, unsigned n, uint32_t c, FILE *o)
{
	unsigned j = 0;

	/* emit the control byte */
	if (UNLIKELY(putc_unlocked((int)c, o) < 0))
		goto fail;

	do {
		if (LIKELY(!m[j].l)) { /* raw byte */
			if (UNLIKELY(putc_unlocked((int)m[j].v, o) < 0))
				goto fail;
		} else { /* back reference */
			if (UNLIKELY(putc_unlocked((int)m[j].o, o) < 0))
				goto fail;
			if (UNLIKELY(putc_unlocked((int)m[j].l, o) < 0))
				goto fail;
		}
	} while (LIKELY(++j != n));

	return 0;
fail:
	if (UNLIKELY(!ferror(o)))
		errno = EIO;
	return errno;
}

/*
 * the compression context
 */
struct ctx {
	unsigned n;
	uint32_t c;
	uint32_t msk;
	struct wnd w;
	struct match m[CHAR_BIT];
};

/*
 * initialize the compression context ctx
 */
static inline void ctx_init(struct ctx *ctx)
{
	wnd_init(&ctx->w);
	ctx->msk = (1 << 31) | (1 << 23) | (1 << 15) | (1 << 7);
}

/*
 * compresses the current segment of the lookahed buffer to file o
 */
static int compress_helper(struct ctx *ctx, FILE *o)
{
	if (UNLIKELY((ctx->msk = rol(ctx->msk)) & 1)) {
		if (LIKELY(ctx->n)) {
			int ret;

			if (UNLIKELY((ret = encode(ctx->m, ctx->n, ctx->c, o))))
				return ret;
			ctx->n = 0;
		}
		ctx->c = 0;
	}

	ctx->m[ctx->n] = match(&ctx->w);

	if (LIKELY(ctx->m[ctx->n].l))
		ctx->c |= ctx->msk;

	return 0;
}

/*
 * compress file i to file o until EOF
 */
static int compress(FILE *i, FILE *o)
{
	struct ctx ctx;
	int ret;

	ctx_init(&ctx);

	/* read data from i and compress it */
	for (ctx.n = 0; LIKELY(!(ret = wnd_read(&ctx.w, i))); ++ctx.n)
		if (UNLIKELY(ret = compress_helper(&ctx, o)))
			return ret;

	if (UNLIKELY(ret != EOF))
		return ret;

	/* compress the remaining data in the lookahed buffer */
	for (; LIKELY(ring_size(&ctx.w.lookahead)); ++ctx.n)
		if (UNLIKELY(ret = compress_helper(&ctx, o)))
			return ret;

	/* encode the last remaining bytes */
	if (LIKELY(ctx.n))
		return encode(ctx.m, ctx.n, ctx.c, o);

	return 0;
}

/*
 * decompress file i to file o until EOF
 */
static int decompress(FILE *i, FILE *o)
{
	uint8_t buf[RING_SIZE];
	struct match m = { 0 };
	register uint32_t map;
	register uint32_t msk = (1 << 31) | (1 << 23) | (1 << 15) | (1 << 7);
	register int c;

	while (LIKELY((c = getc_unlocked(i)) >= 0)) {
		if (CLANG_WI(UNLIKELY((msk = rol(msk)) & 1)))
			if (map = c, UNLIKELY((c = getc_unlocked(i)) < 0))
				goto readfail;
		if (GCC_WI(UNLIKELY(map & msk))) {
			if (m.l = c + 1, UNLIKELY((c = getc_unlocked(i)) < 0))
				goto readfail;
			do {
				buf[m.o] = ICX_WI(buf[(uint8_t)(m.o - m.l)]);
				if (UNLIKELY(putc_unlocked(buf[m.o++], o) < 0))
					goto writefail;
			} while (LIKELY(c--));
		} else if (buf[m.o++] = c, UNLIKELY(putc_unlocked(c, o) < 0))
			goto writefail;
	}
	if (LIKELY(feof((o = i))))
		return 0;
writefail:
	i = o;
readfail:
	if (UNLIKELY(!ferror(i)))
		errno = EIO;
	return errno;
}

/*
 * show usage information and return an error
 */
static int usage(const char *restrict name)
{
	fprintf(stderr,
		"Usage:\t\t%s [-d | --decompress]\n\nExample:\t"
		"tar -c archive | %s >archive.tar.lzpi\n\t\t"
		"%s <archive.tar.lzpi | tar -x\n\t\t"
		"%s -d <archive.tar.lzpi >archive.tar\n",
		name, name, name, name);
	return 1;
}

/*
 * match -d or --decompress for the decompression flag
 */
static inline int match_decompress(const char *s)
{
	return !strcmp(s, "-d") || !strcmp(s, "--decompress");
}

/*
 * lzpi
 * accepts an optional -d or --decompress flag for choosing decompression mode
 * reads a file from stdin and writes the processed output to stdout
 * returns errno on error
 */
int main(int argc, char **argv)
{
	int ret;
	const char *name = strrchr(argv[0], '/') + 1;

	if (name == (const char *)1)
		name = argv[0];

	switch (argc) {
	case 1:
		if (UNLIKELY(ret = compress(stdin, stdout)))
			perror(name);
		break;
	case 2:
		if (LIKELY(match_decompress(argv[1]))) {
			if (UNLIKELY(ret = decompress(stdin, stdout)))
				perror(name);
			break;
		} /* fallthrough */
	default:
		ret = usage(name);
	}
	return ret;
}
