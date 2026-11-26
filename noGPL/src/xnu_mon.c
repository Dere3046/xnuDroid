// SPDX-License-Identifier: APSL-2.0
/*
 * Copyright (C) 2026 dere3046
 * Copyright (C) 2026 Heyang Zhou
 */

#include <linux/module.h>

#include "xnu_main.h"
#include "xnu_mon.h"

static void *g_monitor;
static void *g_fb_thread;

static inline unsigned acct_bucket(unsigned long call)
{
	return (call >= 800 && call < 800 + XNU_ACCT_N - 1) ?
	       (call - 800) : (XNU_ACCT_N - 1);
}

void xnu_acct_add(unsigned long call, u64_t ticks)
{
	unsigned bucket = acct_bucket(call);
	unsigned slot = xnu_this_slot();

	g_xnu.acct_ticks[slot][bucket] += ticks;
	g_xnu.acct_calls[slot][bucket]++;
}

static char *put_dec(char *out, unsigned long val)
{
	char tmp[24];
	int cnt = 0;

	do {
		tmp[cnt++] = '0' + val % 10;
		val /= 10;
	} while (val);
	while (cnt) {
		*out++ = tmp[--cnt];
	}
	return out;
}

static void put_str(char **out, const char *str)
{
	while (*str) {
		*(*out)++ = *str++;
	}
}

static void put_hex(char **out, unsigned long val)
{
	int i;

	for (i = 44; i >= 0; i -= 4) {
		*(*out)++ = "0123456789abcdef"[(val >> i) & 15];
	}
}

static void acct_report(void)
{
	static unsigned long last_t;
	static unsigned long last_ticks[XNU_ACCT_N];
	static unsigned long last_calls[XNU_ACCT_N];
	static unsigned long last_disk;
	unsigned long now = fki_counter();
	unsigned long dt = now - last_t;
	unsigned long total = 0;
	unsigned long disk_bytes = 0;
	char buf[800];
	char *out = buf;
	unsigned bucket;
	unsigned slot;

	if (!last_t) {
		last_t = now;
		return;
	}
	last_t = now;
	put_str(&out, "xnu: perf ms=");
	out = put_dec(out, dt * 1000 / fki_counter_frequency());

	for (bucket = 0; bucket < XNU_ACCT_N && out < buf + sizeof(buf) - 48;
	     bucket++) {
		unsigned long ticks = 0;
		unsigned long calls = 0;
		unsigned long d_ticks;
		unsigned long d_calls;

		for (slot = 0; slot < LINUX64_CPU_SLOTS; slot++) {
			ticks += READ_ONCE(g_xnu.acct_ticks[slot][bucket]);
			calls += READ_ONCE(g_xnu.acct_calls[slot][bucket]);
		}
		d_ticks = ticks - last_ticks[bucket];
		d_calls = calls - last_calls[bucket];
		last_ticks[bucket] = ticks;
		last_calls[bucket] = calls;
		if (!d_calls) {
			continue;
		}
		total += d_ticks;
		put_str(&out, bucket == XNU_ACCT_N - 1 ? " other" : " c");
		if (bucket != XNU_ACCT_N - 1) {
			out = put_dec(out, 800 + bucket);
		}
		put_str(&out, "=");
		out = put_dec(out, d_ticks * 1000 / dt);
		put_str(&out, "%o/");
		out = put_dec(out, d_calls);
	}
	put_str(&out, " host=");
	out = put_dec(out, total * 1000 / dt);

	if (g_xnu.xnu_el0_stat) {
		static const char *cls[LINUX64_EL0_CLASSES] = {
			" svc=", " dabt=", " iabt=", " irq=", " other="
		};
		static unsigned long last_n[LINUX64_EL0_CLASSES];
		static unsigned long last_et[LINUX64_EL0_CLASSES];
		unsigned class_idx;

		for (class_idx = 0; class_idx < LINUX64_EL0_CLASSES;
		     class_idx++) {
			unsigned long cnt = 0;
			unsigned long ticks = 0;

			for (slot = 0; slot < LINUX64_EL0_CPUS; slot++) {
				cnt += READ_ONCE(g_xnu.xnu_el0_stat[(slot * 2 + 0) * LINUX64_EL0_CLASSES + class_idx]);
				ticks += READ_ONCE(g_xnu.xnu_el0_stat[(slot * 2 + 1) * LINUX64_EL0_CLASSES + class_idx]);
			}
			put_str(&out, cls[class_idx]);
			out = put_dec(out, cnt - last_n[class_idx]);
			put_str(&out, "/");
			out = put_dec(out, (ticks - last_et[class_idx]) * 1000 / dt);
			put_str(&out, "%o");
			last_n[class_idx] = cnt;
			last_et[class_idx] = ticks;
		}
	}

	for (slot = 0; slot < LINUX64_CPU_SLOTS; slot++) {
		disk_bytes += READ_ONCE(g_xnu.acct_disk[slot]);
	}
	put_str(&out, "%o diskKB=");
	out = put_dec(out, (disk_bytes - last_disk) >> 10);
	last_disk = disk_bytes;
	*out = 0;
	xnu_logs(buf);
}

