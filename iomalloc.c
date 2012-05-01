/*
** This software is in the public domain, furnished "as is", without technical
** support, and with no warranty, express or implied, as to its usefulness for
** any purpose.
**
** IOMalloc - Strict FIFO Memory Allocator
**
** Author: Hagen Paul Pfeifer <hagen.pfeifer@protocollabs.com>
**
*/

#include <stdlib.h>
#include <errno.h>
#include <string.h>
#include <assert.h>
#include <inttypes.h>
#include <stdio.h>
/* for htons() */
#include <netinet/in.h>
/* for CHAR_BITS */
#include <limits.h>

#undef __always_inline
#if __GNUC_PREREQ (3,2)
# define __always_inline __inline __attribute__ ((__always_inline__))
#else
# define __always_inline __inline
#endif

/*
 * See if our compiler is known to support flexible array members.
 */
#ifndef FLEX_ARRAY
#if defined(__STDC_VERSION__) && \
	(__STDC_VERSION__ >= 199901L) && \
	(!defined(__SUNPRO_C) || (__SUNPRO_C > 0x580))
# define FLEX_ARRAY /* empty */
#elif defined(__GNUC__)
# if (__GNUC__ >= 3)
#  define FLEX_ARRAY /* empty */
# else
#  define FLEX_ARRAY 0 /* older GNU extension */
# endif
#endif
#ifndef FLEX_ARRAY
# define FLEX_ARRAY 1
#endif
#endif

#define min(x,y) ({             \
        typeof(x) _x = (x);     \
        typeof(y) _y = (y);     \
        (void) (&_x == &_y);    \
        _x < _y ? _x : _y; })

#define max(x,y) ({             \
        typeof(x) _x = (x);     \
        typeof(y) _y = (y);     \
        (void) (&_x == &_y);    \
        _x > _y ? _x : _y; })

/* determine the size of an array */
#define ARRAY_SIZE(x) (sizeof(x) / sizeof((x)[0]))
#define BITSIZEOF(x)  (CHAR_BIT * sizeof(x))

/* iom_init() flags */
#define	IOM_MAINLY_EMPTY 0x0

/* iom_push() flags */
#define IOM_HEAD_DROP          0x0
#define	IOM_TAIL_DROP          0x1
#define	IOM_DROP_ALL           0x2

/*
 * Implemented as continues chunk to avoid memory
 * dereferences when queue never fills.
 */
struct iom_buffer {
	unsigned int size;
	/* buf index: no pointer, save 8 byte on some arch's */
	int tail;
	int head;
	unsigned char buf[FLEX_ARRAY];
};

union encoder_cookie {
	uint8_t s[2];
	/* big endian for full encoding */
	uint16_t l;
};

enum {
	MODE_SPLITTED,
	MODE_CONTINUES,
};


unsigned int iom_cnt(struct iom_buffer *iom_buffer)
{
	return (iom_buffer->head - iom_buffer->tail) & (iom_buffer->size - 1);
}


static unsigned int iom_space(struct iom_buffer *iom_buffer)
{
	return (iom_buffer->tail - (iom_buffer->head + 1)) & (iom_buffer->size - 1);
}


unsigned int iom_cnt_to_end(struct iom_buffer *iom_buffer)
{
	int n, end = iom_buffer->size - iom_buffer->tail;
	n = (iom_buffer->head + end) & (iom_buffer->size - 1);
	return n < end ? n : end;
}


static int iom_tail_to_end(struct iom_buffer *iom_buffer)
{
	return iom_buffer->size - iom_buffer->tail;
}

static unsigned int iom_space_to_bound(struct iom_buffer *iom_buffer)
{
	return iom_buffer->size - iom_buffer->head;
}

unsigned int iom_space_to_end(struct iom_buffer *iom_buffer)
{
	int n, end = iom_buffer->size - 1 - iom_buffer->tail;
	n = (end + iom_buffer->tail) & (iom_buffer->size - 1);
	return n < end ? n : end;
}


static void iom_head_inc(struct iom_buffer *iom_buffer, int len)
{
	iom_buffer->head = (iom_buffer->head + len) & (iom_buffer->size - 1);
}


void iom_tail_inc(struct iom_buffer *iom_buffer, int len)
{
	iom_buffer->tail = (iom_buffer->tail + len) & (iom_buffer->size - 1);
}


