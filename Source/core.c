#include "core.h"
#include "PluginLogger.h"
#define DR_WAV_IMPLEMENTATION
#include "dr_wav.h"
#include <samplerate.h>
#include <onnxruntime_c_api.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <stdint.h>
#include <stdbool.h>

/* ------------------------------------------------------------------ */
/* Constants derived from resources_clean.json + Python reference     */
/* ------------------------------------------------------------------ */
#define SAMPLE_RATE          16000
#define FRAME_RATE           100
#define SEGMENT_SAMPLES      80000
#define MAX_GEN_LEN          2000
#define LAYERS               14
#define NUM_KV               (LAYERS * 2)          /* k0..k13 + v0..v13 */
#define EOS_ID               1
#define BOS_ID               1393
#define NUM_TOKENS           1394

#define SHIFT_BASE           3
#define PITCH_BASE           1004
#define VEL_BASE             1132
#define TIE_ID               1134
#define PROGRAM_BASE         1135
#define NUM_PROGRAMS         130
#define DRUM_BASE            1265

#define NUM_DRUMS            128
#define MINIMUM_NOTE_DUR     0.01f
#define MAX_OPEN_NOTES       256
#define MAX_NOTES            8192
#define MAX_JSON             (MAX_NOTES * 128)

static const int64_t INSTRUMENT_GROUP_IDS[] = {
    1,2,3,4,5,6,7,8,9,10,11,12,13,14,15,16,17,18,19,20,
    21,22,23,24,25,26,27,28,29,30,31,32,33,34
};
#define NUM_INSTRUMENTS (sizeof(INSTRUMENT_GROUP_IDS)/sizeof(INSTRUMENT_GROUP_IDS[0]))

static const int64_t DATASET_NAME_NONE_ID = 0;

/* Forbidden token IDs (no-drums) – exact list from resources_clean.json */
static const int FORBIDDEN_IDS[] = {
    1136,1138,1139,1140,1141,1142,1144,1145,1146,1147,1148,1149,1150,
    1152,1153,1154,1155,1156,1157,1158,1160,1162,1163,1165,1166,1169,
    1170,1171,1172,1173,1174,1179,1180,1184,1186,1188,1189,1194,1197,
    1198,1200,1208,1209,1210,1211,1212,1213,1214,1216,1217,1218,1219,
    1220,1221,1222,1224,1225,1226,1227,1228,1229,1230,1231,1232,1233,
    1234,1235,1236,1237,1238,1239,1240,1241,1242,1243,1244,1245,1246,
    1247,1248,1249,1250,1251,1252,1253,1254,1255,1256,1257,1258,1259,
    1260,1261,1262,1263,1264,1265,1266,1267,1268,1269,1270,1271,1272,
    1273,1274,1275,1276,1277,1278,1279,1280,1281,1282,1283,1284,1285,
    1286,1287,1288,1289,1290,1291,1292,1293,1294,1295,1296,1297,1298,
    1299,1300,1301,1302,1303,1304,1305,1306,1307,1308,1309,1310,1311,
    1312,1313,1314,1315,1316,1317,1318,1319,1320,1321,1322,1323,1324,
    1325,1326,1327,1328,1329,1330,1331,1332,1333,1334,1335,1336,1337,
    1338,1339,1340,1341,1342,1343,1344,1345,1346,1347,1348,1349,1350,
    1351,1352,1353,1354,1355,1356,1357,1358,1359,1360,1361,1362,1363,
    1364,1365,1366,1367,1368,1369,1370,1371,1372,1373,1374,1375,1376,
    1377,1378,1379,1380,1381,1382,1383,1384,1385,1386,1387,1388,1389,
    1390,1391,1392
};
#define NUM_FORBIDDEN (sizeof(FORBIDDEN_IDS)/sizeof(FORBIDDEN_IDS[0]))

/* ------------------------------------------------------------------ */
/* Helpers                                                            */
/* ------------------------------------------------------------------ */
static int get_token_id(const char* type, int value)
{
    if (strcmp(type, "shift") == 0)   return SHIFT_BASE + value;
    if (strcmp(type, "pitch") == 0)   return PITCH_BASE + value;
    if (strcmp(type, "velocity") == 0) return VEL_BASE + value;
    if (strcmp(type, "tie") == 0)     return TIE_ID;
    if (strcmp(type, "program") == 0) return PROGRAM_BASE + value;
    if (strcmp(type, "drum") == 0)    return DRUM_BASE + value;
    return -1;
}

static void token_to_type_value(int id, const char** type, int* value)
{
    if (id < 3) {
        static const char* specials[] = { "PAD", "EOS", "UNK" };
        *type = specials[id];
        *value = 0;
        return;
    }
    if (id < PITCH_BASE) {
        *type = "shift";
        *value = id - SHIFT_BASE;
        return;
    }
    if (id < VEL_BASE) {
        *type = "pitch";
        *value = id - PITCH_BASE;
        return;
    }
    if (id < TIE_ID) {
        *type = "velocity";
        *value = id - VEL_BASE;
        return;
    }
    if (id == TIE_ID) {
        *type = "tie";
        *value = 0;
        return;
    }
    if (id < DRUM_BASE) {
        *type = "program";
        *value = id - PROGRAM_BASE;
        return;
    }
    *type = "drum";
    *value = id - DRUM_BASE;
}

