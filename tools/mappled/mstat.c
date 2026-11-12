/*
 * mstat: load, processes and memory on the phone, for timing the UI. Built next to mappled (build.sh), pushed to
 * /private/var/root/mapple-dev/mstat and run through the link (`run mstat ...`); one line per sample on stdout.
 *   mstat cpu MS N       N samples of MS ms: busy % of each CPU (0-5 A55, 6-7 A76) and of each cluster
 *   mstat top MS [N]     the N busiest processes over MS ms (% of one CPU, threads), processes started and gone
 *   mstat mem            free / active / inactive / wired / compressed memory, compressor activity, pressure
 *   mstat burn SEC       eight threads spinning for SEC seconds (a load for the thermal trip test)
 *   mstat ls DIR         names, sizes and change times in DIR (the phone has no ls; to find files to pull)
 *   mstat mv FROM TO     rename(2), e.g. a bundle moved aside back into place (the phone has no mv)
 *   mstat sync           sync(2): before the link's reboot, which resets at once
 *   mstat mkdir DIR      mkdir(2), mode 755
 *   mstat chown U G PATH chown(2), e.g. a pushed preference file back to mobile (501 501)
 *   mstat sysctl NAME..  each value read: an int, a string, or the length and an FNV-1a hash (to compare two names)
 *   mstat kill PID [SIG] kill(2), SIGTERM by default (e.g. backboardd, so the UI starts again)
 *   mstat jetsam [NAME..] memorystatus priority band and memory limit (MB) of those processes (all without names)
 *   mstat pressure PID [warn|critical]  a memory pressure notification to one process (DEVELOPMENT kernels)
 *   mstat gpu            per process using the Metal driver: Mesa's BO cache and what pressure gave back
 *   mstat poster [reset NAME]  PosterBoard's data container and its Application Support; reset moves NAME aside
 *                        (the lock screen falls back to a default poster when PosterBoard starts again)
 */
#include <dirent.h>
#include <mach/mach.h>
#include <mach/mach_time.h>
#include <pthread.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
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
/* sys/kern_memorystatus.h, also not in the SDK */
int memorystatus_control(uint32_t command, int32_t pid, uint32_t flags, void *buffer, size_t buffersize);
#define MEMORYSTATUS_CMD_GET_PRIORITY_LIST 1
typedef struct {
	pid_t pid;
	int32_t priority;
	uint64_t user_data;
	int32_t limit;          /* MB */
	uint32_t state;
} memorystatus_priority_entry_t;

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
cmd_sysctl(int n, char **names)
{
	static unsigned char buf[16384];
	int bad = 0;
	for (int i = 0; i < n; i++) {
		size_t len = sizeof(buf) - 1;
		if (sysctlbyname(names[i], buf, &len, NULL, 0) != 0) {
			printf("%s: ", names[i]);
			fflush(stdout);
			perror(NULL);
			bad = 1;
			continue;
		}
		size_t text = 0;
		while (text < len && buf[text] >= 0x20 && buf[text] < 0x7f) {
			text++;
		}
		if (len == sizeof(int)) {
			printf("%s: %d\n", names[i], *(int *)buf);
		} else if (len > 0 && (text == len || (text == len - 1 && buf[text] == 0))) {
			buf[len] = 0;
			printf("%s: \"%s\"\n", names[i], (char *)buf);
		} else {
			uint32_t h = 2166136261u;
			for (size_t j = 0; j < len; j++) {
				h = (h ^ buf[j]) * 16777619u;
			}
			printf("%s: %zu bytes, fnv %08x\n", names[i], len, h);
		}
	}
	return bad;
}

