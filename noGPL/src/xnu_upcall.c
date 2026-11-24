// SPDX-License-Identifier: APSL-2.0
/*
 * Copyright (C) 2026 dere3046
 * Copyright (C) 2026 Heyang Zhou
 */

#include <linux/module.h>

#include "xnu_boot.h"
#include "xnu_main.h"
#include "xnu_mon.h"
#include "xnu_upcall.h"

typedef unsigned long (*strlen_fn)(const char *);

static int quiet_line(const char *l, unsigned n)
{
	static const char *const pfx[] = {
		"com.apple.xpc.launchd|",
		"kmutil: AppleImage4",
		"cryptexd: AppleImage4",
		"ACMTRM",
		"ACM:",
		"AppleCredentialManagerUserClient",
		"hfs: set hardlink bit",
		"Couldn't alloc class",
		"  dynamic: platformVersion"
	};
	unsigned i;
	unsigned k;

	for (i = 0; i < sizeof(pfx) / sizeof(pfx[0]); i++) {
		k = 0;
		while (pfx[i][k] && k < n && l[k] == pfx[i][k]) {
			k++;
		}
		if (!pfx[i][k]) {
			return 1;
		}
	}
	for (k = 0; k + 18 <= n; k++) {
		if (!x_strncmp(l + k, "triggered unnest", 16)) {
			return 1;
		}
	}
	return 0;
}

static void handle_console_write(unsigned long a1, unsigned long a2)
{
	static char line[1000];
	static unsigned line_len;
	const char *buf = (const char *)a1;
	unsigned long i;

	if (buf && a2) {
		fki_console_write(buf, a2);
	}

	if (fki_console_open_count() > 0) {
		line_len = 0;
		return;
	}

	for (i = 0; buf && i < a2; i++) {
		char ch = buf[i];

		if (ch == '\r') {
			continue;
		}
		if (ch != '\n') {
			line[line_len++] = ch;
		}
		if (ch == '\n' || line_len == sizeof(line) - 1) {
			static char msg[1024];
			const char *prefix = "darwin console: ";
			unsigned msg_len = 0;
			unsigned k;

			while (*prefix) {
				msg[msg_len++] = *prefix++;
			}
			for (k = 0; k < line_len; k++) {
				msg[msg_len++] = line[k];
			}
			msg[msg_len] = 0;
			if (!(g_xnu.quiet_console &&
			      quiet_line(line, line_len))) {
				xnu_logs(msg);
			}
			line_len = 0;
		}
	}
	if (line_len >= 2 && line[line_len - 1] == ' ' &&
	    (line[line_len - 2] == '#' || line[line_len - 2] == '$' ||
	     line[line_len - 2] == '>')) {
		static char msg[1024];
		const char *prefix = "darwin console: ";
		unsigned msg_len = 0;
		unsigned k;

		while (*prefix) {
			msg[msg_len++] = *prefix++;
		}
		for (k = 0; k < line_len; k++) {
			msg[msg_len++] = line[k];
		}
		msg[msg_len] = 0;
		line_len = 0;
		xnu_logs(msg);
	}
}

static void fix_s1pie_desc(unsigned long long *desc, int level)
{
	unsigned long long val = *desc;
	unsigned long long *sub;
	int j;

	if (!(val & 1ULL)) {
		return;
	}
	if (level < 3 && (val & 3ULL) == 3ULL) {
		sub = xnu_tbl_va(val);
		if (!sub) {
			return;
		}
		for (j = 0; j < 512; j++) {
			fix_s1pie_desc(&sub[j], level + 1);
		}
		return;
	}
	/* Linux S1PIE needs bit 51 (PTE_WRITE) set for EL1 writable leaves */
	if (!(val & (3ULL << 6)) && !(val & (1ULL << 51))) {
		*desc = val | (1ULL << 51);
	}
}

