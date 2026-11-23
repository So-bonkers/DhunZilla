#!/usr/bin/env bash
# make_phase1_song.sh — build / check / ear-check the Phase 1 exit-criterion song "Exit Beat" (WP-0197).
#
# The song is tests/fixtures/songs/phase1_beat.opjl: an op journal applied to the canonical
# `opd new` project. Nothing is hand-written except that journal. The 34 MB golden mix .wav is NOT
# committed (owner decision 2026-11-23); its determinism contract lives in
# tests/fixtures/songs/phase1_beat.wav.sha256 and is enforced by --check, so this song stays out of
# tests/fixtures/goldens/ and run_goldens.sh never renders it.
#
# usage: harness/make_phase1_song.sh [--build | --check | --earcheck] [path/to/opd]
#   --build     (default) regenerate kits/kick.wav, phase1_beat.opj and phase1_beat.wav.sha256
#   --check     PASS/FAIL per assertion, final "make_phase1_song: N failure(s)", exit = N
#   --earcheck  write the owner's pack (07_exit_beat_mix.wav, phase1_exit_stems/, 08_..._solo.wav)
#
# Rules honoured: bash + opd + coreutils only (no numpy), mktemp -d scratch with a cleanup trap,
# no wall-clock anywhere, golden render fixed by --seed 1337 (docs/spec/limiter.md, ADR-0004).
set -u

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
SONGS="$ROOT/tests/fixtures/songs"
JOURNAL="$SONGS/phase1_beat.opjl"
OPJ="$SONGS/phase1_beat.opj"
SHAFILE="$SONGS/phase1_beat.wav.sha256"
WAV="$SONGS/phase1_beat.wav"                      # 34 MB golden; may legitimately be absent
KIT_SRC="$ROOT/tests/fixtures/goldens/kits/kick.wav"   # read-only source (never modified)
KIT_DST="$SONGS/kits/kick.wav"

usage() { echo "usage: harness/make_phase1_song.sh [--build | --check | --earcheck] [path/to/opd]" >&2; }

MODE="--build"
OPD="$ROOT/build/opd"
positional=0
for a in "$@"; do
    case "$a" in
        --build|--check|--earcheck) MODE="$a" ;;
        -*) usage; exit 2 ;;
        *) if [ "$positional" = 1 ]; then usage; exit 2; fi; positional=1; OPD="$a" ;;
    esac
done
if [ -f "$OPD" ] || [ -L "$OPD" ]; then OPD="$(cd "$(dirname "$OPD")" && pwd)/$(basename "$OPD")"; fi

if [ ! -x "$OPD" ]; then
    echo "error: opd not found or not executable: $OPD (build it first)" >&2
    exit 2
fi
if [ ! -f "$JOURNAL" ]; then echo "error: missing journal $JOURNAL" >&2; exit 1; fi
if [ ! -f "$KIT_SRC" ]; then echo "error: missing kit source $KIT_SRC" >&2; exit 1; fi

T="$(mktemp -d)"; trap 'rm -rf "$T"' EXIT

GOLDEN_FLAGS=( --seed 1337 --limit-dbfs -1.0 )   # fixed golden render flags (docs/spec/render-harness.md)

# --- the exact `opd new` + journal application from the work package -----------------------------
new_project() {   # $1 = output .opj (absolute); canonical command, every edit comes from the journal
    "$OPD" new "$1" --bpm 96 --bars 72 \
        --track Kick:sampler:kits/kick.wav --track Snare:sampler:kits/kick.wav \
        --track Hat:sampler:kits/kick.wav --track Bass:synth --track Perc:sampler:kits/kick.wav
}
apply_journal() { # $1 = project .opj, $2 = journal; run from tests/fixtures/songs/ like the spec
    ( cd "$SONGS" && "$OPD" apply "$1" "${2:-$JOURNAL}" )
}
render_golden() { # $1 = project (.opj), $2 = out .wav, rest = extra render args; cwd = tests/fixtures/songs/
    ( cd "$SONGS" && "$OPD" render "$1" -o "$2" "${GOLDEN_FLAGS[@]}" "${@:3}" )
}

