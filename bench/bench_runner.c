/*
 * bench_runner: minimal headless libretro frontend for benchmarking and
 * verifying a Snes9x-Z core. No video/audio output, no real-time throttling.
 *
 * Usage: bench_runner <core.so> <rom> <frames> [warmup_frames] [checkpoint_interval] [flags...]
 *
 * checkpoint_interval (default 0 = off): when > 0, hashes every video frame
 * and every audio sample batch (FNV-1a 64-bit, running/cumulative) and
 * prints a "checkpoint" line every N frames. Two runs of the same ROM under
 * the same core, frames, warmup and checkpoint interval must print identical
 * checkpoint lines; a mismatch means the two runs produced different
 * emulated output.
 *
 * Optional flags (any position after the positional args):
 *   --input <file>          Scripted controller input. One directive per
 *                            line: "<frame> [BUTTON[,BUTTON...]]". Blank
 *                            lines and lines starting with # are ignored.
 *                            A button list replaces (not adds to) whatever
 *                            was previously held. Frame numbers are a single
 *                            monotonic counter starting at 0 on the very
 *                            first retro_run() call, including warmup
 *                            frames. Example:
 *                              0
 *                              90 START
 *                              150
 *                              200 START
 *                              260 RIGHT
 *                              600 RIGHT,A
 *                            Without this flag, no button is ever held.
 *   --state-roundtrip <N>    Every N frames, retro_serialize() then
 *                            retro_unserialize() from the same buffer, so a
 *                            bit-exact round trip leaves checkpoint hashes
 *                            unchanged and a broken one does not.
 *   --core-option <key>=<value>
 *                            Answer the core's RETRO_ENVIRONMENT_GET_VARIABLE
 *                            query for <key> with <value>, overriding this
 *                            runner's own built-in defaults. Repeatable.
 *                            Useful for exercising a specific libretro core
 *                            option without a full frontend, e.g.:
 *                              --core-option snes9x_threaded_render=disabled
 */

#ifdef _WIN32
#include "dlfcn_win32.h"
#else
#include <dlfcn.h>
#endif
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <stdbool.h>
#include <time.h>
#include <stdarg.h>

#include "../libretro/libretro.h"

static void *core_handle;

static void (*fn_retro_set_environment)(retro_environment_t);
static void (*fn_retro_set_video_refresh)(retro_video_refresh_t);
static void (*fn_retro_set_audio_sample)(retro_audio_sample_t);
static void (*fn_retro_set_audio_sample_batch)(retro_audio_sample_batch_t);
static void (*fn_retro_set_input_poll)(retro_input_poll_t);
static void (*fn_retro_set_input_state)(retro_input_state_t);
static void (*fn_retro_init)(void);
static void (*fn_retro_deinit)(void);
static void (*fn_retro_get_system_av_info)(struct retro_system_av_info *);
static bool (*fn_retro_load_game)(const struct retro_game_info *);
static void (*fn_retro_unload_game)(void);
static void (*fn_retro_run)(void);
static size_t (*fn_retro_serialize_size)(void);
static bool (*fn_retro_serialize)(void *, size_t);
static bool (*fn_retro_unserialize)(const void *, size_t);

/* --state-roundtrip <N>: every N frames, serialize the core and immediately
 * unserialize from that buffer. A bit-exact round trip leaves the checkpoint
 * hashes unchanged, so this doubles as a save-state test. */
static long state_roundtrip = 0;
static void *state_buf = NULL;
static size_t state_size = 0;

static uint64_t frame_count;
static uint64_t global_frame = 0; /* counts from the very first retro_run(), never reset */

/* Checkpoint hashing state. Only touched when checkpointing is enabled. */
static int      checkpoint_enabled = 0;
static uint64_t video_hash = 0xcbf29ce484222325ULL; /* FNV-1a 64-bit offset basis */
static uint64_t audio_hash = 0xcbf29ce484222325ULL;
static int      pixel_bytes = 2; /* default RETRO_PIXEL_FORMAT_0RGB1555 */

