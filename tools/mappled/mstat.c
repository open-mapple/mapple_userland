/*
 * mstat: load, processes and memory on the phone, for timing the UI. Built next to mappled (build.sh), pushed to
 * /private/var/root/mapple-dev/mstat and run through the link (`run mstat ...`); one line per sample on stdout.
 *   mstat cpu MS N       N samples of MS ms: busy % of each CPU (0-5 A55, 6-7 A76) and of each cluster
 *   mstat top MS [N]     the N busiest processes over MS ms (% of one CPU, threads), processes started and gone
 *   mstat mem            free / active / inactive / wired / compressed memory, compressor activity, pressure
 *   mstat burn SEC       eight threads spinning for SEC seconds (a load for the thermal trip test)
 */
#include <mach/mach.h>
#include <mach/mach_time.h>
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/sysctl.h>
#include <time.h>
#include <unistd.h>

/* libproc and sys/proc_info.h are not in the iOS SDK: the same declarations */
#define PROC_PIDTASKINFO 4
struct proc_taskinfo {
	uint64_t pti_virtual_size, pti_resident_size, pti_total_user, pti_total_system, pti_threads_user,
	    pti_threads_system;
	int32_t pti_policy, pti_faults, pti_pageins, pti_cow_faults, pti_messages_sent, pti_messages_received,
	    pti_syscalls_mach, pti_syscalls_unix, pti_csw, pti_threadnum, pti_numrunning, pti_priority;
};
int proc_listallpids(void *buffer, int buffersize);
int proc_pidinfo(int pid, int flavor, uint64_t arg, void *buffer, int buffersize);
int proc_name(int pid, void *buffer, uint32_t buffersize);

#define NCPU_MAX 16
#define NPROC_MAX 1024

static uint64_t
now_ns(void)
{
	return clock_gettime_nsec_np(CLOCK_UPTIME_RAW);
}

static void
sleep_ms(unsigned ms)
{
	struct timespec ts = { ms / 1000, (long)(ms % 1000) * 1000000 };
	nanosleep(&ts, NULL);
}

static int
cpu_ticks(uint64_t busy[NCPU_MAX], uint64_t total[NCPU_MAX])
{
	natural_t n = 0;
	processor_info_array_t info;
	mach_msg_type_number_t count;
	if (host_processor_info(mach_host_self(), PROCESSOR_CPU_LOAD_INFO, &n, &info, &count) != KERN_SUCCESS) {
		return 0;
	}
	processor_cpu_load_info_t l = (processor_cpu_load_info_t)info;
	for (natural_t i = 0; i < n && i < NCPU_MAX; i++) {
		busy[i] = (uint64_t)l[i].cpu_ticks[CPU_STATE_USER] + l[i].cpu_ticks[CPU_STATE_SYSTEM] + l[i].cpu_ticks[CPU_STATE_NICE];
		total[i] = busy[i] + l[i].cpu_ticks[CPU_STATE_IDLE];
	}
	vm_deallocate(mach_task_self(), (vm_address_t)info, count * sizeof(int));
	return (int)(n < NCPU_MAX ? n : NCPU_MAX);
}

static int
cmd_cpu(unsigned ms, unsigned samples)
{
	uint64_t b0[NCPU_MAX], t0[NCPU_MAX], b1[NCPU_MAX], t1[NCPU_MAX];
	int n = cpu_ticks(b0, t0);
	if (n == 0) {
		printf("mstat: host_processor_info failed\n");
		return 1;
	}
	for (unsigned s = 0; s < samples; s++) {
		sleep_ms(ms);
		cpu_ticks(b1, t1);
		uint64_t cb[2] = { 0, 0 }, ct[2] = { 0, 0 };
		printf("cpu");
		for (int i = 0; i < n; i++) {
			uint64_t db = b1[i] - b0[i], dt = t1[i] - t0[i];
			printf("%s %3llu", i == 6 ? " |" : "", dt ? db * 100 / dt : 0);
			cb[i >= 6] += db;
			ct[i >= 6] += dt;
			b0[i] = b1[i];
			t0[i] = t1[i];
		}
		printf("  A55 %llu%% A76 %llu%%\n", ct[0] ? cb[0] * 100 / ct[0] : 0, ct[1] ? cb[1] * 100 / ct[1] : 0);
		fflush(stdout);
	}
	return 0;
}

struct pinfo {
	int pid;
	uint64_t cpu;
	int threads;
};

static int
snapshot(struct pinfo *p)
{
	static int pids[NPROC_MAX];
	int n = proc_listallpids(pids, sizeof(pids)), k = 0;
	for (int i = 0; i < n && i < NPROC_MAX; i++) {
		struct proc_taskinfo ti;
		if (pids[i] > 0 && proc_pidinfo(pids[i], PROC_PIDTASKINFO, 0, &ti, sizeof(ti)) == (int)sizeof(ti)) {
			p[k++] = (struct pinfo){ pids[i], ti.pti_total_user + ti.pti_total_system, ti.pti_threadnum };
		}
	}
	return k;
}

static int
by_cpu(const void *a, const void *b)
{
	uint64_t x = ((const struct pinfo *)a)->cpu, y = ((const struct pinfo *)b)->cpu;
	return x < y ? 1 : x > y ? -1 : 0;
}

