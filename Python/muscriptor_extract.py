"""
muscriptor_extract.py

Dumps every non-neural resource that test-onnx.py currently pulls from the
`muscriptor` package (vocab, id tables, and the algorithmic glue code), so we
can build a version of the pipeline that only touches:

    prefill.onnx / step.onnx  +  these resource files

...with zero torch, zero muscriptor, zero model.safetensors at inference time.

Run this in your EXISTING working venv (the one where test-onnx.py already
works):

    python muscriptor_extract.py

It writes ./onnx_resources/:
    resources.json     - static/serializable data (vocab, ids, mappings, config)
    source_dump.txt     - real source of the algorithmic pieces (via inspect),
                          for porting to standalone Python / C++
    extract_log.txt     - what succeeded / failed, so we know exactly what's
                          still missing before we build the standalone version

Plus resources_clean.json in the current directory (the cleaned subset).
"""
import inspect
import json
import sys
from pathlib import Path

OUT = Path("onnx_resources")
OUT.mkdir(exist_ok=True)

resources = {}
source_chunks = []
log = []


def note(msg):
    print(msg)
    log.append(msg)


def try_get(label, fn):
    try:
        val = fn()
        note(f"[ok]   {label}")
        return val
    except Exception as e:
        note(f"[FAIL] {label}: {e!r}")
        return None


def try_source(label, obj):
    try:
        src = inspect.getsource(obj)
        source_chunks.append(f"# ===== {label} =====\n{src}\n")
        note(f"[ok]   source: {label}")
    except Exception as e:
        note(f"[FAIL] source: {label}: {e!r}")


# ---- load model (same as test-onnx.py) ----
from muscriptor import TranscriptionModel

model = try_get(
    "TranscriptionModel.load_model('small')",
    lambda: TranscriptionModel.load_model("small", device="cpu"),
)
if model is None:
    note("Cannot continue without a loaded model — aborting.")
    sys.exit(1)

tokenizer = model._tokenizer
inst_cond = model._model.condition_provider.conditioners["instrument_group"]
ds_cond = model._model.condition_provider.conditioners["dataset_name"]
mel_cond = model._model.condition_provider.conditioners["self_wav"]

# ---- static config ----
resources["config"] = try_get(
    "basic config (frame_rate, eos_id, bos id, mel params)",
    lambda: {
        "frame_rate": tokenizer.frame_rate,
        "eos_id": tokenizer.eos_id,
        "initial_token_id": model._model.initial_token_id,
        "sample_rate": getattr(mel_cond, "sample_rate", None),
        "n_fft": getattr(mel_cond, "n_fft", None),
        "n_mel_bins": getattr(mel_cond, "n_mel_bins", None),
    },
)

# ---- full vocab ----
resources["vocab"] = try_get("tokenizer._vocab", lambda: tokenizer._vocab)

# ---- instrument name -> class id, for every known instrument ----
from muscriptor.tokenizer.mt3 import instrument_group_from_names

ALL_INSTRUMENTS = [
    "acoustic_piano", "electric_piano", "chromatic_percussion", "organ",
    "acoustic_guitar", "clean_electric_guitar", "distorted_electric_guitar",
    "acoustic_bass", "electric_bass", "violin", "viola", "cello", "contrabass",
    "orchestral_harp", "timpani", "string_ensemble", "synth_strings", "voice",
    "orchestra_hit", "trumpet", "trombone", "tuba", "french_horn", "brass_section",
    "soprano_and_alto_sax", "tenor_sax", "baritone_sax", "oboe", "english_horn",
    "bassoon", "clarinet", "flutes", "synth_lead", "synth_pad", "drums",
]

inst_table = {}
for name in ALL_INSTRUMENTS:
    def f(name=name):
        return inst_cond.tokenize([instrument_group_from_names([name])]).numpy().tolist()
    inst_table[name] = try_get(f"instrument id: {name}", f)
resources["instrument_group_ids"] = inst_table

resources["dataset_name_none_id"] = try_get(
    "dataset_name id for None",
    lambda: ds_cond.tokenize([None]).numpy().tolist(),
)

# ---- forbidden ids for the exact instrument set your scripts use ----
INSTRUMENTS_NO_DRUMS = [n for n in ALL_INSTRUMENTS if n != "drums"]


def get_forbidden():
    ids = tokenizer.forbidden_token_ids(INSTRUMENTS_NO_DRUMS)
    return ids.tolist() if hasattr(ids, "tolist") else list(ids)


resources["forbidden_ids_no_drums"] = try_get(
    "forbidden_token_ids(INSTRUMENTS_NO_DRUMS)", get_forbidden
)

# ---- instrument-for-program table (used at decode time) ----
resources["instrument_for_program_repr"] = try_get(
    "repr(model._instrument_for_program)",
    lambda: repr(model._instrument_for_program)[:20000],
)

# ---- source of the algorithmic pieces — these are what actually need porting ----
from muscriptor.events import ChunkBoundary, OpenNoteTracker, decode_model_tokens

try_source("tokenizer class (full)", type(tokenizer))
try_source("ChunkBoundary", ChunkBoundary)
try_source("OpenNoteTracker", OpenNoteTracker)
try_source("decode_model_tokens", decode_model_tokens)
try_source("instrument_group_from_names", instrument_group_from_names)
try_source("TranscriptionModel._load_wav", model._load_wav)
try_source("TranscriptionModel.detect_beat_grid_for", model.detect_beat_grid_for)
try_source("TranscriptionModel.events_to_midi_bytes", model.events_to_midi_bytes)
try_source("model._instrument_for_program", model._instrument_for_program)

# ---- write everything out ----
(OUT / "resources.json").write_text(json.dumps(resources, indent=2, default=repr))
(OUT / "source_dump.txt").write_text("\n\n".join(source_chunks))
(OUT / "extract_log.txt").write_text("\n".join(log))

# ---- also write the cleaned subset that the rest of the code expects ----
clean = {
    "config": resources["config"],
    "vocab": resources["vocab"],
    "instrument_group_ids": resources["instrument_group_ids"],
    "dataset_name_none_id": resources["dataset_name_none_id"],
    "forbidden_ids_no_drums": resources["forbidden_ids_no_drums"],
}
Path("resources_clean.json").write_text(json.dumps(clean, indent=2))

print(f"\nDone. Wrote:")
print(f"  {OUT / 'resources.json'}")
print(f"  {OUT / 'source_dump.txt'}")
print(f"  {OUT / 'extract_log.txt'}")
print(f"  resources_clean.json")
print("\nSend these three files back and I'll build the standalone (zero-muscriptor,")
print("zero-torch) test + a concrete C/C++ porting checklist from what's actually in them.")