/* ------------------------------------------------------------------ */
/* OpenNoteTracker (verbatim port of the Python state machine)        */
/* ------------------------------------------------------------------ */
typedef struct {
    int program;
    int pitch;
    float time;
    int used;
} OpenNote;

typedef struct {
    OpenNote open[MAX_OPEN_NOTES];
    int n_open;
    float seek_time;
    float next_seek_time;   /* <0 means none */
    int start_tick;
    int tick_state;
    int program;            /* -1 = none */
    int velocity;           /* -1 = none */
    int in_prologue;
    int skip_rest;
    int chunk_started;
    /* temporary set used only during prologue */
    int tie_set_prog[MAX_OPEN_NOTES];
    int tie_set_pitch[MAX_OPEN_NOTES];
    int n_tie_set;
} OpenNoteTracker;

typedef enum { ACT_START, ACT_END, ACT_DRUM } ActionType;
typedef struct {
    ActionType type;
    int program;
    int pitch;
    float time;
} NoteAction;

static void tracker_init(OpenNoteTracker* t)
{
    memset(t, 0, sizeof(*t));
    t->program = -1;
    t->velocity = -1;
    t->next_seek_time = -1.0f;
}

static void tracker_end_all(OpenNoteTracker* t, float time, NoteAction* out, int* n_out)
{
    for (int i = 0; i < t->n_open; ++i) {
        if (!t->open[i].used) continue;
        out[(*n_out)++] = (NoteAction){ ACT_END, t->open[i].program, t->open[i].pitch, time };
        t->open[i].used = 0;
    }
    t->n_open = 0;
}

static int tracker_find_open(OpenNoteTracker* t, int prog, int pitch)
{
    for (int i = 0; i < t->n_open; ++i)
        if (t->open[i].used && t->open[i].program == prog && t->open[i].pitch == pitch)
            return i;
    return -1;
}

static void tracker_feed_boundary(OpenNoteTracker* t, float seek, float next_seek,
    NoteAction* out, int* n_out)
{
    if (t->chunk_started && t->in_prologue)
        tracker_end_all(t, t->seek_time, out, n_out);

    t->seek_time = seek;
    t->next_seek_time = next_seek;
    t->start_tick = (int)roundf(seek * FRAME_RATE);
    t->tick_state = t->start_tick;
    t->program = -1;
    t->velocity = -1;
    t->in_prologue = 1;
    t->skip_rest = 0;
    t->n_tie_set = 0;
    t->chunk_started = 1;
}

static void tracker_feed_token(OpenNoteTracker* t, int token_id,
    NoteAction* out, int* n_out)
{
    const char* etype;
    int evalue;
    token_to_type_value(token_id, &etype, &evalue);

    if (t->in_prologue) {
        if (strcmp(etype, "tie") == 0) {
            t->in_prologue = 0;
            t->velocity = -1;
            /* end notes that were not tied */
            for (int i = 0; i < t->n_open; ++i) {
                if (!t->open[i].used) continue;
                int keep = 0;
                for (int j = 0; j < t->n_tie_set; ++j)
                    if (t->tie_set_prog[j] == t->open[i].program &&
                        t->tie_set_pitch[j] == t->open[i].pitch) {
                        keep = 1;
                        break;
                    }
                if (!keep) {
                    out[(*n_out)++] = (NoteAction){ ACT_END, t->open[i].program, t->open[i].pitch, t->seek_time };
                    t->open[i].used = 0;
                }
            }
            /* compact */
            int w = 0;
            for (int i = 0; i < t->n_open; ++i)
                if (t->open[i].used)
                    t->open[w++] = t->open[i];
            t->n_open = w;
            return;
        }
        if (strcmp(etype, "shift") == 0) {
            t->in_prologue = 0;
            t->skip_rest = 1;
            tracker_end_all(t, t->seek_time, out, n_out);
            return;
        }
        if (strcmp(etype, "program") == 0)
            t->program = evalue;
        else if (strcmp(etype, "pitch") == 0 && t->program >= 0) {
            if (t->n_tie_set < MAX_OPEN_NOTES) {
                t->tie_set_prog[t->n_tie_set] = t->program;
                t->tie_set_pitch[t->n_tie_set] = evalue;
                t->n_tie_set++;
            }
        }
        return;
    }

    if (t->skip_rest) return;

    if (strcmp(etype, "shift") == 0) {
        if (evalue > 0)
            t->tick_state = t->start_tick + evalue;
    }
    else if (strcmp(etype, "program") == 0) {
        t->program = evalue;
    }
    else if (strcmp(etype, "velocity") == 0) {
        t->velocity = evalue;
    }
    else if (strcmp(etype, "drum") == 0) {
        float time = (float)t->tick_state / FRAME_RATE;
        if (t->next_seek_time < 0.0f || time < t->next_seek_time)
            out[(*n_out)++] = (NoteAction){ ACT_DRUM, 0, evalue, time };
    }
    else if (strcmp(etype, "pitch") == 0) {
        if (t->program < 0 || t->velocity < 0) return;
        float time = (float)t->tick_state / FRAME_RATE;
        if (t->next_seek_time >= 0.0f && time >= t->next_seek_time) return;

        int idx = tracker_find_open(t, t->program, evalue);
        if (idx >= 0) {
            out[(*n_out)++] = (NoteAction){ ACT_END, t->program, evalue, time };
            t->open[idx].used = 0;
        }
        if (t->velocity > 0) {
            /* find free slot */
            int slot = -1;
            for (int i = 0; i < t->n_open; ++i)
                if (!t->open[i].used) { slot = i; break; }
            if (slot < 0 && t->n_open < MAX_OPEN_NOTES)
                slot = t->n_open++;
            if (slot >= 0) {
                t->open[slot] = (OpenNote){ t->program, evalue, time, 1 };
                out[(*n_out)++] = (NoteAction){ ACT_START, t->program, evalue, time };
            }
        }
    }
}

