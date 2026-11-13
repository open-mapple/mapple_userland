/*
 * mstat stack: a stackshot of one process (the kernel's stack_snapshot_with_config, as ReportCrash and spindump take
 * them; nothing runs in the process). Per thread: state, what it waits on and who holds it, user and kernel frames.
 * User frames are printed as address and either cache+OFFSET (from the shared cache's first mapping) or
 * IMAGE-UUID+OFFSET; mapple-import/stack-sym.py turns them into symbols. Kernel frames are unslid.
 */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

/* libsystem_kernel (sys/stackshot.h is not in the SDK) */
typedef struct stackshot_config stackshot_config_t;
stackshot_config_t *stackshot_config_create(void);
int stackshot_config_set_pid(stackshot_config_t *config, int pid);
int stackshot_config_set_flags(stackshot_config_t *config, uint64_t flags);
int stackshot_capture_with_config(stackshot_config_t *config);
void *stackshot_config_get_stackshot_buffer(stackshot_config_t *config);
uint32_t stackshot_config_get_stackshot_size(stackshot_config_t *config);
int stackshot_config_dealloc(stackshot_config_t *config);

/* osfmk/kern/debug.h */
#define SS_GET_DQ                   0x1
#define SS_SAVE_LOADINFO            0x2
#define SS_KCDATA_FORMAT            0x10000
#define SS_COLLECT_SHAREDCACHE_LAYOUT 0x80000
#define SS_NO_IO_STATS              0x800000
#define SS_THREAD_WAITINFO          0x1000000

/* osfmk/kern/kcdata.h */
#define KC_BEGIN_STACKSHOT      0x59a25807u
#define KC_ARRAY                0x11u
#define KC_CONTAINER_BEGIN      0x13u
#define KC_CONTAINER_END        0x14u
#define KC_ARRAY_PAD0           0x20u
#define KC_LOADINFO64           0x31u
#define KC_BUFFER_END           0xf19158edu
#define KC_TASK                 0x903u
#define KC_THREAD               0x904u
#define KC_TASK_SNAPSHOT        0x905u
#define KC_THREAD_SNAPSHOT      0x906u
#define KC_SHAREDCACHE_LOADINFO 0x908u
#define KC_THREAD_NAME          0x909u
#define KC_KERN_STACKLR64       0x914u
#define KC_USER_STACKLR64       0x916u
#define KC_THREAD_WAITINFO      0x91du
#define KC_DISPATCH_QUEUE_LABEL 0x928u
#define KC_TURNSTILEINFO        0x929u

/* osfmk/mach/shared_region.h, arm64 */
#define SHARED_REGION_END       (0x180000000ull + 0x100000000ull)

#define MAX_THREADS 1024

struct item { uint32_t type, size; uint64_t flags; };

struct thread {
	const uint8_t *snap;            /* thread_snapshot_v2 and later: the fields read are in all of them */
	const char *name, *queue;
	int name_len, queue_len;
	const uint8_t *user, *kern;     /* uint64_t frames, unaligned */
	uint32_t n_user, n_kern;
};

struct waits {
	const uint8_t *p;
	uint32_t n, elem;
};

static uint64_t
u64(const uint8_t *p, size_t off)
{
	uint64_t v;
	memcpy(&v, p + off, sizeof(v));
	return v;
}

static uint32_t
u32(const uint8_t *p, size_t off)
{
	uint32_t v;
	memcpy(&v, p + off, sizeof(v));
	return v;
}

static int16_t
i16(const uint8_t *p, size_t off)
{
	int16_t v;
	memcpy(&v, p + off, sizeof(v));
	return v;
}

static void
uuid_text(char out[37], const uint8_t *u)
{
	snprintf(out, 37, "%02X%02X%02X%02X-%02X%02X-%02X%02X-%02X%02X-%02X%02X%02X%02X%02X%02X", u[0], u[1], u[2], u[3],
	    u[4], u[5], u[6], u[7], u[8], u[9], u[10], u[11], u[12], u[13], u[14], u[15]);
}

/* block_hint_t (osfmk/kern/block_hint.h) */
static const char *const wait_types[] = {
	"none", "kernel mutex", "mach_msg receive", "mach_msg receive on port set", "mach_msg send",
	"mach_msg send in transit", "semaphore", "kernel rwlock read", "kernel rwlock write", "kernel rwlock upgrade",
	"ulock", "pthread mutex", "pthread rwlock read", "pthread rwlock write", "pthread condvar", "parked workqueue",
	"workloop sync wait", "waitpid", "sleep with inheritor", "eventlink", "compressor",
};

