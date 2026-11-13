/*
 * mstat stack's kcdata reader against a stackshot built here, laid out as osfmk/kern/kern_stackshot.c writes one.
 *   clang -Wall -o /tmp/stack_test tools/mappled/tests/stack_test.c && /tmp/stack_test [OUT.kcdata]
 * OUT.kcdata keeps the buffer (for the kernel tree's tools/lldbmacros/kcdata.py). As root it also takes a real
 * stackshot of itself and checks that its main thread is there.
 */
#include "../mstat_stack.c"
#include <assert.h>

static uint8_t buf[8192];
static size_t len;

static void
put(uint32_t type, const void *data, uint32_t size, uint64_t flags)
{
	uint32_t pad = (16 - size % 16) % 16;
	struct item it = { type, size + pad, flags };
	memcpy(buf + len, &it, sizeof(it));
	memcpy(buf + len + sizeof(it), data, size);
	memset(buf + len + sizeof(it) + size, 0, pad);
	len += sizeof(it) + size + pad;
}

static void
put_struct(uint32_t type, const void *data, uint32_t size)
{
	put(type, data, size, ((16 - size % 16) % 16) | 0x80);
}

static void
put_array(uint32_t type, const void *data, uint32_t elem, uint32_t count)
{
	uint32_t pad = (16 - elem * count % 16) % 16;
	put(KC_ARRAY_PAD0 | pad, data, elem * count, (uint64_t)type << 32 | count);
}

static void
container(uint32_t marker, uint32_t type, uint64_t id)
{
	put(marker, &type, marker == KC_CONTAINER_BEGIN ? 4 : 0, id);
}

static void
build(void)
{
	static const uint8_t cache_uuid[16] = { 0x8f, 0x97, 0x04, 0x52, 0xc9, 0xe3, 0x3e, 0x88, 0xb1, 0x14, 0xfe, 0xb1, 0x86,
		                                0x4b, 0xc9, 0xe0 };
	uint8_t sc[40] = { 0 }, task[120] = { 0 }, images[48] = { 0 }, th[128] = { 0 }, name[64] = "main-ish";
	uint64_t v, user[3] = { 0x1bf0312a4, 0x100f04abc, 0x5000 }, kern[2] = { 0xfffffff007123456, 0xfffffff007200000 };
	uint8_t wait[31] = { 0 }, ts[28] = { 0 };

	put(KC_BEGIN_STACKSHOT, NULL, 0, 0);
	v = 0x3f030000;
	memcpy(sc, &v, 8);
	memcpy(sc + 8, cache_uuid, 16);
	v = 0x1bf030000;
	memcpy(sc + 24, &v, 8);
	memcpy(sc + 32, &v, 8);
	put_struct(KC_SHAREDCACHE_LOADINFO, sc, sizeof(sc));
	container(KC_CONTAINER_BEGIN, KC_TASK, 1234);
	v = 0x40000400;                 /* kTaskSharedRegionSystem, kTaskIsForeground */
	memcpy(task + 8, &v, 8);
	*(int32_t *)(task + 84) = 57;
	strcpy((char *)task + 88, "SpringBoard");
	put_struct(KC_TASK_SNAPSHOT, task, sizeof(task));
	v = 0x100f00000;
	memcpy(images, &v, 8);
	memset(images + 8, 0xab, 16);
	v = 0x101000000;
	memcpy(images + 24, &v, 8);
	memset(images + 32, 0xcd, 16);
	put_array(KC_LOADINFO64, images, 24, 2);

	container(KC_CONTAINER_BEGIN, KC_THREAD, 0x1c5);
	v = 0x1c5;
	memcpy(th, &v, 8);
	v = 0xfffffff0aabbcc00;
	memcpy(th + 8, &v, 8);
	v = 0x2000;                     /* kThreadMain */
	memcpy(th + 64, &v, 8);
	*(uint32_t *)(th + 88) = 0x1 | 0x8;
	*(int16_t *)(th + 96) = 47;
	*(int16_t *)(th + 98) = 47;
	put_struct(KC_THREAD_SNAPSHOT, th, sizeof(th));
	put(KC_THREAD_NAME, name, sizeof(name), 0);
	put(KC_DISPATCH_QUEUE_LABEL, "com.apple.main-thread", 22, 0);
	put_array(KC_USER_STACKLR64, user, 8, 3);
	put_array(KC_KERN_STACKLR64, kern, 8, 2);
	container(KC_CONTAINER_END, KC_THREAD, 0x1c5);

	container(KC_CONTAINER_BEGIN, KC_THREAD, 0x1d0);
	memset(th, 0, sizeof(th));
	v = 0x1d0;
	memcpy(th, &v, 8);
	*(uint32_t *)(th + 88) = 0x4;
	put_struct(KC_THREAD_SNAPSHOT, th, 0x68);       /* a thread_snapshot_v2 */
	container(KC_CONTAINER_END, KC_THREAD, 0x1d0);

	v = 0x1d0;                      /* owner */
	memcpy(wait, &v, 8);
	v = 0x1c5;                      /* waiter */
	memcpy(wait + 8, &v, 8);
	v = 0xfeed;
	memcpy(wait + 16, &v, 8);
	wait[24] = 11;                  /* kThreadWaitPThreadMutex */
	put_array(KC_THREAD_WAITINFO, wait, sizeof(wait), 1);
	v = 0x1c5;
	memcpy(ts, &v, 8);
	v = 0x1d0;
	memcpy(ts + 8, &v, 8);
	ts[16] = 47;
	ts[17] = 1;
	v = 0x8;                        /* STACKSHOT_TURNSTILE_STATUS_THREAD */
	memcpy(ts + 18, &v, 8);
	put_array(KC_TURNSTILEINFO, ts, sizeof(ts), 1);
	container(KC_CONTAINER_END, KC_TASK, 1234);
	put(KC_BUFFER_END, NULL, 0, 0);
}

