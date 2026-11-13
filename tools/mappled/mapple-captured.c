/*
 * mapple-captured: what iOS's capture clients ask mediaserverd for while its recorder server is off (root-tweaks.py
 * gives this daemon the names). The torch, com.apple.coremedia.flashlight, in full: its level goes to the kernel's
 * sysctl kern.mapple_torch (0 off, 1-3 = 25, 37.5, 50 mA; 0x100 while overheated); no cameras: the capture source
 * answers an empty device list, the capture session and the deferred container answer every request with an error.
 * The protocol is CoreMedia's own server (FigXPCServerStart and friends, looked up at run time): it answers the
 * clients' hello and forwards the torch's notifications. Started by launchd on the first request; never exits.
 */
#include <CoreFoundation/CoreFoundation.h>
#include <dispatch/dispatch.h>
#include <dlfcn.h>
#include <float.h>
#include <stdint.h>
#include <stdio.h>
#include <sys/sysctl.h>
#include <xpc/xpc.h>

typedef int32_t OSStatus;

typedef struct {
	uint64_t version;
	OSStatus (*reply)(xpc_connection_t, xpc_object_t, xpc_object_t);
	void (*noreply)(xpc_connection_t, xpc_object_t);
	void *accept, *coalesce;
} server_callbacks;

static struct {
	OSStatus (*start)(const char *, const server_callbacks *, CFDictionaryRef, void **);
	OSStatus (*op)(xpc_object_t, uint32_t *);
	OSStatus (*associate)(xpc_connection_t, CFTypeRef, void *, void *, void *, uint64_t *);
	OSStatus (*disassociate)(xpc_connection_t, uint64_t);
	void *(*center)(void);
	OSStatus (*post)(void *, CFStringRef, CFTypeRef, CFTypeRef, uint32_t);
	int (*torch_get)(int *);
	int (*torch_set)(int);
} cm;

#define OP(a, b, c, d)  ((uint32_t)(a) << 24 | (uint32_t)(b) << 16 | (uint32_t)(c) << 8 | (uint32_t)(d))
#define UNSUPPORTED     (-12782)        /* CoreMedia's "not implemented", as the flashlight server answers */
#define REFUSED         (-12780)
#define OVERHEATED      0x100

static CFMutableDataRef torch;          /* the one served flashlight object (any heap CF object will do) */
static dispatch_queue_t torch_queue;
static double torch_level;              /* what iOS set, as it reads it back */
static int torch_state = -1;            /* the kernel's, as last seen */

static void
post(const char *name, CFTypeRef value)
{
	CFStringRef key = CFSTR("FlashlightValue"), n = CFStringCreateWithCString(NULL, name, kCFStringEncodingUTF8);
	CFDictionaryRef payload = CFDictionaryCreate(NULL, (const void **)&key, &value, 1, &kCFTypeDictionaryKeyCallBacks,
	    &kCFTypeDictionaryValueCallBacks);

	cm.post(cm.center(), n, torch, payload, 0);
	CFRelease(payload);
	CFRelease(n);
}

static void
post_level(void)
{
	float f = (float)torch_level;
	CFNumberRef n = CFNumberCreate(NULL, kCFNumberFloatType, &f);

	post("FlashlightLevel", n);
	CFRelease(n);
}

static void
post_state(void)
{
	int k = 0;

	post("FlashlightOverheated", cm.torch_get(&k) == 0 && (k & OVERHEATED) ? kCFBooleanTrue : kCFBooleanFalse);
	post("FlashlightAvailable", kCFBooleanTrue);
	post_level();
}

/* iOS's steps 0.25, 0.5, 0.85 and 1.0 (and FLT_MAX for the most) to the kernel's 1-3. */
static int
torch_step(double level)
{
	return level <= 0 ? 0 : level <= 0.25 + 1e-3 ? 1 : level <= 0.5 + 1e-3 ? 2 : 3;
}

/* Once a second while lit or overheated: the kernel may have turned it off (10 minutes, heat). */
static void
torch_watch(void)
{
	int k = 0;

	if (cm.torch_get(&k) != 0 || k == torch_state) {
		return;
	}
	if ((k & OVERHEATED) != (torch_state & OVERHEATED) || torch_state < 0) {
		post("FlashlightOverheated", (k & OVERHEATED) ? kCFBooleanTrue : kCFBooleanFalse);
	}
	if ((k & 0xff) == 0 && torch_level > 0) {
		torch_level = 0;
		post_level();
	}
	torch_state = k;
}

