// SPDX-License-Identifier: APSL-2.0
/*
 * Copyright (C) 2026 dere3046
 * Copyright (C) 2026 Heyang Zhou
 */

#include "adt.h"
#include "xrt.h"

void adt_init(struct adt *a, void *buf, unsigned long cap)
{
	a->buf = buf;
	a->p = buf;
	a->end = a->buf + cap;
	a->err = 0;
}

static void adt_put(struct adt *a, const void *v, unsigned long n)
{
	if (a->p + n > a->end) {
		a->err = 1;
		return;
	}
	x_memcpy(a->p, v, n);
	a->p += n;
}

void adt_node(struct adt *a, unsigned nprops, unsigned nchildren)
{
	unsigned h[2] = { nprops, nchildren };

	adt_put(a, h, sizeof(h));
}

void adt_prop(struct adt *adt, const char *name, const void *val,
	      unsigned len)
{
	static const unsigned char zero[4];
	char name_buf[32] = { 0 };
	unsigned long name_len = x_strlen(name);

	x_memcpy(name_buf, name, name_len < 31 ? name_len : 31);
	adt_put(adt, name_buf, 32);
	adt_put(adt, &len, 4);
	adt_put(adt, val, len);
	adt_put(adt, zero, (4 - (len & 3)) & 3);
}

void adt_prop_str(struct adt *a, const char *name, const char *s)
{
	adt_prop(a, name, s, x_strlen(s) + 1);
}

void adt_prop_u32(struct adt *a, const char *name, unsigned v)
{
	adt_prop(a, name, &v, 4);
}

void adt_prop_u64(struct adt *a, const char *name, unsigned long long v)
{
	adt_prop(a, name, &v, 8);
}
