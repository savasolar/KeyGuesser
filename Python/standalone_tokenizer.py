"""
standalone_tokenizer.py

The tie-forcing + token-decode logic, ported verbatim from the muscriptor
source we extracted (OpenNoteTracker, decode_model_tokens, ChunkBoundary,
MT3Tokenizer.tie_section_token_ids/forbidden_token_ids), backed by the actual
vocab dumped from your installed model. Zero muscriptor import. Zero torch
import. Just stdlib + a JSON resource file.

Instrument NAMES on note events are not yet available here (that needs
program_to_name, coming in round 2) — notes are labeled by raw program number
for now ("program_40" etc). Everything else — pitches, onset/offset times,
tie continuity across chunks, forbidden-token masking — is byte-for-byte the
same logic as the original.
"""
import json
from dataclasses import dataclass
from pathlib import Path
from typing import Iterable, Iterator, Union

_RESOURCES_PATH = Path(__file__).parent / "resources_clean.json"
_R = json.loads(_RESOURCES_PATH.read_text())

VOCAB: list[tuple[str, int]] = [(t, v) for t, v in _R["vocab"]]
NUM_TOKENS = len(VOCAB)
EOS_ID: int = _R["config"]["eos_id"]
BOS_ID: int = _R["config"]["initial_token_id"]
FRAME_RATE: int = _R["config"]["frame_rate"]
SAMPLE_RATE: int = _R["config"]["sample_rate"]
INSTRUMENT_GROUP_IDS: dict[str, int] = {
    name: ids[0][0] for name, ids in _R["instrument_group_ids"].items()
}
DATASET_NAME_NONE_ID: int = _R["dataset_name_none_id"][0][0]
FORBIDDEN_IDS_NO_DRUMS: list[int] = list(_R["forbidden_ids_no_drums"])

_TOKEN_INDEX: dict[tuple[str, int], int] = {
    (t, v): i for i, (t, v) in enumerate(VOCAB)
}


def tie_section_token_ids(open_note_keys: Iterable[tuple[int, int]]) -> list[int]:
    """Ported verbatim from MT3Tokenizer.tie_section_token_ids."""
    tokens: list[int] = []
    program_state: int | None = None
    for program, pitch in sorted(open_note_keys):
        if program != program_state:
            tokens.append(_TOKEN_INDEX[("program", program)])
            program_state = program
        tokens.append(_TOKEN_INDEX[("pitch", pitch)])
    tokens.append(_TOKEN_INDEX[("tie", 0)])
    return tokens


@dataclass
class ChunkBoundary:
    seek_time: float
    next_seek_time: float | None


# --- internal action types (our own, only used to move data between
#     OpenNoteTracker.feed and decode_model_tokens) ---
@dataclass
class _StartNote:
    program: int
    pitch: int
    time: float


@dataclass
class _EndNote:
    program: int
    pitch: int
    time: float


@dataclass
class _DrumHit:
    pitch: int
    time: float


_NoteAction = Union[_StartNote, _EndNote, _DrumHit]


# --- output event types (minimal — just enough fields for what we build
#     downstream right now; NOT yet verified against muscriptor's real
#     NoteStartEvent/NoteEndEvent field set, pending round 2) ---
@dataclass
class NoteStartEvent:
    pitch: int
    start_time: float
    index: int
    instrument: str


@dataclass
class NoteEndEvent:
    end_time: float
    start_event: NoteStartEvent


class ProgressEvent:
    """Placeholder — never appears in the streams we build (no muscriptor
    progress-reporting), only defined so the isinstance check below works."""


MINIMUM_NOTE_DURATION_SEC = 0.01  # PLACEHOLDER — round 2 will give the real constant
_DRUM_INSTRUMENT = "drums"  # matches the "drums" name used throughout


class OpenNoteTracker:
    """Ported verbatim from muscriptor.events.OpenNoteTracker."""

    def __init__(self, vocab: list[tuple[str, int]], frame_rate: int = 100):
        self._vocab = vocab
        self._frame_rate = frame_rate
        self._open: dict[tuple[int, int], float] = {}
        self._seek_time = 0.0
        self._next_seek_time: float | None = None
        self._start_tick = 0
        self._tick_state = 0
        self._program: int | None = None
        self._velocity: int | None = None
        self._in_prologue = True
        self._skip_rest = False
        self._tie_set: set[tuple[int, int]] = set()
        self._chunk_started = False

    def feed(self, item: "int | ChunkBoundary") -> list[_NoteAction]:
        if isinstance(item, ChunkBoundary):
            actions: list[_NoteAction] = []
            if self._chunk_started and self._in_prologue:
                actions = self._end_all(self._seek_time)
            self._seek_time = item.seek_time
            self._next_seek_time = item.next_seek_time
            self._start_tick = round(item.seek_time * self._frame_rate)
            self._tick_state = self._start_tick
            self._program = None
            self._velocity = None
            self._in_prologue = True
            self._skip_rest = False
            self._tie_set = set()
            self._chunk_started = True
            return actions

        etype, evalue = self._vocab[item]

        if self._in_prologue:
            if etype == "tie":
                self._in_prologue = False
                self._velocity = None
                ended = [k for k in self._open if k not in self._tie_set]
                for key in ended:
                    del self._open[key]
                return [_EndNote(*key, self._seek_time) for key in ended]
            if etype == "shift":
                self._in_prologue = False
                self._skip_rest = True
                return self._end_all(self._seek_time)
            if etype == "program":
                self._program = evalue
            elif etype == "pitch" and self._program is not None:
                self._tie_set.add((self._program, evalue))
            return []

        if self._skip_rest:
            return []

        if etype == "shift":
            if evalue > 0:
                self._tick_state = self._start_tick + evalue
        elif etype == "program":
            self._program = evalue
        elif etype == "velocity":
            self._velocity = evalue
        elif etype == "drum":
            time = self._tick_state / self._frame_rate
            if self._next_seek_time is None or time < self._next_seek_time:
                return [_DrumHit(evalue, time)]
        elif etype == "pitch":
            if self._program is None or self._velocity is None:
                return []
            time = self._tick_state / self._frame_rate
            if self._next_seek_time is not None and time >= self._next_seek_time:
                return []
            key = (self._program, evalue)
            actions = []
            if key in self._open:
                del self._open[key]
                actions.append(_EndNote(*key, time))
            if self._velocity > 0:
                self._open[key] = time
                actions.append(_StartNote(*key, time))
            return actions
        return []

    def finish(self) -> list[_NoteAction]:
        if self._chunk_started and self._in_prologue:
            return self._end_all(self._seek_time)
        actions = [
            _EndNote(*key, onset + MINIMUM_NOTE_DURATION_SEC)
            for key, onset in self._open.items()
        ]
        self._open.clear()
        return actions

    def _end_all(self, time: float) -> list[_NoteAction]:
        actions: list[_NoteAction] = [_EndNote(*key, time) for key in self._open]
        self._open.clear()
        return actions

    def open_keys(self) -> list[tuple[int, int]]:
        return sorted(self._open)