/* Scripted input: each entry means "from this global frame onward, hold
 * exactly this button mask" (bit N = RETRO_DEVICE_ID_JOYPAD_N). */
struct input_event { uint64_t frame; uint16_t mask; };
static struct input_event *input_script = NULL;
static int input_script_count = 0;
static int input_script_next = 0;
static uint16_t current_input_mask = 0;

static const char *button_names[12] = {
	"B", "Y", "SELECT", "START", "UP", "DOWN", "LEFT", "RIGHT", "A", "X", "L", "R"
};

/* --core-option overrides, applied before this runner's own defaults. */
#define MAX_CORE_OPTIONS 32
struct core_option_override { char key[64]; char value[64]; };
static struct core_option_override core_option_overrides[MAX_CORE_OPTIONS];
static int core_option_override_count = 0;

/* Defaults for core options the snes9x-Z libretro core reads via
 * RETRO_ENVIRONMENT_GET_VARIABLE. Most options fail safe if this just
 * returns false (the core falls back to its own hardcoded default), but a
 * couple are only ever initialized inside the "if (environ_cb(GET_VARIABLE
 * ...))" branch with no fallback assignment outside it. "snes9x_threaded_render"
 * is set to match the core's own shipped default ("enabled" in
 * libretro_core_options.h), so this runner measures what a real user runs;
 * pass --core-option snes9x_threaded_render=disabled to measure sync mode
 * instead. */
static const struct { const char *key; const char *value; } option_defaults[] = {
	{ "snes9x_overclock_superfx", "100%" },
	{ "snes9x_overclock_cycles",  "disabled" },
	{ "snes9x_threaded_render",   "enabled" },
	{ NULL, NULL },
};

static inline void fnv1a_update(uint64_t *hash, const void *data, size_t len)
{
	const uint8_t *p = (const uint8_t *) data;
	uint64_t h = *hash;
	for (size_t i = 0; i < len; i++)
	{
		h ^= p[i];
		h *= 0x100000001b3ULL; /* FNV-1a 64-bit prime */
	}
	*hash = h;
}

static int button_id_from_name(const char *name)
{
	for (int i = 0; i < 12; i++)
		if (strcmp(name, button_names[i]) == 0)
			return i;
	return -1;
}

static void load_input_script(const char *path)
{
	FILE *f = fopen(path, "r");
	if (!f)
	{
		fprintf(stderr, "bench_runner: cannot open input script %s\n", path);
		exit(1);
	}

	int capacity = 64;
	input_script = malloc(sizeof(*input_script) * capacity);
	input_script_count = 0;

	char line[512];
	int lineno = 0;
	while (fgets(line, sizeof(line), f))
	{
		lineno++;
		char *p = line;
		while (*p == ' ' || *p == '\t') p++;
		if (*p == '#' || *p == '\n' || *p == '\0')
			continue;

		char frame_tok[64] = {0}, rest[448] = {0};
		int n = sscanf(p, "%63s %447[^\n]", frame_tok, rest);
		if (n < 1)
			continue;

		uint64_t frame = strtoull(frame_tok, NULL, 10);
		uint16_t mask = 0;
		if (n == 2)
		{
			char *tok = strtok(rest, ",");
			while (tok)
			{
				while (*tok == ' ' || *tok == '\t') tok++;
				int id = button_id_from_name(tok);
				if (id < 0)
					fprintf(stderr, "bench_runner: %s:%d: unknown button '%s', ignoring\n", path, lineno, tok);
				else
					mask |= (uint16_t) (1u << id);
				tok = strtok(NULL, ",");
			}
		}

		if (input_script_count == capacity)
		{
			capacity *= 2;
			input_script = realloc(input_script, sizeof(*input_script) * capacity);
		}
		input_script[input_script_count].frame = frame;
		input_script[input_script_count].mask = mask;
		input_script_count++;
	}
	fclose(f);
}