static OSStatus
flashlight_reply(xpc_connection_t conn, xpc_object_t msg, xpc_object_t reply)
{
	uint32_t op = 0;
	uint64_t id = 0;
	int k = 0;
	__block OSStatus err = 0;

	if (cm.op(msg, &op) != 0) {
		return UNSUPPORTED;
	}
	switch (op) {
	case OP('c', 'r', 'e', 'a'):
		err = cm.associate(conn, torch, NULL, NULL, NULL, &id);
		if (err == 0) {
			xpc_dictionary_set_uint64(reply, ".objectID", id);
		}
		return err;
	case OP('d', 'i', 's', 'p'):
		return cm.disassociate(conn, xpc_dictionary_get_uint64(msg, ".objectID"));
	case OP('a', 'v', 'a', 'i'):
		xpc_dictionary_set_bool(reply, ".payload", cm.torch_get(&k) == 0);
		return 0;
	case OP('h', 'e', 'a', 't'):
		xpc_dictionary_set_bool(reply, ".payload", cm.torch_get(&k) == 0 && (k & OVERHEATED));
		return 0;
	case OP('p', 'w', 'r', '1'):
		return 0;
	case OP('g', 'l', 'v', 'l'):
		dispatch_sync(torch_queue, ^{ xpc_dictionary_set_double(reply, ".payload", torch_level); });
		return 0;
	case OP('p', 'w', 'r', '0'):
	case OP('s', 'l', 'v', 'l'): {
		double want = op == OP('s', 'l', 'v', 'l') ? xpc_dictionary_get_double(msg, ".payload") : 0;

		dispatch_sync(torch_queue, ^{
			if (cm.torch_set(torch_step(want)) != 0) {
				err = REFUSED;
				return;
			}
			torch_level = want >= FLT_MAX ? 1.0 : want < 0 ? 0 : want > 1 ? 1 : want;
			post_level();
		});
		if (err != 0) {
			dispatch_async(torch_queue, ^{ post_state(); });
		}
		return err;
	}
	default:
		return UNSUPPORTED;
	}
}

static void
flashlight_noreply(__unused xpc_connection_t conn, xpc_object_t msg)
{
	uint32_t op = 0;

	if (cm.op(msg, &op) == 0 && op == OP('n', 'o', 't', 'f')) {
		dispatch_async(torch_queue, ^{ post_state(); });
	}
}

static OSStatus
capturesource_reply(__unused xpc_connection_t conn, xpc_object_t msg, xpc_object_t reply)
{
	uint32_t op = 0;

	if (cm.op(msg, &op) == 0 && op == OP('c', 's', 'c', 'r')) {
		xpc_object_t none = xpc_array_create(NULL, 0);

		xpc_dictionary_set_value(reply, ".Value", none);
		xpc_release(none);
		return 0;
	}
	return UNSUPPORTED;
}

static OSStatus
unsupported_reply(__unused xpc_connection_t conn, __unused xpc_object_t msg, __unused xpc_object_t reply)
{
	return UNSUPPORTED;
}

#ifndef MAPPLE_CAPTURED_TEST
static void
ignored(__unused xpc_connection_t conn, __unused xpc_object_t msg)
{
}

static int
kernel_get(int *value)
{
	size_t len = sizeof(*value);

	return sysctlbyname("kern.mapple_torch", value, &len, NULL, 0);
}

static int
kernel_set(int value)
{
	return sysctlbyname("kern.mapple_torch", NULL, NULL, &value, sizeof(value));
}

int
main(void)
{
	static const struct {
		const char *name;
		server_callbacks cb;
	} servers[] = {
		{ "com.apple.coremedia.flashlight", { 1, flashlight_reply, flashlight_noreply, NULL, NULL } },
		{ "com.apple.coremedia.capturesource", { 1, capturesource_reply, ignored, NULL, NULL } },
		{ "com.apple.coremedia.capturesession", { 1, unsupported_reply, ignored, NULL, NULL } },
		{ "com.apple.coremedia.capture.deferred.containermanager.xpc", { 1, unsupported_reply, ignored, NULL, NULL } },
	};
	void *lib = dlopen("/System/Library/Frameworks/CoreMedia.framework/CoreMedia", RTLD_NOW);
	dispatch_source_t timer;
	int k = 0;

	cm.start = lib ? dlsym(lib, "FigXPCServerStart") : NULL;
	cm.op = lib ? dlsym(lib, "FigXPCMessageGetOpCode") : NULL;
	cm.associate = lib ? dlsym(lib, "FigXPCServerAssociateObjectWithConnection") : NULL;
	cm.disassociate = lib ? dlsym(lib, "FigXPCServerDisassociateObjectWithConnection") : NULL;
	cm.center = lib ? dlsym(lib, "CMNotificationCenterGetDefaultLocalCenter") : NULL;
	cm.post = lib ? dlsym(lib, "CMNotificationCenterPostNotification") : NULL;
	cm.torch_get = kernel_get;
	cm.torch_set = kernel_set;
	if (!cm.start || !cm.op || !cm.associate || !cm.disassociate || !cm.center || !cm.post) {
		fprintf(stderr, "mapple-captured: CoreMedia's server calls not found\n");
		return 1;
	}
	torch = CFDataCreateMutable(NULL, 0);
	torch_queue = dispatch_queue_create("mapple-captured.torch", DISPATCH_QUEUE_SERIAL);
	for (size_t i = 0; i < sizeof(servers) / sizeof(servers[0]); i++) {
		void *server = NULL;
		OSStatus err = cm.start(servers[i].name, &servers[i].cb, NULL, &server);

		fprintf(stderr, "mapple-captured: %s %s (%d)\n", servers[i].name, err == 0 ? "served" : "NOT served", err);
	}
	fprintf(stderr, "mapple-captured: the torch %s\n", kernel_get(&k) == 0 ? "is the kernel's" : "has no kernel side");
	timer = dispatch_source_create(DISPATCH_SOURCE_TYPE_TIMER, 0, 0, torch_queue);
	dispatch_source_set_timer(timer, dispatch_time(DISPATCH_TIME_NOW, NSEC_PER_SEC), NSEC_PER_SEC, NSEC_PER_SEC / 10);
	dispatch_source_set_event_handler(timer, ^{ torch_watch(); });
	dispatch_resume(timer);
	dispatch_main();
}
#endif