# --- measurement helpers (coreutils + awk only) ---------------------------------------------------
sha() { sha256sum "$1" | awk '{print $1}'; }
analyze_field() { # $1 = wav, $2 = top-level JSON key of opd analyze output
    "$OPD" analyze "$1" --json 2>/dev/null | grep -m1 "\"$2\":" | sed -E "s/.*\"$2\": *//; s/, *$//"
}
pcm_data_bytes() { local f; f="$(analyze_field "$1" frames)"; [ -n "$f" ] && echo $(( f * 4 )); }  # stereo pcm16
wav_peak_int16() { # $1 = canonical pcm16 wav; $2 = skip bytes into data; $3 = bytes to read (0 = rest)
    local off=$(( 44 + ${2:-0} )) n="${3:-0}"
    if [ "$n" = 0 ]; then
        od -An -td2 -j "$off" "$1"
    else
        od -An -td2 -j "$off" -N "$n" "$1"
    fi | awk '{for(i=1;i<=NF;i++){v=$i; if(v<0)v=-v; if(v>m)m=v}} END{print m+0}'
}
stem_peak() { wav_peak_int16 "$T/stems/$1.wav" 0 0; }

# scratch copy of the song (opj + kits beside it) so sample paths resolve the same way:
song_copy() { # $1 = scratch subdir, $2 = .opj to copy in
    mkdir -p "$1/kits" && cp "$2" "$1/phase1_beat.opj" && cp "$KIT_DST" "$1/kits/kick.wav"
}

# =============================================================================================================
if [ "$MODE" = "--build" ]; then
    # 1) kit copy so phase1_beat.opj renders in place from tests/fixtures/songs/
    mkdir -p "$SONGS/kits" || { echo "error: cannot create $SONGS/kits" >&2; exit 1; }
    cp "$KIT_SRC" "$KIT_DST" || { echo "error: cannot refresh $KIT_DST" >&2; exit 1; }
    echo "built kit copy: tests/fixtures/songs/kits/kick.wav (from tests/fixtures/goldens/kits/kick.wav)"

    # 2) project: exact `opd new` + journal, staged in scratch then committed into the fixture dir
    new_project "$T/phase1_beat.opj" >/dev/null || { echo "error: opd new failed" >&2; exit 1; }
    apply_msg="$(apply_journal "$T/phase1_beat.opj")" || { echo "error: opd apply failed: $apply_msg" >&2; exit 1; }
    echo "$apply_msg"
    cp "$T/phase1_beat.opj" "$OPJ" || { echo "error: cannot write $OPJ" >&2; exit 1; }
    echo "built project: tests/fixtures/songs/phase1_beat.opj"

    # 3) golden render into scratch (NEVER into tests/fixtures/), then record only its digest
    render_golden phase1_beat.opj "$T/phase1_beat.wav" >/dev/null \
        || { echo "error: golden render failed" >&2; exit 1; }
    frames="$(analyze_field "$T/phase1_beat.wav" frames)"
    peak="$(analyze_field "$T/phase1_beat.wav" peak_dbfs)"
    lufs="$(analyze_field "$T/phase1_beat.wav" integrated_lufs)"
    sha "$T/phase1_beat.wav" > "$SHAFILE" || { echo "error: cannot write $SHAFILE" >&2; exit 1; }
    echo "rendered golden mix in scratch: ${frames} frames, peak ${peak} dBFS, ${lufs} LUFS (not committed by design)"
    echo "wrote digest: tests/fixtures/songs/phase1_beat.wav.sha256 = $(cat "$SHAFILE")"
    exit 0
fi

