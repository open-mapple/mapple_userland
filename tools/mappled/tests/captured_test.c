/*
 * mapple-captured's answers on the Mac: its handlers with real xpc dictionaries, CoreMedia's server calls and the
 * kernel's sysctl faked.
 *   cc -DMAPPLE_CAPTURED_TEST -fblocks -framework CoreFoundation -o /tmp/captured_test tests/captured_test.c && /tmp/captured_test
 */
#include "../mapple-captured.c"
#include <assert.h>
#include <string.h>

static int fake_kernel;                 /* level | 0x100 overheated */
static char posted[16][32];
static int nposted, associated, disassociated;

static OSStatus
fake_op(xpc_object_t msg, uint32_t *op)
{
	*op = (uint32_t)xpc_dictionary_get_uint64(msg, ".Operation");
	return *op ? 0 : -16151;
}

static OSStatus
fake_associate(__unused xpc_connection_t c, CFTypeRef obj, void *a, void *b, void *f, uint64_t *id)
{
	assert(obj == torch && !a && !b && !f);
	associated++;
	*id = 7;
	return 0;
}

static OSStatus
fake_disassociate(__unused xpc_connection_t c, uint64_t id)
{
	assert(id == 7);
	disassociated++;
	return 0;
}

static void *
fake_center(void)
{
	return (void *)1;
}

static OSStatus
fake_post(void *center, CFStringRef name, CFTypeRef obj, CFTypeRef payload, uint32_t flags)
{
	assert(center == (void *)1 && obj == torch && flags == 0 && CFGetTypeID(payload) == CFDictionaryGetTypeID());
	assert(CFDictionaryGetValue(payload, CFSTR("FlashlightValue")) != NULL);
	CFStringGetCString(name, posted[nposted++ % 16], 32, kCFStringEncodingUTF8);
	return 0;
}

static int
fake_get(int *v)
{
	*v = fake_kernel;
	return 0;
}

static int
fake_set(int v)
{
	if (fake_kernel & OVERHEATED) {
		return -1;
	}
	assert(v >= 0 && v <= 3);
	fake_kernel = v;
	return 0;
}

static xpc_object_t
request(uint32_t op)
{
	xpc_object_t m = xpc_dictionary_create(NULL, NULL, 0);

	xpc_dictionary_set_uint64(m, ".Operation", op);
	return m;
}

static OSStatus
ask(OSStatus (*handler)(xpc_connection_t, xpc_object_t, xpc_object_t), xpc_object_t m, xpc_object_t *reply)
{
	*reply = xpc_dictionary_create(NULL, NULL, 0);
	return handler(NULL, m, *reply);
}

static OSStatus
set_level(double level)
{
	xpc_object_t m = request(OP('s', 'l', 'v', 'l')), r;
	OSStatus err;

	xpc_dictionary_set_double(m, ".payload", level);
	err = ask(flashlight_reply, m, &r);
	dispatch_sync(torch_queue, ^{});
	return err;
}

int
main(void)
{
	xpc_object_t r;

	cm.op = fake_op;
	cm.associate = fake_associate;
	cm.disassociate = fake_disassociate;
	cm.center = fake_center;
	cm.post = fake_post;
	cm.torch_get = fake_get;
	cm.torch_set = fake_set;
	torch = CFDataCreateMutable(NULL, 0);
	torch_queue = dispatch_queue_create("test", DISPATCH_QUEUE_SERIAL);

	assert(ask(flashlight_reply, request(OP('c', 'r', 'e', 'a')), &r) == 0 && xpc_dictionary_get_uint64(r, ".objectID") == 7);
	assert(associated == 1);
	assert(ask(flashlight_reply, request(OP('a', 'v', 'a', 'i')), &r) == 0 && xpc_dictionary_get_bool(r, ".payload"));
	assert(ask(flashlight_reply, request(OP('h', 'e', 'a', 't')), &r) == 0 && !xpc_dictionary_get_bool(r, ".payload"));

	/* iOS's four steps to the kernel's 1-3, read back as set, each followed by FlashlightLevel */
	const double steps[] = { 0.25, 0.5, 0.85, 1.0, FLT_MAX, 0 };
	const int want[] = { 1, 2, 3, 3, 3, 0 };
	for (int i = 0; i < 6; i++) {
		nposted = 0;
		assert(set_level(steps[i]) == 0 && fake_kernel == want[i]);
		assert(nposted == 1 && strcmp(posted[0], "FlashlightLevel") == 0);
		assert(ask(flashlight_reply, request(OP('g', 'l', 'v', 'l')), &r) == 0);
		assert(xpc_dictionary_get_double(r, ".payload") == (steps[i] >= FLT_MAX ? 1.0 : steps[i]));
	}

	/* overheated: refused, the state posted; the kernel turning it off is seen by the watch */
	assert(set_level(0.5) == 0);
	fake_kernel = OVERHEATED;
	nposted = 0;
	assert(set_level(1.0) == REFUSED);
	dispatch_sync(torch_queue, ^{});
	assert(nposted == 3 && strcmp(posted[0], "FlashlightOverheated") == 0 && strcmp(posted[1], "FlashlightAvailable") == 0);
	assert(ask(flashlight_reply, request(OP('h', 'e', 'a', 't')), &r) == 0 && xpc_dictionary_get_bool(r, ".payload"));
	nposted = 0;
	dispatch_sync(torch_queue, ^{ torch_watch(); });
	assert(torch_level == 0 && nposted == 2);

	/* notf (no reply): the three states; pwr0 off; disp; unknown ops */
	fake_kernel = 0;
	nposted = 0;
	flashlight_noreply(NULL, request(OP('n', 'o', 't', 'f')));
	dispatch_sync(torch_queue, ^{});
	assert(nposted == 3 && strcmp(posted[2], "FlashlightLevel") == 0);
	assert(set_level(1.0) == 0 && fake_kernel == 3);
	assert(ask(flashlight_reply, request(OP('p', 'w', 'r', '0')), &r) == 0 && fake_kernel == 0);
	xpc_object_t d = request(OP('d', 'i', 's', 'p'));
	xpc_dictionary_set_uint64(d, ".objectID", 7);
	assert(ask(flashlight_reply, d, &r) == 0 && disassociated == 1);
	assert(ask(flashlight_reply, request(OP('x', 'x', 'x', 'x')), &r) == UNSUPPORTED);

	/* no cameras: an empty list at once; everything else an error */
	xpc_object_t c = request(OP('c', 's', 'c', 'r'));
	xpc_dictionary_set_int64(c, ".payload", 1);
	assert(ask(capturesource_reply, c, &r) == 0);
	assert(xpc_get_type(xpc_dictionary_get_value(r, ".Value")) == XPC_TYPE_ARRAY &&
	    xpc_array_get_count(xpc_dictionary_get_value(r, ".Value")) == 0);
	assert(ask(capturesource_reply, request(OP('c', 'r', 'e', 'a')), &r) == UNSUPPORTED);
	assert(ask(unsupported_reply, request(OP('c', 'r', 'e', 'a')), &r) == UNSUPPORTED);
	assert(ask(unsupported_reply, request(OP('d', 'c', 'c', 'm')), &r) == UNSUPPORTED);
	printf("captured_test: ok\n");
	return 0;
}