static void tracker_finish(OpenNoteTracker* t, NoteAction* out, int* n_out)
{
    if (t->chunk_started && t->in_prologue)
        tracker_end_all(t, t->seek_time, out, n_out);
    else {
        for (int i = 0; i < t->n_open; ++i) {
            if (!t->open[i].used) continue;
            out[(*n_out)++] = (NoteAction){
                ACT_END, t->open[i].program, t->open[i].pitch,
                t->open[i].time + MINIMUM_NOTE_DUR
            };
        }
        t->n_open = 0;
    }
}

static void tracker_open_keys(OpenNoteTracker* t, int* progs, int* pitches, int* n)
{
    *n = 0;
    for (int i = 0; i < t->n_open; ++i) {
        if (!t->open[i].used) continue;
        progs[*n] = t->open[i].program;
        pitches[*n] = t->open[i].pitch;
        (*n)++;
    }
    /* sort by (program, pitch) */
    for (int i = 0; i < *n; ++i)
        for (int j = i + 1; j < *n; ++j)
            if (progs[j] < progs[i] || (progs[j] == progs[i] && pitches[j] < pitches[i])) {
                int tp = progs[i]; progs[i] = progs[j]; progs[j] = tp;
                int ti = pitches[i]; pitches[i] = pitches[j]; pitches[j] = ti;
            }
}

/* ------------------------------------------------------------------ */
/* Note collection                                                    */
/* ------------------------------------------------------------------ */
typedef struct {
    int pitch;
    int program;
    float start;
    float end;
    int index;
} CollectedNote;

typedef struct {
    CollectedNote notes[MAX_NOTES];
    int n_notes;
    int next_index;
    /* currently open starts keyed by (program,pitch) */
    int open_prog[MAX_OPEN_NOTES];
    int open_pitch[MAX_OPEN_NOTES];
    int open_idx[MAX_OPEN_NOTES];   /* index into notes[] */
    int n_open_starts;
} NoteCollector;

static void collector_init(NoteCollector* c)
{
    memset(c, 0, sizeof(*c));
}

static void collector_apply(NoteCollector* c, const NoteAction* acts, int n_acts)
{
    for (int i = 0; i < n_acts; ++i) {
        const NoteAction* a = &acts[i];
        if (a->type == ACT_END) {
            for (int j = 0; j < c->n_open_starts; ++j) {
                if (c->open_prog[j] == a->program && c->open_pitch[j] == a->pitch) {
                    c->notes[c->open_idx[j]].end = a->time;
                    /* remove */
                    c->open_prog[j] = c->open_prog[c->n_open_starts - 1];
                    c->open_pitch[j] = c->open_pitch[c->n_open_starts - 1];
                    c->open_idx[j] = c->open_idx[c->n_open_starts - 1];
                    c->n_open_starts--;
                    break;
                }
            }
        }
        else if (a->type == ACT_START || a->type == ACT_DRUM) {
            if (c->n_notes >= MAX_NOTES) continue;
            int idx = c->n_notes++;
            c->notes[idx] = (CollectedNote){
                a->pitch, a->program, a->time,
                a->type == ACT_DRUM ? a->time + MINIMUM_NOTE_DUR : 0.0f,
                c->next_index++
            };
            if (a->type == ACT_START && c->n_open_starts < MAX_OPEN_NOTES) {
                c->open_prog[c->n_open_starts] = a->program;
                c->open_pitch[c->n_open_starts] = a->pitch;
                c->open_idx[c->n_open_starts] = idx;
                c->n_open_starts++;
            }
        }
    }
}

/* ------------------------------------------------------------------ */
/* ONNX helpers                                                       */
/* ------------------------------------------------------------------ */
static const OrtApi* g_ort = NULL;

static void check_status(OrtStatus* status, const char* what)
{
    if (status) {
        const char* msg = g_ort->GetErrorMessage(status);
        plugin_log("ORT ERROR (%s): %s", what, msg);
        g_ort->ReleaseStatus(status);
//        abort();
    }
}

static OrtValue* create_tensor_int64(const int64_t* data, const int64_t* shape, size_t rank)
{
    OrtMemoryInfo* mem = NULL;
    check_status(g_ort->CreateCpuMemoryInfo(OrtArenaAllocator, OrtMemTypeDefault, &mem), "meminfo");
    OrtValue* val = NULL;
    size_t n = 1;
    for (size_t i = 0; i < rank; ++i) n *= (size_t)shape[i];
    check_status(g_ort->CreateTensorWithDataAsOrtValue(
        mem, (void*)data, n * sizeof(int64_t),
        shape, rank, ONNX_TENSOR_ELEMENT_DATA_TYPE_INT64, &val), "create int64");
    g_ort->ReleaseMemoryInfo(mem);
    return val;
}

