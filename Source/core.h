#pragma once

#include <stdint.h>
#include "onnxruntime_c_api.h"

#define MUSCRIPTOR_SAMPLE_RATE          16000
#define MUSCRIPTOR_SEGMENT_SAMPLES      80000   /* exactly as Python */
#define MUSCRIPTOR_FRAME_RATE           100
#define MUSCRIPTOR_LAYERS               14
#define MUSCRIPTOR_MAX_GEN_LEN          2000
#define MUSCRIPTOR_EOS_ID               1
#define MUSCRIPTOR_NUM_INSTRUMENTS_NO_DRUMS 34

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    float* data;
    int    length;
} C_FloatArray;

typedef struct MuscriptorModel {
    OrtEnv* env;
    OrtSessionOptions* session_options;
    OrtSession* prefill_session;
    OrtSession* step_session;
    OrtMemoryInfo* mem_info;

    /* fixed conditioning tensors (created once) */
    OrtValue* instrument_group_tensor;
    OrtValue* dataset_name_tensor;

    /* scratch for forbidden mask */
    int* forbidden_ids;
    int                num_forbidden;
} MuscriptorModel;

MuscriptorModel* create_muscriptor_model(void);
void             destroy_muscriptor_model(MuscriptorModel* model);

/* Loads prefill.onnx + step.onnx (and their .data siblings) from the given directory.
   Returns 0 on success. */
int load_muscriptor_models(MuscriptorModel* model,
    const ORTCHAR_T* prefill_path,
    const ORTCHAR_T* step_path);

/* Exact Python onnx_generate_chunk_tokens.
   wav = host-rate mono float buffer (any length; will be resampled + padded/truncated to 80000).
   forced_prefix / forced_len = the tokens returned by tie_section_token_ids.
   On success writes the generated token ids (including the forced prefix) into out_tokens
   and sets *out_len. Caller must free out_tokens. */
int muscriptor_generate_chunk_tokens(MuscriptorModel* model,
    const float* wav, int wav_len, double host_sr,
    const int* forced_prefix, int forced_len,
    int** out_tokens, int* out_len);

#ifdef __cplusplus
}
#endif