/* STACKSHOT_WAITOWNER_*, from UINT64_MAX - 1 down */
static const char *const wait_owners[] = {
	"kernel", "port locked", "port set locked", "in transit", "mutex spinning", "thread requested", "suspended",
};

static void
print_wait(FILE *out, const uint8_t *w, uint32_t elem)
{
	uint64_t owner = u64(w, 0), context = u64(w, 16);
	uint8_t type = w[24];
	fprintf(out, "    wait: %s", type < sizeof(wait_types) / sizeof(*wait_types) ? wait_types[type] : "type");
	if (type >= sizeof(wait_types) / sizeof(*wait_types)) {
		fprintf(out, " %u", type);
	}
	fprintf(out, " 0x%llx", (unsigned long long)context);
	if (owner >= UINT64_MAX - 7 && owner < UINT64_MAX && type != 17) {
		fprintf(out, ", %s", wait_owners[UINT64_MAX - 1 - owner]);
	} else if (owner == 0) {
		fprintf(out, ", owner unknown");
	} else if (type == 4 || type == 6 || type == 17) {
		fprintf(out, ", pid %lld", (long long)owner);       /* waitpid: -1 any child, other negatives a group */
	} else if (type == 2 || type == 5) {
		fprintf(out, ", port 0x%llx", (unsigned long long)owner);
	} else {
		fprintf(out, ", owner thread 0x%llx", (unsigned long long)owner);
	}
	if (elem >= 31 && (u32(w, 27) & 1)) {
		fprintf(out, ", special reply port");
	}
	fprintf(out, "\n");
}

static void
print_turnstile(FILE *out, const uint8_t *t)
{
	uint64_t context = u64(t, 8), flags = u64(t, 18);
	fprintf(out, "    turnstile: ");
	if (flags & 0x10) {
		fprintf(out, "blocked on pid %llu", (unsigned long long)context);
	} else if (flags & 0x4) {
		fprintf(out, "blocked on workqueue 0x%llx", (unsigned long long)context);
	} else if (flags & 0x8) {
		fprintf(out, "blocked on thread 0x%llx", (unsigned long long)context);
	} else {
		fprintf(out, "inheritor unknown (flags 0x%llx)", (unsigned long long)flags);
	}
	fprintf(out, ", hops %u, priority %u\n", t[17], t[16]);
}

static void
print_frames(FILE *out, int user, const uint8_t *f, uint32_t n, const uint8_t *images, uint32_t n_images,
    uint64_t cache_base)
{
	for (uint32_t i = 0; i < n; i++) {
		uint64_t a = u64(f, i * 8ull);
		fprintf(out, "    %s %2u 0x%016llx", user ? "user" : "kern", i, (unsigned long long)a);
		if (!user) {
			fprintf(out, "\n");
			continue;
		}
		if (cache_base != 0 && a >= cache_base && a < SHARED_REGION_END) {
			fprintf(out, "  cache+0x%llx\n", (unsigned long long)(a - cache_base));
			continue;
		}
		/* ponytail: the nearest image below the address (load info has no sizes); 256 MiB bounds a stray pointer */
		const uint8_t *best = NULL;
		for (uint32_t k = 0; k < n_images; k++) {
			uint64_t base = u64(images, k * 24ull);
			if (base != 0 && base <= a && (best == NULL || base > u64(best, 0))) {
				best = images + k * 24ull;
			}
		}
		if (best != NULL && a - u64(best, 0) < (256u << 20)) {
			char u[37];
			uuid_text(u, best + 8);
			fprintf(out, "  %s+0x%llx\n", u, (unsigned long long)(a - u64(best, 0)));
		} else {
			fprintf(out, "\n");
		}
	}
}

static uint64_t
cache_first_mapping(const uint8_t *d, uint32_t size)
{
	if (size >= 40) {
		return u64(d, 32);      /* sharedCacheSlidFirstMapping */
	}
	return size >= 32 ? u64(d, 24) : 0x180000000ull + u64(d, 0);
}

