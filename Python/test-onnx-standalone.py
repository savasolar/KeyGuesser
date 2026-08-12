"""
test-onnx-standalone.py

Proves the whole chain (mel+transformer via ONNX, tokenizer/vocab,
tie-forcing across chunks, token decode) runs standalone.

Requires: onnxruntime, numpy, soundfile  (pip install onnxruntime numpy soundfile)
"""
import json
import sys
from pathlib import Path

import numpy as np
import onnxruntime as ort
import soundfile as sf

from standalone_tokenizer import (
    VOCAB, EOS_ID, BOS_ID, FRAME_RATE, SAMPLE_RATE,
    INSTRUMENT_GROUP_IDS, DATASET_NAME_NONE_ID, FORBIDDEN_IDS_NO_DRUMS,
    ChunkBoundary, OpenNoteTracker, decode_model_tokens,
    tie_section_token_ids, default_instrument_for_program,
)

LAYERS = 14
KV_NAMES = [f"k{i}" for i in range(LAYERS)] + [f"v{i}" for i in range(LAYERS)]
SEGMENT_SAMPLES = 80_000
MAX_GEN_LEN = 2000

# ORDER MATTERS: this becomes a sequence of conditioning positions fed to the
# transformer, so it must match transcribe.py's list order exactly, not just
# its contents.
INSTRUMENTS_NO_DRUMS = [
    "acoustic_piano", "electric_piano", "chromatic_percussion", "organ",
    "acoustic_guitar", "clean_electric_guitar", "distorted_electric_guitar",
    "acoustic_bass", "electric_bass", "violin", "viola", "cello", "contrabass",
    "orchestral_harp", "timpani", "string_ensemble", "synth_strings", "voice",
    "orchestra_hit", "trumpet", "trombone", "tuba", "french_horn", "brass_section",
    "soprano_and_alto_sax", "tenor_sax", "baritone_sax", "oboe", "english_horn",
    "bassoon", "clarinet", "flutes", "synth_lead", "synth_pad",
]

sess_prefill = ort.InferenceSession("prefill.onnx", providers=["CPUExecutionProvider"])
sess_step = ort.InferenceSession("step.onnx", providers=["CPUExecutionProvider"])

instrument_group_np = np.array(
    [[INSTRUMENT_GROUP_IDS[n] for n in INSTRUMENTS_NO_DRUMS]], dtype=np.int64
)
dataset_name_np = np.array([[DATASET_NAME_NONE_ID]], dtype=np.int64)
forbidden_ids = np.array(FORBIDDEN_IDS_NO_DRUMS)