def decode_model_tokens(
    stream: Iterator["int | ChunkBoundary | ProgressEvent"],
    vocab: list[tuple[str, int]],
    instrument_for_program,
    frame_rate: int = 100,
) -> Iterator[Union[NoteStartEvent, NoteEndEvent, ProgressEvent]]:
    """Ported verbatim from muscriptor.events.decode_model_tokens."""
    tracker = OpenNoteTracker(vocab, frame_rate)
    open_notes: dict[tuple[int, int], NoteStartEvent] = {}
    next_index = 0

    def mint(pitch: int, start_time: float, instrument: str) -> NoteStartEvent:
        nonlocal next_index
        ev = NoteStartEvent(pitch=pitch, start_time=start_time, index=next_index, instrument=instrument)
        next_index += 1
        return ev

    def events_for(actions):
        for action in actions:
            if isinstance(action, _EndNote):
                start = open_notes.pop((action.program, action.pitch))
                yield NoteEndEvent(end_time=action.time, start_event=start)
            elif isinstance(action, _StartNote):
                start = mint(action.pitch, action.time, instrument_for_program(action.program))
                open_notes[(action.program, action.pitch)] = start
                yield start
            else:  # _DrumHit
                start = mint(action.pitch, action.time, _DRUM_INSTRUMENT)
                yield start
                yield NoteEndEvent(end_time=action.time + MINIMUM_NOTE_DURATION_SEC, start_event=start)

    for item in stream:
        if isinstance(item, ProgressEvent):
            yield item
            continue
        yield from events_for(tracker.feed(item))
    yield from events_for(tracker.finish())


def default_instrument_for_program(program: int) -> str:
    """PLACEHOLDER until round 2 gives us the real program_to_name table."""
    return f"program_{program}"


if __name__ == "__main__":
    # self-test: not a full pipeline test (no ONNX/model here), just proving
    # the ported state machine runs correctly against the real vocab and
    # produces sane note events for a hand-built token stream.
    piano_program = INSTRUMENT_GROUP_IDS  # just to show the table loaded
    print(f"Loaded {NUM_TOKENS} tokens, {len(INSTRUMENT_GROUP_IDS)} instrument names.")
    print(f"EOS={EOS_ID} BOS={BOS_ID} frame_rate={FRAME_RATE}")

    # find real token ids for: program=0 (acoustic piano's GM program per the
    # earlier forbidden-id cross-check), velocity=100, pitch=60, shift=50
    prog_id = _TOKEN_INDEX[("program", 0)]
    vel_id = _TOKEN_INDEX[("velocity", 1)]  # vocab's velocity is binary on/off, not 0-127
    pitch_id = _TOKEN_INDEX[("pitch", 60)]
    shift50_id = _TOKEN_INDEX[("shift", 50)]
    tie_id = _TOKEN_INDEX[("tie", 0)]

    # chunk 0: empty tie prologue (just `tie`), then note-on at t=0, note-off
    # (velocity 0 retrigger) at shift 50 (=0.5s @ 100fps)
    vel0_id = _TOKEN_INDEX[("velocity", 0)]
    stream = [
        ChunkBoundary(0.0, 5.0),
        tie_id,                     # empty tie prologue
        prog_id, vel_id, pitch_id,  # note-on: program 0, pitch 60, vel 100
        shift50_id,
        prog_id, vel0_id, pitch_id,  # note-off: same key, vel 0
    ]
    events = list(decode_model_tokens(iter(stream), VOCAB, default_instrument_for_program, FRAME_RATE))
    for e in events:
        print(e)
    assert len(events) == 2, f"expected 1 start + 1 end, got {len(events)}"
    assert isinstance(events[0], NoteStartEvent) and events[0].pitch == 60 and events[0].start_time == 0.0
    assert isinstance(events[1], NoteEndEvent) and events[1].end_time == 0.5
    print("\nself-test passed: tie prologue, program/velocity/pitch decode, and note-off retrigger all work.")
