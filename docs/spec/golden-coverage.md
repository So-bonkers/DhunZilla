# Golden feature coverage — `opd golden-coverage` (WP-0211, additive)

Harness hygiene for the Phase 1 exit. Phase 1 promised bit-exact golden renders; what nobody can
answer today is the other half of the question: **which model features do those goldens actually
exercise?** If a feature has no golden, a later refactor of it can silently break a real song while
CI stays green. This command prints that map, and the list of features NO golden covers, so drafters
know where a new golden is needed.

It is a **report, not a gate**: it never fails because coverage is missing, it writes nothing, and it
changes no render, no model field and no operation. Nothing in CI calls it.

## CLI

```
opd golden-coverage <dir|file.opj> [<dir|file.opj> ...]
```

* Each argument is a `.opj` file, or a directory whose `*.opj` files (name suffix exactly `.opj`,
  non-recursive) are scanned. Arguments may be mixed and repeated; there are **no options**.
* The report goes to stdout. Nothing goes to stdout on an error.
* Exit codes (spec project-model.md convention): `0` success — **including when features are
  uncovered**; `2` usage (`golden-coverage needs at least one <dir or .opj>`, `unknown arg '<arg>'`,
  `no .opj files in <dir>`); `3` not-found (detail = the path, checked before anything is loaded);
  `4` parse (detail = `load_project`'s own message, first bad file wins).
* Read-only and deterministic: no file is written or re-saved; two runs on the same tree are
  byte-identical.

## Report JSON

A nlohmann object dumped with `dump(2)` (two-space indent, object keys alphabetically) plus exactly
one trailing `\n`. Top-level keys, always present:

```json
{
  "features": [ <one entry per feature-table row, in table order>, ... ],
  "goldens": [ <one entry per scanned project, sorted by name ascending>, ... ],
  "uncovered": [ <table keys with an empty covered_by, in table order>, ... ],
  "version": 1
}
```

Feature entry — exactly these three keys:

```json
    {
      "covered_by": [
        "clip_trim",
        "mixer"
      ],
      "key": "mute",
      "label": "track mute"
    }
```

Golden entry — exactly these two keys; `features` lists the table keys it exercises, in table order:

```json
    {
      "features": [
        "arrangement",
        "clip_offset_beats"
      ],
      "name": "clip_trim"
    }
```

`name` is the scanned file's stem (file name without the `.opj` suffix), so the report does not
depend on the current working directory; `covered_by` holds those same names, ascending.

## Feature table

Every predicate reads the **loaded** `op::Project` (loader normalizations apply: a `3/4`
time_signature becomes `beats_per_bar = 3` and leaves no `time_signature` value). Rows are ordered by
key, ASCII-ascending, and that order is what the report uses. `label` is the exact string shown.

| key | label | true when |
| --- | --- | --- |
| `arrangement` | explicit arrangement clips | `arrangement` is not empty |
| `attachments` | attachment references | `attachments` is not empty |
| `beats_per_bar_not_4` | beats_per_bar != 4 | `beats_per_bar != 4` |
| `choke_group` | sampler choke group | any track has `choke_group != 0` |
| `clip_fade_in` | clip fade-in | any clip has `fade_in_beats > 0` |
| `clip_fade_out` | clip fade-out | any clip has `fade_out_beats > 0` |
| `clip_gain_db` | clip gain dB | any clip has `gain_db != 0` |
| `clip_length_beats` | clip trim length | any clip has `length_beats` present |
| `clip_offset_beats` | clip trim offset | any clip has `offset_beats != 0` |
| `clip_repeats` | clip repeats > 1 | any clip has `repeats != 1` |
| `loop_region` | transport loop region | `loop` is present |
| `master_gain_db` | master fader | `master_gain_db != 0` |
| `metronome_accent` | metronome accent_every != 4 | any voice has `accent_every != 4` |
| `mute` | track mute | any track has `mute` true |
| `note_pitch` | note MIDI pitch | any note has `midi_note >= 0` |
| `note_velocity` | note velocity != 1.0 | any note has `velocity != 1.0` |
| `pan` | track pan != 0 | any track has `pan != 0` |
| `pattern_id` | named pattern | any pattern has a non-empty `id` |
| `pattern_length_beats` | explicit pattern length | any pattern has `length_beats` present |
| `sample_rate_not_48k` | sample_rate != 48000 | `sample_rate != 48000` |
| `sampler_repitch` | sampler root_note (repitch) | any voice has `root_note >= 0` |
| `sampler_slice` | sampler slice end_s | any voice has `has_end_s` true |
| `solo` | track solo | any track has `solo` true |
| `swing` | pattern swing != 0 | any pattern has `swing != 0` |
| `synth_v1` | synth voice v1 | any voice has `waveform != Legacy` or an ADSR or `cutoff_hz > 0` |
| `tempo_map` | tempo map | `tempo_map` is not empty |
| `time_signature` | time signature (den != 4) | `time_signature` is present |
| `track_gain_db` | track fader | any track has `gain_db != 0` |
| `voice_kind_metronome` | metronome voice | any voice kind is metronome |
| `voice_kind_sampler` | sampler voice | any voice kind is sampler |
| `voice_kind_sine` | sine voice | any voice kind is sine |
| `voice_kind_synth` | synth voice | any voice kind is synth |

Nothing else is a feature: sample-rate conversion of an asset, limiter/normalize render flags and
other render-only options are **not** model features and must not appear in the table.

## Where the committed goldens stand (informational)

`opd golden-coverage tests/fixtures/goldens` over the 16 committed goldens leaves exactly these six
features uncovered — that is the "add a golden here" list this command exists to produce:

```json
  "uncovered": [
    "attachments",
    "loop_region",
    "note_velocity",
    "pattern_id",
    "pattern_length_beats",
    "sample_rate_not_48k"
  ],
```
