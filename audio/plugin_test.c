/*
 * The plug-in on the Mac, asked what the audio server asks it: its devices and their names, the streams' format,
 * the volume control (and what it gives the kernel), one round of I/O. /dev/mapple-* do not exist here, so the
 * writes are refused, as on a kernel without them. audio/plugin-test.sh builds and runs it.
 */
#include "audio_plugin.c"

static int failures;

#define CHECK(c) do { if (!(c)) { printf("FAILED line %d: %s\n", __LINE__, #c); failures++; } } while (0)

static OSStatus
changed(__unused AudioServerPlugInHostRef h, __unused AudioObjectID o, __unused UInt32 n, __unused const AudioObjectPropertyAddress *a)
{
	return 0;
}

static const AudioServerPlugInHostInterface fake_host = { .PropertiesChanged = changed };

static UInt32
get(AudioServerPlugInDriverRef d, AudioObjectID object, UInt32 selector, UInt32 scope, UInt32 qsize, const void *q, UInt32 room, void *out)
{
	AudioObjectPropertyAddress a = { selector, scope, 0 };
	UInt32 size = 0;

	if ((*d)->GetPropertyData(d, object, 0, &a, qsize, q, room, &size, out) != 0) {
		return 0;
	}
	return size;
}

/* what the kernel is told for a volume (volume_to_kernel()) */
static uint32_t
below_for(Float32 scalar)
{
	return (uint32_t)lround(-scalar_to_db(scalar) * 8);
}

static Float32
set_volume(AudioServerPlugInDriverRef d, UInt32 selector, Float32 value)
{
	AudioObjectPropertyAddress a = { selector, kAudioObjectPropertyScopeGlobal, 0 };
	Float32 now = -1;

	CHECK((*d)->SetPropertyData(d, OBJECT_SPEAKER_VOLUME, 0, &a, 0, NULL, sizeof(value), &value) == 0);
	get(d, OBJECT_SPEAKER_VOLUME, kAudioLevelControlPropertyScalarValue, kAudioObjectPropertyScopeGlobal, 0, NULL, sizeof(now), &now);
	return now;
}

int
main(void)
{
	AudioServerPlugInDriverRef d = MappleAudio_Create(NULL, kAudioServerPlugInTypeUUID);
	CFUUIDRef other = CFUUIDGetConstantUUIDWithBytes(NULL, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15, 16);
	AudioObjectID list[8];
	const char *uids[] = { "Speaker", "Hawking", "Actuator", "Codec" };
	AudioStreamBasicDescription f;
	UInt32 n;

	CHECK(d != NULL && MappleAudio_Create(NULL, other) == NULL);
	CHECK((*d)->Initialize(d, &fake_host) == 0);

	/* the devices, by the names VirtualAudio knows the iPhone X's by */
	n = get(d, OBJECT_PLUGIN, kAudioPlugInPropertyDeviceList, kAudioObjectPropertyScopeGlobal, 0, NULL, sizeof(list), list);
	CHECK(n == 4 * sizeof(AudioObjectID));
	for (unsigned i = 0; i < 4; i++) {
		CFStringRef uid = NULL, want = CFStringCreateWithCString(NULL, uids[i], kCFStringEncodingUTF8);
		AudioObjectID found = 0;

		get(d, list[i], kAudioDevicePropertyDeviceUID, kAudioObjectPropertyScopeGlobal, 0, NULL, sizeof(uid), &uid);
		CHECK(uid != NULL && CFEqual(uid, want));
		get(d, OBJECT_PLUGIN, kAudioPlugInPropertyTranslateUIDToDevice, kAudioObjectPropertyScopeGlobal, sizeof(want), &want,
		    sizeof(found), &found);
		CHECK(found == list[i]);
		if (uid != NULL) {
			CFRelease(uid);
		}
		CFRelease(want);
	}

	/* the streams: 48 kHz, 16-bit, stereo out of the speaker, one channel into the codec */
	CHECK(get(d, OBJECT_SPEAKER_OUTPUT, kAudioStreamPropertyVirtualFormat, kAudioObjectPropertyScopeGlobal, 0, NULL, sizeof(f), &f) == sizeof(f));
	CHECK(f.mSampleRate == 48000 && f.mChannelsPerFrame == 2 && f.mBitsPerChannel == 16 && f.mBytesPerFrame == MAPPLE_AUDIO_FRAME);
	CHECK(get(d, OBJECT_CODEC_INPUT, kAudioStreamPropertyVirtualFormat, kAudioObjectPropertyScopeGlobal, 0, NULL, sizeof(f), &f) == sizeof(f));
	CHECK(f.mChannelsPerFrame == 1);
	{
		AudioObjectPropertyAddress a = { 'zzzz', kAudioObjectPropertyScopeGlobal, 0 };

		CHECK(!(*d)->HasProperty(d, OBJECT_SPEAKER, 0, &a));
	}

	/* the volume: 48 dB in a straight line of decibels; what the kernel gets never goes past its range; NaN is silence */
	CHECK(below_for(1) == 0 && below_for(0.5f) == 192 && below_for(0) == 384);
	CHECK(set_volume(d, kAudioLevelControlPropertyScalarValue, 0.5f) == 0.5f);
	CHECK(set_volume(d, kAudioLevelControlPropertyDecibelValue, -12) == 0.75f);
	CHECK(set_volume(d, kAudioLevelControlPropertyScalarValue, 7) == 1);
	CHECK(set_volume(d, kAudioLevelControlPropertyDecibelValue, -1000) == 0);
	CHECK(set_volume(d, kAudioLevelControlPropertyScalarValue, NAN) == 0);
	CHECK(set_volume(d, kAudioLevelControlPropertyDecibelValue, NAN) == 0);

	/* one I/O round: the speaker written (refused here), the codec read as silence */
	{
		int16_t buffer[2 * 512];
		AudioServerPlugInIOCycleInfo cycle = { 0 };

		memset(buffer, 0x55, sizeof(buffer));
		CHECK((*d)->StartIO(d, OBJECT_SPEAKER, 1) == 0 && (*d)->StartIO(d, OBJECT_CODEC, 1) == 0);
		CHECK((*d)->DoIOOperation(d, OBJECT_SPEAKER, OBJECT_SPEAKER_OUTPUT, 1, kAudioServerPlugInIOOperationWriteMix, 512, &cycle,
		    buffer, NULL) == 0);
		CHECK(written == 512 && refused == 512);
		CHECK((*d)->DoIOOperation(d, OBJECT_CODEC, OBJECT_CODEC_INPUT, 1, kAudioServerPlugInIOOperationReadInput, 512, &cycle,
		    buffer, NULL) == 0);
		n = 0;
		for (unsigned i = 0; i < 512; i++) {
			n |= (UInt32)buffer[i];
		}
		CHECK(n == 0);
		CHECK((*d)->StopIO(d, OBJECT_CODEC, 1) == 0 && (*d)->StopIO(d, OBJECT_SPEAKER, 1) == 0);
	}
	printf("%s\n", failures ? "plugin_test: FAILED" : "plugin_test: ok");
	return failures != 0;
}
