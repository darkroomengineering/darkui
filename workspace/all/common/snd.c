#include <stdio.h>
#include <stdlib.h>

#include <unistd.h>
#include <pthread.h>

#include <stdbool.h>
#include <stdint.h>

#include <msettings.h>

#include "defines.h"
#include "api.h"
#include "utils.h"

///////////////////////////////

// based on picoarch's audio
// implementation, rewritten
// to (try to) understand it
// better

#define MAX_SAMPLE_RATE 48000
#define BATCH_SIZE 100
#ifndef SAMPLES
	#define SAMPLES 512 // default
#endif

#define ms SDL_GetTicks

typedef int (*SND_Resampler)(const SND_Frame frame);
static struct SND_Context {
	int initialized;
	double frame_rate;

	int sample_rate_in;
	int sample_rate_out;

	int buffer_seconds;     // current_audio_buffer_size
	SND_Frame* buffer;		// buf
	size_t frame_count; 	// buf_len

	int frame_in;     // buf_w
	int frame_out;    // buf_r
	int frame_filled; // max_buf_w

	SND_Resampler resample;
} snd = {0};
static void SND_audioCallback(void* userdata, uint8_t* stream, int len) { // plat_sound_callback

	// return (void)memset(stream,0,len); // TODO: tmp, silent

	if (snd.frame_count==0) return;

	int16_t *out = (int16_t *)stream;
	len /= (sizeof(int16_t) * 2);
	// int full_len = len;

	// if (snd.frame_out!=snd.frame_in) LOG_info("%8i consuming samples (%i frames)\n", ms(), len);

	while (snd.frame_out!=snd.frame_in && len>0) {
		*out++ = snd.buffer[snd.frame_out].left;
		*out++ = snd.buffer[snd.frame_out].right;

		snd.frame_filled = snd.frame_out;

		snd.frame_out += 1;
		len -= 1;

		if (snd.frame_out>=snd.frame_count) snd.frame_out = 0;
	}

	int zero = len>0 && len==SAMPLES;
	if (zero) return (void)memset(out,0,len*(sizeof(int16_t) * 2));
	// else if (len>=5) LOG_info("%8i BUFFER UNDERRUN (%i/%i frames)\n", ms(), len,full_len);

	int16_t *in = out-1;
	while (len>0) {
		*out++ = (void*)in>(void*)stream ? *--in : 0;
		*out++ = (void*)in>(void*)stream ? *--in : 0;
		len -= 1;
	}
}
static void SND_resizeBuffer(void) { // plat_sound_resize_buffer
	snd.frame_count = snd.buffer_seconds * snd.sample_rate_in / snd.frame_rate;
	if (snd.frame_count==0) return;

	// LOG_info("frame_count: %i (%i * %i / %f)\n", snd.frame_count, snd.buffer_seconds, snd.sample_rate_in, snd.frame_rate);
	// snd.frame_count *= 2; // no help

	SDL_LockAudio();

	int buffer_bytes = snd.frame_count * sizeof(SND_Frame);
	snd.buffer = realloc(snd.buffer, buffer_bytes);

	memset(snd.buffer, 0, buffer_bytes);

	snd.frame_in = 0;
	snd.frame_out = 0;
	snd.frame_filled = snd.frame_count - 1;

	SDL_UnlockAudio();
}
static int SND_resampleNone(SND_Frame frame) { // audio_resample_passthrough
	snd.buffer[snd.frame_in++] = frame;
	if (snd.frame_in >= snd.frame_count) snd.frame_in = 0;
	return 1;
}
static int SND_resampleNear(SND_Frame frame) { // audio_resample_nearest
	static int diff = 0;
	int consumed = 0;

	if (diff < snd.sample_rate_out) {
		snd.buffer[snd.frame_in++] = frame;
		if (snd.frame_in >= snd.frame_count) snd.frame_in = 0;
		diff += snd.sample_rate_in;
	}

	if (diff >= snd.sample_rate_out) {
		consumed++;
		diff -= snd.sample_rate_out;
	}

	return consumed;
}
static void SND_selectResampler(void) { // plat_sound_select_resampler
	if (snd.sample_rate_in==snd.sample_rate_out) {
		snd.resample =  SND_resampleNone;
	}
	else {
		snd.resample = SND_resampleNear;
	}
}
size_t SND_batchSamples(const SND_Frame* frames, size_t frame_count) { // plat_sound_write / plat_sound_write_resample

	// return frame_count; // TODO: tmp, silent

	if (snd.frame_count==0) return 0;

	// LOG_info("%8i batching samples (%i frames)\n", ms(), frame_count);

	SDL_LockAudio();

	int consumed = 0;
	int consumed_frames = 0;
	while (frame_count > 0) {
		int tries = 0;
		int amount = MIN(BATCH_SIZE, frame_count);

		while (tries < 10 && snd.frame_in==snd.frame_filled) {
			tries++;
			SDL_UnlockAudio();
			SDL_Delay(1);
			SDL_LockAudio();
		}
		// if (tries) LOG_info("%8i waited %ims for buffer to get low...\n", ms(), tries);

		while (amount && snd.frame_in != snd.frame_filled) {
			consumed_frames = snd.resample(*frames);

			frames += consumed_frames;
			amount -= consumed_frames;
			frame_count -= consumed_frames;
			consumed += consumed_frames;
		}
	}
	SDL_UnlockAudio();

	return consumed;
}

void SND_init(double sample_rate, double frame_rate) { // plat_sound_init
	LOG_info("SND_init\n");

	SDL_InitSubSystem(SDL_INIT_AUDIO);

#if defined(USE_SDL2)
	LOG_info("Available audio drivers:\n");
	for (int i=0; i<SDL_GetNumAudioDrivers(); i++) {
		LOG_info("- %s\n", SDL_GetAudioDriver(i));
	}
	LOG_info("Current audio driver: %s\n", SDL_GetCurrentAudioDriver());
#endif

	memset(&snd, 0, sizeof(struct SND_Context));
	snd.frame_rate = frame_rate;

	SDL_AudioSpec spec_in;
	SDL_AudioSpec spec_out;

	spec_in.freq = PLAT_pickSampleRate(sample_rate, MAX_SAMPLE_RATE);
	spec_in.format = AUDIO_S16;
	spec_in.channels = 2;
	spec_in.samples = SAMPLES;
	spec_in.callback = SND_audioCallback;

	if (SDL_OpenAudio(&spec_in, &spec_out)<0) LOG_info("SDL_OpenAudio error: %s\n", SDL_GetError());

	snd.buffer_seconds = 5;
	snd.sample_rate_in  = sample_rate;
	snd.sample_rate_out = spec_out.freq;

	SND_selectResampler();
	SND_resizeBuffer();

	SDL_PauseAudio(0);

	LOG_info("sample rate: %i (req) %i (rec) [samples %i]\n", snd.sample_rate_in, snd.sample_rate_out, SAMPLES);
	snd.initialized = 1;
}
void SND_quit(void) { // plat_sound_finish
	if (!snd.initialized) return;

	SDL_PauseAudio(1);
	SDL_CloseAudio();

	if (snd.buffer) {
		free(snd.buffer);
		snd.buffer = NULL;
	}
}
