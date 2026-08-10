#pragma once

#ifdef __cplusplus
extern "C" {
#endif

	/** Load models + test2.wav, run full MuScriptor-style transcription,
	 *  and plugin_log the resulting notes JSON. */
	void run_ppd_transcription(void);

#ifdef __cplusplus
}
#endif