int continues_chunk_fast(struct iom_buffer *iom_buffer, size_t size)
{
	return !!(iom_buffer->size - iom_buffer->head > size);
}


int iom_init(size_t size, struct iom_buffer **iom_buffer, unsigned flags)
{
	struct iom_buffer *iomb;

	assert(size);
	assert((size & (size - 1)) == 0);

	/* unused yet */
	(void) flags;

	iomb = malloc(sizeof(*iomb) + size);
	if (!iomb)
		return ENOBUFS;

	iomb->tail = iomb->head = 0;
	iomb->size = size;

	*iom_buffer = iomb;

	return 0;
}


void iom_free(struct iom_buffer *iom_buffer)
{
	assert(iom_buffer);
	free(iom_buffer);
}


static int push_mode(struct iom_buffer *iom_buffer, int len)
{
	int byte_till_end = iom_space_to_bound(iom_buffer) + 1;

	if (len + (int)sizeof(union encoder_cookie) > byte_till_end)
		return MODE_SPLITTED;

	return MODE_CONTINUES;
}


static __always_inline void iom_add_fast(struct iom_buffer *iom_buffer,
		                         unsigned char *buf, int len)
{
	union encoder_cookie cookie;
	cookie.l = htons((short)len);

	iom_buffer->buf[iom_buffer->head] = cookie.s[0];
	iom_buffer->buf[iom_buffer->head + 1] = cookie.s[1];
	fprintf(stderr, "cpy: %d hdr: %d size: %d space: %d\n",
			len, iom_buffer->head, iom_buffer->size, iom_space_to_bound(iom_buffer));
	memcpy(&iom_buffer->buf[iom_buffer->head + 2], buf, len);
	iom_head_inc(iom_buffer, len + 2);
}


static void iom_add_slow(struct iom_buffer *iom_buffer,
		         unsigned char *buf, int len)
{
	union encoder_cookie cookie;
	int byte_till_end, remaining;

	cookie.l = htons((short)len);
	byte_till_end = iom_space_to_bound(iom_buffer);

	switch (byte_till_end) {
	case 0:
		iom_buffer->buf[0] = cookie.s[0];
		iom_buffer->buf[1] = cookie.s[1];
		memcpy(&iom_buffer->buf[2], buf, len);
		break;
	case 1:
		iom_buffer->buf[iom_buffer->head] = cookie.s[0];
		iom_buffer->buf[0]                = cookie.s[1];
		memcpy(&iom_buffer->buf[1], buf, len);
		break;
	case 2:
		iom_buffer->buf[iom_buffer->head]     = cookie.s[0];
		iom_buffer->buf[iom_buffer->head + 1] = cookie.s[1];
		memcpy(&iom_buffer->buf[0], buf, len);
		break;
	default:
		iom_buffer->buf[iom_buffer->head]     = cookie.s[0];
		iom_buffer->buf[iom_buffer->head + 1] = cookie.s[1];
		remaining = (byte_till_end - sizeof(cookie));
		memcpy(&iom_buffer->buf[iom_buffer->head + 2], buf, remaining);
		memcpy(&iom_buffer->buf[0], &buf[remaining], len - remaining);
		break;
	}

	iom_head_inc(iom_buffer, len + sizeof(cookie));
}


static int purge_next(struct iom_buffer *iom_buffer)
{
	union encoder_cookie cookie;
	int tail_to_end, encoded_len;
	size_t sc = sizeof(union encoder_cookie);

	tail_to_end = iom_tail_to_end(iom_buffer);
	switch (tail_to_end) {
	case 1:
		cookie.s[0] = iom_buffer->buf[iom_buffer->tail];
		cookie.s[1] = iom_buffer->buf[0];
		encoded_len = ntohs(cookie.l);
		break;
	case 2:
		cookie.s[0] = iom_buffer->buf[iom_buffer->tail];
		cookie.s[1] = iom_buffer->buf[iom_buffer->tail + 1];
		encoded_len = ntohs(cookie.l);
		break;
	default:
		cookie.s[0] = iom_buffer->buf[iom_buffer->tail];
		cookie.s[1] = iom_buffer->buf[iom_buffer->tail + 1];
		encoded_len = ntohs(cookie.l);
		break;
	}

	iom_tail_inc(iom_buffer, encoded_len + sc);

	return 0;
}