static void add_core_option_override(const char *arg)
{
	const char *eq = strchr(arg, '=');
	if (!eq || eq == arg)
	{
		fprintf(stderr, "bench_runner: --core-option expects key=value, got '%s'\n", arg);
		exit(1);
	}
	if (core_option_override_count == MAX_CORE_OPTIONS)
	{
		fprintf(stderr, "bench_runner: too many --core-option overrides (max %d)\n", MAX_CORE_OPTIONS);
		exit(1);
	}

	size_t keylen = (size_t) (eq - arg);
	size_t vallen = strlen(eq + 1);
	if (keylen == 0 || keylen >= sizeof(core_option_overrides[0].key) ||
		vallen >= sizeof(core_option_overrides[0].value))
	{
		fprintf(stderr, "bench_runner: --core-option key or value too long in '%s'\n", arg);
		exit(1);
	}

	struct core_option_override *slot = &core_option_overrides[core_option_override_count++];
	memcpy(slot->key, arg, keylen);
	slot->key[keylen] = '\0';
	memcpy(slot->value, eq + 1, vallen + 1);
}

static void video_refresh_cb(const void *data, unsigned width, unsigned height, size_t pitch)
{
	frame_count++;

	if (!checkpoint_enabled || !data)
		return;

	/* Hash only the valid width*height region, not the pitch padding
	 * (padding bytes are not guaranteed deterministic). */
	const uint8_t *rows = (const uint8_t *) data;
	size_t row_bytes = (size_t) width * (size_t) pixel_bytes;
	for (unsigned y = 0; y < height; y++)
		fnv1a_update(&video_hash, rows + (size_t) y * pitch, row_bytes);
}

static void audio_sample_cb(int16_t left, int16_t right)
{
	if (!checkpoint_enabled)
		return;
	int16_t frame[2] = { left, right };
	fnv1a_update(&audio_hash, frame, sizeof(frame));
}

static size_t audio_sample_batch_cb(const int16_t *data, size_t frames)
{
	if (checkpoint_enabled && data && frames > 0)
		fnv1a_update(&audio_hash, data, frames * 2 * sizeof(int16_t));
	return frames;
}

static void input_poll_cb(void)
{
	/* global_frame is set by the driver loop in main() before calling
	 * retro_run(), and stays constant for the duration of this call. */
	while (input_script_next < input_script_count &&
		input_script[input_script_next].frame <= global_frame)
	{
		current_input_mask = input_script[input_script_next].mask;
		input_script_next++;
	}
}

static int16_t input_state_cb(unsigned port, unsigned device, unsigned index, unsigned id)
{
	(void) index;
	if (port != 0 || device != RETRO_DEVICE_JOYPAD || id >= 12)
		return 0;
	return (current_input_mask & (uint16_t) (1u << id)) ? 1 : 0;
}

static void log_printf_cb(enum retro_log_level level, const char *fmt, ...)
{
	static const char *level_names[] = { "DEBUG", "INFO", "WARN", "ERROR" };
	const char *lvl = level <= RETRO_LOG_ERROR ? level_names[level] : "?";
	fprintf(stderr, "core[%s]: ", lvl);
	va_list ap;
	va_start(ap, fmt);
	vfprintf(stderr, fmt, ap);
	va_end(ap);
}