if [ "$MODE" = "--earcheck" ]; then
    OUT="$ROOT/demos/earcheck"
    mkdir -p "$OUT/phase1_exit_stems" || { echo "error: cannot create $OUT" >&2; exit 1; }

    # mix, byte-identical to the golden by construction (same project, same seed, same flags)
    render_golden phase1_beat.opj "$OUT/07_exit_beat_mix.wav" >/dev/null \
        || { echo "error: ear-check mix render failed" >&2; exit 1; }
    if [ -f "$WAV" ] && ! cmp -s "$OUT/07_exit_beat_mix.wav" "$WAV"; then
        echo "error: 07_exit_beat_mix.wav is not byte-identical to $WAV" >&2; exit 1
    fi
    if [ ! -f "$WAV" ] && [ -f "$SHAFILE" ] && [ "$(sha "$OUT/07_exit_beat_mix.wav")" != "$(tr -d '[:space:]' < "$SHAFILE")" ]; then
        echo "error: 07_exit_beat_mix.wav digest does not match $SHAFILE" >&2; exit 1
    fi

    # the five stems
    render_golden phase1_beat.opj "$T/earcheck_stems_mix.wav" --stems-dir "$OUT/phase1_exit_stems" >/dev/null \
        || { echo "error: stem render failed" >&2; exit 1; }

    # the solo variant: same flags, track 3 (Bass) soloed on a scratch copy
    song_copy "$T/solo" "$OPJ" || { echo "error: cannot stage solo copy" >&2; exit 1; }
    printf '{"op":"set_track_solo","index":3,"solo":true}\n' > "$T/solo.opjl"
    "$OPD" apply "$T/solo/phase1_beat.opj" "$T/solo.opjl" >/dev/null \
        || { echo "error: solo apply failed" >&2; exit 1; }
    "$OPD" render "$T/solo/phase1_beat.opj" -o "$OUT/08_exit_beat_bass_solo.wav" "${GOLDEN_FLAGS[@]}" >/dev/null \
        || { echo "error: solo render failed" >&2; exit 1; }

    echo "exit-song ear-check pack written:"
    echo "  $OUT/07_exit_beat_mix.wav          (byte-identical to the golden render)"
    echo "  $OUT/phase1_exit_stems/            (0_kick.wav 1_snare.wav 2_hat.wav 3_bass.wav 4_perc.wav)"
    echo "  $OUT/08_exit_beat_bass_solo.wav    (track 3 soloed, same flags)"
    exit 0
fi

# --- --check =====================================================================================
fails=0
pass() { echo "PASS $1"; }
fail() { echo "FAIL $1${2:+: $2}"; fails=$((fails+1)); }

# (1) the kit copy is current
if cmp -s "$KIT_DST" "$KIT_SRC"; then pass "kit_copy_matches_goldens"; else fail "kit_copy_matches_goldens" "$KIT_DST missing or stale"; fi

# (2) the journal applies cleanly to the exact `opd new` project and reproduces the committed .opj
if [ ! -f "$OPJ" ]; then
    fail "committed_opj_present" "missing $OPJ (run: bash harness/make_phase1_song.sh --build)"
