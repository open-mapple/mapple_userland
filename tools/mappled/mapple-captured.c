/*
 * mapple-captured: the capture services iOS's clients ask mediaserverd for while its recorder server is off
 * (root-tweaks.py gives this daemon the names). The torch, com.apple.coremedia.flashlight, in full: its level goes to
 * the kernel's sysctl kern.mapple_torch (0 off, 1-3 = 25, 37.5, 50 mA; 0x100 while overheated). One back camera
 * showing a test pattern: the capture source lists it, a capture session previews it into a CAImageQueue and takes
 * JPEG photos of it; recording, brackets and moment capture are refused, the deferred container answers errors.
 * The protocol is CoreMedia's own server (FigXPCServerStart and friends); formats and photo settings are CMCapture's
 * own classes. All of it is looked up at run time. Started by launchd on the first request; never exits.
 */
#include <CoreFoundation/CoreFoundation.h>
#include <CoreGraphics/CoreGraphics.h>
#include <CoreMedia/CoreMedia.h>
#include <IOSurface/IOSurfaceRef.h>
#include <ImageIO/ImageIO.h>
#include <dispatch/dispatch.h>
#include <dlfcn.h>
#include <float.h>
#include <objc/message.h>
#include <objc/runtime.h>
#include <stdatomic.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/sysctl.h>
#include <xpc/xpc.h>

typedef int32_t OSStatus;

typedef struct {
	uint64_t version;
	OSStatus (*reply)(xpc_connection_t, xpc_object_t, xpc_object_t);
	void (*noreply)(xpc_connection_t, xpc_object_t);
	void *accept, *coalesce;
} server_callbacks;

/* As CoreMedia's figXPC_SendOneNotification calls it: object, refcon, name, payload, a substitute payload, the
 * message. 0: the server sends the payload as it is; 2: the serializer filled the message itself. */
typedef int (*serializer)(CFTypeRef, void *, CFStringRef, CFDictionaryRef, CFDictionaryRef *, xpc_object_t);
typedef void (*image_released)(void *, uint64_t, void *);

/* What a session's configuration and a photo request come down to here. */
struct config {
	int64_t id;
	CFStringRef preview;            /* the preview sink's ID, NULL without one */
};

struct still {
	int64_t id;
	uint32_t width, height;
	CFTypeRef request;              /* the client's capture request identifier, passed back as it came */
};

static struct {
	OSStatus (*start)(const char *, const server_callbacks *, CFDictionaryRef, void **);
	OSStatus (*op)(xpc_object_t, uint32_t *);
	OSStatus (*associate)(xpc_connection_t, CFTypeRef, void *, void *, void *, uint64_t *);
	OSStatus (*disassociate)(xpc_connection_t, uint64_t);
	OSStatus (*lookup)(xpc_connection_t, uint64_t, CFTypeRef *, void **);
	OSStatus (*copy_string)(xpc_object_t, const char *, CFStringRef *);
	OSStatus (*copy_data)(xpc_object_t, const char *, CFDataRef *);
	OSStatus (*copy_object)(xpc_object_t, const char *, CFTypeRef *);
	OSStatus (*set_object)(xpc_object_t, const char *, CFTypeRef);
	OSStatus (*set_dictionary)(xpc_object_t, const char *, CFDictionaryRef);
	void *(*center)(void);
	OSStatus (*post)(void *, CFStringRef, CFTypeRef, CFTypeRef, uint32_t);
	xpc_object_t (*surface_xpc)(IOSurfaceRef);
	int (*torch_get)(int *);
	int (*torch_set)(int);
	/* CMCapture's classes */
	xpc_object_t (*format_encode)(CFDictionaryRef);
	int (*still_decode)(CFDataRef, struct still *);
	int (*prepared_decode)(xpc_object_t, int64_t *);
	/* CoreAnimation's image queue in a render server slot */
	void *(*preview_open)(int, int, uint32_t *);
	uint64_t (*preview_register)(void *, IOSurfaceRef);
	void (*preview_insert)(void *, uint64_t, image_released, void *);
	void (*preview_collect)(void *);
	void (*preview_close)(void *, uint32_t);
} cm;

#define OP(a, b, c, d)  ((uint32_t)(a) << 24 | (uint32_t)(b) << 16 | (uint32_t)(c) << 8 | (uint32_t)(d))
#define UNSUPPORTED     (-12782)        /* CoreMedia's "not implemented", as the flashlight server answers */
#define REFUSED         (-12780)
#define NO_PROPERTY     (-12784)
#define OVERHEATED      0x100

static void
logf_cf(const char *what, CFTypeRef cf)
{
	CFStringRef d = !cf ? NULL : CFGetTypeID(cf) == CFStringGetTypeID() ? CFRetain(cf) : CFCopyDescription(cf);
	char buf[2048] = "(null)";

	if (d) {
		CFStringGetCString(d, buf, sizeof(buf), kCFStringEncodingUTF8);
		CFRelease(d);
	}
	fprintf(stderr, "mapple-captured: %s %s\n", what, buf);
}

/* Every request in the log: its operation and keys, for the phone run. */
static void
log_request(const char *service, xpc_object_t msg)
{
	uint32_t op = 0;
	char buf[512], *line = buf;
	__block size_t n;

	cm.op(msg, &op);
	n = (size_t)snprintf(line, sizeof(buf), "mapple-captured: %s '%c%c%c%c'", service, op >> 24, (op >> 16) & 0xff,
	    (op >> 8) & 0xff, op & 0xff);
	xpc_dictionary_apply(msg, ^bool (const char *key, __unused xpc_object_t v) {
		if (n < sizeof(buf) - 1) {
			n += (size_t)snprintf(line + n, sizeof(buf) - n, " %s", key);
		}
		return true;
	});
	fprintf(stderr, "%s\n", line);
}

static CFNumberRef
num_int(int64_t v)
{
	return CFNumberCreate(NULL, kCFNumberSInt64Type, &v);
}

static CFNumberRef
num_float(double v)
{
	return CFNumberCreate(NULL, kCFNumberDoubleType, &v);
}

static void
put(CFMutableDictionaryRef d, CFStringRef key, CFTypeRef owned)
{
	if (owned) {
		CFDictionarySetValue(d, key, owned);
		CFRelease(owned);
	}
}