static __nocfi long handle_kernel_tables(unsigned long a1)
{
	static unsigned long long tte[512];
	int cnt = 0;
	int i;

	x_memcpy(tte, (const void *)a1, sizeof(tte));
	xnu_walk_log("xnu: XNU table walk for arm_init",
		     g_xnu.diag_va[0], tte);
	xnu_walk_log("xnu: XNU table walk for Mach-O",
		     g_xnu.diag_va[1], tte);
	xnu_walk_log("xnu: XNU table walk for boot args",
		     g_xnu.diag_va[2], tte);
	xnu_walk_log("xnu: XNU table walk for cpu_tte", a1, tte);

	for (i = (int)g_xnu.l1_first; i <= (int)g_xnu.l1_last; i++) {
		if (!(tte[i] & 1)) {
			continue;
		}
		if (g_xnu.window_l1[i] &&
		    &g_xnu.window_l1[i] != g_xnu.window_slot) {
			xnu_logkv("xnu: L1 slot in use by Linux", i);
			return 1;
		}
		fix_s1pie_desc(&tte[i], 1);
		fki_window_set(&g_xnu.window_l1[i], tte[i]);
		g_xnu.window_owned[i] = 1;
		cnt++;
	}
	xnu_logkv("xnu: XNU L1 entries installed", cnt);
	xnu_logkv("xnu: probe text word",
		  *(volatile unsigned int *)g_xnu.diag_va[0]);
	xnu_logkv("xnu: probe Mach-O magic",
		  *(volatile unsigned int *)g_xnu.diag_va[1]);
	xnu_logkv("xnu: probe boot args version",
		  ((volatile struct boot_args_v2 *)g_xnu.diag_va[2])->version);
	xnu_logkv("xnu: probe ADT word",
		  *(volatile unsigned int *)g_xnu.diag_va[3]);
	if (g_xnu.diag_va[4]) {
		volatile unsigned int *pc = (void *)g_xnu.diag_va[4];
		unsigned val = *pc;

		*pc = val;
		xnu_logkv("xnu: probe port_crumb", val);
	}
	if (g_xnu.diag_va[5]) {
		unsigned long len = ((strlen_fn)g_xnu.diag_va[5])("xnulinux");

		xnu_logkv("xnu: probe exec strlen", len);
	}
	xnu_logs("xnu: probes done");
	return 0;
}