static OrtValue* create_tensor_float(const float* data, const int64_t* shape, size_t rank)
{
    OrtMemoryInfo* mem = NULL;
    check_status(g_ort->CreateCpuMemoryInfo(OrtArenaAllocator, OrtMemTypeDefault, &mem), "meminfo");
    OrtValue* val = NULL;
    size_t n = 1;
    for (size_t i = 0; i < rank; ++i) n *= (size_t)shape[i];
    check_status(g_ort->CreateTensorWithDataAsOrtValue(
        mem, (void*)data, n * sizeof(float),
        shape, rank, ONNX_TENSOR_ELEMENT_DATA_TYPE_FLOAT, &val), "create float");
    g_ort->ReleaseMemoryInfo(mem);
    return val;
}

/* ------------------------------------------------------------------ */
/* Generation for one chunk                                           */
/* ------------------------------------------------------------------ */
static void onnx_generate_chunk(
    OrtSession* sess_prefill, OrtSession* sess_step,
    const float* wav_1x1x80000,          /* [1,1,80000] */
    const int* forced_prefix, int n_forced,
    int* out_tokens, int* n_out_tokens)
{
    plugin_log("onnx_generate_chunk ENTER (n_forced = %d)", n_forced);


    /* ---- prefill ---- */
    int64_t wav_shape[3] = { 1, 1, SEGMENT_SAMPLES };
    OrtValue* wav_val = create_tensor_float(wav_1x1x80000, wav_shape, 3);

    int64_t ig_data[NUM_INSTRUMENTS];
    for (size_t i = 0; i < NUM_INSTRUMENTS; ++i) ig_data[i] = INSTRUMENT_GROUP_IDS[i];
    int64_t ig_shape[2] = { 1, (int64_t)NUM_INSTRUMENTS };
    OrtValue* ig_val = create_tensor_int64(ig_data, ig_shape, 2);

    int64_t dn_data[1] = { DATASET_NAME_NONE_ID };
    int64_t dn_shape[2] = { 1, 1 };
    OrtValue* dn_val = create_tensor_int64(dn_data, dn_shape, 2);


    plugin_log("tensors created, about to Run prefill");


    const char* prefill_in_names[] = { "self_wav", "instrument_group", "dataset_name" };
    const OrtValue* prefill_ins[] = { wav_val, ig_val, dn_val };

    /* outputs: logits + 28 KV – use the real names from the model */
    OrtValue* prefill_outs[1 + NUM_KV] = { 0 };

    const char* prefill_out_names[1 + NUM_KV] = {
        "logits",
        "k0","k1","k2","k3","k4","k5","k6","k7","k8","k9","k10","k11","k12","k13",
        "v0","v1","v2","v3","v4","v5","v6","v7","v8","v9","v10","v11","v12","v13"
    };

    check_status(g_ort->Run(sess_prefill, NULL,
        prefill_in_names, prefill_ins, 3,
        prefill_out_names, 1 + NUM_KV, prefill_outs), "prefill run");

    plugin_log("prefill Run returned");

    // Safety check – make sure we actually got the outputs
    for (int i = 0; i < 1 + NUM_KV; ++i) {
        if (prefill_outs[i] == NULL) {
            plugin_log("ERROR: prefill_outs[%d] is NULL!", i);
        }
    }

    g_ort->ReleaseValue(wav_val);
    g_ort->ReleaseValue(ig_val);
    g_ort->ReleaseValue(dn_val);

    /* past = outs[1..] */
    OrtValue* past[NUM_KV];
    for (int i = 0; i < NUM_KV; ++i) past[i] = prefill_outs[1 + i];

    // Get real sequence length from KV cache (not logits)
    OrtTensorTypeAndShapeInfo* kvinfo = NULL;
    check_status(g_ort->GetTensorTypeAndShape(past[0], &kvinfo), "kv shape for position");
    size_t kv_dim_count = 0;
    g_ort->GetDimensionsCount(kvinfo, &kv_dim_count);
    int64_t kv_dims[8] = { 0 };
    g_ort->GetDimensions(kvinfo, kv_dims, kv_dim_count);
    g_ort->ReleaseTensorTypeAndShapeInfo(kvinfo);

    // past[0] shape is [batch, seq, heads, dim] → seq is dimension 1
    int64_t position = (kv_dim_count >= 2) ? kv_dims[1] : 1;
    plugin_log("position after prefill = %lld (from past[0] shape)", (long long)position);

    plugin_log("about to GetTensorMutableData on logits");
    float* logits_data = NULL;
    check_status(g_ort->GetTensorMutableData(prefill_outs[0], (void**)&logits_data), "logits data");
    plugin_log("GetTensorMutableData returned, logits_data = %p", (void*)logits_data);

    /* ---- step loop ---- */
    *n_out_tokens = 0;

    /* forced prefix first */
    for (int f = 0; f < n_forced; ++f) {
        out_tokens[(*n_out_tokens)++] = forced_prefix[f];

        int64_t tok_data[1] = { forced_prefix[f] };
        int64_t tok_shape[2] = { 1, 1 };
        OrtValue* tok_val = create_tensor_int64(tok_data, tok_shape, 2);

        int64_t pos_data[1] = { position };
        int64_t pos_shape[1] = { 1 };
        OrtValue* pos_val = create_tensor_int64(pos_data, pos_shape, 1);

        const char* step_in_names[2 + NUM_KV];
        step_in_names[0] = "token";
        step_in_names[1] = "position";
        char kbuf[LAYERS][16], vbuf[LAYERS][16];
        for (int i = 0; i < LAYERS; ++i) {
            snprintf(kbuf[i], sizeof(kbuf[i]), "k%d", i);
            snprintf(vbuf[i], sizeof(vbuf[i]), "v%d", i);
            step_in_names[2 + i] = kbuf[i];
            step_in_names[2 + LAYERS + i] = vbuf[i];
        }
        /* NOTE: the actual names must match the exported model.
           The Python reference uses exactly k0..k13 / v0..v13. */

        const OrtValue* step_ins[2 + NUM_KV];
        step_ins[0] = tok_val;
        step_ins[1] = pos_val;
        for (int i = 0; i < NUM_KV; ++i) step_ins[2 + i] = past[i];

        OrtValue* step_outs[1 + NUM_KV] = { 0 };

        const char* step_out_names[1 + NUM_KV] = {
            "logits",
            "new_k0","new_k1","new_k2","new_k3","new_k4","new_k5","new_k6","new_k7",
            "new_k8","new_k9","new_k10","new_k11","new_k12","new_k13",
            "new_v0","new_v1","new_v2","new_v3","new_v4","new_v5","new_v6","new_v7",
            "new_v8","new_v9","new_v10","new_v11","new_v12","new_v13"
        };

        check_status(g_ort->Run(sess_step, NULL,
            step_in_names, step_ins, 2 + NUM_KV,
            step_out_names, 1 + NUM_KV, step_outs), "step run");

        g_ort->ReleaseValue(tok_val);
        g_ort->ReleaseValue(pos_val);
        g_ort->ReleaseValue(prefill_outs[0]); /* old logits */
        for (int i = 0; i < NUM_KV; ++i) g_ort->ReleaseValue(past[i]);

        prefill_outs[0] = step_outs[0];
        for (int i = 0; i < NUM_KV; ++i) past[i] = step_outs[1 + i];
        check_status(g_ort->GetTensorMutableData(prefill_outs[0], (void**)&logits_data), "logits");
        position += 1;
    }

    /* free generation – stop early once we have the unique pitches */
    const int max_tokens_this_chunk = 100;   // hard safety ceiling

    int seen_pitch[128] = { 0 };
    int n_unique = 0;
    int tokens_since_new = 0;

    for (int gen = 0; gen < max_tokens_this_chunk; ++gen) {
        /* logits shape is typically [1, seq, vocab] – take last position */
        OrtTensorTypeAndShapeInfo* linfo = NULL;
        check_status(g_ort->GetTensorTypeAndShape(prefill_outs[0], &linfo), "lshape");
        size_t ldims_cnt = 0;
        g_ort->GetDimensionsCount(linfo, &ldims_cnt);
        int64_t ldims[4];
        g_ort->GetDimensions(linfo, ldims, ldims_cnt);
        g_ort->ReleaseTensorTypeAndShapeInfo(linfo);

        int64_t seq = (ldims_cnt >= 2) ? ldims[ldims_cnt - 2] : 1;
        int64_t vocab = ldims[ldims_cnt - 1];
        float* last = logits_data + (seq - 1) * vocab;

        // Copy last row exactly like Python (do NOT mutate ORT buffer)
        float* step_logits = (float*)malloc((size_t)vocab * sizeof(float));
        memcpy(step_logits, last, (size_t)vocab * sizeof(float));

        /* mask forbidden on the copy */
        for (size_t i = 0; i < NUM_FORBIDDEN; ++i)
            if (FORBIDDEN_IDS[i] < vocab)
                step_logits[FORBIDDEN_IDS[i]] = -INFINITY;

        /* argmax on the copy */
        int next_id = 0;
        float best = step_logits[0];
        for (int64_t i = 1; i < vocab; ++i)
            if (step_logits[i] > best) { best = step_logits[i]; next_id = (int)i; }

        free(step_logits);

        plugin_log("free gen %d: next_id = %d  (seq=%lld vocab=%lld)", gen, next_id, (long long)seq, (long long)vocab);

        if (next_id == EOS_ID) break;

        out_tokens[(*n_out_tokens)++] = next_id;

        /* safety: stop if model jumps past the end of the window */
        if (next_id >= SHIFT_BASE && next_id < PITCH_BASE) {
            int frames = next_id - SHIFT_BASE;
            if (frames > 520)
                break;
        }

        /* unique-pitch early-stop */
        if (next_id >= PITCH_BASE && next_id < VEL_BASE) {
            int p = next_id - PITCH_BASE;
            if (p >= 0 && p < 128) {
                if (!seen_pitch[p]) {
                    seen_pitch[p] = 1;
                    n_unique++;
                    tokens_since_new = 0;
                }
                else {
                    tokens_since_new++;
                }
            }
        }
        else {
            tokens_since_new++;
        }

        /* once we have a few pitches and the model is just repeating, stop */
        if (n_unique >= 3 && tokens_since_new > 20)
            break;

        /* step with next_id */
        int64_t tok_data[1] = { next_id };
        int64_t tok_shape[2] = { 1, 1 };
        OrtValue* tok_val = create_tensor_int64(tok_data, tok_shape, 2);

        int64_t pos_data[1] = { position };
        int64_t pos_shape[1] = { 1 };
        OrtValue* pos_val = create_tensor_int64(pos_data, pos_shape, 1);

        const char* step_in_names[2 + NUM_KV];
        step_in_names[0] = "token";
        step_in_names[1] = "position";
        char kbuf[LAYERS][16], vbuf[LAYERS][16];
        for (int i = 0; i < LAYERS; ++i) {
            snprintf(kbuf[i], sizeof(kbuf[i]), "k%d", i);
            snprintf(vbuf[i], sizeof(vbuf[i]), "v%d", i);
            step_in_names[2 + i] = kbuf[i];
            step_in_names[2 + LAYERS + i] = vbuf[i];
        }

        const OrtValue* step_ins[2 + NUM_KV];
        step_ins[0] = tok_val;
        step_ins[1] = pos_val;
        for (int i = 0; i < NUM_KV; ++i) step_ins[2 + i] = past[i];

        OrtValue* step_outs[1 + NUM_KV] = { 0 };

        const char* step_out_names[1 + NUM_KV] = {
            "logits",
            "new_k0","new_k1","new_k2","new_k3","new_k4","new_k5","new_k6","new_k7",
            "new_k8","new_k9","new_k10","new_k11","new_k12","new_k13",
            "new_v0","new_v1","new_v2","new_v3","new_v4","new_v5","new_v6","new_v7",
            "new_v8","new_v9","new_v10","new_v11","new_v12","new_v13"
        };

        plugin_log("ABOUT TO CALL step Run (position=%lld, next_id=%d)", (long long)position, next_id);
        check_status(g_ort->Run(sess_step, NULL,
            step_in_names, step_ins, 2 + NUM_KV,
            step_out_names, 1 + NUM_KV, step_outs), "step run");

        plugin_log("step Run succeeded, new position will be %lld", (long long)(position + 1));

        g_ort->ReleaseValue(tok_val);
        g_ort->ReleaseValue(pos_val);
        g_ort->ReleaseValue(prefill_outs[0]);
        for (int i = 0; i < NUM_KV; ++i) g_ort->ReleaseValue(past[i]);

        prefill_outs[0] = step_outs[0];
        for (int i = 0; i < NUM_KV; ++i) past[i] = step_outs[1 + i];
        check_status(g_ort->GetTensorMutableData(prefill_outs[0], (void**)&logits_data), "logits");
        position += 1;
    }












    /* cleanup remaining */
    g_ort->ReleaseValue(prefill_outs[0]);
    for (int i = 0; i < NUM_KV; ++i) g_ort->ReleaseValue(past[i]);
}

