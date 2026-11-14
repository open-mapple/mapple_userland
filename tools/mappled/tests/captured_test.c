/*
 * mapple-captured's answers on the Mac: its handlers with real xpc dictionaries; CoreMedia's server calls, CMCapture's
 * classes, CoreAnimation's image queue and the kernel's sysctl faked; IOSurface, ImageIO and the test pattern real.
 *   cc -DMAPPLE_CAPTURED_TEST -fblocks -framework CoreFoundation -framework CoreGraphics -framework CoreMedia \
 *     -framework ImageIO -framework IOSurface -lobjc -o /tmp/captured_test tests/captured_test.c && /tmp/captured_test
 */
#include "../mapple-captured.c"
#include <assert.h>
#include <string.h>
#include <unistd.h>

static int fake_kernel;                 /* level | 0x100 overheated */
static char posted[64][48];
static CFDictionaryRef payloads[64];
static int nposted, associated, disassociated;

static struct {
	CFTypeRef object;
	void *refcon;
	void (*destructor)(void *);
	void *serializer;
} served[8];

static OSStatus
fake_op(xpc_object_t msg, uint32_t *op)
{
	*op = (uint32_t)xpc_dictionary_get_uint64(msg, ".Operation");
	return *op ? 0 : -16151;
}

static OSStatus
fake_associate(__unused xpc_connection_t c, CFTypeRef obj, void *refcon, void *destructor, void *serializer, uint64_t *id)
{
	assert(obj == torch ? !refcon && !destructor && !serializer : obj == cam.source ? !refcon : refcon && serializer);
	associated++;
	*id = (uint64_t)associated;
	served[associated] = (typeof(served[0])){ obj, refcon, destructor, serializer };
	return 0;
}

static OSStatus
fake_disassociate(__unused xpc_connection_t c, uint64_t id)
{
	assert(id > 0 && id < 8 && served[id].object);
	if (served[id].destructor) {
		served[id].destructor(served[id].refcon);
	}
	served[id].object = NULL;
	disassociated++;
	return 0;
}

static OSStatus
fake_lookup(__unused xpc_connection_t c, uint64_t id, CFTypeRef *obj, void **refcon)
{
	if (id == 0 || id >= 8 || !served[id].object) {
		return -1;
	}
	*obj = CFRetain(served[id].object);
	*refcon = served[id].refcon;
	return 0;
}

static OSStatus
fake_copy_string(xpc_object_t msg, const char *key, CFStringRef *out)
{
	const char *s = xpc_dictionary_get_string(msg, key);

	*out = s ? CFStringCreateWithCString(NULL, s, kCFStringEncodingUTF8) : NULL;
	return s ? 0 : -1;
}

static OSStatus
fake_copy_data(xpc_object_t msg, const char *key, CFDataRef *out)
{
	size_t n = 0;
	const void *p = xpc_dictionary_get_data(msg, key, &n);

	*out = p ? CFDataCreate(NULL, p, (CFIndex)n) : NULL;
	return p ? 0 : -1;
}

static OSStatus
fake_copy_object(xpc_object_t msg, const char *key, CFTypeRef *out)
{
	xpc_object_t v = xpc_dictionary_get_value(msg, key);

	*out = v ? num_int(xpc_int64_get_value(v)) : NULL;
	return 0;
}

static OSStatus
fake_set_object(xpc_object_t msg, const char *key, CFTypeRef value)
{
	int64_t v = 0;

	assert(CFGetTypeID(value) == CFNumberGetTypeID());
	CFNumberGetValue(value, kCFNumberSInt64Type, &v);
	xpc_dictionary_set_int64(msg, key, v);
	return 0;
}

static CFDictionaryRef last_set_dictionary;

