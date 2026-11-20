# ROADMAP — DhunZilla

Source plan: `../ai-daw-plan.md` (workspace root). This file is the in-repo index; work packages live in `TASKS/`.

## Phases
| Phase | Theme | Status |
|---|---|---|
| 0 | Harness-first: repo, CI matrix, deterministic render core, golden harness, sidecar skeleton + adapters | **IN PROGRESS** |
| 1 | Skeleton DAW (transport, channel rack, playlist, mixer, sampler) | pending |
| 2 | Real DAW (piano roll, recording, automation, CLAP/VST3 hosting) | pending |
| 3 | AI hook (native stem-splitter, audio→MIDI, sing→instrument, sample search) | pending |
| 4 | Generation layer (adapter SDK live, ACE-Step/YuE2, beatbox→drums, mix assistant) | pending |
| 5 | Copilot & polish (scripting API + NL editing, realtime AI FX, 1.0) | pending |

## Work packages — Phase 0
| WP | Title | Status | Owner |
|---|---|---|---|
| WP-0172 | Sidecar job event log — every state/progress change a numbered, resumable event (`GET /jobs/{id}/events?after=`) | **done** | qwen-goldfish |
| WP-0171 | `opd peaks` — min/max waveform peaks per bin as JSON (new engine module `peaks`) | **done** | qwen-goldfish |
| WP-0170 | `opd lint` — read-only project sanity checks ("why is my beat silent?") as JSON (new engine module `lint`) | **done** | qwen-goldfish |
| WP-0169 | `opd normalize` — gain a WAV to a target integrated LUFS under a sample-peak ceiling (new engine module `normalize`) | **done** | qwen-goldfish |
| WP-0168 | `opd loudness` — true peak (dBTP), momentary/short-term max LUFS, loudness range (new engine module `loudness`) | **done** | qwen-goldfish |
| WP-0167 | Sampler repitch — per-voice `root_note`, notes play at 2^((P-root)/12), `set_sampler_root`; golden `sampler_repitch` | **done** | qwen-goldfish |
| WP-0166 | Pattern swing — per-pattern groove (render-time delay of odd 16ths), `set_pattern_swing`, `opd diff` support | **done** | qwen-goldfish |
| WP-0165 | `opd new` + Document-free `op::new_project` factory — canonical new project, `--track` kinds, `--force` | **done** | qwen-goldfish |
| WP-0164 | CI `app` job — builds the JUCE app (`dhunzilla`, `DZ_BUILD_APP=ON`) on both arches, runs headless `--snapshot` under Xvfb, asserts exit 0 + 1100×700 PNG; JUCE download/build cache keyed to 9.0.5 | **done** | qwen-goldfish |
| WP-0163 | `opd samples` — deterministic JSON index of a sample directory (backend for the sample browser) | **done** | qwen-goldfish |
| WP-0162 | Pattern `length_beats` — explicit loop period (renderer, MIDI export, `set_pattern_length`/`clear_pattern_length`, `opd steps` default width, diff) | **done** | qwen-goldfish |
| WP-0161 | Synth voice v1 — waveform sine/saw/square, ADSR with release tail, low-pass filter, `set_synth_params`; golden `synth_v1` | **done** | qwen-goldfish |
| WP-0160 | `op_tests` per-process temp directory (`tmp_dir()`/`tmp_file()`) instead of fixed /tmp paths — concurrent runs no longer collide | **done** | qwen-goldfish |
| WP-0153 | App playback — Play/Stop, background offline render, lock-free buffer swap, bar.beat position, `--render-playback`/`--position-at`; JUCE-free `dzplayer` core | **done** | qwen-goldfish |
| WP-0152 | App track editing — gain/pan/mute/solo through `op::Document`, Undo/Redo/Save, headless scripted-input mode (`--script`) | **done** | qwen-goldfish |
| WP-0159 | `opd diff` supports clip `pattern`/`repeats` via `set_clip_pattern`/`set_clip_repeats` (step 1b before pattern removal; positional clip compare) | **done** | haiku+orchestrator |
| WP-0158 | AIFF / AIFF-C (`NONE`, `sowt`) reader — signature dispatch, exact 80-bit rate decode, sampler + `opd info`; renders byte-identical to the same samples as WAV | **done** | haiku+orchestrator |
| WP-0157 | Document autosave + crash recovery — atomic temp+rename, revision-gated, content-compared recovery state, `open_recovered` | **done** | haiku+orchestrator |
| WP-0156 | Sampler trim (`start_s`/`end_s` slice after resampling) + `set_sampler_trim`; golden `sampler_trim` | **done** | haiku+orchestrator |
| WP-0155 | Transport loop region — model field, `set_loop`/`clear_loop`, `opd info`, `opd render --loop`, diff support | **done** | haiku+orchestrator |
| WP-0154 | Step sequencer ops — `set_step`/`clear_steps` over a 1/4-beat grid, `op::step_grid`, `opd steps` | **done** | haiku+orchestrator |
| WP-0151 | Sidecar job cancellation — `cancelling` state, cooperative cancel in mock backends, cancelled jobs leave no artifacts, `GET /jobs` listing, job_client support | **done** | subagent+orchestrator |
| WP-0150 | Resampler polyphase tap table — bit-identical to the direct method (1,014-case matrix), ≥8× faster, bounded table with direct fallback | **done** | subagent+orchestrator |
| WP-0149 | `opd render --format pcm16|pcm24|f32` (mix + stems) — pcm16 default byte-identical, exact pcm24 rounding rule, IEEE-float WAV; reader accepts 24-bit | **done** | subagent+orchestrator |
| WP-0148 | `opd click` — tempo-map-aware metronome track (accented downbeats), onsets from the renderer's own beat→sample mapping | **done** | subagent+orchestrator |
| WP-0147 | Clip ops `set_clip_repeats` / `set_clip_pattern` / `duplicate_clip` / `reorder_clip` — validated via the model parse, 32-bit no-wrap, History undo byte-exact | **done** | haiku+orchestrator |
| WP-0146 | `opd diff a b -o edits.opjl` — journal of existing ops that turns a into b (apply ∘ diff byte-exact; 360 random pairs), fixed op order, unsupported fields named | **done** | haiku+orchestrator |
| WP-0145 | `opd render --from-bar/--to-bar` — bit-exact slice of the full render via the tempo map, stems too, exact usage errors | **done** | subagent+orchestrator |
| WP-0144 | `opd analyze <wav>` — peak / RMS / BS.1770-4 integrated LUFS / spectral centroid as canonical JSON (`docs/spec/analyze.md`) | **done** | subagent+orchestrator |
| WP-0143 | JUCE app window (`DZ_BUILD_APP`, target `dhunzilla`) — transport bar + track list from `Document`, headless `--snapshot` PNG 1100×700 with `--apply` journals (WP-0137b) | **done** | subagent+orchestrator |
| WP-0142 | `op::project_bytes` — one canonical serializer shared by `save_project` and `Document` | **done** | subagent+orchestrator |
| WP-0138 | Sampler resamples any WAV rate at load — deterministic windowed-sinc (Blackman, 16 zero-crossings, double accumulation), 8k–192k in-rate policy with exit-5 error, native-rate path bit-identical, golden `resampled.opj` + 44.1 kHz kit generator | **done** | subagent+orchestrator |
| WP-0141 | GUI-free Document layer (`src/doc/`, new `dzdoc` target) — open/save with byte-exact dirty flag, undo/redo over `op::History`, slider gestures with exactly one undo entry each, revision counter, derived track rows; unit-tested | **done** | subagent |
| WP-0140 | Glue scripts resolve relative project/audio paths against the caller's cwd (midi + stem fixed; prompt already correct) + relative-path tests; stray `ai-sidecar/ai/transcribed.mid` explained (pre-WP-0133 `cd`) | **done** | orchestrator |
| WP-0139 | CI hardening — `-Werror` on our targets (`DZ_WERROR`, ON by default) + ASAN/UBSAN build (`DZ_SANITIZE`, fake-stack off so the suite runs in ~6 s) + `sanitizers` CI job (unit tests, CLI contracts, goldens) | **done** | subagent+orchestrator |
| WP-0136 | Mixer: track `mute`/`solo` + project `master_gain_db` — audibility rule (mute wins), inaudible tracks skipped in render, all-zero stems for inaudible, master applied to mix + every stem, gain/pan range validation closed, ops `set_track_mute`/`set_track_solo`/`set_master_gain`, `opd info` markers, golden `mixer.opj` | **done** | subagent+orchestrator |
| WP-0135 | Model operations + undo/redo history + `opd apply` journal CLI — atomic ops validated via save→parse path, JSON-Lines all-or-nothing journals, snapshot History (cap 256) | **done** | subagent+orchestrator |
| WP-0001 | Repo scaffold | **done** | orchestrator |
| WP-0002 | Deterministic render core v0 | **done** | orchestrator |
| WP-0003 | Golden-file harness | **done** | orchestrator |
| WP-0004 | Sidecar v0 (FastAPI + adapters + conformance) | **done** (59 tests) | subagent+orchestrator |
| WP-0005 | Model manager (manifest, sha256, license gates) | **done** | subagent+orchestrator |
| WP-0006 | Demucs CPU cartridge (opt-in) + e2e separation through /jobs | **done** (aarch64 CPU) | orchestrator |
| WP-0007 | CI workflow: Linux matrix, x86_64 + aarch64 (ADR-0006) | **done** — green on push (run 44277877478) | orchestrator |
| WP-0008 | Basic-Pitch transcription adapter — ONNX route (ADR-0005), cartridge-gated; conformance + eval corpus green | **done** | orchestrator |
| WP-0134 | Three glue scripts now run in CI (mock backends) via test_glue_scripts.sh — artifacts + exit-code gates (0/3/2) + byte-identical opj, both arches | **done** | subagent+orchestrator |
| WP-0133 | Glue dedup — shared harness/job_client.py (lifecycle+polling), --help on all three, byte-identical results proved | **done** | orchestrator |
| WP-0131 | Sidecar API robustness suite — malformed bodies, bad paths, lifecycle edges: zero product gaps, contract documented | **done** | orchestrator |
| WP-0130 | MIDI export flattens arrangement (clips/repeats/offsets; gain baked to velocity) — legacy exports byte-identical | **done** | orchestrator |
| WP-0128 | .opd structural fuzz — fixes heap OOB read (crafted central-dir name length) in unpack+list; duplicate entries rejected | **done** | orchestrator |
| WP-0127 | transcription eval corpus (register/transient/legato stress) + shared scorer — real ONNX mean event F1 ≈ 0.99, all gates pass | **done** | orchestrator |
| WP-0126 | Sidecar wav reader robustness — mirror of C++ fuzz classes; fixes ZeroDivisionError on bits=0, mono/stereo+rate domains | **done** | orchestrator |
| WP-0125 | WAV reader fuzz+hardening — strict fmt validation (float bits==32, sr range); crafted-invalid now rejected | **done** | orchestrator |
| WP-0124 | prompt→editable-song e2e chain (generate→transcribe→opj+provenance+render), deterministic, mock CI + real ONNX proven | **done** | orchestrator |
| WP-0123 | Remove dead _WIN32 blocks in engine validators (ADR-0006 Linux-only hygiene) | **done** | orchestrator |
| WP-0122 | JSON model fuzz (truncation+mutation+type-confusion) — fixes 22 escaping type_error paths in load_project | **done** | orchestrator |
| WP-0121 | README CLI reference table + harness workflow index; usage refresh | **done** | orchestrator |
| WP-0120 | Clip-level gain (dB) in arrangement — mix-target scaling, unity bit-exact, [-96,24] validated | **done** | orchestrator |
| WP-0119 | hum→project glue + mocktrans CI backend — transcribe job → import-midi → attach; conformance dispatch gap fixed | **done** | orchestrator |
| WP-0118 | stem→project glue: sidecar separation + auto `opd attach` provenance, .opd carries AI stems (mock CI + real demucs proven) | **done** | orchestrator |
| WP-0117 | `opd info *.mid` — track/note/tempo-map summary via existing loader (zero new engine code) | **done** | orchestrator |
| WP-0116 | Attachments — stems/transcriptions as first-class project refs w/ provenance; carried by .opd | **done** | orchestrator |
| WP-0115 | Tempo map ⇄ MIDI — FF51-per-point export, multi-tempo import into tempo_map (legacy single-tempo byte-stable) | **done** | orchestrator |
| WP-0114 | `opd import-midi` — merge a MIDI file into an existing project as new tracks (grid semantics, untouched-on-failure) | **done** | orchestrator |
| WP-0113 | Ear-check pack generator (deterministic audio for human review + AI round-trip 02-vs-06 comparison) | **done** | orchestrator |
| WP-0112 | `opd info *.opd` + engine list_bundle (metadata-only archive view) | **done** | orchestrator |
| WP-0111 | Note velocity (dynamics) — mix-target scaling, MIDI import/export capture, legacy bit-exact | **done** | orchestrator |
| WP-0110 | .opd bundle (store-only ZIP, deterministic, CRC+traversal-guarded) — shareable song files | **done** | orchestrator |
| WP-0109 | MIDI mutation fuzzer (seeded, invariant-checked) — caught+fixed illegal-data-byte acceptance | **done** | orchestrator |
| WP-0108 | Event-level round-trip metric (onset ±80ms, greedy match; both F1 gates live) | **done** | orchestrator |
| WP-0107 | Stem separation over live HTTP (demucs cartridge via uvicorn, non-silence verified) — gated demo | **done** | orchestrator |
| WP-0106 | Live sidecar HTTP smoke harness (health/jobs/artifacts/provenance/error-path over real uvicorn) | **done** | orchestrator |
| WP-0105 | Tempo map (bar-anchored piecewise tempo) — legacy path bit-exact, golden w/ mid-song drop | **done** | orchestrator |
| WP-0104 | `opd opj2mid` SMF writer — byte-deterministic export, exact round-trip with loader (DAW interop) | **done** | orchestrator |
| WP-0103a | Synth voice (MIDI-pitched, 12-TET) + note pitch field — closes hum→MIDI→sound engine path | **done** | orchestrator |
| WP-0103b | `opd midi2opj` SMF reader → project; MIDI→edit→render golden chain | **done** | orchestrator |
| WP-0101 | Arrangement layer: clips {pattern, start_bar, repeats}, legacy-compatible, bit-exact | **done** | orchestrator |
| WP-0102 | Basic-Pitch transcription cartridge via ONNX runtime (ADR-0005; real MIDI e2e on aarch64) | **done** | orchestrator |
| WP-0010 | Native stem export (`--stems-dir`) — solo stems recombine bit-exact to mix | **done** | orchestrator |