def load_wav_mono_16k(path: str) -> np.ndarray:
    """Returns shape [1, T] float32 mono @ 16kHz."""
    wav, sr = sf.read(path, dtype="float32", always_2d=True)  # [T, C]
    wav = wav.mean(axis=1)  # downmix to mono
    if sr != SAMPLE_RATE:
        from scipy.signal import resample_poly
        from math import gcd
        g = gcd(sr, SAMPLE_RATE)
        wav = resample_poly(wav, SAMPLE_RATE // g, sr // g).astype(np.float32)
    return wav[None, :]  # [1, T]


def onnx_generate_chunk_tokens(self_wav_np, forced_prefix=(), max_gen_len=MAX_GEN_LEN):
    """Mirrors test-onnx.py's onnx_generate_chunk_tokens exactly."""
    outs = sess_prefill.run(
        None,
        {"self_wav": self_wav_np, "instrument_group": instrument_group_np, "dataset_name": dataset_name_np},
    )
    logits, past = outs[0], list(outs[1:])
    position = past[0].shape[1]

    print("logits.shape =", logits.shape)
    print("past[0].shape =", past[0].shape)          # first key
    print("past[14].shape =", past[14].shape)        # first value (or whatever index is v0)
    print("position =", position)
    print("logits.dtype =", logits.dtype)
    print("logits[0, -1, :5] =", logits[0, -1, :5])  # first few values
    print("argmax of last logits =", int(logits[0, -1].argmax()))

    def step(token_id):
        nonlocal logits, past, position
        feed = {"token": np.array([[token_id]], dtype=np.int64),
                "position": np.array([position], dtype=np.int64),
                **dict(zip(KV_NAMES, past))}
        outs = sess_step.run(None, feed)
        logits, past = outs[0], list(outs[1:])
        position += 1

    tokens = []
    for forced_id in forced_prefix:
        tokens.append(forced_id)
        step(forced_id)

    for _ in range(max(0, max_gen_len - len(forced_prefix))):
        step_logits = logits[0, -1].copy()
        step_logits[forbidden_ids] = -np.inf
        next_id = int(step_logits.argmax())
        if next_id == EOS_ID:
            break
        tokens.append(next_id)
        step(next_id)
    else:
        print(f"[onnx] chunk did not emit EOS within {max_gen_len} tokens", file=sys.stderr)
    return tokens


def main():
    from standalone_tokenizer import _TOKEN_INDEX   # add this import

    print("1134 =", VOCAB[1134])
    print("18   =", VOCAB[18])
    print("1137 =", VOCAB[1137])
    print("1133 =", VOCAB[1133])
    print("1064 =", VOCAB[1064])
    print("1132 =", VOCAB[1132])
    print("1135 (tie) =", VOCAB[1135] if 1135 < len(VOCAB) else "out of range")
    print("program 2 token id =", _TOKEN_INDEX.get(("program", 2)))
    print("pitch 60 token id  =", _TOKEN_INDEX.get(("pitch", 60)))
    print("velocity 1 token id=", _TOKEN_INDEX.get(("velocity", 1)))

    wav = load_wav_mono_16k("test.wav")
    num_chunks = -(-wav.shape[-1] // SEGMENT_SAMPLES)  # ceil div

    tracker = OpenNoteTracker(VOCAB, FRAME_RATE)
    full_stream = []

    for i in range(num_chunks):
        chunk = wav[:, i * SEGMENT_SAMPLES: (i + 1) * SEGMENT_SAMPLES]
        if chunk.shape[-1] < SEGMENT_SAMPLES:
            chunk = np.pad(chunk, ((0, 0), (0, SEGMENT_SAMPLES - chunk.shape[-1])))
        self_wav_np = chunk[None, :, :].astype(np.float32)  # [1, 1, 80000]

        bnd = ChunkBoundary(i * 5.0, (i + 1) * 5.0 if i + 1 < num_chunks else None)
        tracker.feed(bnd)
        full_stream.append(bnd)

        forced_prefix = tie_section_token_ids(tracker.open_keys()) if i > 0 else ()
        chunk_tokens = onnx_generate_chunk_tokens(self_wav_np, forced_prefix=forced_prefix)
        print(chunk_tokens)
        print(f"chunk {i}: {len(chunk_tokens)} tokens generated ({len(forced_prefix)} forced)")

        for t in chunk_tokens:
            tracker.feed(t)
            full_stream.append(t)

    events = list(decode_model_tokens(
        iter(full_stream), VOCAB, default_instrument_for_program, frame_rate=FRAME_RATE
    ))

    # pair NoteStart/NoteEnd into flat note dicts
    starts = {}
    notes = []
    for ev in events:
        if type(ev).__name__ == "NoteStartEvent":
            starts[ev.index] = ev
        else:  # NoteEndEvent
            s = ev.start_event
            notes.append({
                "pitch": s.pitch,
                "instrument": s.instrument,
                "start": round(s.start_time, 3),
                "end": round(ev.end_time, 3),
            })

    notes.sort(key=lambda n: n["start"])
    out_path = Path("onnx-standalone-notes.json")
    out_path.write_text(json.dumps(notes, indent=2))
    print(f"\n{len(notes)} notes decoded, zero muscriptor/torch imports used.")
    print(f"Saved note events to {out_path.resolve()}")

if __name__ == "__main__":
    main()