static CFMutableDictionaryRef
dict(void)
{
	return CFDictionaryCreateMutable(NULL, 0, &kCFTypeDictionaryKeyCallBacks, &kCFTypeDictionaryValueCallBacks);
}

/* -- the torch -- */

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

/* -- the camera -- */

/* The back camera of the iPhone X as its AVCaptureSession.plist describes the "12MP" photo format: 4032x3024
 * photos, a 1504x1128 preview, here at a fixed 30 fps. */
#define PHOTO_W         4032
#define PHOTO_H         3024
#define PREVIEW_W       1504
#define PREVIEW_H       1128
#define FPS             30
#define PREVIEW_BUFFERS 3
#define FOURCC(s)       ((uint32_t)(s)[0] << 24 | (uint32_t)(s)[1] << 16 | (uint32_t)(s)[2] << 8 | (uint32_t)(s)[3])

/* The frame source: fills a BGRA buffer. The test pattern now; the kernel's raw frames through the ISP later. */
typedef void (*frame_source)(uint8_t *bgra, size_t stride, int width, int height, uint64_t frame);

/* Colour bars, a block bouncing across them and the frame number as 16 bits along the bottom (white = 1). */
static void
pattern(uint8_t *bgra, size_t stride, int w, int h, uint64_t frame)
{
	static const uint32_t bars[8] = { 0xffffff, 0xffff00, 0x00ffff, 0x00ff00, 0xff00ff, 0xff0000, 0x0000ff, 0x000000 };
	int side = h / 6, span = w - side, pos = (int)(frame * (uint64_t)w / 90 % (uint64_t)(2 * span));
	int bx = pos < span ? pos : 2 * span - pos, by = (h - side) / 2, bit = w / 20, base = h - 2 * bit;

	for (int y = 0; y < h; y++) {
		uint32_t *row = (uint32_t *)(bgra + (size_t)y * stride);

		for (int x = 0; x < w; x++) {
			uint32_t rgb = bars[x * 8 / w];

			if (x >= bx && x < bx + side && y >= by && y < by + side) {
				rgb = 0x808080;
			} else if (y >= base && y < base + bit && x / bit >= 2 && x / bit < 18 && x % bit < bit - 2) {
				rgb = (frame >> (17 - x / bit) & 1) ? 0xffffff : 0x202020;
			}
			row[x] = 0xff000000 | rgb;      /* BGRA in memory, little-endian */
		}
	}
}

static frame_source fill_frame = pattern;

static struct {
	dispatch_queue_t queue;         /* sessions and the source's properties */
	dispatch_queue_t photos;        /* JPEG encoding, off the preview's queue */
	CFMutableDataRef source;        /* the one served capture source */
	CFDictionaryRef attributes;
	xpc_object_t format;            /* its one format, in CMCapture's XPC encoding */
	CFMutableDictionaryRef properties;
	uint64_t frame;
} cam;

struct session {
	CFMutableDataRef object;        /* what its notifications are posted on */
	CFMutableDictionaryRef properties;
	struct config config;
	bool running, live;
	void *queue;                    /* the preview's CAImageQueue */
	uint32_t slot;
	dispatch_source_t timer;
	IOSurfaceRef surface[PREVIEW_BUFFERS];
	uint64_t image[PREVIEW_BUFFERS];
	atomic_bool shown[PREVIEW_BUFFERS];
};

static CFDictionaryRef
camera_attributes(void)
{
	CFMutableDictionaryRef a = dict();
	CFStringRef id = CFSTR("com.apple.avfoundation.avcapturedevice.built-in_video:0");

	CFDictionarySetValue(a, CFSTR("UniqueID"), id);
	CFDictionarySetValue(a, CFSTR("ModelID"), id);
	CFDictionarySetValue(a, CFSTR("Manufacturer"), CFSTR("Apple Inc."));
	CFDictionarySetValue(a, CFSTR("NonLocalizedName"), CFSTR("Back Camera"));
	put(a, CFSTR("Position"), num_int(1));         /* back */
	put(a, CFSTR("SourceType"), num_int(1));       /* video */
	put(a, CFSTR("DeviceType"), num_int(2));       /* AVCaptureDeviceTypeBuiltInWideAngleCamera */
	CFDictionarySetValue(a, CFSTR("MinFrameRate"), kCFBooleanTrue);
	CFDictionarySetValue(a, CFSTR("MaxFrameRate"), kCFBooleanTrue);
	CFDictionarySetValue(a, CFSTR("Exposure"), kCFBooleanFalse);
	CFDictionarySetValue(a, CFSTR("Focus"), kCFBooleanFalse);
	CFDictionarySetValue(a, CFSTR("WhiteBalance"), kCFBooleanFalse);
	CFDictionarySetValue(a, CFSTR("Flash"), kCFBooleanTrue);       /* Camera.app asks the iPhone X's flash modes */
	CFDictionarySetValue(a, CFSTR("Torch"), kCFBooleanFalse);
	CFDictionarySetValue(a, CFSTR("HEIF"), kCFBooleanFalse);
	CFDictionarySetValue(a, CFSTR("HEVC"), kCFBooleanFalse);
	return a;
}