/* ------------------------------------------------------------------ */
/* Audio load (dr_wav + optional libsamplerate)                       */
/* ------------------------------------------------------------------ */
/* Downmix an interleaved multi-channel buffer to mono and resample it to
 * SAMPLE_RATE if needed. `interleaved` is NOT freed and NOT modified — it is
 * owned by the caller. Returns a newly malloc'd mono float buffer (caller
 * must free it), or NULL on failure. This is the exact downmix/resample
 * logic that used to live inline in load_wav_mono_16k(); it is now shared
 * so callers that already have raw PCM in memory (e.g. audio decoded from
 * BinaryData) go through the identical postprocessing path as file loads. */
static float* downmix_and_resample(const float* interleaved, int64_t n_frames,
    unsigned channels, unsigned sample_rate, int64_t* out_n_samples)
{
    /* downmix */
    float* mono = (float*)malloc((size_t)n_frames * sizeof(float));
    for (int64_t i = 0; i < n_frames; ++i) {
        float s = 0.0f;
        for (unsigned c = 0; c < channels; ++c)
            s += interleaved[i * channels + c];
        mono[i] = s / (float)channels;
    }

    if (sample_rate == SAMPLE_RATE) {
        *out_n_samples = n_frames;
        return mono;
    }

    /* resample */
    double ratio = (double)SAMPLE_RATE / (double)sample_rate;
    int64_t out_len = (int64_t)(n_frames * ratio) + 16;
    float* resampled = (float*)malloc((size_t)out_len * sizeof(float));

    SRC_DATA src;
    src.data_in = mono;
    src.input_frames = (long)n_frames;
    src.data_out = resampled;
    src.output_frames = (long)out_len;
    src.src_ratio = ratio;
    src.end_of_input = 1;

    int err = src_simple(&src, SRC_SINC_BEST_QUALITY, 1);
    free(mono);
    if (err) {
        plugin_log("libsamplerate error: %s", src_strerror(err));
        free(resampled);
        return NULL;
    }
    *out_n_samples = src.output_frames_gen;
    return resampled;
}

