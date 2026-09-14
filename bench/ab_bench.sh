#!/usr/bin/env bash
#
# ab_bench.sh - interleaved A/B timing comparison of two libretro cores.
#
#   ab_bench.sh <baseline-core> <patched-core> [rom_dir] [frames] [warmup] [reps] [-- runner-args...]
#
# Runs every ROM in rom_dir under both cores, alternating which core goes
# first on each repetition, and writes one row per run to a CSV under
# results/. The CSV carries a provenance header (both SHAs, platform,
# compiler, the core's build flags), because a results table nobody can
# trace back to a pair of builds is not evidence.
#
# Strictly serial: this reports wall-clock, so it must never be run
# alongside anything else competing for the CPU.
#
# Interleaving matters more than repetition count. Running all of core A's
# reps and then all of core B's lets any drift over the session (thermal,
# other tenants, frequency scaling) land entirely on one side and
# masquerade as a result. Alternating pairs it off instead.
#
# Anything after a literal `--` is passed through to bench_runner for both
# cores, e.g.:
#   ab_bench.sh baseline.so patched.so my_roms -- --core-option snes9x_threaded_render=disabled
#
# Env:
#   SNES9X_BENCH_RUNNER  path to bench_runner (default: alongside this script)
#   SNES9X_BASELINE_SHA  commit/tag the baseline core was built from
#   SNES9X_PATCHED_SHA   commit/tag the patched core was built from
#   SNES9X_AB_CSV        exact output path, instead of the generated name

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_DIR="$(cd "$SCRIPT_DIR/.." && pwd)"

# lib.sh is linted on its own; shellcheck only follows sources under -x.
# shellcheck source=bench/lib.sh disable=SC1091
. "$SCRIPT_DIR/lib.sh"

usage() {
    cat >&2 <<'EOF'
usage: ab_bench.sh <baseline-core> <patched-core> [rom_dir] [frames] [warmup] [reps] [-- runner-args...]

Defaults: rom_dir=bench/roms frames=3000 warmup=300 reps=6

Build the two cores first, into separate paths -- see the bench README.
bench_runner dlopens whatever path it is given, so the staged copies need
no platform-specific extension.
EOF
    exit 2
}

[ $# -ge 2 ] || usage

BASELINE="$1"
PATCHED="$2"
shift 2

ROM_DIR="$SCRIPT_DIR/roms"
FRAMES=3000
WARMUP=300
REPS=6

# Positional args stop at the first `--` or the first flag-shaped token.
while [ $# -gt 0 ]; do
    case "$1" in
        --) shift; break ;;
        -*) break ;;
        *)
            if [ -z "${_pos1_set:-}" ]; then ROM_DIR="$1"; _pos1_set=1
            elif [ -z "${_pos2_set:-}" ]; then FRAMES="$1"; _pos2_set=1
            elif [ -z "${_pos3_set:-}" ]; then WARMUP="$1"; _pos3_set=1
            elif [ -z "${_pos4_set:-}" ]; then REPS="$1"; _pos4_set=1
            else usage
            fi
            shift
            ;;
    esac
done
RUNNER_ARGS=("$@")

RUNNER="${SNES9X_BENCH_RUNNER:-$SCRIPT_DIR/$(s9x_bench_runner_name)}"
RESULTS_DIR="$SCRIPT_DIR/results"

for core in "$BASELINE" "$PATCHED"; do
    if [ ! -f "$core" ]; then
        echo "ab_bench.sh: core not found: $core" >&2
        exit 1
    fi
done

if [ ! -x "$RUNNER" ]; then
    echo "ab_bench.sh: $RUNNER not found or not executable; run 'make -C bench'" >&2
    exit 1
fi

if [ ! -d "$ROM_DIR" ] || [ -z "$(ls -A "$ROM_DIR" 2>/dev/null || true)" ]; then
    echo "ab_bench.sh: no ROMs found in $ROM_DIR" >&2
    exit 1
fi

for n in "$FRAMES" "$WARMUP" "$REPS"; do
    case "$n" in
        ''|*[!0-9]*) echo "ab_bench.sh: frames/warmup/reps must be integers" >&2; exit 1 ;;
    esac
done
if [ "$REPS" -lt 2 ]; then
    echo "ab_bench.sh: reps must be at least 2; one pair cannot estimate variance" >&2
    exit 1
fi

# --- provenance -------------------------------------------------------------

# <arch>-<os>, e.g. arm64-darwin or x86_64-linux. Ends up in the filename, so
# anything exotic in uname's output is flattened.
platform="$(printf '%s-%s' "$(uname -m)" "$(uname -s)" |
            tr '[:upper:]' '[:lower:]' | sed 's/[^a-z0-9_.-]/-/g')"