/* The format as CMCapture's own camera source builds it from that plist (its keys are CMCapture's). */
static CFDictionaryRef
camera_format(void)
{
	CFMutableDictionaryRef f = dict();
	const void *presets[] = { CFSTR("AVCaptureSessionPresetPhoto"), CFSTR("AVCaptureSessionPresetHigh") };

	CFDictionarySetValue(f, CFSTR("Name"), CFSTR("12MP"));
	CFDictionarySetValue(f, CFSTR("MediaType"), CFSTR("vide"));
	put(f, CFSTR("Index"), num_int(0));
	put(f, CFSTR("Width"), num_int(PHOTO_W));
	put(f, CFSTR("Height"), num_int(PHOTO_H));
	put(f, CFSTR("SensorWidth"), num_int(PHOTO_W));
	put(f, CFSTR("SensorHeight"), num_int(PHOTO_H));
	put(f, CFSTR("PixelFormatType"), num_int(FOURCC("420f")));
	put(f, CFSTR("PreferredPreviewWidth"), num_int(PREVIEW_W));
	put(f, CFSTR("PreferredPreviewHeight"), num_int(PREVIEW_H));
	put(f, CFSTR("VideoMinFrameRate"), num_float(FPS));
	put(f, CFSTR("VideoMaxFrameRate"), num_float(FPS));
	put(f, CFSTR("VideoDefaultMinFrameRate"), num_float(FPS));
	put(f, CFSTR("VideoDefaultMaxFrameRate"), num_float(FPS));
	put(f, CFSTR("VideoFieldOfView"), num_float(65.0));
	CFDictionarySetValue(f, CFSTR("DefaultActiveFormat"), kCFBooleanTrue);
	CFDictionarySetValue(f, CFSTR("HDRSupported"), kCFBooleanTrue);   /* and its HDR modes; both taken, not acted on */
	put(f, CFSTR("AVCaptureSessionPresets"), CFArrayCreate(NULL, presets, 2, &kCFTypeArrayCallBacks));
	return f;
}

static bool
camera_ready(void)
{
	static dispatch_once_t once;

	dispatch_once(&once, ^{
		CFDictionaryRef f = camera_format();

		cam.attributes = camera_attributes();
		cam.format = cm.format_encode(f);
		CFRelease(f);
		cam.properties = dict();
		put(cam.properties, CFSTR("ActiveMinFrameRate"), num_float(FPS));
		put(cam.properties, CFSTR("ActiveMaxFrameRate"), num_float(FPS));
		if (!cam.format) {
			fprintf(stderr, "mapple-captured: no camera: CMCapture's format class is missing\n");
		}
	});
	return cam.format != NULL;
}

/* .cpy and .set of a property kept in a dictionary; the source's active format is its XPC encoding. */
static OSStatus
copy_property(CFDictionaryRef props, xpc_object_t msg, xpc_object_t reply)
{
	CFStringRef name = NULL;
	__block OSStatus err = NO_PROPERTY;

	if (cm.copy_string(msg, ".PropertyName", &name) != 0 || !name) {
		return REFUSED;
	}
	dispatch_sync(cam.queue, ^{
		CFTypeRef v = CFDictionaryGetValue(props, name);

		if (props == cam.properties && CFEqual(name, CFSTR("ActiveFormat"))) {
			xpc_dictionary_set_value(reply, ".Value", cam.format);
			err = 0;
		} else if (props == cam.properties && CFEqual(name, CFSTR("SourceToken"))) {
			xpc_dictionary_set_int64(reply, ".Value", 1);   /* names the source in session configurations */
			err = 0;
		} else if (v) {
			err = cm.set_object(reply, ".Value", v);
		}
	});
	logf_cf(err == 0 ? "  copied" : "  no property", name);
	CFRelease(name);
	return err;
}

static OSStatus
set_property(CFMutableDictionaryRef props, xpc_object_t msg)
{
	CFStringRef name = NULL;
	CFTypeRef value = NULL;

	if (cm.copy_string(msg, ".PropertyName", &name) != 0 || !name) {
		return REFUSED;
	}
	cm.copy_object(msg, ".Value", &value);
	logf_cf("  set", name);
	dispatch_sync(cam.queue, ^{
		if (value) {
			CFDictionarySetValue(props, name, value);
		} else {
			CFDictionaryRemoveValue(props, name);
		}
	});
	if (value) {
		CFRelease(value);
	}
	CFRelease(name);
	return 0;
}

static OSStatus
capturesource_reply(xpc_connection_t conn, xpc_object_t msg, xpc_object_t reply)
{
	uint32_t op = 0;
	uint64_t id = 0;

	log_request("capturesource", msg);
	if (cm.op(msg, &op) != 0) {
		return UNSUPPORTED;
	}
	switch (op) {
	case OP('c', 's', 'r', 'c'): {            /* .payload: the media types asked for, 1 video, 2 audio */
		xpc_object_t list = xpc_array_create(NULL, 0);

		if ((xpc_dictionary_get_int64(msg, ".payload") & 1) && camera_ready() && cm.associate(conn, cam.source, NULL, NULL, NULL, &id) == 0) {
			xpc_object_t source = xpc_dictionary_create(NULL, NULL, 0), formats = xpc_array_create(NULL, 0);

			xpc_dictionary_set_uint64(source, "objectID", id);
			xpc_dictionary_set_uint64(source, "sourceType", 1);
			cm.set_dictionary(source, "attributesDictionary", cam.attributes);
			xpc_array_append_value(formats, cam.format);
			xpc_dictionary_set_value(source, "formatsArray", formats);
			xpc_array_append_value(list, source);
			xpc_release(formats);
			xpc_release(source);
		}
		xpc_dictionary_set_value(reply, ".Value", list);
		fprintf(stderr, "mapple-captured:   %zu camera(s)\n", xpc_array_get_count(list));
		xpc_release(list);
		return 0;
	}
	case OP('.', 'c', 'p', 'y'):
		return camera_ready() ? copy_property(cam.properties, msg, reply) : UNSUPPORTED;
	case OP('.', 's', 'e', 't'):
		return camera_ready() ? set_property(cam.properties, msg) : UNSUPPORTED;
	case OP('c', 'c', 'l', 'k'):
		xpc_dictionary_set_int64(reply, "clockType", 1);        /* the host time clock */
		return 0;
	case OP('d', 'i', 's', 'p'):
		return cm.disassociate(conn, xpc_dictionary_get_uint64(msg, ".objectID"));
	case OP('c', 't', 'c', 'c'):            /* camera access: granted */
	case OP('l', 'o', 'c', 'k'):
	case OP('u', 'n', 'l', 'k'):
	case OP('i', 'n', 'v', 'a'):
	case OP('c', 'p', 'p', 'd'):
	case OP('s', 't', 'p', 'd'):
	case OP('r', 'n', 'p', 'd'):
	case OP('u', 'n', 'p', 'd'):
		return 0;
	default:
		return UNSUPPORTED;
	}
}

static void
capturesource_noreply(__unused xpc_connection_t conn, xpc_object_t msg)
{
	log_request("capturesource (no reply)", msg);
}

/* -- capture sessions -- */