static OSStatus
fake_set_dictionary(xpc_object_t msg, const char *key, CFDictionaryRef d)
{
	if (last_set_dictionary) {
		CFRelease(last_set_dictionary);
	}
	last_set_dictionary = CFRetain(d);
	xpc_dictionary_set_uint64(msg, key, (uint64_t)CFDictionaryGetCount(d));
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
	assert(center == (void *)1 && flags == 0 && CFGetTypeID(payload) == CFDictionaryGetTypeID());
	if (obj == torch) {
		assert(CFDictionaryGetValue(payload, CFSTR("FlashlightValue")) != NULL);
	}
	assert(nposted < 64);
	CFStringGetCString(name, posted[nposted], sizeof(posted[0]), kCFStringEncodingUTF8);
	payloads[nposted++] = CFRetain(payload);
	return 0;
}

static xpc_object_t
fake_surface_xpc(IOSurfaceRef s)
{
	return xpc_uint64_create(IOSurfaceGetAllocSize(s));
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

static CFDictionaryRef encoded_format;

static xpc_object_t
fake_format_encode(CFDictionaryRef format)
{
	encoded_format = CFRetain(format);
	return xpc_dictionary_create(NULL, NULL, 0);
}

/* A configuration as FigCaptureSessionConfiguration encodes it: Camera.app's photo sink, and a preview sink if asked. */
static xpc_object_t
configuration(int64_t id, bool preview)
{
	xpc_object_t c = xpc_dictionary_create(NULL, NULL, 0), list = xpc_array_create(NULL, 0);
	const char *sinks[][2] = { { "FigCaptureIrisSinkConfiguration", "PhotoSink" },
	    { "FigCaptureVideoPreviewSinkConfiguration", "PreviewSink" } };

	for (int i = 0; i < (preview ? 2 : 1); i++) {
		xpc_object_t conn = xpc_dictionary_create(NULL, NULL, 0), sink = xpc_dictionary_create(NULL, NULL, 0);

		xpc_dictionary_set_string(sink, "class", sinks[i][0]);
		xpc_dictionary_set_string(sink, "sinkID", sinks[i][1]);
		xpc_dictionary_set_string(conn, "class", "FigCaptureConnectionConfiguration");
		xpc_dictionary_set_int64(conn, "sourceIndex", 0);
		xpc_dictionary_set_value(conn, "sink", sink);
		xpc_array_append_value(list, conn);
	}
	xpc_dictionary_set_int64(c, "configurationID", id);
	xpc_dictionary_set_value(c, "connections", list);
	return c;
}

static int
fake_still_decode(CFDataRef archive, struct still *st)
{
	assert(CFDataGetLength(archive) == 4);
	st->id = 42;
	st->width = PHOTO_W;
	st->height = PHOTO_H;
	st->request = CFRetain(CFSTR("request-1"));
	return 0;
}

static int
fake_prepared_decode(__unused xpc_object_t x, int64_t *settings)
{
	*settings = 41;
	return 0;
}

static int queue_open, queue_closed, inserted;
static uint64_t last_image;

static void *
fake_preview_open(int w, int h, uint32_t *slot)
{
	assert(w == PREVIEW_W && h == PREVIEW_H);
	queue_open++;
	*slot = 77;
	return CFDataCreateMutable(NULL, 0);
}

static uint64_t
fake_preview_register(__unused void *q, IOSurfaceRef s)
{
	assert(IOSurfaceGetWidth(s) == PREVIEW_W && IOSurfaceGetPixelFormat(s) == FOURCC("BGRA"));
	return 1000 + (uint64_t)queue_open * 10 + (uint64_t)inserted;
}

static void
fake_preview_insert(void *q, uint64_t image, image_released done, void *info)
{
	inserted++;
	last_image = image;
	done(q, image, info);
}

static void
fake_preview_close(void *q, uint32_t slot)
{
	assert(slot == 77);
	queue_closed++;
	CFRelease(q);
}

static xpc_object_t
request(uint32_t op)
{
	xpc_object_t m = xpc_dictionary_create(NULL, NULL, 0);

	xpc_dictionary_set_uint64(m, ".Operation", op);
	return m;
}

static xpc_object_t
on(uint32_t op, uint64_t object)
{
	xpc_object_t m = request(op);

	xpc_dictionary_set_uint64(m, ".objectID", object);
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

/* The index of the first notification called NAME from FROM on, or -1. */
static int
find(const char *name, int from)
{
	for (int i = from; i < nposted; i++) {
		if (strcmp(posted[i], name) == 0) {
			return i;
		}
	}
	return -1;
}

static int64_t
number(CFDictionaryRef d, const char *key)
{
	CFStringRef k = CFStringCreateWithCString(NULL, key, kCFStringEncodingUTF8);
	CFNumberRef n = CFDictionaryGetValue(d, k);
	int64_t v = INT64_MIN;

	if (n) {
		CFNumberGetValue(n, kCFNumberSInt64Type, &v);
	}
	CFRelease(k);
	return v;
}

static void
torch_tests(void)
{
	xpc_object_t r;

	assert(ask(flashlight_reply, request(OP('c', 'r', 'e', 'a')), &r) == 0 && xpc_dictionary_get_uint64(r, ".objectID") == 1);
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
	assert(ask(flashlight_reply, on(OP('d', 'i', 's', 'p'), 1), &r) == 0 && disassociated == 1);
	assert(ask(flashlight_reply, request(OP('x', 'x', 'x', 'x')), &r) == UNSUPPORTED);
}

/* The camera list: one back camera for video, none for audio; its properties. */
static void
source_tests(void)
{
	xpc_object_t r, c, list, source;
	CFNumberRef n;
	int64_t v;

	c = request(OP('c', 's', 'r', 'c'));
	xpc_dictionary_set_int64(c, ".payload", 2);
	assert(ask(capturesource_reply, c, &r) == 0 && xpc_array_get_count(xpc_dictionary_get_value(r, ".Value")) == 0);
	xpc_dictionary_set_int64(c, ".payload", 1);
	assert(ask(capturesource_reply, c, &r) == 0);
	list = xpc_dictionary_get_value(r, ".Value");
	assert(xpc_get_type(list) == XPC_TYPE_ARRAY && xpc_array_get_count(list) == 1);
	source = xpc_array_get_value(list, 0);
	assert(xpc_dictionary_get_uint64(source, "objectID") == (uint64_t)associated && served[associated].object == cam.source);
	assert(xpc_dictionary_get_uint64(source, "sourceType") == 1);
	assert(xpc_dictionary_get_uint64(source, "attributesDictionary") == (uint64_t)CFDictionaryGetCount(cam.attributes));
	assert(last_set_dictionary == cam.attributes);
	assert(number(cam.attributes, "Position") == 1 && number(cam.attributes, "DeviceType") == 2);
	assert(CFDictionaryGetValue(cam.attributes, CFSTR("Flash")) == kCFBooleanTrue);
	assert(CFEqual(CFDictionaryGetValue(cam.attributes, CFSTR("UniqueID")),
	    CFSTR("com.apple.avfoundation.avcapturedevice.built-in_video:0")));
	assert(xpc_array_get_count(xpc_dictionary_get_value(source, "formatsArray")) == 1);
	assert(number(encoded_format, "Width") == 4032 && number(encoded_format, "Height") == 3024);
	assert(number(encoded_format, "PreferredPreviewWidth") == 1504 && number(encoded_format, "PreferredPreviewHeight") == 1128);
	assert(number(encoded_format, "PixelFormatType") == FOURCC("420f"));
	n = CFDictionaryGetValue(encoded_format, CFSTR("VideoMaxFrameRate"));
	assert(n && CFNumberGetValue(n, kCFNumberSInt64Type, &v) && v == 30);
	assert(CFDictionaryGetValue(encoded_format, CFSTR("Name")) != NULL);
	assert(CFDictionaryGetValue(encoded_format, CFSTR("HDRSupported")) == kCFBooleanTrue);

	/* the active format is the format's encoding; set properties read back; unknown ones are not there */
	c = request(OP('.', 'c', 'p', 'y'));
	xpc_dictionary_set_string(c, ".PropertyName", "ActiveFormat");
	assert(ask(capturesource_reply, c, &r) == 0 && xpc_dictionary_get_value(r, ".Value") == cam.format);
	xpc_dictionary_set_string(c, ".PropertyName", "SourceToken");
	assert(ask(capturesource_reply, c, &r) == 0 && xpc_dictionary_get_int64(r, ".Value") == 1);
	xpc_dictionary_set_string(c, ".PropertyName", "VideoZoomFactor");
	assert(ask(capturesource_reply, c, &r) == NO_PROPERTY);
	xpc_object_t s = request(OP('.', 's', 'e', 't'));
	xpc_dictionary_set_string(s, ".PropertyName", "VideoZoomFactor");
	xpc_dictionary_set_int64(s, ".Value", 2);
	assert(ask(capturesource_reply, s, &r) == 0);
	assert(ask(capturesource_reply, c, &r) == 0 && xpc_dictionary_get_int64(r, ".Value") == 2);

	assert(ask(capturesource_reply, request(OP('c', 'c', 'l', 'k')), &r) == 0 && xpc_dictionary_get_int64(r, "clockType") == 1);
	assert(ask(capturesource_reply, request(OP('c', 't', 'c', 'c')), &r) == 0);
	assert(ask(capturesource_reply, request(OP('c', 'r', 'e', 'a')), &r) == UNSUPPORTED);
	assert(ask(capturesource_reply, request(OP('c', 's', 'c', 'r')), &r) == UNSUPPORTED);
	capturesource_noreply(NULL, request(OP('n', 'o', 't', 'f')));
}

static void
session_tests(void)
{
	xpc_object_t r, m;
	uint64_t id;
	struct session *s;
	int at;

	assert(ask(capturesession_reply, request(OP('c', 'r', 'e', 'a')), &r) == 0);
	id = xpc_dictionary_get_uint64(r, ".objectID");
	assert(id == (uint64_t)associated && served[id].serializer == (void *)session_serialize);
	s = served[id].refcon;

	/* without a preview sink (run 38's first configurations): running, no preview */
	nposted = 0;
	m = on(OP('c', 'n', 'f', 'g'), id);
	xpc_dictionary_set_value(m, ".payload", configuration(4, false));
	capturesession_noreply(NULL, m);
	capturesession_noreply(NULL, on(OP('b', 'r', 'u', 'n'), id));
	assert(find("DidStartRunning", 0) > 0 && find("ImageQueueUpdated", 0) < 0 && queue_open == 0 && !s->config.preview);
	capturesession_noreply(NULL, on(OP('e', 'r', 'u', 'n'), id));

	/* the configuration: committed with its ID and the host clock */
	nposted = 0;
	m = on(OP('c', 'n', 'f', 'g'), id);
	xpc_dictionary_set_value(m, ".payload", configuration(5, true));
	capturesession_noreply(NULL, m);
	assert(nposted == 1 && strcmp(posted[0], "ConfigurationCommitted") == 0);
	assert(number(payloads[0], "ConfigurationID") == 5 && number(payloads[0], "MasterClockType") == 1);
	assert(number(payloads[0], "ErrorStatus") == 0);
	capturesession_noreply(NULL, on(OP('c', 'n', 'f', 'g'), id));
	assert(nposted == 2 && number(payloads[1], "ErrorStatus") == REFUSED);

	/* running: the preview's slot announced, frames queued, the pattern drawn */
	nposted = 0;
	capturesession_noreply(NULL, on(OP('b', 'r', 'u', 'n'), id));
	assert(find("WillStartRunning", 0) == 0);
	at = find("ImageQueueUpdated", 0);
	assert(at > 0 && number(payloads[at], "ImageQueueSlot") == 77 && number(payloads[at], "ImageQueueWidth") == 1504);
	assert(CFEqual(CFDictionaryGetValue(payloads[at], CFSTR("SectionID")), CFSTR("PreviewSink")));
	assert(find("DidStartPreviewing", at) > at && find("DidStartRunning", at) > at);
	assert(find("ConfigurationDidBecomeLive", at) > find("DidStartRunning", at));
	assert(queue_open == 1);
	usleep(200000);
	dispatch_sync(cam.queue, ^{});
	assert(inserted >= 2);
	IOSurfaceLock(s->surface[0], kIOSurfaceLockReadOnly, NULL);
	uint32_t *px = IOSurfaceGetBaseAddress(s->surface[0]);
	assert(px[0] == 0xffffffff && px[PREVIEW_W - 1] == 0xff000000);   /* white bar on the left, black on the right */
	IOSurfaceUnlock(s->surface[0], kIOSurfaceLockReadOnly, NULL);

	/* a photo (AVCapturePhotoOutput's iris sink): prepared, then the JPEG in an IOSurface */
	nposted = 0;
	m = on(OP('i', 's', 'i', 'p'), id);
	xpc_dictionary_set_string(m, "FigCaptureSessionMsgParam_NodeID", "PhotoSink");
	capturesession_noreply(NULL, m);
	assert(nposted == 1 && strcmp(posted[0], "IrisPreparationComplete") == 0 && number(payloads[0], "SettingsID") == 41);
	nposted = 0;
	m = on(OP('i', 's', 'i', 'c'), id);
	xpc_dictionary_set_string(m, "FigCaptureSessionMsgParam_NodeID", "PhotoSink");
	xpc_dictionary_set_data(m, ".payload", "abcd", 4);
	capturesession_noreply(NULL, m);
	dispatch_sync(cam.photos, ^{});
	at = find("IrisWillBeginCapture", 0);
	assert(at == 0 && number(payloads[0], "StillWidth") == 4032 && number(payloads[0], "SettingsID") == 42);
	assert(CFArrayGetCount(CFDictionaryGetValue(payloads[0], CFSTR("PhotoManifest"))) == 1);
	assert(find("IrisWillCaptureStillImage", 0) == 1 && find("IrisDidCaptureStillImage", 0) == 2);
	at = find("IrisStillImageComplete", 0);
	assert(at == 3 && number(payloads[at], "ErrorStatus") == 0 && number(payloads[at], "SettingsID") == 42);
	assert(CFEqual(CFDictionaryGetValue(payloads[at], CFSTR("SectionID")), CFSTR("PhotoSink")));
	assert(CFEqual(CFDictionaryGetValue(payloads[at], CFSTR("CaptureRequestIdentifier")), CFSTR("request-1")));
	assert(number(payloads[at], "PhotoCodec") == FOURCC("jpeg") && number(payloads[at], "PhotoFileType") == FOURCC("jfif"));
	IOSurfaceRef jpeg = (IOSurfaceRef)CFDictionaryGetValue(payloads[at], CFSTR("Surface"));
	int64_t size = number(payloads[at], "SurfaceSize");
	assert(jpeg && size > 1000 && (size_t)size <= IOSurfaceGetAllocSize(jpeg));
	IOSurfaceLock(jpeg, kIOSurfaceLockReadOnly, NULL);
	const uint8_t *b = IOSurfaceGetBaseAddress(jpeg);
	assert(b[0] == 0xff && b[1] == 0xd8 && b[size - 2] == 0xff && b[size - 1] == 0xd9);
	IOSurfaceUnlock(jpeg, kIOSurfaceLockReadOnly, NULL);

	/* the surface leaves as its XPC object, the rest as the payload */
	xpc_object_t out = xpc_dictionary_create(NULL, NULL, 0);
	assert(session_serialize(CFSTR("IrisStillImageComplete"), s->object, s, payloads[at], out, NULL) == 2);
	assert(xpc_dictionary_get_uint64(out, "Surface_Serialized") == IOSurfaceGetAllocSize(jpeg));
	assert(!CFDictionaryContainsKey(last_set_dictionary, CFSTR("Surface")));
	assert(CFDictionaryContainsKey(last_set_dictionary, CFSTR("SurfaceSize")));
	assert(session_serialize(CFSTR("DidStartRunning"), s->object, s, payloads[0], out, NULL) == 0);

	/* the still image output's request, the same photo under its own names */
	nposted = 0;
	m = on(OP('s', 't', 'i', 'l'), id);
	xpc_dictionary_set_data(m, ".payload", "abcd", 4);
	capturesession_noreply(NULL, m);
	dispatch_sync(cam.photos, ^{});
	assert(find("IrisWillBeginCapture", 0) < 0 && find("WillCaptureStillImage", 0) == 0);
	at = find("StillImageComplete", 0);
	assert(at == 2 && number(payloads[at], "ErrorStatus") == 0);

	/* refused: brackets and recording say so; moment capture is only logged */
	nposted = 0;
	capturesession_noreply(NULL, on(OP('p', 'b', 'r', 'k'), id));
	capturesession_noreply(NULL, on(OP('b', 'r', 'e', 'c'), id));
	capturesession_noreply(NULL, on(OP('i', 'b', 'm', 'c'), id));
	assert(nposted == 2 && strcmp(posted[0], "BracketPreparationComplete") == 0 && number(payloads[0], "ErrorStatus") == UNSUPPORTED);
	assert(strcmp(posted[1], "DidStopRecording") == 0 && CFDictionaryGetValue(payloads[1], CFSTR("RecordingSucceeded")) == kCFBooleanFalse);

	/* properties, a session that is not there, unknown ops */
	m = on(OP('.', 's', 'e', 't'), id);
	xpc_dictionary_set_string(m, ".PropertyName", "Muted");
	xpc_dictionary_set_int64(m, ".Value", 1);
	assert(ask(capturesession_reply, m, &r) == 0);
	m = on(OP('.', 'c', 'p', 'y'), id);
	xpc_dictionary_set_string(m, ".PropertyName", "Muted");
	assert(ask(capturesession_reply, m, &r) == 0 && xpc_dictionary_get_int64(r, ".Value") == 1);
	assert(ask(capturesession_reply, on(OP('.', 'c', 'p', 'y'), 7), &r) == REFUSED);
	assert(ask(capturesession_reply, on(OP('b', 'r', 'u', 'n'), id), &r) == UNSUPPORTED);
	assert(ask(unsupported_reply, request(OP('d', 'c', 'c', 'm')), &r) == UNSUPPORTED);

	/* stopped, then gone */
	nposted = 0;
	capturesession_noreply(NULL, on(OP('e', 'r', 'u', 'n'), id));
	assert(nposted == 2 && strcmp(posted[0], "DidStopPreviewing") == 0 && strcmp(posted[1], "DidStopRunning") == 0);
	int before = inserted;
	usleep(100000);
	dispatch_sync(cam.queue, ^{});
	assert(inserted == before);
	assert(ask(capturesession_reply, on(OP('d', 'i', 's', 'p'), id), &r) == 0 && queue_closed == 1);
	(void)last_image;
}

int
main(void)
{
	cm.op = fake_op;
	cm.associate = fake_associate;
	cm.disassociate = fake_disassociate;
	cm.lookup = fake_lookup;
	cm.copy_string = fake_copy_string;
	cm.copy_data = fake_copy_data;
	cm.copy_object = fake_copy_object;
	cm.set_object = fake_set_object;
	cm.set_dictionary = fake_set_dictionary;
	cm.center = fake_center;
	cm.post = fake_post;
	cm.surface_xpc = fake_surface_xpc;
	cm.torch_get = fake_get;
	cm.torch_set = fake_set;
	cm.format_encode = fake_format_encode;
	cm.still_decode = fake_still_decode;
	cm.prepared_decode = fake_prepared_decode;
	cm.preview_open = fake_preview_open;
	cm.preview_register = fake_preview_register;
	cm.preview_insert = fake_preview_insert;
	cm.preview_close = fake_preview_close;
	torch = CFDataCreateMutable(NULL, 0);
	torch_queue = dispatch_queue_create("test", DISPATCH_QUEUE_SERIAL);
	cam.source = CFDataCreateMutable(NULL, 0);
	cam.queue = dispatch_queue_create("test.camera", DISPATCH_QUEUE_SERIAL);
	cam.photos = dispatch_queue_create("test.photos", DISPATCH_QUEUE_SERIAL);

	torch_tests();
	source_tests();
	session_tests();
	printf("captured_test: ok\n");
	return 0;
}
