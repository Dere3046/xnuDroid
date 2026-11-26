// SPDX-License-Identifier: APSL-2.0
/*
 * Copyright (C) 2026 dere3046
 * Copyright (C) 2026 Heyang Zhou
 */

#ifndef XNU_XRT_H
#define XNU_XRT_H

static inline void *x_memcpy(void *dst, const void *src, unsigned long len)
{
	char *out = dst;
	const char *in = src;

	while (len--) {
		*out++ = *in++;
	}
	return dst;
}

static inline int x_strncmp(const char *lhs, const char *rhs, unsigned long len)
{
	for (; len; len--, lhs++, rhs++) {
		if (*lhs != *rhs) {
			return (unsigned char)*lhs - (unsigned char)*rhs;
		}
		if (!*lhs) {
			return 0;
		}
	}
	return 0;
}

static inline int x_strcmp(const char *lhs, const char *rhs)
{
	while (*lhs && *lhs == *rhs) {
		lhs++;
		rhs++;
	}
	return (unsigned char)*lhs - (unsigned char)*rhs;
}

static inline unsigned long x_strlen(const char *str)
{
	const char *ptr = str;

	while (*ptr) {
		ptr++;
	}
	return ptr - str;
}

#endif