static float* load_wav_mono_16k(const char* path, int64_t* out_n_samples)
{
    drwav wav;
    if (!drwav_init_file(&wav, path, NULL)) {
        plugin_log("Failed to open wav: %s", path);
        return NULL;
    }

    size_t total = (size_t)wav.totalPCMFrameCount * wav.channels;
    float* interleaved = (float*)malloc(total * sizeof(float));
    drwav_read_pcm_frames_f32(&wav, wav.totalPCMFrameCount, interleaved);

    float* out = downmix_and_resample(interleaved, (int64_t)wav.totalPCMFrameCount,
        wav.channels, wav.sampleRate, out_n_samples);

    free(interleaved);
    drwav_uninit(&wav);
    return out;
}

/* ------------------------------------------------------------------ */
/* Public entry point                                                 */
/* ------------------------------------------------------------------ */

/* ------------------------------------------------------------------ */
/* Persistent ONNX state                                              */
/* ------------------------------------------------------------------ */
static OrtEnv* g_env = NULL;
static OrtSession* g_sess_prefill = NULL;
static OrtSession* g_sess_step = NULL;

#ifdef _WIN32
#include <windows.h>
#endif

void ppd_load_models(const char* prefill_path_utf8, const char* step_path_utf8)
{
    if (g_sess_prefill && g_sess_step) {
        plugin_log("Models already loaded");
        return;
    }

    if (!prefill_path_utf8 || !step_path_utf8) {
        plugin_log("ppd_load_models: null path(s)");
        return;
    }

    plugin_log("=== Loading models ===");
    plugin_log("  prefill: %s", prefill_path_utf8);
    plugin_log("  step:    %s", step_path_utf8);

    g_ort = OrtGetApiBase()->GetApi(ORT_API_VERSION);
    if (!g_ort) {
        plugin_log("Failed to get ORT API");
        return;
    }

    check_status(g_ort->CreateEnv(ORT_LOGGING_LEVEL_WARNING, "PPD", &g_env), "env");

    OrtSessionOptions* opts = NULL;
    check_status(g_ort->CreateSessionOptions(&opts), "opts");
    g_ort->SetIntraOpNumThreads(opts, 1);
    g_ort->SetInterOpNumThreads(opts, 1);
    g_ort->SetSessionGraphOptimizationLevel(opts, ORT_ENABLE_BASIC);

#ifdef _WIN32
    wchar_t prefill_w[MAX_PATH];
    wchar_t step_w[MAX_PATH];

    if (MultiByteToWideChar(CP_UTF8, 0, prefill_path_utf8, -1, prefill_w, MAX_PATH) == 0 ||
        MultiByteToWideChar(CP_UTF8, 0, step_path_utf8, -1, step_w, MAX_PATH) == 0)
    {
        plugin_log("Failed to convert model paths to wide strings");
        g_ort->ReleaseSessionOptions(opts);
        return;
    }

    check_status(g_ort->CreateSession(g_env, prefill_w, opts, &g_sess_prefill), "prefill session");
    check_status(g_ort->CreateSession(g_env, step_w, opts, &g_sess_step), "step session");
#else
    check_status(g_ort->CreateSession(g_env, prefill_path_utf8, opts, &g_sess_prefill), "prefill session");
    check_status(g_ort->CreateSession(g_env, step_path_utf8, opts, &g_sess_step), "step session");
#endif

    g_ort->ReleaseSessionOptions(opts);

    plugin_log("Models loaded successfully");
}

