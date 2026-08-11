#pragma once

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

	/** Plain-C container for raw audio handed in from the host (e.g. JUCE).
	 *  `data` is interleaved (num_channels samples per frame) and is owned
	 *  by the CALLER — core.c only reads from it. */
	typedef struct {
		float* data;          /* interleaved samples, length = num_samples * num_channels */
		int64_t num_samples;   /* number of frames (per channel), NOT the raw array length */
		int     num_channels;
		int     sample_rate;
	} C_FloatArray;

	/** Load prefill + step models. Call once when the plugin starts. */
	void ppd_load_models(void);

	/** Run transcription on caller-supplied audio (e.g. decoded from
	 *  BinaryData in the plugin), reusing the exact same downmix/resample/
	 *  inference pipeline as ppd_run_test(). Safe to call after ppd_load_models(). */
	void ppd_run_test_buffer(const C_FloatArray* audio);

	/** Optional: release sessions / env. Call from processor destructor if you want. */
	void ppd_shutdown(void);

#ifdef __cplusplus
}
#endif