static void
notify(struct session *s, CFStringRef name, CFDictionaryRef payload)
{
	logf_cf("  posted", name);
	cm.post(cm.center(), name, s->object, payload, 0);
}

static CFMutableDictionaryRef
status_payload(OSStatus err)
{
	CFMutableDictionaryRef p = dict();

	put(p, CFSTR("ErrorStatus"), num_int(err));
	return p;
}

static CFMutableDictionaryRef
section_payload(CFStringRef section, int64_t settings, OSStatus err)
{
	CFMutableDictionaryRef p = status_payload(err);

	if (section) {
		CFDictionarySetValue(p, CFSTR("SectionID"), section);
	}
	put(p, CFSTR("SettingsID"), num_int(settings));
	return p;
}

static void
notify_status(struct session *s, CFStringRef name, OSStatus err)
{
	CFMutableDictionaryRef p = status_payload(err);

	notify(s, name, p);
	CFRelease(p);
}

static void
notify_config(struct session *s, CFStringRef name)
{
	CFMutableDictionaryRef p = status_payload(0);

	put(p, CFSTR("ConfigurationID"), num_int(s->config.id));
	if (CFEqual(name, CFSTR("ConfigurationCommitted"))) {
		put(p, CFSTR("MasterClockType"), num_int(1));  /* the host time clock */
	}
	notify(s, name, p);
	CFRelease(p);
}

static IOSurfaceRef
surface_create(int width, int height, int bytes, uint32_t format)
{
	CFMutableDictionaryRef p = dict();
	IOSurfaceRef s;

	put(p, kIOSurfaceWidth, num_int(width));
	put(p, kIOSurfaceHeight, num_int(height));
	put(p, kIOSurfaceBytesPerElement, num_int(bytes));
	if (format) {
		put(p, kIOSurfacePixelFormat, num_int(format));
	}
	s = IOSurfaceCreate(p);
	CFRelease(p);
	return s;
}

static atomic_uint ticks, queued, released;

static void
image_shown(__unused void *queue, __unused uint64_t image, void *flag)
{
	atomic_store((atomic_bool *)flag, false);
	atomic_fetch_add(&released, 1);
}

/* One preview frame: the next buffer CoreAnimation has let go of, filled and queued. Its releases come only from
 * collecting the queue, as CMCapture's preview sink does before each frame. */
static void
preview_tick(struct session *s)
{
	cm.preview_collect(s->queue);
	if (atomic_fetch_add(&ticks, 1) % (5 * FPS) == 0) {
		fprintf(stderr, "mapple-captured: preview: %u frames queued, %u let go by CoreAnimation\n",
		    atomic_load(&queued), atomic_load(&released));
	}
	for (int i = 0; i < PREVIEW_BUFFERS; i++) {
		bool idle = false;

		if (atomic_compare_exchange_strong(&s->shown[i], &idle, true)) {
			IOSurfaceRef surface = s->surface[i];

			IOSurfaceLock(surface, 0, NULL);
			fill_frame(IOSurfaceGetBaseAddress(surface), IOSurfaceGetBytesPerRow(surface), PREVIEW_W, PREVIEW_H,
			    cam.frame++);
			IOSurfaceUnlock(surface, 0, NULL);
			cm.preview_insert(s->queue, s->image[i], image_shown, &s->shown[i]);
			atomic_fetch_add(&queued, 1);
			return;
		}
	}
}

static void
preview_announce(struct session *s)
{
	CFMutableDictionaryRef p = dict();

	CFDictionarySetValue(p, CFSTR("SectionID"), s->config.preview);
	put(p, CFSTR("ImageQueueSlot"), num_int(s->slot));
	put(p, CFSTR("RotationDegrees"), num_int(90));        /* a landscape sensor in a portrait UI */
	put(p, CFSTR("ImageQueueWidth"), num_int(PREVIEW_W));
	put(p, CFSTR("ImageQueueHeight"), num_int(PREVIEW_H));
	notify(s, CFSTR("ImageQueueUpdated"), p);
	CFRelease(p);
	p = dict();
	CFDictionarySetValue(p, CFSTR("SectionID"), s->config.preview);
	notify(s, CFSTR("DidStartPreviewing"), p);
	CFRelease(p);
}

static void
preview_start(struct session *s)
{
	if (!s->config.preview || s->timer) {
		return;
	}
	if (!s->queue) {
		s->queue = cm.preview_open(PREVIEW_W, PREVIEW_H, &s->slot);
		for (int i = 0; s->queue && i < PREVIEW_BUFFERS; i++) {
			s->surface[i] = surface_create(PREVIEW_W, PREVIEW_H, 4, FOURCC("BGRA"));
			s->image[i] = s->surface[i] ? cm.preview_register(s->queue, s->surface[i]) : 0;
			atomic_store(&s->shown[i], s->surface[i] == NULL);
		}
		if (!s->queue) {
			fprintf(stderr, "mapple-captured: no preview: no image queue\n");
			return;
		}
	}
	preview_announce(s);
	s->timer = dispatch_source_create(DISPATCH_SOURCE_TYPE_TIMER, 0, 0, cam.queue);
	dispatch_source_set_timer(s->timer, DISPATCH_TIME_NOW, NSEC_PER_SEC / FPS, NSEC_PER_SEC / FPS / 10);
	dispatch_source_set_event_handler(s->timer, ^{ preview_tick(s); });
	dispatch_resume(s->timer);
}

static void
preview_stop(struct session *s)
{
	if (s->timer) {
		CFMutableDictionaryRef p = dict();

		dispatch_source_cancel(s->timer);
		dispatch_release(s->timer);
		s->timer = NULL;
		CFDictionarySetValue(p, CFSTR("SectionID"), s->config.preview);
		notify(s, CFSTR("DidStopPreviewing"), p);
		CFRelease(p);
	}
}

static void
session_free(void *refcon)
{
	struct session *s = refcon;

	dispatch_sync(cam.queue, ^{
		preview_stop(s);
		if (s->queue) {
			cm.preview_close(s->queue, s->slot);
		}
		for (int i = 0; i < PREVIEW_BUFFERS; i++) {
			if (s->surface[i]) {
				CFRelease(s->surface[i]);
			}
		}
		if (s->config.preview) {
			CFRelease(s->config.preview);
		}
		CFRelease(s->properties);
		CFRelease(s->object);
	});
	free(s);
}

