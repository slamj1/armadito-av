/*
  compile with:
  gcc -g -Iinclude/ -DDO_TEST_DEBUG_MAIN -o brpc buffer.c brpc.c
*/

#include <brpc.h>
#include <stdarg.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>

#include "buffer.h"

/*

     Offset +-----------------------+
          0 | buffer size           |
            +                       +
          1 |                       |
            +-----------------------+
          2 | buffer type           |
            +-----------------------+
          3 | method                |
            +-----------------------+
          4 | id                    |
            +                       +
          5 |                       |
            +                       +
          6 |                       |
            +                       +
          7 |                       |
            +-----------------------+
          8 | arg table entry 0     | arg table entries:
            +                       + an arg table entry contains:
          9 |                       |   * arg type, on 2 bits
            +-----------------------+   * arg offset from the beginning of the buffer, on 14 bits
          A | arg table entry 1     | N is the maximum number of args (currently 16)
            +                       +
          B |                       |
            +-----------------------+
            |                       |
            .                       .
            .                       .
            .                       .
            |                       |
            +-----------------------+
            | arg table entry N-1   |
            +                       +
            |                       |
            +-----------------------+
      8+2*N | arg 0            0xce | arguments:
            + (here an int32)       + an argument can be:
            |                  0xfa |   * int32: 4 bytes long integer
            +                       +   * int64: 8 bytes long integer
            |                  0xca |   * str: (strlen+1) long string
            +                       + int arguments are aligned on their size
            |                  0xca | str which are aligned on 4 (on 32 bits arch) or 8 (on 64 bits arch)
            +-----------------------+
    8+2*N+4 | arg 1             't' |
            + (here a string)       +
    8+2*N+5 |                   'o' |
            +                       +
    8+2*N+6 |                   't' |
            +                       +
    8+2*N+7 |                   'o' |
            +                       +
    8+2*N+8 |                  '\0' | strings are null-terminated
            +                       +
    8+2*N+9 |                   PAD | and padded to align next entry
            +                       +
    8+2*N+A |                   PAD |
            +                       +
    8+2*N+B |                   PAD |
            +-----------------------+
    8+2*N+C | arg 2                 |
            .                       .
            .                       .
            .                       .

*/

#define BSIZE_OFF                    0
#define BSIZE_SIZE                   2
#define BSIZE_ADDR(B)                ((uint16_t *)((B) + BSIZE_OFF))
#define BTYPE_OFF                    (BSIZE_OFF + BSIZE_SIZE)
#define BTYPE_SIZE                   1
#define BTYPE_ADDR(B)                ((char *)((B) + BTYPE_OFF))
#define METHOD_OFF                   (BTYPE_OFF + BTYPE_SIZE)
#define METHOD_SIZE                  1
#define METHOD_ADDR(B)               ((char *)((B) + METHOD_OFF))
#define ID_OFF                       (METHOD_OFF + METHOD_SIZE)
#define ID_SIZE                      4
#define ID_ADDR(B)                   ((uint32_t *)((B) + ID_OFF))
#define ATABLE_OFF                   (ID_OFF + ID_SIZE)
#define ATABLE_MAX_ENTRIES           16
#define ATENTRY_SIZE                 2
#define ATABLE_SIZE                  (ATABLE_MAX_ENTRIES * ATENTRY_SIZE)
#define ATABLE_ADDR(B)               ((uint16_t *)((B) + ATABLE_OFF))
#define ATENTRY_ADDR(B, I)           (ATABLE_ADDR(B) + (I))
#define ARG_OFF                      (ATABLE_OFF + ATABLE_SIZE)

#define ARG_NONE                     0
#define ARG_NONE_SIZE                0
#define ARG_NONE_ALIGN               0
#define ARG_INT32                    1
#define ARG_INT32_SIZE               4
#define ARG_INT32_ALIGN              4
#define ARG_INT64                    2
#define ARG_INT64_SIZE               8
#define ARG_INT64_ALIGN              8
#define ARG_STR                      3
/* must #define it to 4 on 32 bits arch */
#define ARG_STR_ALIGN                8