static bool environment_cb(unsigned cmd, void *data)
{
	switch (cmd)
	{
		case RETRO_ENVIRONMENT_SET_PIXEL_FORMAT:
		{
			enum retro_pixel_format fmt = *(enum retro_pixel_format *) data;
			switch (fmt)
			{
				case RETRO_PIXEL_FORMAT_XRGB8888: pixel_bytes = 4; break;
				case RETRO_PIXEL_FORMAT_RGB565:   pixel_bytes = 2; break;
				case RETRO_PIXEL_FORMAT_0RGB1555:
				default:                          pixel_bytes = 2; break;
			}
			return true;
		}

		case RETRO_ENVIRONMENT_GET_LOG_INTERFACE:
		{
			struct retro_log_callback *cb = (struct retro_log_callback *) data;
			cb->log = log_printf_cb;
			return true;
		}

		case RETRO_ENVIRONMENT_GET_VARIABLE:
		{
			struct retro_variable *var = (struct retro_variable *) data;
			var->value = NULL;
			for (int i = 0; i < core_option_override_count; i++)
			{
				if (strcmp(var->key, core_option_overrides[i].key) == 0)
				{
					var->value = core_option_overrides[i].value;
					return true;
				}
			}
			for (int i = 0; option_defaults[i].key; i++)
			{
				if (strcmp(var->key, option_defaults[i].key) == 0)
				{
					var->value = option_defaults[i].value;
					return true;
				}
			}
			return false;
		}

		case RETRO_ENVIRONMENT_GET_SYSTEM_DIRECTORY:
		case RETRO_ENVIRONMENT_GET_SAVE_DIRECTORY:
		default:
			return false;
	}
}

static void *load_symbol(const char *name)
{
	void *sym = dlsym(core_handle, name);
	if (!sym)
	{
		fprintf(stderr, "bench_runner: missing symbol %s: %s\n", name, dlerror());
		exit(1);
	}
	return sym;
}

static double now_seconds(void)
{
	struct timespec ts;
	clock_gettime(CLOCK_MONOTONIC, &ts);
	return (double) ts.tv_sec + (double) ts.tv_nsec / 1e9;
}