/* Photos: the encoded image in an IOSurface, sent as its XPC object (CMCapture's own "_Serialized" keys). */
static int
session_serialize(__unused CFTypeRef object, __unused void *refcon, __unused CFStringRef name, CFDictionaryRef payload,
    __unused CFDictionaryRef *substitute, xpc_object_t msg)
{
	static const char *const keys[] = { "Surface", "PreviewSurface", "ThumbnailSurface" };
	CFMutableDictionaryRef rest = NULL;
	int done = 0;

	for (size_t i = 0; payload && i < sizeof(keys) / sizeof(keys[0]); i++) {
		CFStringRef key = CFStringCreateWithCString(NULL, keys[i], kCFStringEncodingUTF8);
		CFTypeRef v = CFDictionaryGetValue(payload, key);

		if (v && CFGetTypeID(v) == IOSurfaceGetTypeID()) {
			xpc_object_t x = cm.surface_xpc((IOSurfaceRef)v);
			char serialized[64];

			if (!rest) {
				rest = CFDictionaryCreateMutableCopy(NULL, 0, payload);
			}
			CFDictionaryRemoveValue(rest, key);
			snprintf(serialized, sizeof(serialized), "%s_Serialized", keys[i]);
			if (x) {
				xpc_dictionary_set_value(msg, serialized, x);
				xpc_release(x);
			}
		}
		CFRelease(key);
	}
	if (rest) {
		done = cm.set_dictionary(msg, ".payload", rest) == 0 ? 2 : 1;
		CFRelease(rest);
	}
	return done;
}

static IOSurfaceRef
photo_jpeg(uint64_t frame, size_t *size)
{
	size_t stride = PHOTO_W * 4;
	uint8_t *pixels = malloc(stride * PHOTO_H);
	CFMutableDataRef jpeg;
	CGColorSpaceRef rgb;
	CGContextRef ctx;
	CGImageRef image;
	CGImageDestinationRef out;
	CFMutableDictionaryRef opts;
	IOSurfaceRef surface = NULL;

	if (!pixels) {
		return NULL;
	}
	jpeg = CFDataCreateMutable(NULL, 0);
	rgb = CGColorSpaceCreateDeviceRGB();
	opts = dict();
	fill_frame(pixels, stride, PHOTO_W, PHOTO_H, frame);
	ctx = CGBitmapContextCreate(pixels, PHOTO_W, PHOTO_H, 8, stride, rgb,
	    kCGImageAlphaNoneSkipFirst | kCGBitmapByteOrder32Little);
	image = CGBitmapContextCreateImage(ctx);
	out = image ? CGImageDestinationCreateWithData(jpeg, CFSTR("public.jpeg"), 1, NULL) : NULL;
	put(opts, kCGImageDestinationLossyCompressionQuality, num_float(0.85));
	put(opts, kCGImagePropertyOrientation, num_int(6));   /* the sensor's landscape, held upright */
	if (out) {
		CGImageDestinationAddImage(out, image, opts);
	}
	if (out && CGImageDestinationFinalize(out)) {
		*size = (size_t)CFDataGetLength(jpeg);
		surface = surface_create(4096, (int)((*size + 4095) / 4096), 1, 0);
	}
	if (surface) {
		IOSurfaceLock(surface, 0, NULL);
		memcpy(IOSurfaceGetBaseAddress(surface), CFDataGetBytePtr(jpeg), *size);
		IOSurfaceUnlock(surface, 0, NULL);
	}
	CFRelease(opts);
	if (out) {
		CFRelease(out);
	}
	CGImageRelease(image);
	CGContextRelease(ctx);
	CGColorSpaceRelease(rgb);
	CFRelease(jpeg);
	free(pixels);
	return surface;
}

/* A photo request: AVCapturePhotoOutput's (iris, isic) or AVCaptureStillImageOutput's (stil). */
static void
take_photo(struct session *s, CFStringRef section, CFDataRef archive, bool iris)
{
	struct still st = { 0 };
	CFStringRef will = iris ? CFSTR("IrisWillCaptureStillImage") : CFSTR("WillCaptureStillImage");
	CFStringRef did = iris ? CFSTR("IrisDidCaptureStillImage") : CFSTR("DidCaptureStillImage");
	CFStringRef complete = iris ? CFSTR("IrisStillImageComplete") : CFSTR("StillImageComplete");
	CFMutableDictionaryRef p;
	CFTypeRef object = s->object;
	uint64_t frame = cam.frame;

	if (!archive || cm.still_decode(archive, &st) != 0) {
		p = section_payload(section, 0, REFUSED);
		notify(s, complete, p);
		CFRelease(p);
		return;
	}
	fprintf(stderr, "mapple-captured:   photo %lld, %ux%u asked\n", (long long)st.id, st.width, st.height);
	if (iris) {
		const int zero = 0;
		CFNumberRef flags = CFNumberCreate(NULL, kCFNumberIntType, &zero);
		const void *one[] = { flags };

		p = section_payload(section, st.id, 0);
		put(p, CFSTR("StillWidth"), num_int(PHOTO_W));
		put(p, CFSTR("StillHeight"), num_int(PHOTO_H));
		put(p, CFSTR("PhotoManifest"), CFArrayCreate(NULL, one, 1, &kCFTypeArrayCallBacks));
		CFRelease(flags);
		notify(s, CFSTR("IrisWillBeginCapture"), p);
		CFRelease(p);
	}
	p = section_payload(section, st.id, 0);
	notify(s, will, p);
	notify(s, did, p);
	CFRelease(p);
	CFRetain(object);
	CFRetain(section);
	dispatch_async(cam.photos, ^{
		size_t size = 0;
		IOSurfaceRef jpeg = photo_jpeg(frame, &size);
		CFMutableDictionaryRef c = section_payload(section, st.id, jpeg ? 0 : REFUSED);

		if (jpeg) {
			CMTime now = CMClockGetTime(CMClockGetHostTimeClock());

			CFDictionarySetValue(c, CFSTR("Surface"), jpeg);
			put(c, CFSTR("SurfaceSize"), num_int((int64_t)size));
			put(c, CFSTR("PhotoCodec"), num_int(FOURCC("jpeg")));
			put(c, CFSTR("PhotoFileType"), num_int(FOURCC("jfif")));
			put(c, CFSTR("PresentationTimestamp"), CMTimeCopyAsDictionary(now, NULL));
			put(c, CFSTR("Metadata"), dict());
			CFRelease(jpeg);
		}
		if (st.request) {
			CFDictionarySetValue(c, CFSTR("CaptureRequestIdentifier"), st.request);
			CFRelease(st.request);
		}
		fprintf(stderr, "mapple-captured:   photo %lld: %zu bytes of JPEG\n", (long long)st.id, size);
		logf_cf("  posted", complete);
		cm.post(cm.center(), complete, object, c, 0);
		CFRelease(c);
		CFRelease(section);
		CFRelease(object);
	});
}