else
    if new_project "$T/rebuild.opj" >/dev/null; then pass "opd_new_canonical_project"; else fail "opd_new_canonical_project" "opd new failed"; fi
    apply_msg="$(apply_journal "$T/rebuild.opj")"; rc=$?
    if [ $rc -eq 0 ] && printf '%s\n' "$apply_msg" | grep -q '^apply: 114 op(s)'; then
        pass "journal_applies_cleanly_114_ops"
    else
        fail "journal_applies_cleanly_114_ops" "[$apply_msg]"
    fi
    if cmp -s "$T/rebuild.opj" "$OPJ"; then
        pass "opj_matches_journal_byte_for_byte"
    else
        fail "opj_matches_journal_byte_for_byte" "fresh opd new + journal differs from the committed .opj"
    fi

    # (3) golden render: twice, bit-identical
    if render_golden phase1_beat.opj "$T/mix1.wav" >/dev/null 2>&1 && \
       render_golden phase1_beat.opj "$T/mix2.wav" >/dev/null 2>&1; then
        if cmp -s "$T/mix1.wav" "$T/mix2.wav"; then pass "golden_render_bit_exact_twice"; else fail "golden_render_bit_exact_twice" "run-to-run diff"; fi
        frames="$(analyze_field "$T/mix1.wav" frames)"
        if [ "$frames" = "8640000" ]; then pass "mix_frames_8640000"; else fail "mix_frames_8640000" "got '$frames'"; fi

        # (4) the committed .wav equals a fresh render; if it is absent (text-only patch series) the
        #     fresh render IS the regeneration from the journal — verify it against the committed digest.
        if [ -f "$SHAFILE" ] && [ "$(tr -d '[:space:]' < "$SHAFILE")" = "$(sha "$T/mix1.wav")" ]; then
            pass "golden_render_matches_committed_sha256"
        else
            fail "golden_render_matches_committed_sha256" "digest mismatch (run --build)"
        fi
        if [ -f "$WAV" ]; then
            if cmp -s "$WAV" "$T/mix1.wav"; then pass "committed_wav_equals_fresh_render"; else fail "committed_wav_equals_fresh_render"; fi
        else
            pass "wav_absent_regenerated_from_journal"
        fi

        # (5) stems: exact names, muted Perc all-zero, every other stem non-silent
        if render_golden phase1_beat.opj "$T/stems_mix.wav" --stems-dir "$T/stems" >/dev/null 2>&1; then
            got="$(cd "$T/stems" && ls | sort)"
            want="$(printf '%s\n' 0_kick.wav 1_snare.wav 2_hat.wav 3_bass.wav 4_perc.wav | sort)"
            if [ "$got" = "$want" ]; then pass "stems_exactly_five_expected_names"; else fail "stems_exactly_five_expected_names" "[$(echo "$got" | tr '\n' ' ')]"; fi
            p="$(stem_peak 4_perc)"; if [ "$p" = "0" ]; then pass "stem_perc_silent_track_muted"; else fail "stem_perc_silent_track_muted" "peak $p"; fi
            for s in 0_kick 1_snare 2_hat 3_bass; do
                p="$(stem_peak "$s")"
                if [ "$p" -gt 2000 ]; then pass "stem_${s}_audible (peak $p > 2000)"; else fail "stem_${s}_audible" "peak $p not > 2000"; fi
            done

            # (6) the limiter: bounded mix peak, un-limited render hotter, bytes different
            lp="$(analyze_field "$T/mix1.wav" peak_dbfs)"
            if awk -v p="$lp" 'BEGIN{exit !(p >= -3.0 && p <= -0.9)}'; then
                pass "limited_mix_peak_in_range (peak_dbfs $lp)"
            else
                fail "limited_mix_peak_in_range" "peak_dbfs $lp not in [-3.0, -0.9]"
            fi
            if ( cd "$SONGS" && "$OPD" render phase1_beat.opj -o "$T/unlimited.wav" --seed 1337 ) >/dev/null 2>&1; then
                up="$(analyze_field "$T/unlimited.wav" peak_dbfs)"
                if awk -v p="$up" 'BEGIN{exit !(p >= -0.3)}'; then
                    pass "unlimited_render_peaks_at_or_above_-0.3 (peak_dbfs $up)"
                else
                    fail "unlimited_render_peaks_at_or_above_-0.3" "peak_dbfs $up"
                fi
                if cmp -s "$T/mix1.wav" "$T/unlimited.wav"; then
                    fail "limiter_changes_render_bytes" "limited and un-limited renders are identical"
                else
                    pass "limiter_changes_render_bytes"
                fi
            else
                fail "unlimited_render_peaks_at_or_above_-0.3" "un-limited render failed"
                fail "limiter_changes_render_bytes" "un-limited render failed"
            fi

            # (7) soloing track 3 (Bass) leaves only the Bass stem non-silent
            song_copy "$T/solo" "$OPJ"
            printf '{"op":"set_track_solo","index":3,"solo":true}\n' > "$T/solo.opjl"
            if "$OPD" apply "$T/solo/phase1_beat.opj" "$T/solo.opjl" >/dev/null 2>&1 && \
               "$OPD" render "$T/solo/phase1_beat.opj" -o "$T/solo_mix.wav" "${GOLDEN_FLAGS[@]}" --stems-dir "$T/solo_stems" >/dev/null 2>&1; then
                solo_ok=1
                p="$(wav_peak_int16 "$T/solo_stems/3_bass.wav" 0 0)"; [ "$p" -gt 2000 ] || { solo_ok=0; echo "  bass stem peak $p"; }
                for s in 0_kick 1_snare 2_hat 4_perc; do
                    p="$(wav_peak_int16 "$T/solo_stems/$s.wav" 0 0)"; [ "$p" = "0" ] || { solo_ok=0; echo "  $s stem peak $p"; }
                done
                if [ "$solo_ok" = 1 ]; then pass "solo_track3_leaves_only_bass_audible"; else fail "solo_track3_leaves_only_bass_audible"; fi
            else
                fail "solo_track3_leaves_only_bass_audible" "solo render failed"
            fi

            # (8) fades + trim are audible: a twin journal that nulls every set_clip_fades side and
            #     clears the trim with nulls must change the render measurably.
            sed -E \
              -e 's/(\{"op":"set_clip_fades","index":[0-9]+,)"fade_in_beats":[0-9.]+,"fade_out_beats":[0-9.]+\}/\1"fade_in_beats":null,"fade_out_beats":null}/' \
              -e 's/(\{"op":"set_clip_trim","index":[0-9]+,)"offset_beats":[0-9.]+,"length_beats":[0-9.]+\}/\1"offset_beats":null,"length_beats":null}/' \
              "$JOURNAL" > "$T/twin.opjl"
            # (set_pattern_length also carries "length_beats" — the guard is scoped to the trim op lines)
            if grep '"op":"set_clip_trim"' "$T/twin.opjl" | grep -qE '"(offset_beats|length_beats)": *[^n]' || \
               grep '"op":"set_clip_fades"' "$T/twin.opjl" | grep -qE '"(fade_in_beats|fade_out_beats)": *[^n]'; then
                fail "twin_journal_zeroes_fades_and_trim" "a fade/trim value survived"
            else
                pass "twin_journal_zeroes_fades_and_trim"
            fi
            mkdir -p "$T/twinproj/kits"
            new_project "$T/twinproj/phase1_beat.opj" >/dev/null 2>&1
            cp "$KIT_DST" "$T/twinproj/kits/kick.wav"
            if "$OPD" apply "$T/twinproj/phase1_beat.opj" "$T/twin.opjl" >/dev/null 2>&1 && \
               "$OPD" render "$T/twinproj/phase1_beat.opj" -o "$T/twin.wav" "${GOLDEN_FLAGS[@]}" >/dev/null 2>&1; then
                if cmp -s "$T/mix1.wav" "$T/twin.wav"; then
                    fail "twin_render_differs" "twin render is byte-identical to the golden"
                else
                    pass "twin_render_differs"
                fi
                head_mix="$(wav_peak_int16 "$T/mix1.wav" 0 19200)"    # first 100 ms (4800 frames x 4 B)
                head_twin="$(wav_peak_int16 "$T/twin.wav" 0 19200)"
                if [ "$head_twin" -gt 0 ] && [ $(( head_mix * 8 )) -le "$head_twin" ]; then
                    pass "intro_fade_in_audible (mix head $head_mix vs twin $head_twin, ratio >= 8x)"
                else
                    fail "intro_fade_in_audible" "mix head peak $head_mix vs twin $head_twin, ratio < 8x"
                fi
                data="$(pcm_data_bytes "$T/mix1.wav")"; tail_off=$(( data - 192000 ))   # last 1 s
                tail_mix="$(wav_peak_int16 "$T/mix1.wav" "$tail_off" 192000)"
                tail_twin="$(wav_peak_int16 "$T/twin.wav" "$tail_off" 192000)"
                if [ "$tail_mix" -lt 20 ]; then
                    pass "outro_last_second_near_silent_in_mix (peak $tail_mix < 20)"
                else
                    fail "outro_last_second_near_silent_in_mix" "peak $tail_mix not < 20"
                fi
                if [ "$tail_twin" -gt 2000 ]; then
                    pass "outro_last_second_audible_in_untrimmed_twin (peak $tail_twin > 2000)"
                else
                    fail "outro_last_second_audible_in_untrimmed_twin" "peak $tail_twin not > 2000"
                fi
            else
                fail "twin_render_differs" "twin build/render failed"
                fail "intro_fade_in_audible" "twin build/render failed"
                fail "outro_last_second_near_silent_in_mix" "twin build/render failed"
                fail "outro_last_second_audible_in_untrimmed_twin" "twin build/render failed"
            fi
        else
            fail "stems_exactly_five_expected_names" "stem render failed"
        fi
    else
        fail "golden_render_bit_exact_twice" "golden render failed"
    fi
fi

echo "make_phase1_song: $fails failure(s)"
exit "$fails"
