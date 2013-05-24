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
	unsigned int chunks;
	/* buf index: no pointer, save 8 byte on some arch's */
	int tail;
	int head;
	unsigned char buf[FLEX_ARRAY];
};

struct iom_iterator {
	int tail;
	int head;
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


static unsigned int iom_cnt_int(int head, int tail, unsigned int size)
{
	return (head - tail) & (size - 1);
}


unsigned int iom_cnt(struct iom_buffer *iom_buffer)
{
	return iom_cnt_int(iom_buffer->head, iom_buffer->tail, iom_buffer->size);
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


unsigned int iom_chunks(struct iom_buffer *iom_buffer)
{
	return iom_buffer->chunks;
}


static int iom_tail_to_end_int(unsigned int size, int tail)
{
	return size - tail;
}


static int iom_tail_to_end(struct iom_buffer *iom_buffer)
{
	return iom_tail_to_end_int(iom_buffer->size, iom_buffer->tail);
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


static int iom_tail_inc_int(int tail, unsigned int size, int len)
{
	return (tail + len) & (size - 1);
}


void iom_tail_inc(struct iom_buffer *iom_buffer, int len)
{
	iom_buffer->tail = iom_tail_inc_int(iom_buffer->tail, iom_buffer->size, len);
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

	/*
	 * Flags are not supported in this version,
	 * make this API future proof and check flag
	 * argument
	 */
	if (flags != 0)
		return EINVAL;

	if (size == 0)
		return EINVAL;

	iomb = malloc(sizeof(*iomb) + size);
	if (!iomb)
		return ENOBUFS;

	iomb->tail   = iomb->head = 0;
	iomb->size   = size;
	iomb->chunks = 0;

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


static void purge_next(struct iom_buffer *iom_buffer)
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
}


static int enforce_buf_policy(struct iom_buffer *iom_buffer,
		              size_t len, int flags)
{
	const size_t sc = sizeof(union encoder_cookie);

	switch (flags) {
	case IOM_TAIL_DROP:
		if (iom_space(iom_buffer) < len + sc)
			return ENOBUFS;
		break;
	case IOM_HEAD_DROP:
		while (iom_space(iom_buffer) < len + sc) {
			purge_next(iom_buffer);
			iom_buffer->chunks--;
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

	iom_buffer->chunks++;

	return 0;
}

/*
 * If ring is empty iom_shift return EINVAL, arguments are untouched.
 */
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
		if (tail_to_end - (int)sc >= encoded_len) {
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
	iom_buffer->chunks--;

	/* reset to 0 if to keep memory reference local */
	if (iom_space(iom_buffer) == iom_buffer->size - 1) {
		//iom_buffer->tail = iom_buffer->head = 0;
		;
	}

	return 0;
}


int iom_peek(struct iom_buffer *iom_buffer, unsigned char *buf,
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

	return 0;
}


/*
 * iom_peek_update() can be used after iom_peek() to
 * remove the still remaining chunk from the buffer.
 * A iom_peek() followed by a iom_peek_update() is
 * functional identical to iom_shift().
 *
 * This function return 0 for success or any other value
 * to indicate an error.
 *
 * o EINVAL indicates that no chunk was saved.
 */
int iom_peek_update(struct iom_buffer *iom_buffer)
{
	int tail_to_end, encoded_len;
	union encoder_cookie cookie;
	size_t sc = sizeof(union encoder_cookie);

	assert(iom_buffer);

	if (!iom_cnt(iom_buffer))
		return EINVAL;

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
	iom_buffer->chunks--;

	/* reset to 0 if to keep memory reference local */
	if (iom_space(iom_buffer) == iom_buffer->size - 1) {
		//iom_buffer->tail = iom_buffer->head = 0;
		;
	}

	return 0;
}

struct iom_iterator *iom_iterator_new(struct iom_buffer *iom_buffer)
{
	struct iom_iterator *iom_iterator;

	iom_iterator = malloc(sizeof(*iom_iterator));
	if (!iom_iterator)
		return NULL;

	memset(iom_iterator, 0, sizeof(*iom_iterator));
	iom_iterator->head = iom_buffer->head;
	iom_iterator->tail = iom_buffer->tail;

	return iom_iterator;
}


void iom_iterator_free(struct iom_iterator *iom_iterator)
{
	free(iom_iterator);
}


int iom_iterator_peek_next(struct iom_iterator *iom_iterator,
			   struct iom_buffer *iom_buffer,
			   unsigned char *buf, int *buf_len, int max_size)
{
	int tail_to_end, encoded_len, remaining;
	union encoder_cookie cookie;
	size_t sc = sizeof(union encoder_cookie);

	assert(iom_buffer);
	assert(max_size);
	assert(buf_len);

	if (!iom_cnt_int(iom_iterator->head, iom_iterator->tail, iom_buffer->size))
		return EINVAL;

	tail_to_end = iom_tail_to_end_int(iom_buffer->size, iom_iterator->tail);
	switch (tail_to_end) {
	case 1:
		cookie.s[0] = iom_buffer->buf[iom_iterator->tail];
		cookie.s[1] = iom_buffer->buf[0];
		encoded_len = ntohs(cookie.l);
		if (encoded_len > max_size) return ENOBUFS;
		memcpy(buf, &iom_buffer->buf[1], encoded_len);
		break;
	case 2:
		cookie.s[0] = iom_buffer->buf[iom_iterator->tail];
		cookie.s[1] = iom_buffer->buf[iom_iterator->tail + 1];
		encoded_len = ntohs(cookie.l);
		if (encoded_len > max_size) return ENOBUFS;
		memcpy(buf, &iom_buffer->buf[0], encoded_len);
		break;
	default:
		cookie.s[0] = iom_buffer->buf[iom_iterator->tail];
		cookie.s[1] = iom_buffer->buf[iom_iterator->tail + 1];
		encoded_len = ntohs(cookie.l);
		if (encoded_len > max_size) return ENOBUFS;
		if (tail_to_end >= encoded_len) {
			memcpy(buf, &iom_buffer->buf[iom_iterator->tail + sc],
			       encoded_len);
		} else {
			remaining = encoded_len - (tail_to_end - sc);
			memcpy(buf, &iom_buffer->buf[iom_iterator->tail + sc],
			       tail_to_end - sc);
			memcpy(&buf[tail_to_end -  sc], iom_buffer->buf,
			       remaining);
		}
		break;
	}

	*buf_len = encoded_len;
	iom_iterator->tail = iom_tail_inc_int(iom_iterator->tail,
					      iom_buffer->size,
					      encoded_len + sc);

	return 0;
}


size_t iom_nearest_power_two(size_t k)
{
	size_t i;

	if (k == 1) return 2;
	k--;
	for (i = 1; i < sizeof(size_t) * CHAR_BIT; i <<= 1)
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
	size_t size = iom_nearest_power_two(32000);

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


int space_test3(void)
{
	int ret;
	struct iom_buffer *iom_buffer;
	unsigned char buf;

	ret = iom_init(8, &iom_buffer, 0);
	if (ret) {
		fputs("Cannot allocate iom_buffer\n", stderr);
		return EXIT_FAILURE;
	}

	buf = 0;
	ret = iom_push(iom_buffer, &buf, sizeof(buf), IOM_TAIL_DROP);
	assert(ret == 0);

	buf = 1;
	ret = iom_push(iom_buffer, &buf, sizeof(buf), IOM_TAIL_DROP);
	assert(ret == 0);

	/* the following test MUST fail! Why, because local
	 * accouning requires 2 byte for each chunk. If we save
	 * 2 x 1byte we require 6 byte. If we want to save another
	 * byte (with additional overhead of 2 bytes), the operation
	 * MUST fail
	 */
	buf = 23;
	ret = iom_push(iom_buffer, &buf, sizeof(buf), IOM_TAIL_DROP);
	assert(ret != 0);

	iom_free(iom_buffer);

	return 0;
}


int space_test4(void)
{
	int ret, rbuf_len;
	struct iom_buffer *iom_buffer;
	unsigned char *buf;
	unsigned char rbuf[3];

	ret = iom_init(8, &iom_buffer, 0);
	if (ret) {
		fputs("Cannot allocate iom_buffer\n", stderr);
		return EXIT_FAILURE;
	}

	buf = (unsigned char *)"AAA";
	ret = iom_push(iom_buffer, buf, 3, IOM_TAIL_DROP);
	assert(ret == 0);

	ret = iom_shift(iom_buffer, rbuf, &rbuf_len, sizeof(rbuf));
	if (ret) {
		fprintf(stderr, "Failed to get buffer (%d)\n", ret);
		return EXIT_FAILURE;
	}

	/* ok, head is now 3 + 2 byte shifted within iom_buffer */

	buf = (unsigned char *)"BB";
	ret = iom_push(iom_buffer, buf, 3, IOM_TAIL_DROP);
	assert(ret == 0);


	ret = iom_shift(iom_buffer, rbuf, &rbuf_len, sizeof(rbuf));
	if (ret) {
		fprintf(stderr, "Failed to get buffer (%d)\n", ret);
		return EXIT_FAILURE;
	}
	assert(rbuf_len == 3);

	iom_free(iom_buffer);

	return 0;
}


int peek_test(void)
{
	int ret;
	struct iom_buffer *iom_buffer;
	size_t size = 8;
	unsigned char data;
	unsigned char rdata;
	int rdata_len;

	ret = iom_init(size, &iom_buffer, 0);
	if (ret) {
		fputs("Cannot allocate iom_buffer\n", stderr);
		return EXIT_FAILURE;
	}

	data = 1;

	ret = iom_push(iom_buffer, &data, sizeof(data), IOM_TAIL_DROP);
	if (ret) {
		fprintf(stderr, "ERROR (ret: %d)\n", ret);
		return EXIT_FAILURE;
	}
	fprintf(stderr, "  push iteration: %d\n", data);

	/* two successive tests, same result MUST be presented */
	ret = iom_peek(iom_buffer, &rdata, &rdata_len, sizeof(rdata));
	if (ret) {
		fprintf(stderr, "Failed to get buffer (%d)\n", ret);
		return EXIT_FAILURE;
	}
	fprintf(stderr, "  shift iteration: %d, %d byte\n", rdata, rdata_len);
	assert(rdata == data);

	ret = iom_peek(iom_buffer, &rdata, &rdata_len, sizeof(rdata));
	if (ret) {
		fprintf(stderr, "Failed to get buffer (%d)\n", ret);
		return EXIT_FAILURE;
	}
	fprintf(stderr, "  shift iteration: %d, %d byte\n", rdata, rdata_len);
	assert(rdata == data);

	/* now new finally remove the peeked element from the buffer */
	ret = iom_peek_update(iom_buffer);
	assert(ret == 0);

	/* this must fail, because no more chunks are in buffer */
	ret = iom_peek_update(iom_buffer);
	assert(ret != 0);

	iom_free(iom_buffer);

	return 0;
}


int iterator_test(void)
{
	int ret;
	struct iom_buffer *iom_buffer;
	struct iom_iterator *iom_iterator;
	size_t size = 8;
	unsigned char data;
	unsigned char rdata;
	int rdata_len;

	ret = iom_init(size, &iom_buffer, 0);
	if (ret) {
		fputs("Cannot allocate iom_buffer\n", stderr);
		return EXIT_FAILURE;
	}

	data = 1;
	ret = iom_push(iom_buffer, &data, sizeof(data), IOM_TAIL_DROP);
	if (ret) {
		fprintf(stderr, "ERROR (ret: %d)\n", ret);
		return EXIT_FAILURE;
	}
	fprintf(stderr, "  push iteration: %d\n", data);

	data = 2;
	ret = iom_push(iom_buffer, &data, sizeof(data), IOM_TAIL_DROP);
	if (ret) {
		fprintf(stderr, "ERROR (ret: %d)\n", ret);
		return EXIT_FAILURE;
	}
	fprintf(stderr, "  push iteration: %d\n", data);

	iom_iterator = iom_iterator_new(iom_buffer);
	assert(iom_iterator);

	ret = iom_iterator_peek_next(iom_iterator, iom_buffer, &rdata,
				     &rdata_len, sizeof(rdata));
	assert(ret == 0);
	assert(rdata_len == sizeof(data));
	assert(rdata == 1);


	ret = iom_iterator_peek_next(iom_iterator, iom_buffer, &rdata,
				     &rdata_len, sizeof(rdata));
	assert(ret == 0);
	assert(rdata_len == sizeof(data));
	assert(rdata == 2);

	ret = iom_iterator_peek_next(iom_iterator, iom_buffer, &rdata,
				     &rdata_len, sizeof(rdata));
	assert(ret != 0);

	iom_iterator_free(iom_iterator);
	iom_free(iom_buffer);

	return 0;
}

int nearest_power_test(void)
{
	assert(0 == iom_nearest_power_two(0));
	assert(4 == iom_nearest_power_two(3));
	assert(4 == iom_nearest_power_two(4));
	assert(8 == iom_nearest_power_two(5));
	assert(8 == iom_nearest_power_two(8));

	return 0;
}


int chunks_number_test(void)
{
	int ret, rbuf_len;
	struct iom_buffer *iom_buffer;
	unsigned char buf;
	unsigned char rbuf[3];

	ret = iom_init(8, &iom_buffer, 0);
	if (ret) {
		fputs("Cannot allocate iom_buffer\n", stderr);
		return EXIT_FAILURE;
	}
	assert(iom_chunks(iom_buffer) == 0);

	buf = 0;
	ret = iom_push(iom_buffer, &buf, sizeof(buf), IOM_TAIL_DROP);
	assert(ret == 0);
	assert(iom_chunks(iom_buffer) == 1);

	buf = 1;
	ret = iom_push(iom_buffer, &buf, sizeof(buf), IOM_TAIL_DROP);
	assert(ret == 0);
	assert(iom_chunks(iom_buffer) == 2);

	ret = iom_shift(iom_buffer, rbuf, &rbuf_len, sizeof(rbuf));
	assert(ret == 0);
	assert(iom_chunks(iom_buffer) == 1);

	ret = iom_shift(iom_buffer, rbuf, &rbuf_len, sizeof(rbuf));
	assert(ret == 0);
	assert(iom_chunks(iom_buffer) == 0);

	/*
	 * following shift operation MUST fail. We test to failed
	 * conditition and additionally test that the chunk size is
	 * still zero
	 */
	ret = iom_shift(iom_buffer, rbuf, &rbuf_len, sizeof(rbuf));
	assert(ret != 0);
	assert(iom_chunks(iom_buffer) == 0);

	iom_free(iom_buffer);

	return 0;
}


#define IOM_BUF_SIZE 16
int size_test(void)
{
	int ret, rbuf_len;
	struct iom_buffer *iom_buffer;
	unsigned char buf[4];
	unsigned char rbuf[4];

	ret = iom_init(IOM_BUF_SIZE, &iom_buffer, 0);
	if (ret) {
		fputs("Cannot allocate iom_buffer\n", stderr);
		return EXIT_FAILURE;
	}
	assert(iom_chunks(iom_buffer) == 0);
	assert(iom_space(iom_buffer) == IOM_BUF_SIZE - 1);


	ret = iom_push(iom_buffer, buf, sizeof(buf), IOM_TAIL_DROP);
	assert(ret == 0);
	assert(iom_space(iom_buffer) == IOM_BUF_SIZE - 1 - sizeof(buf) - 2);


	ret = iom_push(iom_buffer, buf, sizeof(buf), IOM_TAIL_DROP);
	assert(ret == 0);
	assert(iom_space(iom_buffer) == IOM_BUF_SIZE - 1 - (sizeof(buf) + 2) * 2);
	assert(iom_chunks(iom_buffer) == 2);


	ret = iom_shift(iom_buffer, rbuf, &rbuf_len, sizeof(rbuf));
	assert(ret == 0);
	assert(iom_chunks(iom_buffer) == 1);
	assert(iom_space(iom_buffer) == IOM_BUF_SIZE - 1 - sizeof(buf) - 2);


	ret = iom_shift(iom_buffer, rbuf, &rbuf_len, sizeof(rbuf));
	assert(ret == 0);
	assert(iom_chunks(iom_buffer) == 0);
	assert(iom_space(iom_buffer) == IOM_BUF_SIZE - 1);

	/*
	 * following shift operation MUST fail. We test to failed
	 * conditition and additionally test that the chunk size is
	 * still zero
	 */
	ret = iom_shift(iom_buffer, rbuf, &rbuf_len, sizeof(rbuf));
	assert(ret != 0);
	assert(iom_chunks(iom_buffer) == 0);
	assert(iom_space(iom_buffer) == IOM_BUF_SIZE - 1);


	ret = iom_push(iom_buffer, buf, sizeof(buf), IOM_TAIL_DROP);
	assert(ret == 0);
	assert(iom_space(iom_buffer) == IOM_BUF_SIZE - 1 - sizeof(buf) - 2);

	ret = iom_shift(iom_buffer, rbuf, &rbuf_len, sizeof(rbuf));
	assert(ret == 0);
	assert(iom_chunks(iom_buffer) == 0);
	assert(iom_space(iom_buffer) == IOM_BUF_SIZE - 1);

	iom_free(iom_buffer);

	return 0;
}


int chunk_headdrop_test(void)
{
	int ret, i;
	struct iom_buffer *iom_buffer;
	unsigned char buf;

	ret = iom_init(8, &iom_buffer, 0);
	if (ret) {
		fputs("Cannot allocate iom_buffer\n", stderr);
		return EXIT_FAILURE;
	}
	assert(iom_chunks(iom_buffer) == 0);

	buf = 0;
	ret = iom_push(iom_buffer, &buf, sizeof(buf), IOM_HEAD_DROP);
	assert(ret == 0);
	assert(iom_chunks(iom_buffer) == 1);

	i = 10;
	do {
		buf = 1;
		ret = iom_push(iom_buffer, &buf, sizeof(buf), IOM_HEAD_DROP);
		assert(ret == 0);
		assert(iom_chunks(iom_buffer) == 2);
	} while (i--);

	iom_free(iom_buffer);

	return 0;
}


int main(void)
{
	int ret;

	ret = space_test();
	if (ret) {
		fprintf(stderr, "space test 1 failed\n");
		return EXIT_FAILURE;
	}
	fprintf(stderr, "space test 1 passed\n");

	ret = space_test2();
	if (ret) {
		fprintf(stderr, "space test 2 failed\n");
		return EXIT_FAILURE;
	}
	fprintf(stderr, "space test 2 passed\n");

	ret = space_test3();
	if (ret) {
		fprintf(stderr, "space test 3 failed\n");
		return EXIT_FAILURE;
	}
	fprintf(stderr, "space test 3 passed\n");

	ret = space_test4();
	if (ret) {
		fprintf(stderr, "space test 4 failed\n");
		return EXIT_FAILURE;
	}
	fprintf(stderr, "space test 4 passed\n");

	ret = peek_test();
	if (ret) {
		fprintf(stderr, "peek test failed\n");
		return EXIT_FAILURE;
	}
	fprintf(stderr, "peek test passed\n");

	ret = iterator_test();
	if (ret) {
		fprintf(stderr, "iterator test failed\n");
		return EXIT_FAILURE;
	}
	fprintf(stderr, "iterator test passed\n");

	ret = nearest_power_test();
	if (ret) {
		fprintf(stderr, "nearest power test failed\n");
		return EXIT_FAILURE;
	}
	fprintf(stderr, "nearest power of two test passed\n");

	ret = chunks_number_test();
	if (ret) {
		fprintf(stderr, "chunks number test failed\n");
		return EXIT_FAILURE;
	}
	fprintf(stderr, "chunks number test passed\n");

	ret = size_test();
	if (ret) {
		fprintf(stderr, "size test failed\n");
		return EXIT_FAILURE;
	}
	fprintf(stderr, "size test passed\n");

	ret = chunk_headdrop_test();
	if (ret) {
		fprintf(stderr, "chunk headdrop test failed\n");
		return EXIT_FAILURE;
	}
	fprintf(stderr, "chunk headdrop test passed\n");


	return EXIT_SUCCESS;
}

#endif /* TEST_BUILD */