static long xnu_hook_body(unsigned long call, unsigned long a1,
			  unsigned long a2)
{
	if (call == LINUX64_CALL_CONSOLE_READ) {
		struct linux64_buf_req *req = (struct linux64_buf_req *)a1;
		long got = 0;

		if (!req) {
			return 0;
		}
		if (req->buf && req->len > 0) {
			got = fki_console_read(req->buf,
					       (unsigned long)req->len);
		}
		if (got == 0 && g_xnu.ci_file) {
			got = fki_file_rw(g_xnu.ci_file, 0, req->buf,
					  (unsigned long)req->len,
					  g_xnu.ci_off);

			if (got > 0) {
				g_xnu.ci_off += got;
			}
		}
		req->result = got > 0 ? got : 0;
		return 0;
	}

	if (call == LINUX64_CALL_INPUT_READ) {
		struct linux64_buf_req *req = (struct linux64_buf_req *)a1;

		if (!req) {
			return 0;
		}
		req->result = 0;
		if (g_xnu.in_file && req->len >= 8) {
			long got = fki_file_rw(g_xnu.in_file, 0, req->buf,
					       (unsigned long)(req->len & ~7ULL),
					       g_xnu.in_off);

			if (got > 0) {
				got &= ~7L;
				g_xnu.in_off += got;
				req->result = got;
			}
		}
		return 0;
	}

	if (call == LINUX64_CALL_NET_SEND || call == LINUX64_CALL_NET_RECV) {
		struct linux64_buf_req *req = (struct linux64_buf_req *)a1;

		if (!req) {
			return 0;
		}
		req->result = (call == LINUX64_CALL_NET_SEND) ?
			      fki_net_send(req->buf, (unsigned long)req->len) :
			      fki_net_recv(req->buf, (unsigned long)req->len);
		return 0;
	}

	if (call == LINUX64_CALL_WALLCLOCK) {
		if (a1) {
			fki_realtime(&((fki_u64 *)a1)[0], &((fki_u64 *)a1)[1]);
		}
		return 0;
	}

	if (call == LINUX64_CALL_RANDOM) {
		if (a1) {
			fki_random((void *)a1, a2);
		}
		return 0;
	}

	if (call == LINUX64_CALL_HOSTCALL) {
		struct linux64_hostcall_req *req =
			(struct linux64_hostcall_req *)a1;

		if (req) {
			req->result = fki_hostcall(req->req,
						   (unsigned long)req->len,
						   req->reply,
						   (unsigned long)req->max);
		}
		return 0;
	}

	if (call == LINUX64_CALL_DISK_SIZE) {
		if (a1) {
			*(u64_t *)a1 = g_xnu.dk_file ? g_xnu.dk_size : 0;
		}
		return 0;
	}

	if (call == LINUX64_CALL_DISK_RW) {
		struct linux64_rw_req *req = (struct linux64_rw_req *)a1;

		if (req) {
			req->result = g_xnu.dk_file ?
				      fki_file_rw(g_xnu.dk_file,
						  (int)req->write, req->buf,
						  (unsigned long)req->len,
						  req->off) : -6;
		}
		if (req && req->result > 0) {
			g_xnu.acct_disk[xnu_this_slot()] +=
				(unsigned long)req->result;
		}
		return 0;
	}

	if (call == LINUX64_CALL_KEYSTORE_RW) {
		struct linux64_rw_req *req = (struct linux64_rw_req *)a1;

		if (!req) {
			return 0;
		}
		req->result = g_xnu.ks_file ?
			      fki_file_rw(g_xnu.ks_file, (int)req->write,
					  req->buf, (unsigned long)req->len,
					  req->off) : -6;
		return 0;
	}

	if (call == LINUX64_CALL_RAMDISK_SIZE) {
		if (a1) {
			*(u64_t *)a1 = g_xnu.hd_file ? g_xnu.hd_size : 0;
		}
		return 0;
	}

	if (call == LINUX64_CALL_RAMDISK_RW) {
		struct linux64_rw_req *req = (struct linux64_rw_req *)a1;

		if (!req) {
			return 0;
		}
		if (!g_xnu.hd_file) {
			req->result = -6;
		} else if (req->write && !g_xnu.hostdisk_rw) {
			req->result = -30;
		} else {
			req->result = fki_file_rw(g_xnu.hd_file,
						  (int)req->write, req->buf,
						  (unsigned long)req->len,
						  req->off);
		}
		return 0;
	}

	if (call == LINUX64_CALL_CONSOLE_WRITE) {
		handle_console_write(a1, a2);
		return 0;
	}

	if (call == LINUX64_CALL_KPRINTF) {
		char msg[260];
		const char *fmt = (const char *)a1;
		unsigned i = 0;
		unsigned out_len = 0;

		msg[out_len++] = 'x';
		msg[out_len++] = 'n';
		msg[out_len++] = 'u';
		msg[out_len++] = ']';
		msg[out_len++] = ' ';
		while (fmt && fmt[i] && out_len < sizeof(msg) - 1) {
			if (fmt[i] != '\n') {
				msg[out_len++] = fmt[i];
			}
			i++;
		}
		msg[out_len] = 0;
		if (!(fmt && !x_strncmp(fmt, "kernel_startup_initialize_upto[",
					31)) &&
		    !(g_xnu.quiet_console &&
		      quiet_line(msg + 5, out_len - 5))) {
			xnu_logs(msg);
		}
		if (g_xnu.kmax && ++g_xnu.kcount >= g_xnu.kmax) {
			xnu_logs("xnu: kprintf budget reached");
			return 1;
		}
		return 0;
	}

	if (call == LINUX64_CALL_KERNEL_TABLES && g_xnu.window_l1) {
		return handle_kernel_tables(a1);
	}

	if (call == LINUX64_CALL_PREEMPTED) {
		fki_yield();
		return 0;
	}

	if (call == LINUX64_CALL_IDLE) {
		static unsigned long idles;

		idles++;
		if ((idles & (idles - 1)) == 0) {
			xnu_logkv("xnu: XNU idle, count", idles);
		}
		if (!a1) {
			fki_yield();
		} else {
			u64_t ns = a1 < 20000000ULL ? a1 : 20000000ULL;

			fki_idle_until(fki_counter() +
				       ns * fki_counter_frequency() / 1000000000ULL,
				       ns, (const volatile unsigned int *)a2);
		}
		return g_xnu.idle_max && idles >= (unsigned long)g_xnu.idle_max;
	}

	if (call == LINUX64_CALL_ASSERT) {
		const struct linux64_assert_info *info = (const void *)a1;
		const char *parts[3] = {
			"xnu: XNU assert failed: ", info->file, ": "
		};
		char msg[240];
		unsigned out_len = 0;
		unsigned i;
		int part_idx;

		for (part_idx = 0; part_idx < 3; part_idx++) {
			for (i = 0; parts[part_idx] && parts[part_idx][i] &&
			     out_len < sizeof(msg) - 1; i++) {
				msg[out_len++] = parts[part_idx][i];
			}
		}
		for (i = 0; info->expr && info->expr[i] &&
		     out_len < sizeof(msg) - 1; i++) {
			msg[out_len++] = info->expr[i];
		}
		msg[out_len] = 0;
		xnu_logs(msg);
		xnu_logkv("xnu:   at line", info->line);
		return g_xnu.assert_fatal;
	}

	if (call == LINUX64_CALL_USER_ENTER) {
		xnu_logkv("xnu: XNU enters user space; pc", a1);
		return 0;
	}

	if (call == LINUX64_CALL_PANIC) {
		char msg[500];
		const char *str = (const char *)a1;
		unsigned i = 0;
		unsigned long *fp;

		while (str && str[i] && i < sizeof(msg) - 1) {
			msg[i] = str[i];
			i++;
		}
		msg[i] = 0;
		xnu_logs_alert("xnu: XNU panic:");
		xnu_logs_alert(msg);
		xnu_logkv("xnu: panic caller", a2);

		fp = __builtin_frame_address(0);
		for (i = 0; i < 24 && fp && !((unsigned long)fp & 15) &&
		     (unsigned long)fp >> 48 == 0xffff; i++) {
			unsigned long lr = fp[1] | 0xffffff8000000000ul;

			xnu_logkv("xnu:   frame", lr);
			if ((unsigned long *)fp[0] <= fp) {
				break;
			}
			fp = (unsigned long *)fp[0];
		}
		return 1;
	}

	if (call == LINUX64_CALL_CPU_START) {
		int cpu = g_xnu.linux_cpu_of_slot[LINUX64_CPU_SLOT(a1)];

		if (cpu <= 0 || cpu >= g_xnu.cpus || !g_xnu.cpu_thread[cpu] ||
		    !a2 || g_xnu.cpu_start_arg[cpu]) {
			xnu_logkv("xnu: invalid CPU start MPIDR", a1);
			return 0;
		}
		g_xnu.cpu_running[cpu] = 1;
		WRITE_ONCE(g_xnu.cpu_start_arg[cpu], a2);
		fki_thread_wake(g_xnu.cpu_thread[cpu]);
		return 0;
	}

	if (call == LINUX64_CALL_IPI) {
		int cpu = g_xnu.linux_cpu_of_slot[LINUX64_CPU_SLOT(a1)];

		if (cpu >= 0) {
			fki_kick_cpu(cpu);
		}
		return 0;
	}

	if (call == LINUX64_CALL_KERNEL_TABLES) {
		return 0;
	}

	{
		static unsigned long told;

		if (told != call) {
			told = call;
			xnu_logkv("xnu: unknown call from XNU", call);
		}
	}
	return 0;
}

long xnu_hook(unsigned long call, unsigned long a1, unsigned long a2)
{
	unsigned in_s = xnu_this_slot();
	u64_t start_cnt;
	long ret;

	g_xnu.upcalls_slot[in_s]++;
	g_xnu.last_call_slot[in_s] = call;
	{
		static int told;
		unsigned long sp;

		__asm__ volatile("mov %0, sp" : "=r"(sp));
		if ((sp & 0x4000) && told < 3) {
			told++;
			xnu_logkv("xnu: upcall sp bit 14 set, sp", sp);
			xnu_logkv("xnu:   call", call);
		}
	}
	g_xnu.in_xnu_slot[in_s] = 0;
	if (*(volatile int *)&g_xnu.stop_run) {
		xnu_logs("xnu: run stopped by the host");
		return 1;
	}
	start_cnt = fki_counter();
	g_xnu.in_call_slot[in_s] = 1;
	ret = xnu_hook_body(call, a1, a2);
	g_xnu.in_call_slot[in_s] = 0;
	xnu_acct_add(call, fki_counter() - start_cnt);
	if (!ret) {
		g_xnu.in_xnu_slot[in_s] = 1;
	}
	return ret;
}
