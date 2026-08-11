#pragma once

#ifdef __cplusplus
extern "C" {
#endif

	/** Load prefill + step models. Call once when the plugin starts. */
	void ppd_load_models(void);

	/** Run transcription on the hard-coded test2.wav and log the notes JSON.
	 *  Safe to call after ppd_load_models(). */
	void ppd_run_test(void);

	/** Optional: release sessions / env. Call from processor destructor if you want. */
	void ppd_shutdown(void);

#ifdef __cplusplus
}
#endif