static void
expect(const char *out, const char *line)
{
	if (strstr(out, line) == NULL) {
		fprintf(stderr, "missing: %s\n--- output:\n%s", line, out);
		exit(1);
	}
}

int
main(int argc, char **argv)
{
	char *out = NULL;
	size_t out_len = 0;
	FILE *f = open_memstream(&out, &out_len);

	build();
	if (argc > 1) {
		FILE *k = fopen(argv[1], "wb");
		fwrite(buf, 1, len, k);
		fclose(k);
	}
	assert(stack_print(f, buf, (uint32_t)len) == 0);
	fclose(f);
	expect(out, "pid 57 SpringBoard: 2 threads, suspend count 0, foreground\n");
	expect(out, "cache 8F970452-C9E3-3E88-B114-FEB1864BC9E0 at 0x1bf030000, slide 0x3f030000\n");
	expect(out, "thread 0x1c5 \"main-ish\" queue \"com.apple.main-thread\": wait uninterruptible, priority 47 (base 47), "
	    "main, event 0xfffffff0aabbcc00\n");
	expect(out, "    wait: pthread mutex 0xfeed, owner thread 0x1d0\n");
	expect(out, "    turnstile: blocked on thread 0x1d0, hops 1, priority 47\n");
	expect(out, "    user  0 0x00000001bf0312a4  cache+0x12a4\n");
	expect(out, "    user  1 0x0000000100f04abc  ABABABAB-ABAB-ABAB-ABAB-ABABABABABAB+0x4abc\n");
	expect(out, "    user  2 0x0000000000005000\n");
	expect(out, "    kern  1 0xfffffff007200000\n");
	expect(out, "thread 0x1d0: run, priority 0 (base 0)\n");
	free(out);

	f = open_memstream(&out, &out_len);
	assert(stack_print(f, buf, 40) == 1);   /* cut short */
	fclose(f);
	free(out);
	printf("synthetic stackshot: ok\n");

	if (geteuid() == 0) {
		stackshot_config_t *c = stackshot_config_create();
		stackshot_config_set_pid(c, getpid());
		stackshot_config_set_flags(c, SS_KCDATA_FORMAT | SS_GET_DQ | SS_SAVE_LOADINFO | SS_THREAD_WAITINFO);
		int err = stackshot_capture_with_config(c);
		assert(err == 0);
		f = open_memstream(&out, &out_len);
		assert(stack_print(f, stackshot_config_get_stackshot_buffer(c), stackshot_config_get_stackshot_size(c)) == 0);
		fclose(f);
		expect(out, ", main");
		expect(out, "    user  0 ");
		printf("%sstackshot of this process: ok\n", out);
		stackshot_config_dealloc(c);
	}
	return 0;
}