static int enforce_buf_policy(struct iom_buffer *iom_buffer,
		              size_t len, int flags)
{
	int ret;
	const size_t sc = sizeof(union encoder_cookie);

	switch (flags) {
	case IOM_TAIL_DROP:
		if (iom_space(iom_buffer) < len + sc)
			return ENOBUFS;
		break;
	case IOM_HEAD_DROP:
		while (iom_space(iom_buffer) < len + sc) {
			ret = purge_next(iom_buffer);
			if (ret)
				return ret;
		};
		break;
	case IOM_DROP_ALL:
		iom_buffer->tail = iom_buffer->head = 0;
		break;
	default:
		return ENOTSUP;
		break;
	}

	return 0;
}


int iom_push(struct iom_buffer *iom_buffer, unsigned char *buf,
	     size_t len, int flags)
{
	int ret;
	const size_t sc = sizeof(union encoder_cookie);

	assert(iom_buffer);

	if (iom_buffer->size < len + sc)
		return EINVAL;

	ret = enforce_buf_policy(iom_buffer, len, flags);
	if (ret) /* failure or out of memory */
		return ret;

	switch (push_mode(iom_buffer, len + sc)) {
	case MODE_CONTINUES:
		iom_add_fast(iom_buffer, buf, len);
		break;
	case MODE_SPLITTED:
		iom_add_slow(iom_buffer, buf, len);
		break;
	default:
		assert(0);
		break;
	}

	return 0;
}


int iom_shift(struct iom_buffer *iom_buffer, unsigned char *buf,
	     int *buf_len, int max_size)
{
	int tail_to_end, encoded_len, remaining;
	union encoder_cookie cookie;
	size_t sc = sizeof(union encoder_cookie);

	assert(iom_buffer);
	assert(max_size);
	assert(buf_len);

	if (!iom_cnt(iom_buffer))
		return EINVAL;

	tail_to_end = iom_tail_to_end(iom_buffer);
	switch (tail_to_end) {
	case 1:
		cookie.s[0] = iom_buffer->buf[iom_buffer->tail];
		cookie.s[1] = iom_buffer->buf[0];
		encoded_len = ntohs(cookie.l);
		if (encoded_len > max_size) return ENOBUFS;
		memcpy(buf, &iom_buffer->buf[1], encoded_len);
		break;
	case 2:
		cookie.s[0] = iom_buffer->buf[iom_buffer->tail];
		cookie.s[1] = iom_buffer->buf[iom_buffer->tail + 1];
		encoded_len = ntohs(cookie.l);
		if (encoded_len > max_size) return ENOBUFS;
		memcpy(buf, &iom_buffer->buf[0], encoded_len);
		break;
	default:
		cookie.s[0] = iom_buffer->buf[iom_buffer->tail];
		cookie.s[1] = iom_buffer->buf[iom_buffer->tail + 1];
		encoded_len = ntohs(cookie.l);
		if (encoded_len > max_size) return ENOBUFS;
		if (tail_to_end >= encoded_len) {
			memcpy(buf, &iom_buffer->buf[iom_buffer->tail + sc],
			       encoded_len);
		} else {
			remaining = encoded_len - (tail_to_end - sc);
			memcpy(buf, &iom_buffer->buf[iom_buffer->tail + sc],
			       tail_to_end - sc);
			memcpy(&buf[tail_to_end -  sc], iom_buffer->buf,
			       remaining);
		}
		break;
	}

	*buf_len = encoded_len;
	iom_tail_inc(iom_buffer, encoded_len + sc);

	/* reset to 0 if to keep memory reference local */
	if (iom_space(iom_buffer) == iom_buffer->size - 1) {
		//iom_buffer->tail = iom_buffer->head = 0;
		;
	}

	return 0;
}

int nearest_power_two(int k)
{
	int i;

	k--;

	for (i = 1; i < (int)sizeof(int) * CHAR_BIT; i <<= 1)
		k = k | k >> i;
	return k + 1;
}


#if defined(TEST_BUILD)
#include <time.h>