static int
cmd_top(unsigned ms, unsigned shown)
{
	static struct pinfo a[NPROC_MAX], b[NPROC_MAX], d[NPROC_MAX];
	mach_timebase_info_data_t tb;
	mach_timebase_info(&tb);
	int na = snapshot(a);
	uint64_t t0 = now_ns();
	sleep_ms(ms);
	int nb = snapshot(b);
	uint64_t wall = now_ns() - t0, sum = 0;
	int nd = 0, started = 0, gone = 0;
	for (int i = 0; i < nb; i++) {
		int j = 0;
		while (j < na && a[j].pid != b[i].pid) {
			j++;
		}
		uint64_t used = b[i].cpu - (j < na ? a[j].cpu : 0);
		started += j == na;
		d[nd++] = (struct pinfo){ b[i].pid, used * tb.numer / tb.denom, b[i].threads };
		sum += d[nd - 1].cpu;
	}
	for (int j = 0; j < na; j++) {
		int i = 0;
		while (i < nb && b[i].pid != a[j].pid) {
			i++;
		}
		gone += i == nb;
	}
	qsort(d, (size_t)nd, sizeof(d[0]), by_cpu);
	printf("top over %llu ms: %d processes (%d started, %d gone), all together %llu.%llu%% of one CPU\n",
	    wall / 1000000, nb, started, gone, sum * 100 / wall, sum * 1000 / wall % 10);
	for (int i = 0; i < nd && i < (int)shown; i++) {
		char name[64] = "?";
		proc_name(d[i].pid, name, sizeof(name));
		printf("  %5d %-28s %3llu.%llu%% %3d threads\n", d[i].pid, name, d[i].cpu * 100 / wall,
		    d[i].cpu * 1000 / wall % 10, d[i].threads);
	}
	fflush(stdout);
	return 0;
}

static int
sysctl_int(const char *name)
{
	int v = -1;
	size_t len = sizeof(v);
	return sysctlbyname(name, &v, &len, NULL, 0) == 0 ? v : -1;
}

static int
cmd_mem(void)
{
	vm_statistics64_data_t vs;
	mach_msg_type_number_t count = HOST_VM_INFO64_COUNT;
	vm_size_t page = 0;
	uint64_t memsize = 0;
	size_t len = sizeof(memsize);
	if (host_statistics64(mach_host_self(), HOST_VM_INFO64, (host_info64_t)&vs, &count) != KERN_SUCCESS ||
	    host_page_size(mach_host_self(), &page) != KERN_SUCCESS) {
		printf("mstat: host_statistics64 failed\n");
		return 1;
	}
	sysctlbyname("hw.memsize", &memsize, &len, NULL, 0);
	uint64_t mb = page / 1024;
#define MB(x) ((uint64_t)(x) * mb / 1024)
	printf("mem of %llu MB: free %llu, active %llu, inactive %llu, speculative %llu, wired %llu, compressor %llu MB "
	    "(holding %llu MB); compressions %llu, decompressions %llu, swapins %llu; level %d%%, pressure %d (1 normal, 2 "
	    "warn, 4 critical), available %d pages\n", memsize >> 20, MB(vs.free_count), MB(vs.active_count),
	    MB(vs.inactive_count), MB(vs.speculative_count), MB(vs.wire_count), MB(vs.compressor_page_count),
	    MB(vs.total_uncompressed_pages_in_compressor), vs.compressions, vs.decompressions, vs.swapins,
	    sysctl_int("kern.memorystatus_level"), sysctl_int("kern.memorystatus_vm_pressure_level"),
	    sysctl_int("kern.memorystatus_available_pages"));
	fflush(stdout);
	return 0;
}

static volatile int burning = 1;

static void *
spin(void *arg)
{
	volatile uint64_t x = (uint64_t)(uintptr_t)arg;
	while (burning) {
		x = x * 6364136223846793005ull + 1;
	}
	return NULL;
}

static int
cmd_burn(unsigned sec)
{
	pthread_t t[8];
	for (int i = 0; i < 8; i++) {
		pthread_create(&t[i], NULL, spin, (void *)(uintptr_t)(i + 1));
	}
	printf("burn: 8 threads for %u s\n", sec);
	fflush(stdout);
	sleep(sec);
	burning = 0;
	for (int i = 0; i < 8; i++) {
		pthread_join(t[i], NULL);
	}
	printf("burn: done\n");
	return 0;
}

int
main(int argc, char **argv)
{
	unsigned a = argc > 2 ? (unsigned)strtoul(argv[2], NULL, 0) : 0, b = argc > 3 ? (unsigned)strtoul(argv[3], NULL, 0) : 0;
	if (argc > 1 && strcmp(argv[1], "cpu") == 0 && a > 0) {
		return cmd_cpu(a, b ? b : 1);
	}
	if (argc > 1 && strcmp(argv[1], "top") == 0 && a > 0) {
		return cmd_top(a, b ? b : 15);
	}
	if (argc > 1 && strcmp(argv[1], "mem") == 0) {
		return cmd_mem();
	}
	if (argc > 1 && strcmp(argv[1], "burn") == 0 && a > 0) {
		return cmd_burn(a > 600 ? 600 : a);
	}
	printf("usage: mstat cpu MS N | top MS [N] | mem | burn SEC\n");
	return 2;
}