/* Runs the chunked ONNX inference + note-tracking + JSON logging over a
 * mono 16 kHz buffer. `wav` is NOT freed here — the caller owns it and is
 * responsible for freeing it after this returns. Shared by both the
 * file-based (ppd_run_test) and buffer-based (ppd_run_test_buffer) entry
 * points so the two paths stay byte-for-byte identical downstream of the
 * mono/16k conversion. */
static void run_transcription_pipeline(const float* wav, int64_t n_samples, C_PitchResult* out_pitches) 
{
    plugin_log("Audio loaded: %lld samples @ 16 kHz", (long long)n_samples);

    int num_chunks = (int)((n_samples + SEGMENT_SAMPLES - 1) / SEGMENT_SAMPLES);
    plugin_log("About to enter chunk loop (num_chunks = %d)", num_chunks);

    OpenNoteTracker tracker;
    tracker_init(&tracker);
    NoteCollector collector;
    collector_init(&collector);

    int tokens_buf[MAX_GEN_LEN + 256];
    NoteAction acts[512];

    for (int i = 0; i < num_chunks; ++i) {
        float* chunk = (float*)calloc(1 * 1 * SEGMENT_SAMPLES, sizeof(float));
        if (!chunk) {
            plugin_log("Out of memory for chunk buffer");
            break;
        }

        int64_t start = (int64_t)i * SEGMENT_SAMPLES;
        int64_t n = n_samples - start;
        if (n > SEGMENT_SAMPLES) n = SEGMENT_SAMPLES;
        memcpy(chunk, wav + start, (size_t)n * sizeof(float));

        ///* Hold the last sample instead of zero-padding.
        //   Zero-padding makes the model see silence and frequently output 0 notes. */
        //if (n > 0 && n < SEGMENT_SAMPLES) {
        //    float last = chunk[n - 1];
        //    for (int64_t j = n; j < SEGMENT_SAMPLES; ++j)
        //        chunk[j] = last;
        //}

        /* Zero-pad the remainder (matches the Python reference).
           Hold-last turns short events into long sustained tones and causes
           the model to emit hundreds of repeated note events. */
        if (n < SEGMENT_SAMPLES) {
            memset(chunk + n, 0, (size_t)(SEGMENT_SAMPLES - n) * sizeof(float));
        }





        float seek = (float)i * 5.0f;
        float next_seek = (i + 1 < num_chunks) ? (float)(i + 1) * 5.0f : (float)n_samples / (float)SAMPLE_RATE;

        int n_acts = 0;
        tracker_feed_boundary(&tracker, seek, next_seek, acts, &n_acts);
        collector_apply(&collector, acts, n_acts);

        int forced[512];
        int n_forced = 0;
        if (i > 0) {
            int progs[MAX_OPEN_NOTES], pitches[MAX_OPEN_NOTES], n_keys = 0;
            tracker_open_keys(&tracker, progs, pitches, &n_keys);
            int cur_prog = -1;
            for (int k = 0; k < n_keys; ++k) {
                if (progs[k] != cur_prog) {
                    forced[n_forced++] = get_token_id("program", progs[k]);
                    cur_prog = progs[k];
                }
                forced[n_forced++] = get_token_id("pitch", pitches[k]);
            }
            forced[n_forced++] = TIE_ID;
        }

        int n_tok = 0;
        onnx_generate_chunk(g_sess_prefill, g_sess_step, chunk,
            forced, n_forced, tokens_buf, &n_tok);
        plugin_log("chunk %d: %d tokens (%d forced)", i, n_tok, n_forced);

        for (int t = 0; t < n_tok; ++t) {
            n_acts = 0;
            tracker_feed_token(&tracker, tokens_buf[t], acts, &n_acts);
            collector_apply(&collector, acts, n_acts);
        }

        free(chunk);
    }

    {
        int n_acts = 0;
        tracker_finish(&tracker, acts, &n_acts);
        collector_apply(&collector, acts, n_acts);
    }

    /* sort notes by start time */
    for (int i = 0; i < collector.n_notes; ++i)
        for (int j = i + 1; j < collector.n_notes; ++j)
            if (collector.notes[j].start < collector.notes[i].start) {
                CollectedNote tmp = collector.notes[i];
                collector.notes[i] = collector.notes[j];
                collector.notes[j] = tmp;
            }


    /*plugin_log("=== %d notes ===", collector.n_notes);
    for (int i = 0; i < collector.n_notes; ++i) {
        CollectedNote* n = &collector.notes[i];
        plugin_log("  [%d] pitch=%d  program=%d  start=%.3f  end=%.3f",
            i, n->pitch, n->program, n->start, n->end);
    }*/


    /* Collect every pitch token that appeared, ignore everything else */
    int seen[128] = { 0 };          /* MIDI note 0-127 */
    int pitches[128];
    int n_pitches = 0;

    for (int i = 0; i < collector.n_notes; ++i) {
        int p = collector.notes[i].pitch;
        if (p >= 0 && p < 128 && !seen[p]) {
            seen[p] = 1;
            pitches[n_pitches++] = p;
        }
    }

    /* Also scan the raw token buffer of the last chunk in case the collector stayed empty */
    /* (simple extra safety – not perfect across all chunks but good enough for now) */

    /*plugin_log("=== %d unique pitches ===", n_pitches);
    for (int i = 0; i < n_pitches; ++i)
        plugin_log("  %d", pitches[i]);*/

    plugin_log("=== %d unique pitches ===", n_pitches);
    for (int i = 0; i < n_pitches; ++i)
        plugin_log("  %d", pitches[i]);

    if (out_pitches) {
        int count = n_pitches;
        if (count > 128) count = 128;
        for (int i = 0; i < count; ++i)
            out_pitches->pitches[i] = pitches[i];
        out_pitches->num_pitches = count;
    }
}