static int monitor_fn(void *arg)
{
	static const struct {
		int idx;
		const char *name;
	} dec[] = {
		{ LINUX64_CTR_IDLE_THREAD, " idle_thread=" },
		{ LINUX64_CTR_MACHINE_IDLE, " machine_idle=" },
		{ LINUX64_CTR_CPU_IDLE, " cpu_idle=" },
		{ LINUX64_CTR_HOST_IDLE, " host_idle=" },
		{ LINUX64_CTR_SWITCHES, " switches=" },
		{ LINUX64_CTR_LAST_CALL, " last_call=" },
		{ LINUX64_CTR_RTCLOCK, " rtclock=" },
		{ LINUX64_CTR_EL0_EXC, " el0exc=" },
	}, hex[] = {
		{ LINUX64_CTR_EL0_ESR, " esr=0x" },
		{ LINUX64_CTR_EL0_FAR, " far=0x" },
		{ LINUX64_CTR_EL0_PC, " pc=0x" },
		{ LINUX64_CTR_BAD_ESR, " bad_esr=0x" },
		{ LINUX64_CTR_THREAD, " thread=0x" },
	};
	int lines = 0;
	unsigned long ticks = 0;

	(void)arg;
	while (!fki_thread_should_stop()) {
		static unsigned long seen[XNU_MAX_CPUS];
		static unsigned long still[XNU_MAX_CPUS];
		char buf[420];
		char *out;
		unsigned i;
		int cpu;

		fki_msleep(2000);
		if (g_xnu.xnu_dbg && *(volatile int *)&g_xnu.dump_threads) {
			((volatile unsigned long *)g_xnu.xnu_dbg)[LINUX64_CTR_DUMP_REQ] = 1;
			g_xnu.dump_threads = 0;
		}
		for (cpu = 0; cpu < g_xnu.cpus && g_xnu.xnu_dbg; cpu++) {
			unsigned slot = LINUX64_CPU_SLOT(g_xnu.cpu_mpidr[cpu]);

			if ((cpu && !g_xnu.cpu_running[cpu]) ||
			    g_xnu.upcalls_slot[slot] != seen[cpu]) {
				seen[cpu] = g_xnu.upcalls_slot[slot];
				still[cpu] = 0;
				continue;
			}
			if (++still[cpu] == 1) {
				struct xnu_world *world = xnu_worlds[slot];
				unsigned long fp = world->xnu_regs[10];
				char ab[160];
				char *ap = ab;
				int frame_idx;

				put_str(&ap, "xnu: XNU CPU ");
				ap = put_dec(ap, (unsigned long)cpu);
				put_str(&ap, " silent for 2 s, last call ");
				ap = put_dec(ap, g_xnu.last_call_slot[slot]);
				put_str(&ap, g_xnu.in_call_slot[slot] ?
					" (inside)" : " (in XNU)");
				*ap = 0;
				xnu_logs_alert(ab);
				xnu_logkv("xnu:   lr at call",
					  world->xnu_regs[11]);
				for (frame_idx = 0; frame_idx < 16 &&
				     fp >= XNU_VA_LO && fp < XNU_VA_HI &&
				     !(fp & 7); frame_idx++) {
					xnu_logkv("xnu:   frame",
						  ((unsigned long *)fp)[1]);
					fp = ((unsigned long *)fp)[0];
				}
			}
		}
		if (g_xnu.perf && g_xnu.cpus > 1) {
			static char cb[16 + XNU_MAX_CPUS * 42];
			char *cp = cb;

			put_str(&cp, "xnu: calls");
			for (cpu = 0; cpu < g_xnu.cpus; cpu++) {
				unsigned slot = LINUX64_CPU_SLOT(g_xnu.cpu_mpidr[cpu]);

				put_str(&cp, " ");
				cp = put_dec(cp, g_xnu.upcalls_slot[slot]);
				put_str(&cp, "/");
				cp = put_dec(cp, g_xnu.last_call_slot[slot]);
			}
			*cp = 0;
			xnu_logs(cb);
		}
		if (g_xnu.perf) {
			acct_report();
		}
		if (!g_xnu.xnu_dbg || (lines >= 90 && (++ticks % 10) != 0)) {
			continue;
		}
		out = buf;
		put_str(&out, "xnu: monitor");
		for (i = 0; i < sizeof(dec) / sizeof(dec[0]); i++) {
			put_str(&out, dec[i].name);
			out = put_dec(out, g_xnu.xnu_dbg[dec[i].idx]);
		}
		put_str(&out, " kfaults=");
		out = put_dec(out, g_xnu.kfaults);
		put_str(&out, " moved=");
		out = put_dec(out, g_xnu.kfault_moved);
		for (i = 0; i < sizeof(hex) / sizeof(hex[0]); i++) {
			put_str(&out, hex[i].name);
			put_hex(&out, g_xnu.xnu_dbg[hex[i].idx]);
		}
		*out = 0;
		xnu_logs(buf);
		lines++;
	}
	return 0;
}

