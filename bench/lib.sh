# shellcheck shell=bash
#
# lib.sh - platform facts shared by the bench scripts. This file is sourced,
# never executed, so it deliberately has no shebang and is not +x.
#
# The platform-dependent helpers accept the `uname -s` string as an optional
# trailing argument, defaulting to the real one, so the .so/.dll branches can
# be exercised in a test regardless of the host actually running it.

# True for a plain positive integer. Used to validate every CPU-count probe,
# since some of them report success while printing something unusable.
s9x__is_count() {
    case "$1" in
        ''|*[!0-9]*) return 1 ;;
    esac
    [ "$1" -gt 0 ]
}

# Shared-library extension of the libretro core, mirroring the per-platform
# TARGET in libretro/Makefile (.so for unix, .dylib for osx, .dll for
# windows). That Makefile is the source of truth; this is the single place
# these scripts restate it, instead of hardcoding .so at each call site.
s9x_lib_ext() {
    case "${1:-$(uname -s)}" in
        Darwin)               echo dylib ;;
        CYGWIN*|MINGW*|MSYS*) echo dll ;;
        *)                    echo so ;;
    esac
}

# Basename of the core as the libretro Makefile writes it.
s9x_core_name() {
    echo "snes9x-Z_libretro.$(s9x_lib_ext "$@")"
}

# Translate a path to the form a native (non-Cygwin/non-MSYS) Windows .exe
# can open, for any path about to be handed to $RUNNER as an argv element.
#
# bench_runner.exe is built with mingw-w64, i.e. it links the plain Win32
# CRT, not Cygwin's or MSYS's POSIX layer. Cygwin's bash resolves paths via
# `pwd`, which prints a POSIX-style absolute path (/cygdrive/c/... or
# /home/...). Handing that to a plain Win32 fopen() does not resolve --
# `cygpath -w` converts it to the native form the CRT understands.
#
# A no-op wherever `cygpath` isn't on PATH (Linux, macOS, or a native
# Windows shell that never had a POSIX path to begin with), so this is safe
# to apply unconditionally rather than gating it on uname -s.
s9x_native_path() {
    if command -v cygpath >/dev/null 2>&1; then
        cygpath -w "$1"
    else
        printf '%s\n' "$1"
    fi
}

# Basename of the bench_runner binary as bench-pub/Makefile writes it: bare
# on unix/macOS, .exe on Windows (mirrors bench-pub/Makefile's own $(EXE)
# logic, which keys off $(CC) rather than uname -s since it runs at
# cross-compile time on a non-Windows host).
s9x_bench_runner_name() {
    case "${1:-$(uname -s)}" in
        CYGWIN*|MINGW*|MSYS*) echo bench_runner.exe ;;
        *)                    echo bench_runner ;;
    esac
}

# Print the path of the core to run, or explain why there isn't one.
#
#   s9x_resolve_core <repo_dir> <prog> [uname_s]
#
# repo_dir is a parameter rather than a global so a test can point this at a
# throwaway tree. SNES9X_CORE wins when set and non-empty; an empty
# SNES9X_CORE falls through to auto-detection rather than being treated as a
# path.
#
# When the native core is missing but a foreign one is present, this reports
# it and still fails rather than loading it: a wrong-architecture library
# would otherwise resurface much later as an opaque dlopen error.
s9x_resolve_core() {
    local repo_dir="$1"
    local prog="$2"
    shift 2

    local ext native other foreign

    if [ -n "${SNES9X_CORE:-}" ]; then
        if [ ! -f "$SNES9X_CORE" ]; then
            echo "$prog: SNES9X_CORE=$SNES9X_CORE not found" >&2
            return 1
        fi
        printf '%s\n' "$SNES9X_CORE"
        return 0
    fi

    ext="$(s9x_lib_ext "$@")"
    native="$repo_dir/libretro/snes9x-Z_libretro.$ext"

    if [ -f "$native" ]; then
        printf '%s\n' "$native"
        return 0
    fi

    # An unmatched glob expands to itself, so the -f test is what filters it.
    foreign=""
    for other in "$repo_dir"/libretro/snes9x-Z_libretro.*; do
        [ -f "$other" ] || continue
        foreign="${foreign}  ${other}"$'\n'
    done

    {
        echo "$prog: no core for this platform at"
        echo "  $native"
        if [ -n "$foreign" ]; then
            echo
            echo "Found a non-native build instead:"
            printf '%s' "$foreign"
            echo
            echo "This platform (${1:-$(uname -s)}) builds .$ext."
        fi
        echo "Run 'make -C libretro', or point SNES9X_CORE at a core explicitly."
    } >&2

    return 1
}

# Number of usable CPUs. `nproc` is GNU coreutils and does not exist on
# macOS. getconf is implemented by both glibc and macOS so it goes first,
# but it can print "undefined" while still exiting 0, hence the guard on
# every probe.
s9x_ncpu() {
    local n

    n="$(getconf _NPROCESSORS_ONLN 2>/dev/null)" || n=""
    if ! s9x__is_count "$n"; then
        n="$(nproc 2>/dev/null)" || n=""
    fi
    if ! s9x__is_count "$n"; then
        n="$(sysctl -n hw.ncpu 2>/dev/null)" || n=""
    fi
    if ! s9x__is_count "$n"; then
        n=4
    fi

    echo "$n"
}

# Pull the frames-per-second figure out of a bench_runner summary line.
#
#   s9x_parse_fps <line>
#
# The leading space in ' fps=' is load-bearing: the same line also carries
# native_fps=60.0988, and a bare fps= pattern matches inside that too.
#
# Fails rather than printing an empty string when the line has no fps at
# all, so a crashed or mis-invoked runner surfaces at the call site instead
# of becoming a blank CSV field.
s9x_parse_fps() {
    local fps
    fps="$(printf '%s\n' "$1" | grep -oE ' fps=[0-9.]+' | cut -d= -f2)"
    if [ -z "$fps" ]; then
        return 1
    fi
    printf '%s\n' "$fps"
}

# Pick the runner's machine-readable summary line out of its whole stdout,
# from a file argument or from stdin.
#
#   s9x_fps_line <file>   |   <cmd> | s9x_fps_line
#
# Deliberately not `tail -n1`: select by content, not position, so the
# number of neighbouring lines a given core prints never matters. The
# leading space in ' fps=' is load-bearing here for the same reason as in
# s9x_parse_fps. Fails rather than printing nothing when there is no
# summary, so a crashed run stops its caller instead of contributing a
# blank field.
s9x_fps_line() {
    local matches
    if [ $# -ge 1 ]; then
        matches="$(grep ' fps=' "$1")" || return 1
    else
        matches="$(grep ' fps=')" || return 1
    fi
    [ -n "$matches" ] || return 1
    printf '%s\n' "$matches" | tail -n1
}