static int
cmd_jetsam(int n, char **names)
{
	int size = memorystatus_control(MEMORYSTATUS_CMD_GET_PRIORITY_LIST, 0, 0, NULL, 0);
	if (size <= 0) {
		perror("memorystatus_control");
		return 1;
	}
	memorystatus_priority_entry_t *e = malloc((size_t)size + 4096);
	size = memorystatus_control(MEMORYSTATUS_CMD_GET_PRIORITY_LIST, 0, 0, e, (size_t)size + 4096);
	if (size <= 0) {
		perror("memorystatus_control");
		return 1;
	}
	for (int i = 0; i < size / (int)sizeof(*e); i++) {
		char name[64] = "";
		proc_name(e[i].pid, name, sizeof(name));
		int want = n == 0;
		for (int k = 0; k < n; k++) {
			want |= strcmp(name, names[k]) == 0;
		}
		if (want) {
			printf("%6d %-28s band %3d, limit %d MB\n", e[i].pid, name, e[i].priority, e[i].limit);
		}
	}
	free(e);
	return 0;
}

static int
cmd_pressure(int pid, const char *level)
{
	uint64_t flags = level != NULL && strcmp(level, "critical") == 0 ? 0x4 : 0x2;   /* NOTE_MEMORYSTATUS_PRESSURE_* */
	uint64_t v = (flags << 32) | (uint32_t)pid;
	if (sysctlbyname("kern.memorystatus_vm_pressure_send", NULL, NULL, &v, sizeof(v)) != 0) {
		perror("kern.memorystatus_vm_pressure_send");
		return 1;
	}
	printf("pressure %s sent to %d\n", flags == 0x4 ? "critical" : "warn", pid);
	return 0;
}

/* The Metal driver's statistics pages (driver/vmg_shm_stats.h): version 4 ends with the BO cache's bytes, the pressure
 * notifications that emptied it and what they gave back. */
static int
cmd_gpu(void)
{
	const char *dir = "/private/var/tmp/vmg-stats";
	DIR *d = opendir(dir);
	if (d == NULL) {
		perror(dir);
		return 1;
	}
	struct dirent *de;
	while ((de = readdir(d)) != NULL) {
		char path[512];
		uint32_t head[4];
		snprintf(path, sizeof(path), "%s/%s", dir, de->d_name);
		FILE *f = fopen(path, "r");
		if (f == NULL) {
			continue;
		}
		uint64_t tail[3] = { 0, 0, 0 };
		if (fread(head, sizeof(head), 1, f) == 1 && head[0] == 0x534d4756u && head[1] >= 4 && head[2] >= 40 &&
		    fseek(f, (long)head[2] - (long)sizeof(tail), SEEK_SET) == 0 && fread(tail, sizeof(tail), 1, f) == 1) {
			char name[64] = "?";
			proc_name((int)head[3], name, sizeof(name));
			if (kill((pid_t)head[3], 0) == 0) {
				printf("%6u %-28s BO cache %llu KiB, %llu pressure notifications gave back %llu KiB\n", head[3], name,
				    tail[0] >> 10, tail[1], tail[2] >> 10);
			}
		}
		fclose(f);
	}
	closedir(d);
	return 0;
}

static int cmd_ls(const char *dir);

/* PosterBoard's data container: the one whose metadata names com.apple.PosterBoard. */
static int
poster_container(char *out, size_t len)
{
	const char *base = "/private/var/mobile/Containers/Data/Application";
	DIR *d = opendir(base);
	if (d == NULL) {
		perror(base);
		return 0;
	}
	struct dirent *de;
	int found = 0;
	while (!found && (de = readdir(d)) != NULL) {
		char meta[512], buf[4096];
		snprintf(meta, sizeof(meta), "%s/%s/.com.apple.mobile_container_manager.metadata.plist", base, de->d_name);
		FILE *f = fopen(meta, "r");
		if (f == NULL) {
			continue;
		}
		size_t n = fread(buf, 1, sizeof(buf), f);
		fclose(f);
		if (memmem(buf, n, "com.apple.PosterBoard", 21) != NULL) {
			snprintf(out, len, "%s/%s", base, de->d_name);
			found = 1;
		}
	}
	closedir(d);
	return found;
}