static int fb_fn(void *arg)
{
	void *file = fki_file_open(g_xnu.fb_dump, 1, 0);

	(void)arg;
	if (!file) {
		xnu_logs("xnu: cannot open fb_dump");
		return 0;
	}
	while (!fki_thread_should_stop()) {
		fki_msleep(g_xnu.fb_dump_ms);
		if (g_xnu.mode == 4 && g_xnu.m4_chunks) {
			u64_t rem = XNU_FB_SIZE;
			u64_t off = g_xnu.fb_off;
			u64_t foff = 0;

			while (rem) {
				unsigned int ci = (unsigned int)
					(off >> LINUX64_CHUNK_SHIFT);
				u64_t coff = off & (LINUX64_CHUNK_SIZE - 1);
				u64_t take = LINUX64_CHUNK_SIZE - coff;

				if (take > rem) {
					take = rem;
				}
				if (ci >= g_xnu.m4_n_chunks) {
					break;
				}
				fki_file_pwrite(file,
						(unsigned char *)g_xnu.m4_chunks[ci].va + coff,
						(unsigned long)take, foff);
				off += take;
				foff += take;
				rem -= take;
			}
		} else {
			fki_file_pwrite(file,
					(unsigned char *)g_xnu.img.region.va +
					g_xnu.fb_off,
					XNU_FB_SIZE, 0);
		}
	}
	fki_file_close(file);
	return 0;
}

void xnu_mon_start(void)
{
	int host_cpu = g_xnu.cpus < fki_num_cpus() ?
		       g_xnu.cpus : fki_num_cpus() - 1;

	if (g_xnu.monitor_cpu == -2) {
		g_xnu.monitor_cpu = host_cpu;
	}
	if (g_xnu.xnu_dbg && g_xnu.monitor_cpu >= 0) {
		g_monitor = fki_thread_start(monitor_fn, 0, g_xnu.monitor_cpu,
					     "xnu-monitor");
	}
	if (g_xnu.fb_off && g_xnu.fb_dump && g_xnu.fb_dump_ms > 0 &&
	    (g_xnu.img.region.va || (g_xnu.mode == 4 && g_xnu.m4_chunks))) {
		g_fb_thread = fki_thread_start(fb_fn, 0,
					       g_xnu.monitor_cpu >= 0 ?
					       g_xnu.monitor_cpu : host_cpu,
					       "xnu-fb");
	}
}

void xnu_mon_stop(void)
{
	fki_thread_stop(g_monitor);
	g_monitor = 0;
	fki_thread_stop(g_fb_thread);
	g_fb_thread = 0;
}