#define ATENTRY_BITS                 (ATENTRY_SIZE * 8)
#define ATENTRY_OFFSET_SHIFT         0
#define ATENTRY_OFFSET_BITS          14
#define ATENTRY_TYPE_SHIFT           ATENTRY_OFFSET_BITS
#define ATENTRY_TYPE_BITS            (ATENTRY_BITS - ATENTRY_OFFSET_BITS)
#define MASK(B)                      (~(~0 << (B)))
#define GETBITS(V, S, B)             (((V) >> S) & MASK(B))
#define ATENTRY(TYPE, OFFSET)        ((((OFFSET) & MASK(ATENTRY_OFFSET_BITS)) << ATENTRY_OFFSET_SHIFT) | (((TYPE) & MASK(ATENTRY_TYPE_BITS)) << ATENTRY_TYPE_SHIFT))
#define ATENTRY_GET_TYPE(B, I)       GETBITS(*ATENTRY_ADDR(B, I), ATENTRY_TYPE_SHIFT, ATENTRY_TYPE_BITS)
#define ATENTRY_GET_OFFSET(B, I)     GETBITS(*ATENTRY_ADDR(B, I), ATENTRY_OFFSET_SHIFT, ATENTRY_OFFSET_BITS)
#define ATENTRY_MAX_OFFSET           (1 << ATENTRY_OFFSET_BITS)

/*
   1) compute buffer length by walking through the arguments
   2) allocate buffer
   3) fill buffer by walking again through the arguments (make 2 functions taking va_list arguments)
*/
enum brpc_buffer_type {
	REQUEST = 1,
	NOTIFY,
	RESPONSE,
	ERROR,
};

static size_t align_gap(size_t offset, size_t alignment)
{
	return (offset % alignment == 0) ? 0 : alignment - offset % alignment;
}

static size_t align_and_increment(size_t offset, size_t alignment, size_t increment)
{
	return offset + align_gap(offset, increment) + increment;
}

#define APPROXIMATE_STRLEN 64

static size_t brpc_buffer_approximate_size(const char *fmt)
{
	const char *p;
	size_t size = ARG_OFF;

 	for(p = fmt; *p; p++) {
		switch(*p) {
		case 'i':
			size = align_and_increment(size, ARG_INT32_ALIGN, ARG_INT32_SIZE);
			break;
		case 'l':
			size = align_and_increment(size, ARG_INT64_ALIGN, ARG_INT64_SIZE);
			break;
		case 's':
			size = align_and_increment(size, ARG_STR_ALIGN, APPROXIMATE_STRLEN);
			break;
		default:
			return 0;
		}
	}

	return size;
}

static char *add_arg(struct buffer *b, int arg_count, uint8_t arg_type, size_t arg_size, size_t arg_alignment)
{
	char *arg_p;
	size_t arg_offset;

	buffer_fill(b, 0, align_gap(buffer_size(b), arg_alignment));
	buffer_make_room(b, arg_size);

	arg_offset = buffer_size(b);
	if (arg_offset >= ATENTRY_MAX_OFFSET)
		return NULL;

	arg_p = buffer_end(b);
	buffer_increment(b, arg_size);

	*ATENTRY_ADDR(buffer_data(b), arg_count) = ATENTRY(arg_type, arg_offset);

	return arg_p;
}

brpc_buffer_t *brpc_buffer_new(const char *fmt, ...)
{
	va_list args;
	struct buffer *b;
	brpc_buffer_t *ret;
	const char *p, *s;
	int arg_count;
	char *arg_p;
	size_t approximate_size, l;

	approximate_size = brpc_buffer_approximate_size(fmt);
	if (approximate_size == 0)
		return NULL;
	b = buffer_new(approximate_size);
	buffer_fill(b, 0, ARG_OFF);

	va_start(args, fmt);
	for(p = fmt, arg_count = 0; *p; p++, arg_count++) {
		switch(*p) {
		case 'i':
			arg_p = add_arg(b, arg_count, ARG_INT32, ARG_INT32_SIZE, ARG_INT32_ALIGN);
			if (arg_p == NULL)
				goto ret_error;
			*(int32_t *)arg_p = va_arg(args, int32_t);
			break;
		case 'l':
			arg_p = add_arg(b, arg_count, ARG_INT64, ARG_INT64_SIZE, ARG_INT64_ALIGN);
			if (arg_p == NULL)
				goto ret_error;
			*(int64_t *)arg_p = va_arg(args, int64_t);
			break;
		case 's':
			s = va_arg(args, const char *);
			l = strlen(s) + 1;
			arg_p = add_arg(b, arg_count, ARG_STR, l, ARG_STR_ALIGN);
			if (arg_p == NULL)
				goto ret_error;
			strncpy(arg_p, s, l);
			break;
		default:
			goto ret_error;
		}
	}
	va_end(args);

	*BSIZE_ADDR(buffer_data(b)) = buffer_size(b);
	ret = buffer_data(b);
	buffer_free(b, 0);

	return ret;

ret_error:
	va_end(args);
	buffer_free(b, 1);
	return NULL;
}

