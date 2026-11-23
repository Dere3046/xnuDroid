// SPDX-License-Identifier: APSL-2.0
/*
 * Copyright (C) 2026 dere3046
 * Copyright (C) 2026 Heyang Zhou
 */

#ifndef XNU_ADT_H
#define XNU_ADT_H

struct adt {
	unsigned char *buf;
	unsigned char *p;
	unsigned char *end;
	int err;
};

void adt_init(struct adt *a, void *buf, unsigned long cap);
void adt_node(struct adt *a, unsigned nprops, unsigned nchildren);
void adt_prop(struct adt *a, const char *name, const void *val, unsigned len);
void adt_prop_str(struct adt *a, const char *name, const char *s);
void adt_prop_u32(struct adt *a, const char *name, unsigned v);
void adt_prop_u64(struct adt *a, const char *name, unsigned long long v);

static inline unsigned long adt_size(struct adt *a)
{
	return a->p - a->buf;
}

#endif
