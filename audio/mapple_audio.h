/*
 * /dev/mapple-audio: the sound Darwin plays, on its way to whatever can make it heard.
 *
 * Darwin expects its output devices from Apple's audio drivers through IOAudio2; neither exists
 * here. Instead the audio server (mediaserverd) is given a plug-in of ours that presents one
 * stereo output device, and the plug-in writes what the server has mixed for that device
 * here. In the kernel the samples go to a sink -- a driver for something that plays them --
 * or, with none, nowhere (bsd/dev/mapple_audio.c).
 *
 *   write(): whole frames in the format below. Never blocks: the writer is the audio server's
 *            real-time thread, which paces itself (48000 frames a second while anything
 *            plays, nothing at all otherwise). What the sink cannot take is dropped.
 */
#ifndef _SYS_MAPPLE_AUDIO_H_
#define _SYS_MAPPLE_AUDIO_H_

#include <stdint.h>

#define MAPPLE_AUDIO_DEVICE    "/dev/mapple-audio"

/* The one format: 16-bit signed little-endian samples, left then right. */
#define MAPPLE_AUDIO_RATE      48000
#define MAPPLE_AUDIO_CHANNELS  2
#define MAPPLE_AUDIO_FRAME     4       /* bytes */

/*
 * ioctl(MAPPLE_AUDIO_SET_VOLUME): the speakers' volume, as Darwin sets it on the plug-in's volume
 * control -- how many eighths of a dB under the loudest they may play, and whether they are
 * muted (Darwin's volume at 0). The loudest is the kernel's to say (on the phone: Android's own
 * level less mp_audio_below, bsd/dev/mtk_audio.c); the plug-in only says how far under it.
 */
#include <sys/ioccom.h>

struct mapple_audio_volume {
	uint32_t below;                 /* eighths of a dB */
	uint32_t muted;
};
#define MAPPLE_AUDIO_SET_VOLUME _IOW('r', 1, struct mapple_audio_volume)

/* ioctl(MAPPLE_AUDIO_SET_ROUTE): where what is written plays, the speaker or the earpiece (the port Darwin has chosen). */
#define MAPPLE_AUDIO_ROUTE_SPEAKER      0
#define MAPPLE_AUDIO_ROUTE_RECEIVER     1
#define MAPPLE_AUDIO_SET_ROUTE  _IOW('r', 2, uint32_t)

#endif /* _SYS_MAPPLE_AUDIO_H_ */