static int check_arg(const brpc_buffer_t *b, uint8_t index, uint8_t arg_type, int *error)
{
	if (index >= ATABLE_MAX_ENTRIES) {
		if (error != NULL)
			*error = BRPC_ERROR_ARGC_OUT_OF_BOUND;
		return 1;
	}

	if (ATENTRY_GET_TYPE(b, index) != arg_type) {
		if (error != NULL)
			*error = BRPC_ERROR_INVALID_ARGUMENT_TYPE;
		return 1;
	}

	return 0;
}


int32_t brpc_buffer_get_int32(const brpc_buffer_t *b, uint8_t index, int *error)
{
	if (check_arg(b, index, ARG_INT32, error))
		return -1;

	return *(int32_t *)(b + ATENTRY_GET_OFFSET(b, index));
}

int64_t brpc_buffer_get_int64(const brpc_buffer_t *b, uint8_t index, int *error)
{
	if (check_arg(b, index, ARG_INT64, error))
		return -1;

	return *(int64_t *)(b + ATENTRY_GET_OFFSET(b, index));
}

char *brpc_buffer_get_str(const brpc_buffer_t *b, uint8_t index, int *error)
{
	if (check_arg(b, index, ARG_STR, error))
		return "";

	return (char *)(b + ATENTRY_GET_OFFSET(b, index));
}

void brpc_buffer_print(brpc_buffer_t *b)
{
	int argc = 0;

	while (argc < ATABLE_MAX_ENTRIES && ATENTRY_GET_TYPE(b, argc) != ARG_NONE) {
		switch(ATENTRY_GET_TYPE(b, argc)) {
		case ARG_INT32:
			printf("[%d] %d\n", argc, brpc_buffer_get_int32(b, argc, NULL));
			break;
		case ARG_INT64:
			printf("[%d] %ld\n", argc, brpc_buffer_get_int64(b, argc, NULL));
			break;
		case ARG_STR:
			printf("[%d] \"%s\"\n", argc, brpc_buffer_get_str(b, argc, NULL));
			break;
		}

		argc++;
	}
}

/* internal functions */
static void brpc_buffer_set_type(brpc_buffer_t *b, uint8_t type)
{
	*BTYPE_ADDR(b) = type;
}

static void brpc_buffer_set_method(brpc_buffer_t *b, uint8_t method)
{
	*METHOD_ADDR(b) = method;
}

static void brpc_buffer_set_id(brpc_buffer_t *b, uint32_t id)
{
	*ID_ADDR(b) = id;
}

#define MAPPER_MAX_METHODS 64

struct brpc_mapper {
	brpc_method_t method_table[MAPPER_MAX_METHODS];
};

struct brpc_mapper *brpc_mapper_new(void)
{
	struct brpc_mapper *m = calloc(1, sizeof(struct brpc_mapper));

	return m;
}

int brpc_mapper_add(struct brpc_mapper *mapper, uint8_t method, brpc_method_t method_cb)
{
	if (method >= MAPPER_MAX_METHODS)
		return 1;

	if (mapper->method_table[method] != NULL)
		return 1;

	mapper->method_table[method] = method_cb;

	return 0;
}

static brpc_method_t brpc_mapper_get(struct brpc_mapper *mapper, uint8_t method)
{
	if (method >= MAPPER_MAX_METHODS)
		return NULL;

	return mapper->method_table[method];
}

#ifdef DO_TEST_DEBUG_MAIN

#include <assert.h>

int main(int argc, char **argv)
{
	brpc_buffer_t *b;

	b = brpc_buffer_new("sissil", "foo", 66, "bar", "joe", 99, 0xdeadbeeffeedfaceL);
	assert(b != NULL);
	brpc_buffer_print(b);

	b = brpc_buffer_new("zob", "foo", 66, "bar", "joe", 99);
	assert(b != NULL);
	brpc_buffer_print(b);

	return 0;
}

#endif
