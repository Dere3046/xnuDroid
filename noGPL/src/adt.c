// SPDX-License-Identifier: APSL-2.0
/*
 * Copyright (C) 2026 dere3046
 * Copyright (C) 2026 Heyang Zhou
 */

#include "adt.h"
#include "xrt.h"

void adt_init(struct adt *adt, void *buf, unsigned long cap)
{
	adt->buf = buf;
	adt->p = buf;
	adt->end = adt->buf + cap;
	adt->err = 0;
}

static void adt_put(struct adt *adt, const void *val, unsigned long len)
{
	if (adt->p + len > adt->end) {
		adt->err = 1;
		return;
	}
	x_memcpy(adt->p, val, len);
	adt->p += len;
}

void adt_node(struct adt *adt, unsigned nprops, unsigned nchildren)
{
	unsigned hdr[2] = { nprops, nchildren };

	adt_put(adt, hdr, sizeof(hdr));
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

void adt_prop_str(struct adt *adt, const char *name, const char *str)
{
	adt_prop(adt, name, str, x_strlen(str) + 1);
}

void adt_prop_u32(struct adt *adt, const char *name, unsigned val)
{
	adt_prop(adt, name, &val, 4);
}

void adt_prop_u64(struct adt *adt, const char *name, unsigned long long val)
{
	adt_prop(adt, name, &val, 8);
}