static struct session *
session_of(xpc_connection_t conn, xpc_object_t msg)
{
	CFTypeRef object = NULL;
	void *refcon = NULL;

	if (cm.lookup(conn, xpc_dictionary_get_uint64(msg, ".objectID"), &object, &refcon) != 0 || !refcon) {
		fprintf(stderr, "mapple-captured:   no such session\n");
		return NULL;
	}
	if (object) {
		CFRelease(object);
	}
	return refcon;
}

/* The configuration as FigCaptureSessionConfiguration encodes it. Not through that class: its sources look themselves
 * up by token in CMCapture's own source server, which does not run here, and without them it drops every connection. */
static int
config_parse(xpc_object_t x, struct config *c)
{
	bool valid = x && xpc_get_type(x) == XPC_TYPE_DICTIONARY;
	xpc_object_t connections = valid ? xpc_dictionary_get_value(x, "connections") : NULL;

	if (!connections || xpc_get_type(connections) != XPC_TYPE_ARRAY) {
		return -1;
	}
	c->id = xpc_dictionary_get_int64(x, "configurationID");
	fprintf(stderr, "mapple-captured:   configuration %lld, %zu connection(s)\n", (long long)c->id,
	    xpc_array_get_count(connections));
	xpc_array_apply(connections, ^bool (__unused size_t i, xpc_object_t connection) {
		xpc_object_t sink = xpc_get_type(connection) == XPC_TYPE_DICTIONARY ?
		    xpc_dictionary_get_value(connection, "sink") : NULL;
		const char *class = sink ? xpc_dictionary_get_string(sink, "class") : NULL;
		CFStringRef sink_id = NULL;
		char what[128];

		if (!class || cm.copy_string(sink, "sinkID", &sink_id) != 0 || !sink_id) {
			return true;
		}
		snprintf(what, sizeof(what), "  sink %s", class);
		logf_cf(what, sink_id);
		if (!c->preview && strcmp(class, "FigCaptureVideoPreviewSinkConfiguration") == 0) {
			c->preview = CFRetain(sink_id);
		}
		CFRelease(sink_id);
		return true;
	});
	return 0;
}

static OSStatus
capturesession_reply(xpc_connection_t conn, xpc_object_t msg, xpc_object_t reply)
{
	uint32_t op = 0;
	uint64_t id = 0;
	struct session *s;

	log_request("capturesession", msg);
	if (cm.op(msg, &op) != 0) {
		return UNSUPPORTED;
	}
	switch (op) {
	case OP('c', 'r', 'e', 'a'):
		if (!camera_ready() || !(s = calloc(1, sizeof(*s)))) {
			return UNSUPPORTED;
		}
		s->object = CFDataCreateMutable(NULL, 0);
		s->properties = dict();
		if (cm.associate(conn, s->object, s, (void *)session_free, (void *)session_serialize, &id) != 0) {
			CFRelease(s->properties);
			CFRelease(s->object);
			free(s);
			return REFUSED;
		}
		xpc_dictionary_set_uint64(reply, ".objectID", id);
		return 0;
	case OP('d', 'i', 's', 'p'):
		return cm.disassociate(conn, xpc_dictionary_get_uint64(msg, ".objectID"));
	case OP('.', 'c', 'p', 'y'):
		return (s = session_of(conn, msg)) ? copy_property(s->properties, msg, reply) : REFUSED;
	case OP('.', 's', 'e', 't'):
		return (s = session_of(conn, msg)) ? set_property(s->properties, msg) : REFUSED;
	case OP('s', 's', 'p', 'r'):
	case OP('s', 'w', 'b', 'e'):
	case OP('s', 'w', 'b', 'u'):
		return 0;
	default:
		return UNSUPPORTED;
	}
}

