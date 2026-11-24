// SPDX-License-Identifier: APSL-2.0
/*
 * Copyright (C) 2026 dere3046
 * Copyright (c) 2026 xnulinux contributors. All rights reserved.
 *
 * @APPLE_OSREFERENCE_LICENSE_HEADER_START@
 * This file contains Original Code and/or Modifications of Original Code as defined in and that are subject to the Apple
 * Public Source License Version 2.0 (the 'License'). You may not use this file except in compliance with the License.
 * @APPLE_OSREFERENCE_LICENSE_HEADER_END@
 */

#ifndef XNU_LINUX64_ABI_H
#define XNU_LINUX64_ABI_H

#define LINUX64_ABI_VERSION 3

#define LINUX64_CHUNK_SHIFT 14
#define LINUX64_CHUNK_SIZE  (1ULL << LINUX64_CHUNK_SHIFT)
#define LINUX64_CHUNK_MASK  (~(LINUX64_CHUNK_SIZE - 1))

struct linux64_h2g_slot {
	__UINT32_TYPE__ hpa_tag;	/* shifted hpa plus one */
	__UINT32_TYPE__ chunk_idx;
};

struct linux64_phys_map {
	__UINT64_TYPE__ gpa_base;
	__UINT64_TYPE__ gpa_end;
	const __UINT32_TYPE__ *g2h;	/* shifted hpa per 16k chunk */
	const struct linux64_h2g_slot *h2g;
	__UINT32_TYPE__ n_chunks;
	__UINT32_TYPE__ hash_mask;
};

enum linux64_call {
	LINUX64_CALL_KERNEL_TABLES = 210,
	LINUX64_CALL_IDLE = 810,
	LINUX64_CALL_PREEMPTED = 811,
	LINUX64_CALL_WALLCLOCK = 812,
	LINUX64_CALL_NET_SEND = 816,
	LINUX64_CALL_NET_RECV = 817,
	LINUX64_CALL_INPUT_READ = 818,
	LINUX64_CALL_KEYSTORE_RW = 819,
	LINUX64_CALL_DISK_RW = 820,
	LINUX64_CALL_DISK_SIZE = 821,
	LINUX64_CALL_RANDOM = 822,
	LINUX64_CALL_HOSTCALL = 830,
	LINUX64_CALL_CPU_START = 840,
	LINUX64_CALL_IPI = 841,
	LINUX64_CALL_CONSOLE_READ = 993,
	LINUX64_CALL_RAMDISK_RW = 994,
	LINUX64_CALL_RAMDISK_SIZE = 995,
	LINUX64_CALL_CONSOLE_WRITE = 996,
	LINUX64_CALL_ASSERT = 997,
	LINUX64_CALL_KPRINTF = 998,
	LINUX64_CALL_PANIC = 999,
	LINUX64_CALL_USER_ENTER = 9100,
};

struct linux64_buf_req {
	void *buf;
	__UINT64_TYPE__ len;
	__INT64_TYPE__ result;
};

struct linux64_rw_req {
	__UINT64_TYPE__ write;
	__UINT64_TYPE__ off;
	__UINT64_TYPE__ len;
	void *buf;
	__INT64_TYPE__ result;
};

struct linux64_hostcall_req {
	const void *req;
	__UINT64_TYPE__ len;
	void *reply;
	__UINT64_TYPE__ max;
	__INT64_TYPE__ result;
};

struct linux64_assert_info {
	const char *file;
	long line;
	const char *expr;
};

struct linux64_input_event {
	unsigned short type;
	unsigned short code;
	int value;
};

#define LINUX64_CPU_SLOTS 256
#define LINUX64_CPU_SLOT(mpidr) ((unsigned)(((mpidr) & 0xf) | (((mpidr) >> 4) & 0xf0)))

#define LINUX64_EL0_CLASSES 5
#define LINUX64_EL0_CPUS 8

enum linux64_counter {
	LINUX64_CTR_IDLE_THREAD = 0,
	LINUX64_CTR_MACHINE_IDLE = 1,
	LINUX64_CTR_CPU_IDLE = 2,
	LINUX64_CTR_HOST_IDLE = 3,
	LINUX64_CTR_SWITCHES = 4,
	LINUX64_CTR_LAST_CALL = 6,
	LINUX64_CTR_RTCLOCK = 7,
	LINUX64_CTR_THREAD = 9,
	LINUX64_CTR_EL0_EXC = 10,
	LINUX64_CTR_EL0_ESR = 11,
	LINUX64_CTR_EL0_FAR = 12,
	LINUX64_CTR_EL0_PC = 13,
	LINUX64_CTR_BAD_ESR = 14,
	LINUX64_CTR_DUMP_REQ = 15,
	LINUX64_NCOUNTERS = 16,
};

#endif