## Open human decisions (blocking nothing; flagged for review)
1. **Name — DECIDED 2026-11-18: "DhunZilla"** (धुन = tune/melody; "the melody monster"). Full `dhunzilla` namespace free (`.com/.io/.app/.dev/.in`, GitHub, npm, PyPI); no US mark found; formal US/EU/IN clearance before filing. Screen + caveats: [report](docs/legal/trademark-screen-2026-11-18.md). Rename tracked as `TASKS/WP-0129-rename-dhunzilla.md` (done; GitHub repo renamed). **Domain registration deliberately deferred by owner on 2026-11-18 until a feature-complete DAW exists** — risk accepted; revisit at first public beta.
2. Signing certs budget (~$100–500/yr): Apple dev account + Azure Trusted Signing/EV.
3. **ADRs approved 2026-11-18** by product owner: ADR-0001 (GPLv3 core), ADR-0002 (headless-core-first, JUCE deferred), ADR-0003 (HTTP sidecar v0), ADR-0005 (ONNX transcription), ADR-0006 (Linux-only target). Distribution deferred by product owner — not blocked, just not scheduled.
4. **ADR-0007 approved 2026-11-18** by product owner: JUCE 9 for the GUI shell + live audio in a new `src/app/` module (engine/CLI stay JUCE-free); PipeWire-via-ALSA default at 128 samples/48 kHz; licensing consequence accepted (shipped GUI app effectively AGPLv3 via JUCE 9; core remains GPLv3).
5. **Platform scope** — Linux x86_64 + aarch64 only for now (ADR-0006). Revisit at first GUI/public beta; macOS/Windows are deferred, not dropped.

## Nightly (CI live on push)
Linux CI (x86_64 + aarch64) green on `main`; reference-rig (DGX Spark aarch64) CPU-inference evals; determinism sweep across the two Linux arches. macOS/Windows CI is deferred (ADR-0006) and returns only when a GUI exists to port.