static void
capturesession_noreply(xpc_connection_t conn, xpc_object_t msg)
{
	uint32_t op = 0;
	struct session *s;
	CFStringRef section = NULL;
	CFDataRef archive = NULL;

	log_request("capturesession", msg);
	if (cm.op(msg, &op) != 0 || !(s = session_of(conn, msg))) {
		return;
	}
	cm.copy_string(msg, "FigCaptureSessionMsgParam_NodeID", &section);
	switch (op) {
	case OP('c', 'n', 'f', 'g'): {
		struct config c = { 0 };
		int err = config_parse(xpc_dictionary_get_value(msg, ".payload"), &c);

		dispatch_sync(cam.queue, ^{
			bool moved = s->running && !(c.preview && s->config.preview && CFEqual(c.preview, s->config.preview));

			if (err != 0) {
				notify_status(s, CFSTR("ConfigurationCommitted"), REFUSED);
				return;
			}
			if (moved) {
				preview_stop(s);
			}
			if (s->config.preview) {
				CFRelease(s->config.preview);
			}
			s->config = c;
			notify_config(s, CFSTR("ConfigurationCommitted"));
			if (s->running) {
				notify_config(s, CFSTR("ConfigurationDidBecomeLive"));
				preview_start(s);
			}
		});
		break;
	}
	case OP('b', 'r', 'u', 'n'):
		dispatch_sync(cam.queue, ^{
			if (s->running) {
				return;
			}
			s->running = true;
			notify_status(s, CFSTR("WillStartRunning"), 0);
			preview_start(s);
			notify_status(s, CFSTR("DidStartRunning"), 0);
			notify_config(s, CFSTR("ConfigurationDidBecomeLive"));
		});
		break;
	case OP('e', 'r', 'u', 'n'):
		dispatch_sync(cam.queue, ^{
			preview_stop(s);
			s->running = false;
			notify_status(s, CFSTR("DidStopRunning"), 0);
		});
		break;
	case OP('i', 's', 'i', 'p'): {
		int64_t settings = 0;
		CFMutableDictionaryRef p;

		cm.prepared_decode(xpc_dictionary_get_value(msg, ".payload"), &settings);
		p = section_payload(section, settings, 0);
		notify(s, CFSTR("IrisPreparationComplete"), p);
		CFRelease(p);
		break;
	}
	case OP('i', 's', 'i', 'c'):
	case OP('s', 't', 'i', 'l'):
		cm.copy_data(msg, ".payload", &archive);
		take_photo(s, section ? section : CFSTR(""), archive, op == OP('i', 's', 'i', 'c'));
		break;
	case OP('p', 'b', 'r', 'k'): {
		CFMutableDictionaryRef p = section_payload(section, 0, UNSUPPORTED);

		notify(s, CFSTR("BracketPreparationComplete"), p);
		CFRelease(p);
		break;
	}
	case OP('b', 'r', 'e', 'c'): {
		CFMutableDictionaryRef p = section_payload(section, 0, UNSUPPORTED);

		CFDictionarySetValue(p, CFSTR("RecordingSucceeded"), kCFBooleanFalse);
		notify(s, CFSTR("DidStopRecording"), p);
		CFRelease(p);
		break;
	}
	default:                /* moment capture, pause and resume, prewarm: nothing to do */
		break;
	}
	if (archive) {
		CFRelease(archive);
	}
	if (section) {
		CFRelease(section);
	}
}

static OSStatus
unsupported_reply(__unused xpc_connection_t conn, __unused xpc_object_t msg, __unused xpc_object_t reply)
{
	return UNSUPPORTED;
}

#ifndef MAPPLE_CAPTURED_TEST
/* -- CMCapture's classes and CoreAnimation, through the Objective-C runtime -- */

static void *(*ca_queue_create)(uint32_t, uint32_t, uint32_t);
static void (*ca_queue_flags)(void *, uint32_t, uint32_t);
static uint64_t (*ca_queue_register)(void *, IOSurfaceRef, uint32_t);
static uint64_t (*ca_queue_collect)(void *);
static uint64_t (*ca_queue_insert)(void *, double, uint32_t, uint64_t, uint32_t, image_released, void *);
static bool (*ca_server_running)(void);
static double (*ca_media_time)(void);
static id ca_context;

void *objc_autoreleasePoolPush(void);
void objc_autoreleasePoolPop(void *);

static id
send(id o, const char *s)
{
	return o ? ((id (*)(id, SEL))objc_msgSend)(o, sel_registerName(s)) : NULL;
}

static id
send1(id o, const char *s, const void *a)
{
	return o ? ((id (*)(id, SEL, const void *))objc_msgSend)(o, sel_registerName(s), a) : NULL;
}

static long long
send_ll(id o, const char *s)
{
	return o ? ((long long (*)(id, SEL))objc_msgSend)(o, sel_registerName(s)) : 0;
}

static uint32_t
send_u32(id o, const char *s)
{
	return o ? ((uint32_t (*)(id, SEL))objc_msgSend)(o, sel_registerName(s)) : 0;
}

static id
alloc(const char *class)
{
	return send((id)objc_getClass(class), "alloc");
}

static xpc_object_t
format_encode(CFDictionaryRef format)
{
	id f = send1(alloc("FigCaptureSourceVideoFormat"), "initWithFigCaptureStreamFormatDictionary:", format);
	xpc_object_t x = (xpc_object_t)send(f, "copyXPCEncoding");

	send(f, "release");
	return x;
}

static int
still_decode(CFDataRef archive, struct still *st)
{
	void *pool = objc_autoreleasePoolPush();
	id u = ((id (*)(id, SEL, CFDataRef, void *))objc_msgSend)(alloc("NSKeyedUnarchiver"),
	    sel_registerName("initForReadingFromData:error:"), archive, NULL);
	id settings;

	if (u) {
		((void (*)(id, SEL, bool))objc_msgSend)(u, sel_registerName("setRequiresSecureCoding:"), false);
	}
	settings = send1(u, "decodeObjectForKey:", CFSTR("root"));      /* NSKeyedArchiveRootObjectKey */
	if (settings) {
		CFTypeRef request = (CFTypeRef)send(settings, "captureRequestIdentifier");

		logf_cf("photo settings", (CFTypeRef)send(settings, "description"));
		st->id = send_ll(settings, "settingsID");
		st->width = send_u32(settings, "outputWidth");
		st->height = send_u32(settings, "outputHeight");
		st->request = request ? CFRetain(request) : NULL;
	}
	send(u, "finishDecoding");
	send(u, "release");
	objc_autoreleasePoolPop(pool);
	return settings ? 0 : -1;
}

static int
prepared_decode(xpc_object_t encoding, int64_t *settings)
{
	id p = encoding ? send1(alloc("FigCaptureIrisPreparedSettings"), "initWithXPCEncoding:", encoding) : NULL;

	*settings = send_ll(p, "settingsID");
	send(p, "release");
	return p ? 0 : -1;
}

static void
transaction(void (^change)(void))
{
	id t = (id)objc_getClass("CATransaction");

	send(t, "begin");
	((void (*)(id, SEL, bool))objc_msgSend)(t, sel_registerName("activateBackground:"), true);
	change();
	send(t, "commit");
	send(t, "synchronize");
}