/* One stackshot buffer, as the kernel wrote it (KCDATA_BUFFER_BEGIN_STACKSHOT, not compressed). */
int
stack_print(FILE *out, const void *buffer, uint32_t size)
{
	static struct thread th[MAX_THREADS];
	const uint8_t *p = buffer, *end = p + size;
	const uint8_t *task = NULL, *images = NULL, *cache = NULL, *task_cache = NULL;
	uint32_t n_th = 0, n_images = 0, cache_size = 0, task_cache_size = 0, depth = 0, cur = MAX_THREADS;
	uint32_t containers[8];
	struct waits waits = { 0 }, turnstiles = { 0 };
	struct item it;

	if (size < sizeof(it) || (memcpy(&it, p, sizeof(it)), it.type != KC_BEGIN_STACKSHOT)) {
		fprintf(out, "stack: not a stackshot (begins 0x%x)\n", size >= 4 ? u32(p, 0) : 0);
		return 1;
	}
	for (p += sizeof(it) + it.size; p + sizeof(it) <= end; p += sizeof(it) + it.size) {
		memcpy(&it, p, sizeof(it));
		const uint8_t *d = p + sizeof(it);
		if (it.size > (size_t)(end - d)) {
			fprintf(out, "stack: item 0x%x runs past the end\n", it.type);
			return 1;
		}
		if (it.type == KC_BUFFER_END) {
			break;
		}
		uint32_t type = it.type, count = 1, elem = it.size, outer = depth ? containers[depth - 1] : 0;
		if (type == KC_ARRAY || (type & ~0xfu) == KC_ARRAY_PAD0) {
			type = (uint32_t)(it.flags >> 32);
			count = (uint32_t)it.flags;
			elem = count ? (it.size - (it.type == KC_ARRAY ? 0 : (it.type & 0xf))) / count : 0;
		}
		switch (type) {
		case KC_CONTAINER_BEGIN:
			if (depth == sizeof(containers) / sizeof(*containers)) {
				fprintf(out, "stack: containers nested too deep\n");
				return 1;
			}
			containers[depth++] = u32(d, 0);
			if (containers[depth - 1] == KC_THREAD && outer == KC_TASK) {
				cur = n_th < MAX_THREADS ? n_th++ : MAX_THREADS;
				if (cur < MAX_THREADS) {
					memset(&th[cur], 0, sizeof(th[cur]));
				}
			}
			break;
		case KC_CONTAINER_END:
			if (depth > 0 && containers[--depth] == KC_THREAD) {
				cur = MAX_THREADS;
			}
			break;
		case KC_TASK_SNAPSHOT:
			task = elem >= 120 ? d : NULL;
			break;
		case KC_LOADINFO64:
			if (outer == KC_TASK && elem == 24) {
				images = d;
				n_images = count;
			}
			break;
		case KC_SHAREDCACHE_LOADINFO:
			*(outer == KC_TASK ? &task_cache : &cache) = d;
			*(outer == KC_TASK ? &task_cache_size : &cache_size) = it.size;
			break;
		case KC_THREAD_WAITINFO:
		case KC_TURNSTILEINFO:
			if (outer == KC_TASK) {
				*(type == KC_THREAD_WAITINFO ? &waits : &turnstiles) = (struct waits){ d, count, elem };
			}
			break;
		default:
			if (cur == MAX_THREADS || outer != KC_THREAD) {
				break;
			}
			if (type == KC_THREAD_SNAPSHOT && elem >= 0x68) {
				th[cur].snap = d;
			} else if (type == KC_THREAD_NAME) {
				th[cur].name = (const char *)d;
				th[cur].name_len = (int)strnlen((const char *)d, it.size);
			} else if (type == KC_DISPATCH_QUEUE_LABEL) {
				th[cur].queue = (const char *)d;
				th[cur].queue_len = (int)strnlen((const char *)d, it.size);
			} else if (type == KC_USER_STACKLR64 && elem == 8) {
				th[cur].user = d;
				th[cur].n_user = count;
			} else if (type == KC_KERN_STACKLR64 && elem == 8) {
				th[cur].kern = d;
				th[cur].n_kern = count;
			}
			break;
		}
	}
	if (task == NULL) {
		fprintf(out, "stack: no task in the stackshot (gone?)\n");
		return 1;
	}

	uint64_t tflags = u64(task, 8), cache_base = 0;
	const uint8_t *c = task_cache != NULL ? task_cache : cache;
	fprintf(out, "pid %d %.32s: %u threads, suspend count %u%s%s%s\n", (int)u32(task, 84), (const char *)task + 88,
	    n_th, u32(task, 56), tflags & 0x10 ? ", pid suspended" : "", tflags & 0x400 ? ", foreground" : "",
	    tflags & 0x200000 ? ", load info paged out" : "");
	if (c != NULL && !(tflags & 0x20000000)) {
		char u[37];
		uint32_t cs = c == task_cache ? task_cache_size : cache_size;
		cache_base = cache_first_mapping(c, cs);
		uuid_text(u, c + 8);
		fprintf(out, "cache %s at 0x%llx, slide 0x%llx\n", u, (unsigned long long)cache_base,
		    (unsigned long long)u64(c, 0));
	}
	for (uint32_t k = 0; k < n_images; k++) {
		char u[37];
		uuid_text(u, images + k * 24ull + 8);
		fprintf(out, "image %s at 0x%llx\n", u, (unsigned long long)u64(images, k * 24ull));
	}

	static const char *const states[] = { "wait", "suspended", "run", "uninterruptible", "terminating", "terminated",
		                              "", "idle" };
	for (uint32_t i = 0; i < n_th; i++) {
		struct thread *t = &th[i];
		if (t->snap == NULL) {
			continue;
		}
		uint64_t tid = u64(t->snap, 0), event = u64(t->snap, 8), cont = u64(t->snap, 16), sf = u64(t->snap, 64);
		uint32_t state = u32(t->snap, 88);
		fprintf(out, "thread 0x%llx", (unsigned long long)tid);
		if (t->name_len) {
			fprintf(out, " \"%.*s\"", t->name_len, t->name);
		}
		if (t->queue_len) {
			fprintf(out, " queue \"%.*s\"", t->queue_len, t->queue);
		}
		fprintf(out, ":");
		for (int b = 0; b < 8; b++) {
			if ((state & (1u << b)) && states[b][0]) {
				fprintf(out, " %s", states[b]);
			}
		}
		fprintf(out, ", priority %d (base %d)%s%s%s%s", i16(t->snap, 98), i16(t->snap, 96), sf & 0x2000 ? ", main" : "",
		    sf & 0x800 ? ", on core" : "", sf & 0x1000 ? ", idle worker" : "", sf & 0x80 ? ", truncated" : "");
		if (event) {
			fprintf(out, ", event 0x%llx", (unsigned long long)event);
		}
		if (cont) {
			fprintf(out, ", continuation 0x%llx", (unsigned long long)cont);
		}
		fprintf(out, "\n");
		for (uint32_t k = 0; k < waits.n; k++) {
			if (u64(waits.p, k * (size_t)waits.elem + 8) == tid) {
				print_wait(out, waits.p + k * (size_t)waits.elem, waits.elem);
			}
		}
		for (uint32_t k = 0; k < turnstiles.n; k++) {
			if (u64(turnstiles.p, k * (size_t)turnstiles.elem) == tid) {
				print_turnstile(out, turnstiles.p + k * (size_t)turnstiles.elem);
			}
		}
		print_frames(out, 1, t->user, t->n_user, images, n_images, cache_base);
		print_frames(out, 0, t->kern, t->n_kern, NULL, 0, 0);
	}
	if (n_th == MAX_THREADS) {
		fprintf(out, "stack: more than %d threads, the rest left out\n", MAX_THREADS);
	}
	return 0;
}

int
mstat_stack(int pid, unsigned count)
{
	int bad = 0;
	for (unsigned i = 0; i < count; i++) {
		if (i > 0) {
			sleep(1);
		}
		stackshot_config_t *c = stackshot_config_create();
		if (c == NULL) {
			return 1;
		}
		stackshot_config_set_pid(c, pid);
		stackshot_config_set_flags(c, SS_KCDATA_FORMAT | SS_GET_DQ | SS_SAVE_LOADINFO | SS_THREAD_WAITINFO |
		    SS_COLLECT_SHAREDCACHE_LAYOUT | SS_NO_IO_STATS);
		int err = stackshot_capture_with_config(c);
		printf("stack %u of %u\n", i + 1, count);
		if (err != 0) {
			printf("stack: stackshot of %d: %s%s\n", pid, strerror(err), err == 1 ? " (root needed)" : "");
			bad = 1;
		} else {
			bad |= stack_print(stdout, stackshot_config_get_stackshot_buffer(c), stackshot_config_get_stackshot_size(c));
		}
		fflush(stdout);
		stackshot_config_dealloc(c);
	}
	return bad;
}