int main(int argc, char **argv)
{
	if (argc < 4)
	{
		fprintf(stderr, "usage: %s <core.so> <rom> <frames> [warmup_frames] [checkpoint_interval] [flags...]\n", argv[0]);
		return 1;
	}

	const char *core_path = argv[1];
	const char *rom_path  = argv[2];
	long frames  = strtol(argv[3], NULL, 10);
	long warmup  = argc > 4 ? strtol(argv[4], NULL, 10) : 0;
	long checkpoint_interval = argc > 5 ? strtol(argv[5], NULL, 10) : 0;
	checkpoint_enabled = checkpoint_interval > 0;

	for (int i = 6; i < argc; i++)
	{
		if (strcmp(argv[i], "--input") == 0 && i + 1 < argc)
			load_input_script(argv[++i]);
		else if (strcmp(argv[i], "--state-roundtrip") == 0 && i + 1 < argc)
			state_roundtrip = strtol(argv[++i], NULL, 10);
		else if (strcmp(argv[i], "--core-option") == 0 && i + 1 < argc)
			add_core_option_override(argv[++i]);
		else
			fprintf(stderr, "bench_runner: unknown flag '%s', ignoring\n", argv[i]);
	}

	core_handle = dlopen(core_path, RTLD_NOW | RTLD_LOCAL);
	if (!core_handle)
	{
		fprintf(stderr, "bench_runner: dlopen failed: %s\n", dlerror());
		return 1;
	}

	fn_retro_set_environment        = load_symbol("retro_set_environment");
	fn_retro_set_video_refresh      = load_symbol("retro_set_video_refresh");
	fn_retro_set_audio_sample       = load_symbol("retro_set_audio_sample");
	fn_retro_set_audio_sample_batch = load_symbol("retro_set_audio_sample_batch");
	fn_retro_set_input_poll         = load_symbol("retro_set_input_poll");
	fn_retro_set_input_state        = load_symbol("retro_set_input_state");
	fn_retro_init                   = load_symbol("retro_init");
	fn_retro_deinit                 = load_symbol("retro_deinit");
	fn_retro_get_system_av_info     = load_symbol("retro_get_system_av_info");
	fn_retro_load_game              = load_symbol("retro_load_game");
	fn_retro_unload_game            = load_symbol("retro_unload_game");
	fn_retro_run                    = load_symbol("retro_run");
	fn_retro_serialize_size         = load_symbol("retro_serialize_size");
	fn_retro_serialize              = load_symbol("retro_serialize");
	fn_retro_unserialize            = load_symbol("retro_unserialize");

	fn_retro_set_environment(environment_cb);
	fn_retro_set_video_refresh(video_refresh_cb);
	fn_retro_set_audio_sample(audio_sample_cb);
	fn_retro_set_audio_sample_batch(audio_sample_batch_cb);
	fn_retro_set_input_poll(input_poll_cb);
	fn_retro_set_input_state(input_state_cb);

	fn_retro_init();

	FILE *f = fopen(rom_path, "rb");
	if (!f)
	{
		fprintf(stderr, "bench_runner: cannot open ROM %s\n", rom_path);
		return 1;
	}
	fseek(f, 0, SEEK_END);
	long rom_size = ftell(f);
	fseek(f, 0, SEEK_SET);
	void *rom_data = malloc(rom_size);
	if (fread(rom_data, 1, rom_size, f) != (size_t) rom_size)
	{
		fprintf(stderr, "bench_runner: short read on ROM %s\n", rom_path);
		return 1;
	}
	fclose(f);

	struct retro_game_info game_info = {
		.path = rom_path,
		.data = rom_data,
		.size = (size_t) rom_size,
		.meta = NULL,
	};

	if (!fn_retro_load_game(&game_info))
	{
		fprintf(stderr, "bench_runner: retro_load_game failed for %s\n", rom_path);
		return 1;
	}

	struct retro_system_av_info av_info;
	fn_retro_get_system_av_info(&av_info);
	double native_fps = av_info.timing.fps;

	for (long i = 0; i < warmup; i++)
	{
		fn_retro_run();
		global_frame++;
	}

	frame_count = 0;
	video_hash = 0xcbf29ce484222325ULL;
	audio_hash = 0xcbf29ce484222325ULL;

	double t0 = now_seconds();
	for (long i = 0; i < frames; i++)
	{
		fn_retro_run();
		global_frame++;

		if (state_roundtrip > 0 && global_frame % state_roundtrip == 0)
		{
			size_t need = fn_retro_serialize_size();
			if (need > state_size)
			{
				state_buf = realloc(state_buf, need);
				state_size = need;
			}
			if (!fn_retro_serialize(state_buf, need) ||
				!fn_retro_unserialize(state_buf, need))
			{
				fprintf(stderr, "bench_runner: state round trip failed at frame %llu\n",
					(unsigned long long) global_frame);
				return 1;
			}
		}

		if (checkpoint_enabled && checkpoint_interval > 0 &&
			(i + 1) % checkpoint_interval == 0)
		{
			printf("checkpoint frame=%ld video_hash=%016llx audio_hash=%016llx\n",
				i + 1, (unsigned long long) video_hash, (unsigned long long) audio_hash);
		}
	}
	double t1 = now_seconds();

	if (checkpoint_enabled)
	{
		printf("checkpoint frame=%ld video_hash=%016llx audio_hash=%016llx final=1\n",
			frames, (unsigned long long) video_hash, (unsigned long long) audio_hash);
	}

	double elapsed = t1 - t0;
	double achieved_fps = (double) frame_count / elapsed;
	double speed_pct = native_fps > 0.0 ? (achieved_fps / native_fps) * 100.0 : 0.0;

	/* Machine-parseable summary on stdout. The leading space before "fps="
	 * matters: it is what keeps a naive `grep fps=` from also matching
	 * "native_fps=" on the same line. */
	printf("rom=%s frames=%llu elapsed_s=%.6f fps=%.3f native_fps=%.4f speed_pct=%.2f\n",
		rom_path, (unsigned long long) frame_count, elapsed, achieved_fps, native_fps, speed_pct);

	fn_retro_unload_game();
	fn_retro_deinit();
	free(rom_data);
	dlclose(core_handle);

	return 0;
}