void ppd_run_test_buffer(const C_FloatArray* audio, C_PitchResult* out_pitches)
{
    if (out_pitches) out_pitches->num_pitches = 0;

    if (!g_sess_prefill || !g_sess_step) {
        plugin_log("Models not loaded – call ppd_load_models() first");
        return;
    }
    if (!audio || !audio->data || audio->num_samples <= 0 || audio->num_channels <= 0) {
        plugin_log("ppd_run_test_buffer: invalid input buffer");
        return;
    }

    plugin_log("=== PPD transcription start (buffer) ===");

    int64_t n_samples = 0;
    float* wav = downmix_and_resample(audio->data, audio->num_samples,
        (unsigned)audio->num_channels, (unsigned)audio->sample_rate, &n_samples);
    if (!wav) {
        plugin_log("Failed to process input audio buffer");
        return;
    }

    run_transcription_pipeline(wav, n_samples, out_pitches);
    free(wav);

    plugin_log("=== PPD transcription done ===");
}

void ppd_shutdown(void)
{
    if (g_sess_prefill) { g_ort->ReleaseSession(g_sess_prefill); g_sess_prefill = NULL; }
    if (g_sess_step) { g_ort->ReleaseSession(g_sess_step);    g_sess_step = NULL; }
    if (g_env) { g_ort->ReleaseEnv(g_env);              g_env = NULL; }
    plugin_log("PPD shutdown");
}
