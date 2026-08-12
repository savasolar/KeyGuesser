#pragma once

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

	/** Plain-C container for raw audio handed in from the host (e.g. JUCE).
	 *  `data` is interleaved (num_channels samples per frame) and is owned
	 *  by the CALLER, core.c only reads from it. */
	typedef struct {
		float* data;          /* interleaved samples, length = num_samples * num_channels */
		int64_t num_samples;   /* number of frames (per channel), NOT the raw array length */
		int     num_channels;
		int     sample_rate;
	} C_FloatArray;

	/** Load prefill + step models from the given UTF-8 paths.
	 *  Call once when the plugin starts (paths should point next to the plugin binary). */
	void ppd_load_models(const char* prefill_path_utf8, const char* step_path_utf8);

	/** Unique MIDI pitches (0-127) detected in the most recent transcription,
	 *  in ascending order. Caller owns the storage - stack-allocate one of
	 *  these and pass its address into ppd_run_test_buffer(). */
	typedef struct {
		int pitches[128];   /* unique MIDI note numbers */
		int num_pitches;    /* number of valid entries in `pitches` */
	} C_PitchResult;

	/** Run transcription on caller-supplied audio (e.g. decoded from
	 *  BinaryData in the plugin), reusing the exact same downmix/resample/
	 *  inference pipeline as ppd_run_test(). Safe to call after ppd_load_models().
	 *  If out_pitches is non-NULL it is filled with the unique MIDI pitches
	 *  detected (num_pitches is set to 0 if none were found). */
	void ppd_run_test_buffer(const C_FloatArray* audio, C_PitchResult* out_pitches);

	/** Optional: release sessions / env. Call from processor destructor if you want. */
	void ppd_shutdown(void);

#ifdef __cplusplus
}
#endif