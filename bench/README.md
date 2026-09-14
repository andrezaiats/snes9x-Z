# bench/: reproduce the numbers yourself

This directory has the tools to independently verify two things about this
fork: that it is bit-exact against upstream Snes9x, and that it is faster.
Both checks run against ROMs you supply yourself.

## What's here, what isn't

Ships: `bench_runner` (a minimal headless libretro frontend, no video/audio
output, no throttling), `ab_bench.sh` (interleaved A/B timing), `ab_stats.awk`
(the statistics `ab_bench.sh` reports), `lib.sh`/`core_path.sh` (shared
shell helpers).

Does not ship: any ROM, any ROM checksum or filename list, and any
previously recorded reference output. This fork holds no license on the
commercial games it was tuned against, so it says nothing further about
them here beyond what's needed to point these tools at your own dumps.
Nothing below requires more than that.

## Building

Build this fork's core first (see the top-level README's "Building"
section for every supported platform; the common case is):

```bash
make -C libretro clean && make -C libretro -j$(nproc)
```

Then build the bench tools:

```bash
make -C bench clean && make -C bench
```

Windows cross-build (requires `x86_64-w64-mingw32-gcc-posix`):

```bash
make -C bench clean && make -C bench CC=x86_64-w64-mingw32-gcc-posix
```

`./bench/core_path.sh` prints the built core's path without you having to
know whether this platform produces `.so`, `.dylib` or `.dll`; `./bench/core_path.sh --jobs`
prints a portable CPU count for `-j` (`nproc` doesn't exist on macOS).

## Reproducing the published A/B numbers

The published table compares this fork against unmodified upstream Snes9x
at a specific commit. Build that too, in a separate checkout:

```bash
git clone https://github.com/snes9xgit/snes9x.git /tmp/snes9x-upstream
cd /tmp/snes9x-upstream && git checkout 7a8878f1306f65594c30b7d86dee41d972c2e495
make -C libretro
```

That produces `/tmp/snes9x-upstream/libretro/snes9x_libretro.so` (note: no
`-Z` in the filename; upstream and this fork use different core names on
purpose, so you can never accidentally compare a core against itself).

Now run the interleaved A/B from this repository:

```bash
./bench/ab_bench.sh /tmp/snes9x-upstream/libretro/snes9x_libretro.so "$(./bench/core_path.sh)" my_roms/
```

`my_roms/` is a directory of your own ROM files; `ab_bench.sh` benchmarks
every file in it. Defaults are 3000 frames, 300 warmup frames and 6
repetitions per ROM, alternating which core runs first each repetition so
host drift over the session can't land on one side, the same defaults the
published table used. Override them positionally:
`ab_bench.sh <baseline> <patched> [rom_dir] [frames] [warmup] [reps]`.

The run prints one line per repetition, then a summary:

```
ROM   <name>          <delta%>  <t-statistic>  <positive-reps>/<total-reps>
GROUP all              <mean delta%>            <rom count>
```

The per-ROM delta is `(mean(patched) - mean(baseline)) / mean(baseline)`,
the same ratio-of-means estimator the published table uses, so a result
here is directly comparable in kind (not in exact value, see the caveats
below) to that table. `GROUP all` is the unweighted arithmetic mean across
your ROMs, not a geometric mean.

By default the run measures this fork's shipped default: the threaded
renderer, `snes9x_threaded_render`, on. To isolate how much of the gain
comes from that specifically, force it off and compare:

```bash
./bench/ab_bench.sh /tmp/snes9x-upstream/libretro/snes9x_libretro.so "$(./bench/core_path.sh)" \
    my_roms/ -- --core-option snes9x_threaded_render=disabled
```

Anything after a literal `--` is passed straight to `bench_runner` for both
cores; upstream simply ignores it, since it never had this option to begin
with.

## Building your own bit-exactness check

`bench_runner`'s `checkpoint_interval` argument hashes video and audio
output (FNV-1a) every N frames and prints one `checkpoint frame=... video_hash=... audio_hash=...`
line per checkpoint. Two runs of the same ROM, same frame/warmup/checkpoint
counts, that produce identical checkpoint lines produced identical emulated
output. Run the same ROM through both cores and diff:

```bash
./bench/bench_runner /tmp/snes9x-upstream/libretro/snes9x_libretro.so my_roms/game.sfc 4800 0 60 \
    | grep '^checkpoint' > upstream.out
./bench/bench_runner "$(./bench/core_path.sh)" my_roms/game.sfc 4800 0 60 \
    | grep '^checkpoint' > ours.out
diff upstream.out ours.out
```

An empty diff means bit-exact output over the whole run, video and audio
both, not a "feels the same" comparison. This works with the default
(threaded) core option already, no `--core-option` needed: threaded
rendering is a different code path internally, but it's built and gated to
produce the same output as sync, and this diff is exactly how you'd catch
it if that were ever untrue on your hardware. To also check the sync path
directly, add `--core-option snes9x_threaded_render=disabled` to the second
command.

`--input <file>` plays back scripted controller input instead of leaving
the game idle; the file format (one `<frame> [BUTTON[,BUTTON...]]` directive
per line) is documented in `bench_runner`'s own `--help`-style usage
comment at the top of `bench_runner.c`. `--state-roundtrip <N>` serializes
and immediately deserializes the core's state every N frames, so a
save-state bug shows up as a checkpoint mismatch too.

## What this doesn't include, and why that's still enough

Two things stay private, deliberately: the ROM corpus the published table
measured, and the recorded reference hashes (`golden_check.sh`'s
`.golden` files) that gate every change to this fork before it ships.
Neither is a license problem for us to keep, but redistributing either
one would be. What's here doesn't need them: the diff above works from a
byte-identical build of upstream at a public, citable commit, so the
correctness claim is independently checkable end to end, and the A/B works
from any ROM you already own.

## What to expect, honestly

You will not reproduce the published numbers ROM for ROM, and shouldn't
expect to. A few things that move the result and aren't secrets, just not
controllable from here:

- **Your ROM dump matters.** A different revision or region of the same
  game can exercise different code paths. The published table names exact
  games, not exact dump checksums, on purpose (see above).
- **Idle versus played matters.** Only a few titles in the published corpus
  were driven through scripted gameplay; most ran idle at whatever screen
  loading leaves them on. An idle run and a played run of the same game are
  different workloads for the PPU and APU, and will show a different gain.
  Use `--input` if you want played gameplay in your own numbers.
- **Free CPU cores matter, a lot.** The threaded renderer's whole gain
  comes from having a spare core to hand work to. The core option's own
  description already says it "may cost more than it buys on a single-core
  system"; on a busy or small machine, the gap between threaded and sync
  can be much smaller than published, or in the worst case slightly
  negative. This is exactly why `--core-option snes9x_threaded_render=disabled`
  is documented above, so you can measure both and see the gap on your own
  hardware rather than take ours on faith.
- **The published figures cite specific hardware.** See the top-level
  README's benchmark section for what that was. Different CPU generations,
  thermal limits and background load all move raw fps; the relative
  comparison (threaded vs sync, patched vs upstream) is the portable part.