int space_test(void)
{
	int ret, i, j, rbuf_len;
	struct iom_buffer *iom_buffer;
	unsigned char buf[2048] = { 0 };
	unsigned char rbuf[2048];
	size_t size = 16;

	ret = iom_init(size, &iom_buffer, 0);
	if (ret) {
		fputs("Cannot allocate iom_buffer\n", stderr);
		return EXIT_FAILURE;
	}

	srand(time(NULL));

	i = 1000;
	while (i--) {
		int iter, cnt = (rand() % 10) + 1;
		iter = (rand() % 100) + 1;
		fprintf(stderr, "push %d\n", cnt);

		for (j = 0; j < iter; j++) {
			ret = iom_push(iom_buffer, buf, cnt, IOM_TAIL_DROP);
			if (ret) {
				if (ret == ENOBUFS) {
					/* reach limit with IOM_TAIL_DROP */
					iter = j;
					break;
				}
				fprintf(stderr, "ERROR (ret: %d)\n", ret);
				return EXIT_FAILURE;
			}
			fprintf(stderr, "  push iteration: %d\n", j);
		}

		for (j = 0; j < iter; j++) {
			ret = iom_shift(iom_buffer, rbuf, &rbuf_len, sizeof(rbuf));
			if (ret) {
				fprintf(stderr, "Failed to get buffer (%d)\n", ret);
				return EXIT_FAILURE;
			}
			fprintf(stderr, "  shift iteration: %d, %d byte\n", j, rbuf_len);

			assert(rbuf_len == cnt);
		}

		fprintf(stderr, "rbuf: %d\n", rbuf_len);
		if (iom_space(iom_buffer) != size - 1) {
			fprintf(stderr, "wrong capacity %d\n", iom_space(iom_buffer));
			assert(0);
		}

		fputs("\n\n", stderr);
	}

	iom_free(iom_buffer);

	return 0;
}

int space_test2(void)
{
	int ret, i, j, rbuf_len;
	struct iom_buffer *iom_buffer;
	unsigned char buf[2048] = { 0 };
	unsigned char rbuf[2048];
	size_t size = nearest_power_two(32000);

	ret = iom_init(size, &iom_buffer, 0);
	if (ret) {
		fputs("Cannot allocate iom_buffer\n", stderr);
		return EXIT_FAILURE;
	}

	i = 1000;
	while (i--) {
		int iter, cnt = (rand() % 1500) + 1;
		iter = (rand() % 100) + 1;
		fprintf(stderr, "push %d byte\n", cnt);

		for (j = 0; j < iter; j++) {
			ret = iom_push(iom_buffer, buf, cnt, IOM_TAIL_DROP);
			if (ret) {
				if (ret == ENOBUFS) {
					/* reach limit with IOM_TAIL_DROP */
					iter = j;
					break;
				}
				fprintf(stderr, "ERROR (ret: %d)\n", ret);
				return EXIT_FAILURE;
			}
			fprintf(stderr, "  push iteration: %d\n", j);
		}

		ret = iom_shift(iom_buffer, rbuf, &rbuf_len, sizeof(rbuf));
		if (ret) {
			fprintf(stderr, "Failed to get buffer (%d)\n", ret);
			return EXIT_FAILURE;
		}
		fprintf(stderr, "  shift iteration: %d, %d byte\n", j, rbuf_len);

		ret = iom_push(iom_buffer, buf, cnt, IOM_TAIL_DROP);
		if (ret) {
			if (ret == ENOBUFS) {
				/* reach limit with IOM_TAIL_DROP */
				iter = j;
				break;
			}
			fprintf(stderr, "ERROR (ret: %d)\n", ret);
			return EXIT_FAILURE;
		}
		fprintf(stderr, "  push iteration: %d\n", j);

		for (j = 0; j < iter; j++) {
			ret = iom_shift(iom_buffer, rbuf, &rbuf_len, sizeof(rbuf));
			if (ret) {
				fprintf(stderr, "Failed to get buffer (%d)\n", ret);
				return EXIT_FAILURE;
			}
			fprintf(stderr, "  shift iteration: %d, %d byte\n", j, rbuf_len);

			assert(rbuf_len == cnt);
		}

		fprintf(stderr, "rbuf: %d\n", rbuf_len);
		if (iom_space(iom_buffer) != size - 1) {
			fprintf(stderr, "wrong capacity %d\n", iom_space(iom_buffer));
			assert(0);
		}

		fputs("\n\n", stderr);
	}

	iom_free(iom_buffer);

	return 0;
}

int main(void)
{
	int ret;

	ret = space_test();
	if (ret) {
		fprintf(stderr, "space test failed\n");
		return EXIT_FAILURE;
	}

	ret = space_test2();
	if (ret) {
		fprintf(stderr, "space test failed\n");
		return EXIT_FAILURE;
	}


	return EXIT_SUCCESS;
}

#endif /* TEST_BUILD */