static int
cmd_poster(const char *reset)
{
	char c[512], dir[700], from[900], to[960];
	if (!poster_container(c, sizeof(c))) {
		printf("no PosterBoard data container\n");
		return 1;
	}
	snprintf(dir, sizeof(dir), "%s/Library/Application Support", c);
	printf("PosterBoard: %s\n", c);
	if (reset == NULL) {
		return cmd_ls(dir);
	}
	snprintf(from, sizeof(from), "%s/%s", dir, reset);
	snprintf(to, sizeof(to), "%s.mapple-saved-%ld", from, (long)time(NULL));
	if (rename(from, to) != 0) {
		perror(from);
		return 1;
	}
	sync();
	printf("%s -> %s (synced; kill PosterBoard for a default lock screen)\n", from, to);
	return 0;
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

static int
cmd_ls(const char *dir)
{
	DIR *d = opendir(dir);
	if (d == NULL) {
		perror(dir);
		return 1;
	}
	struct dirent *e;
	while ((e = readdir(d)) != NULL) {
		char path[1024];
		struct stat st;
		snprintf(path, sizeof(path), "%s/%s", dir, e->d_name);
		if (e->d_name[0] != '.' && lstat(path, &st) == 0) {
			printf("%10lld %10ld %s%s\n", (long long)st.st_size, (long)st.st_ctime, e->d_name, S_ISDIR(st.st_mode) ? "/" : "");
		}
	}
	closedir(d);
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
	if (argc > 2 && strcmp(argv[1], "ls") == 0) {
		return cmd_ls(argv[2]);
	}
	if (argc > 2 && strcmp(argv[1], "mkdir") == 0) {
		if (mkdir(argv[2], 0755) != 0) {
			perror("mkdir");
			return 1;
		}
		printf("%s made\n", argv[2]);
		return 0;
	}
	if (argc > 4 && strcmp(argv[1], "chown") == 0) {
		if (chown(argv[4], (uid_t)strtoul(argv[2], NULL, 10), (gid_t)strtoul(argv[3], NULL, 10)) != 0) {
			perror("chown");
			return 1;
		}
		printf("%s owned by %s:%s\n", argv[4], argv[2], argv[3]);
		return 0;
	}
	if (argc > 1 && strcmp(argv[1], "sync") == 0) {
		sync();
		printf("synced\n");
		return 0;
	}
	if (argc > 3 && strcmp(argv[1], "mv") == 0) {
		if (rename(argv[2], argv[3]) != 0) {
			perror("rename");
			return 1;
		}
		printf("%s -> %s\n", argv[2], argv[3]);
		return 0;
	}
	if (argc > 2 && strcmp(argv[1], "kill") == 0) {
		int sig = argc > 3 ? atoi(argv[3]) : SIGTERM;
		if (kill((pid_t)a, sig) != 0) {
			perror("kill");
			return 1;
		}
		printf("signal %d sent to %u\n", sig, a);
		return 0;
	}
	if (argc > 1 && strcmp(argv[1], "jetsam") == 0) {
		return cmd_jetsam(argc - 2, argv + 2);
	}
	if (argc > 2 && strcmp(argv[1], "pressure") == 0) {
		return cmd_pressure((int)a, argc > 3 ? argv[3] : NULL);
	}
	if (argc > 1 && strcmp(argv[1], "gpu") == 0) {
		return cmd_gpu();
	}
	if (argc > 1 && strcmp(argv[1], "poster") == 0) {
		return cmd_poster(argc > 3 && strcmp(argv[2], "reset") == 0 ? argv[3] : NULL);
	}
	if (argc > 2 && strcmp(argv[1], "sysctl") == 0) {
		return cmd_sysctl(argc - 2, argv + 2);
	}
	printf("usage: mstat cpu MS N | top MS [N] | mem | burn SEC | ls DIR | mv FROM TO | sync | mkdir DIR | chown U G PATH | sysctl NAME.. | kill PID [SIG] |\n"
	    "       jetsam [NAME..] | pressure PID [warn|critical] | gpu | poster [reset NAME]\n");
	return 2;
}