patched_sha="${SNES9X_PATCHED_SHA:-unknown}"
if [ "$patched_sha" = "unknown" ]; then
    echo "ab_bench.sh: warning: SNES9X_PATCHED_SHA is not set, recording 'unknown'." >&2
fi

baseline_sha="${SNES9X_BASELINE_SHA:-}"
if [ -z "$baseline_sha" ]; then
    baseline_sha="unknown"
    echo "ab_bench.sh: warning: SNES9X_BASELINE_SHA is not set, recording 'unknown'." >&2
    echo "  A results file whose baseline cannot be identified is not reproducible." >&2
fi

cc_version="$("${CXX:-c++}" --version 2>/dev/null | head -n1 || true)"
[ -n "$cc_version" ] || cc_version="unknown"

# The flags the core is actually built with, worth recording because a
# benchmark run against a debug build (-g added for profiling, say) is a
# silent wrong answer.
core_cflags="$(grep -m1 -E '^[[:space:]]*CXXFLAGS \+= -O3' "$REPO_DIR/libretro/Makefile" 2>/dev/null |
               sed 's/^[[:space:]]*CXXFLAGS[[:space:]]*+=[[:space:]]*//' || true)"
[ -n "$core_cflags" ] || core_cflags="unknown"

TIMESTAMP="$(date -u +%Y%m%dT%H%M%SZ)"
mkdir -p "$RESULTS_DIR"
OUT_CSV="${SNES9X_AB_CSV:-$RESULTS_DIR/ab_${platform}_${patched_sha}_${TIMESTAMP}.csv}"

{
    echo "# baseline_sha=$baseline_sha"
    echo "# patched_sha=$patched_sha"
    echo "# platform=$platform  cc=$cc_version"
    echo "# core_cflags=$core_cflags"
    echo "# frames=$FRAMES warmup=$WARMUP reps=$REPS"
    echo "# baseline_core=$BASELINE"
    echo "# patched_core=$PATCHED"
    echo "# runner_args=${RUNNER_ARGS[*]:-}"
    echo "rom,rep,core,fps"
} > "$OUT_CSV"

# --- run --------------------------------------------------------------------

# run_one <core> <rom> -> fps on stdout
run_one() {
    local core="$1" rom="$2"
    local out line fps

    # Capture the whole output rather than piping straight into the line
    # selector: a runner that failed and a runner that ran but printed no fps
    # are different faults deserving different messages, and a pipeline
    # collapses them into one exit status.
    if ! out="$("$RUNNER" "$(s9x_native_path "$core")" "$(s9x_native_path "$rom")" "$FRAMES" "$WARMUP" 0 "${RUNNER_ARGS[@]+"${RUNNER_ARGS[@]}"}")"; then
        echo "ab_bench.sh: runner failed for $core on $rom" >&2
        return 1
    fi
    if ! line="$(printf '%s\n' "$out" | s9x_fps_line)" || ! fps="$(s9x_parse_fps "$line")"; then
        {
            echo "ab_bench.sh: no fps in runner output for $core on $rom"
            echo "  last line: $(printf '%s\n' "$out" | tail -n1)"
        } >&2
        return 1
    fi
    printf '%s\n' "$fps"
}

echo "snes9x-Z interleaved A/B"
echo "baseline: $BASELINE ($baseline_sha)"
echo "patched:  $PATCHED ($patched_sha)"
echo "platform: $platform"
echo "frames: $FRAMES  warmup: $WARMUP  reps: $REPS"
echo

for rom in "$ROM_DIR"/*; do
    [ -f "$rom" ] || continue
    name="$(basename "$rom")"

    printf '%s\n' "$name"
    for ((i = 1; i <= REPS; i++)); do
        if [ $((i % 2)) -eq 1 ]; then
            b="$(run_one "$BASELINE" "$rom")"
            p="$(run_one "$PATCHED"  "$rom")"
        else
            p="$(run_one "$PATCHED"  "$rom")"
            b="$(run_one "$BASELINE" "$rom")"
        fi
        printf '%s,%s,baseline,%s\n' "$name" "$i" "$b" >> "$OUT_CSV"
        printf '%s,%s,patched,%s\n'  "$name" "$i" "$p" >> "$OUT_CSV"
        printf '  rep %s: baseline=%-12s patched=%-12s\n' "$i" "$b" "$p"
    done
done

echo
awk -f "$SCRIPT_DIR/ab_stats.awk" "$OUT_CSV"
echo
echo "csv=$OUT_CSV"
