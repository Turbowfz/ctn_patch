/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef CTN_PATCH_INTERNAL_H
#define CTN_PATCH_INTERNAL_H

#ifdef __KERNEL__
#include <linux/errno.h>
#include <linux/string.h>
#else
#include <errno.h>
#include <stddef.h>
#include <string.h>
#endif

#define CT_NUM		2
#define CT_NAME_LEN	100
#define CT_INPUT_LEN	256

static inline int ctn_is_space(unsigned char c)
{
	return c == ' ' || c == '\t' || c == '\n' || c == '\r' ||
	       c == '\v' || c == '\f';
}

/*
 * Parse the actual write bytes, not a NUL-terminated approximation.  The
 * kernel and host tests include this same implementation.  A caller must
 * publish out only after success; rejected input may leave partial output.
 */
static inline int ctn_parse_names(const char *input, size_t count,
				  char out[CT_NUM][CT_NAME_LEN])
{
	size_t pos = 0, start, len;
	int slot;

	if (!count || count >= CT_INPUT_LEN)
		return -E2BIG;
	if (!input || !out || memchr(input, '\0', count))
		return -EINVAL;

	for (slot = 0; slot < CT_NUM; slot++) {
		while (pos < count && ctn_is_space((unsigned char)input[pos]))
			pos++;
		if (pos == count) {
			if (!slot)
				return -EINVAL;
			memcpy(out[1], out[0], strlen(out[0]) + 1);
			return 0;
		}

		start = pos;
		while (pos < count && !ctn_is_space((unsigned char)input[pos]))
			pos++;
		len = pos - start;
		if (len >= CT_NAME_LEN)
			return -ENAMETOOLONG;
		memcpy(out[slot], input + start, len);
		out[slot][len] = '\0';
	}

	while (pos < count && ctn_is_space((unsigned char)input[pos]))
		pos++;
	return pos == count ? 0 : -EINVAL;
}

#endif /* CTN_PATCH_INTERNAL_H */