/* A CAImageQueue in a slot of the render server's context: the client's preview layer shows the slot. */
static void *
preview_open(int width, int height, uint32_t *slot)
{
	void *pool = objc_autoreleasePoolPush();
	void *queue = ca_queue_create ? ca_queue_create((uint32_t)width, (uint32_t)height, 8) : NULL;

	if (queue && !ca_context) {
		id c = (id)objc_getClass("CAContext");

		/* CARenderServerIsRunning() is true only inside the render server itself (backboardd) */
		bool local = ca_server_running && ca_server_running();

		ca_context = send(send(c, local ? "localContext" : "remoteContext"), "retain");
		fprintf(stderr, "mapple-captured: preview context %u (%s)\n", send_u32(ca_context, "contextId"),
		    local ? "local" : "remote");
	}
	if (queue && ca_context) {
		ca_queue_flags(queue, 0x19, 0x19);
		transaction(^{
			*slot = send_u32(ca_context, "createSlot");
			((void (*)(id, SEL, void *, uint32_t))objc_msgSend)(ca_context,
			    sel_registerName("setObject:forSlot:"), queue, *slot);
		});
		fprintf(stderr, "mapple-captured: preview in slot %u\n", *slot);
	}
	objc_autoreleasePoolPop(pool);
	return ca_context ? queue : NULL;
}

static uint64_t
preview_register(void *queue, IOSurfaceRef surface)
{
	return ca_queue_register(queue, surface, 0);
}

static void
preview_insert(void *queue, uint64_t image, image_released done, void *info)
{
	ca_queue_insert(queue, ca_media_time(), 3, image, 1, done, info);  /* shown now: CMCapture's flags, its host time */
}

static void
preview_collect(void *queue)
{
	ca_queue_collect(queue);
}

static void
preview_close(void *queue, uint32_t slot)
{
	void *pool = objc_autoreleasePoolPush();

	transaction(^{
		((void (*)(id, SEL, uint32_t))objc_msgSend)(ca_context, sel_registerName("deleteSlot:"), slot);
	});
	CFRelease(queue);
	objc_autoreleasePoolPop(pool);
}

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
		{ "com.apple.coremedia.capturesource", { 1, capturesource_reply, capturesource_noreply, NULL, NULL } },
		{ "com.apple.coremedia.capturesession", { 1, capturesession_reply, capturesession_noreply, NULL, NULL } },
		{ "com.apple.coremedia.capture.deferred.containermanager.xpc", { 1, unsupported_reply, ignored, NULL, NULL } },
	};
	void *lib = dlopen("/System/Library/Frameworks/CoreMedia.framework/CoreMedia", RTLD_NOW);
	void *ca = dlopen("/System/Library/Frameworks/QuartzCore.framework/QuartzCore", RTLD_NOW);
	void *surfaces = dlopen("/System/Library/Frameworks/IOSurface.framework/IOSurface", RTLD_NOW);
	dispatch_source_t timer;
	int k = 0;

	cm.start = lib ? dlsym(lib, "FigXPCServerStart") : NULL;
	cm.op = lib ? dlsym(lib, "FigXPCMessageGetOpCode") : NULL;
	cm.associate = lib ? dlsym(lib, "FigXPCServerAssociateObjectWithConnection") : NULL;
	cm.disassociate = lib ? dlsym(lib, "FigXPCServerDisassociateObjectWithConnection") : NULL;
	cm.lookup = lib ? dlsym(lib, "FigXPCServerLookupAndRetainAssociatedObject") : NULL;
	cm.copy_string = lib ? dlsym(lib, "FigXPCMessageCopyCFString") : NULL;
	cm.copy_data = lib ? dlsym(lib, "FigXPCMessageCopyCFData") : NULL;
	cm.copy_object = lib ? dlsym(lib, "FigXPCMessageCopyCFObject") : NULL;
	cm.set_object = lib ? dlsym(lib, "FigXPCMessageSetCFObject") : NULL;
	cm.set_dictionary = lib ? dlsym(lib, "FigXPCMessageSetCFDictionary") : NULL;
	cm.center = lib ? dlsym(lib, "CMNotificationCenterGetDefaultLocalCenter") : NULL;
	cm.post = lib ? dlsym(lib, "CMNotificationCenterPostNotification") : NULL;
	cm.surface_xpc = surfaces ? dlsym(surfaces, "IOSurfaceCreateXPCObject") : NULL;
	cm.torch_get = kernel_get;
	cm.torch_set = kernel_set;
	if (!cm.start || !cm.op || !cm.associate || !cm.disassociate || !cm.lookup || !cm.copy_string || !cm.copy_data ||
	    !cm.copy_object || !cm.set_object || !cm.set_dictionary || !cm.center || !cm.post || !cm.surface_xpc) {
		fprintf(stderr, "mapple-captured: CoreMedia's server calls not found\n");
		return 1;
	}
	ca_queue_create = ca ? dlsym(ca, "CAImageQueueCreate") : NULL;
	ca_queue_flags = ca ? dlsym(ca, "CAImageQueueSetFlags") : NULL;
	ca_queue_register = ca ? dlsym(ca, "CAImageQueueRegisterIOSurfaceBuffer") : NULL;
	ca_queue_insert = ca ? dlsym(ca, "CAImageQueueInsertImage") : NULL;
	ca_queue_collect = ca ? dlsym(ca, "CAImageQueueCollect") : NULL;
	ca_server_running = ca ? dlsym(ca, "CARenderServerIsRunning") : NULL;
	ca_media_time = ca ? dlsym(ca, "CACurrentMediaTime") : NULL;
	if (!ca_queue_flags || !ca_queue_register || !ca_queue_insert || !ca_queue_collect || !ca_media_time) {
		ca_queue_create = NULL;
	}
	if (!dlopen("/System/Library/PrivateFrameworks/CMCapture.framework/CMCapture", RTLD_NOW)) {
		fprintf(stderr, "mapple-captured: no CMCapture: %s\n", dlerror());
	}
	cm.format_encode = format_encode;
	cm.still_decode = still_decode;
	cm.prepared_decode = prepared_decode;
	cm.preview_open = preview_open;
	cm.preview_register = preview_register;
	cm.preview_insert = preview_insert;
	cm.preview_collect = preview_collect;
	cm.preview_close = preview_close;
	torch = CFDataCreateMutable(NULL, 0);
	torch_queue = dispatch_queue_create("mapple-captured.torch", DISPATCH_QUEUE_SERIAL);
	cam.source = CFDataCreateMutable(NULL, 0);
	cam.queue = dispatch_queue_create("mapple-captured.camera", DISPATCH_QUEUE_SERIAL);
	cam.photos = dispatch_queue_create("mapple-captured.photos", DISPATCH_QUEUE_SERIAL);
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
