/* Copyright (C) 2019 Nunuhara Cabbage <nunuhara@haniwa.technology>
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, see <http://gnu.org/licenses/>.
 */

#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <ctype.h>
#include <dirent.h>
#include <ctype.h>
#include <getopt.h>
#include <time.h>
#include <math.h>
#include <limits.h>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#define NOGDI
#include <windows.h>
#include <shellapi.h>
#endif

#include "system4.h"
#include "system4/ain.h"
#include "system4/file.h"
#include "system4/ini.h"
#include "system4/instructions.h"
#include "system4/little_endian.h"
#include "system4/string.h"
#include "system4/utfsjis.h"

#include "xsystem4.h"
#include "asset_manager.h"
#include "debugger.h"
#include "gfx/gfx.h"
#include "gfx/font.h"
#include "vm.h"

#include "version.h"

void set_msgskip_delay(struct ain *ain, unsigned ms);
void apply_game_specific_hacks(struct ain *ain);

struct config config = {
	.game_name = NULL,
	.boot_name = NULL,
	.ain_filename = NULL,
	.vm_name = NULL,
	.game_dir = NULL,
	.save_dir = NULL,
	.home_dir = NULL,
	.view_width = 800,
	.view_height = 600,
	.mixer_nr_channels = 0,
	.mixer_channels = NULL,
	.default_volume = 100,
	.joypad = false,
	.echo = false,
	.text_x_scale = 1.0,
	.manual_text_x_scale = false,
	.save_format = SAVE_FORMAT_RSM,
	.msgskip_delay = 0,

	.bgi_path = NULL,
	.wai_path = NULL,
	.ex_path = NULL,
	.fnl_path = NULL,
	.font_paths = { NULL, NULL },
};

static bool is_ascii_text(const char *text)
{
	for (const unsigned char *p = (const unsigned char *)text; *p; p++) {
		if (*p & 0x80)
			return false;
	}
	return true;
}

#ifdef _WIN32
static wchar_t *decode_multibyte_string(const char *text, UINT codepage)
{
	DWORD flags = codepage == CP_UTF8 ? MB_ERR_INVALID_CHARS : 0;
	int nr_wchars = MultiByteToWideChar(codepage, flags, text, -1, NULL, 0);
	if (nr_wchars <= 0)
		return NULL;

	wchar_t *wide = xmalloc(nr_wchars * sizeof(wchar_t));
	if (!MultiByteToWideChar(codepage, flags, text, -1, wide, nr_wchars)) {
		free(wide);
		return NULL;
	}
	return wide;
}

static char *encode_wide_string(const wchar_t *text, UINT codepage)
{
	BOOL used_default_char = FALSE;
	int nr_chars = WideCharToMultiByte(codepage, WC_NO_BEST_FIT_CHARS, text, -1,
			NULL, 0, NULL, &used_default_char);
	if (nr_chars <= 0)
		return NULL;

	char *encoded = xmalloc(nr_chars);
	if (!WideCharToMultiByte(codepage, WC_NO_BEST_FIT_CHARS, text, -1,
			encoded, nr_chars, NULL, &used_default_char)) {
		free(encoded);
		return NULL;
	}
	return encoded;
}

static char *decode_multibyte_to_utf8(const char *text, UINT codepage)
{
	wchar_t *wide = decode_multibyte_string(text, codepage);
	if (!wide)
		return NULL;
	char *utf8 = wchar_to_utf8(wide);
	free(wide);
	return utf8;
}

/* Detected game text codepage: 932 (SJIS, default) or 20932 (EUC-JP). */
static UINT game_str_codepage = 932;

/* Conversion function passed to ain_open_conv(): converts game-native encoding -> UTF-8. */
static char *game_text_to_utf8(const char *str)
{
	if (!str)
		return NULL;
	if (is_ascii_text(str))
		return strdup(str);
	return decode_multibyte_to_utf8(str, game_str_codepage);
}

static int score_decoded_string(const wchar_t *text)
{
	int score = 0;

	for (; *text; text++) {
		wchar_t c = *text;
		if (c < 0x20 && c != '\t' && c != '\r' && c != '\n') {
			score -= 20;
		} else if (c >= 0xE000 && c <= 0xF8FF) {
			score -= 20;
		} else if ((c >= 0x3040 && c <= 0x30FF)
				|| (c >= 0x3400 && c <= 0x4DBF)
				|| (c >= 0x4E00 && c <= 0x9FFF)
				|| (c >= 0xFF10 && c <= 0xFF19)
				|| (c >= 0xFF21 && c <= 0xFF3A)
				|| (c >= 0xFF41 && c <= 0xFF5A)) {
			score += 4;
		} else if ((c >= 0x20 && c <= 0x7E) || c == 0x3000) {
			score += 2;
		} else if (c == 0x30FB || c == 0xFF65) {
			score -= 2;
		} else if (c >= 0xFF61 && c <= 0xFF9F) {
			score -= 1;
		}
	}

	return score;
}

static char *normalize_ini_game_text(const char *text)
{
	if (!text)
		return NULL;
	if (is_ascii_text(text))
		return strdup(text);

	/* Try UTF-8, then system ANSI (GBK on Chinese Windows, CP_ACP),
	 * then EUC-JP (cp20932), then cp51932, then SJIS/cp932. */
	static const UINT codepages[] = { CP_UTF8, CP_ACP, 20932, 51932, 932 };
	wchar_t *best = NULL;
	int best_score = INT_MIN;

	for (size_t i = 0; i < sizeof(codepages) / sizeof(codepages[0]); i++) {
		wchar_t *candidate = decode_multibyte_string(text, codepages[i]);
		if (!candidate)
			continue;

		int score = score_decoded_string(candidate);
		if (!best || score > best_score) {
			free(best);
			best = candidate;
			best_score = score;
		} else {
			free(candidate);
		}
	}

	if (!best)
		return strdup(text);

	char *utf8 = wchar_to_utf8(best);
	free(best);
	if (!utf8)
		return strdup(text);
	return utf8;
}

static char *utf8_to_game_text(const char *text)
{
	/* On Windows all config strings are UTF-8; return a plain copy. */
	return text ? strdup(text) : NULL;
}

static char *resolve_ini_ain_filename(const char *game_dir, const char *raw_name)
{
	if (!raw_name)
		return NULL;

	/*
	 * The CodeName value in alicestart.ini may be encoded in EUC-JP (cp20932)
	 * rather than the expected SJIS, particularly in older Alice Soft titles.
	 * Try UTF-8, EUC-JP (cp20932), SJIS in order; pick the first whose decoded
	 * name exists on the filesystem.  cp51932 is intentionally omitted because
	 * it is not reliably available on all Windows installations.
	 */
	static const UINT codepages[] = { CP_UTF8, 20932, 932 };
	for (size_t i = 0; i < sizeof(codepages) / sizeof(codepages[0]); i++) {
		char *utf8_name = decode_multibyte_to_utf8(raw_name, codepages[i]);
		if (!utf8_name)
			continue;

		char *candidate = path_join(game_dir, utf8_name);
		bool exists = file_exists(candidate);
		free(candidate);
		if (exists) {
			/* Record the detected encoding for ain_open_conv(). */
			game_str_codepage = codepages[i];
			char *resolved = utf8_to_game_text(utf8_name);
			free(utf8_name);
			return resolved;
		}
		free(utf8_name);
	}

	return normalize_ini_game_text(raw_name);
}

static char **normalize_argv_utf8(int *argc)
{
	int wargc = 0;
	LPWSTR *wargv = CommandLineToArgvW(GetCommandLineW(), &wargc);
	if (!wargv)
		return NULL;

	char **utf8_argv = xcalloc(wargc + 1, sizeof(char *));
	for (int i = 0; i < wargc; i++) {
		utf8_argv[i] = wchar_to_utf8(wargv[i]);
	}
	LocalFree(wargv);
	*argc = wargc;
	return utf8_argv;
}
#else
static char *normalize_ini_game_text(const char *text)
{
	return text ? strdup(text) : NULL;
}

static char *utf8_to_game_text(const char *text)
{
	return text ? utf2sjis(text, strlen(text)) : NULL;
}

static char *resolve_ini_ain_filename(const char *game_dir, const char *raw_name)
{
	(void)game_dir;
	return normalize_ini_game_text(raw_name);
}
#endif

static struct string *ini_string(struct ini_entry *entry)
{
	if (entry->value.type != INI_STRING)
		ERROR("ini value for '%s' is not a string", entry->name->text);
	return entry->value.s;
}

static int ini_integer(struct ini_entry *entry)
{
	if (entry->value.type != INI_INTEGER)
		ERROR("ini value for '%s' is not an integer", entry->name->text);
	return entry->value.i;
}

static int ini_boolean(struct ini_entry *entry)
{
	if (entry->value.type != INI_BOOLEAN)
		ERROR("ini value for '%s' is not a boolean", entry->name->text);
	return entry->value.i;
}

static void read_mixer_channels(struct ini_entry *entry)
{
	if (entry->value.type != INI_LIST)
		ERROR("ini value for 'VolumeValancer' is not a list");

	config.mixer_nr_channels = entry->value.list_size;
	config.mixer_channels = xcalloc(entry->value.list_size, sizeof(char*));
	config.mixer_volumes = xcalloc(entry->value.list_size, sizeof(int));
	for (size_t i = 0; i < entry->value.list_size; i++) {
		if (entry->value.list[i].type == INI_NULL) {
			config.mixer_channels[i] = strdup("Unnamed");
			config.mixer_volumes[i] = 0;
		} else if (entry->value.list[i].type == INI_STRING) {
			config.mixer_channels[i] = strdup(entry->value.list[i].s->text);
			// XXX: assumes default_volume was already initialized
			config.mixer_volumes[i] = config.default_volume;
		} else if (entry->value.list[i].type == INI_LIST) {
			if (entry->value.list[i].list_size != 2)
				ERROR("ini value for 'VolumeValancer' is list of wrong size");
			if (entry->value.list[i].list[0].type != INI_STRING)
				ERROR("ini value for 'VolumeValancer' name is not a string");
			if (entry->value.list[i].list[1].type != INI_INTEGER)
				ERROR("ini value for 'VolumeValancer' level is not an integer");
			config.mixer_channels[i] = strdup(entry->value.list[i].list[0].s->text);
			config.mixer_volumes[i] = entry->value.list[i].list[1].i;
		}
	}
}

static bool read_config(const char *path)
{
	int ini_size;
	struct ini_entry *ini = ini_parse(path, &ini_size);
	if (!ini)
		return false;

	for (int i = 0; i < ini_size; i++) {
		if (!strcmp(ini[i].name->text, "GameName")) {
			config.game_name = normalize_ini_game_text(ini_string(&ini[i])->text);
		} else if (!strcmp(ini[i].name->text, "BootName")) {
			config.boot_name = normalize_ini_game_text(ini_string(&ini[i])->text);
		} else if (!strcmp(ini[i].name->text, "CodeName")) {
			config.ain_filename = strdup(ini_string(&ini[i])->text);
		} else if (!strcmp(ini[i].name->text, "MainVM")) {
			config.vm_name = normalize_ini_game_text(ini_string(&ini[i])->text);
		} else if (!strcmp(ini[i].name->text, "SaveFolder")) {
			config.save_dir = normalize_ini_game_text(ini_string(&ini[i])->text);
		} else if (!strcmp(ini[i].name->text, "ViewWidth")) {
			config.view_width = ini_integer(&ini[i]);
		} else if (!strcmp(ini[i].name->text, "ViewHeight")) {
			config.view_height = ini_integer(&ini[i]);
		} else if (!strcmp(ini[i].name->text, "VolumeValancer")) {
			read_mixer_channels(&ini[i]);
		} else if (!strcmp(ini[i].name->text, "DefaultVolumeRate")) {
			config.default_volume = ini_integer(&ini[i]);
		} else if (!strcmp(ini[i].name->text, "UseJoypad")) {
			config.joypad = ini_boolean(&ini[i]);
		}
		ini_free_entry(&ini[i]);
	}
	free(ini);
	return true;
}

static void read_user_config_file(const char *path)
{
	int ini_size;
	struct ini_entry *ini = ini_parse(path, &ini_size);
	if (!ini)
		return;

	for (int i = 0; i < ini_size; i++) {
		if (!strcmp(ini[i].name->text, "font-mincho")) {
			config.font_paths[FONT_MINCHO] = xstrdup(ini_string(&ini[i])->text);
		} else if (!strcmp(ini[i].name->text, "font-gothic")) {
			config.font_paths[FONT_GOTHIC] = xstrdup(ini_string(&ini[i])->text);
		} else if (!strcmp(ini[i].name->text, "font-fnl")) {
			config.fnl_path = xstrdup(ini_string(&ini[i])->text);
		} else if (!strcmp(ini[i].name->text, "font-x-scale")) {
			float f = strtof(ini_string(&ini[i])->text, NULL);
			if (fabsf(f) < 0.01) {
				WARNING("Invalid value for font-x-scale in config: \"%s\"",
						ini_string(&ini[i])->text);
			} else {
				config.manual_text_x_scale = true;
				config.text_x_scale = f;
			}
		} else if (!strcmp(ini[i].name->text, "msgskip-delay")) {
			config.msgskip_delay = ini_integer(&ini[i]);
			if (config.msgskip_delay < 0) {
				WARNING("Invalid value for msgskip-delay in config: \"%s\"",
						ini_string(&ini[i])->text);
				config.msgskip_delay = 0;
			}
		} else if (!strcmp(ini[i].name->text, "save-folder")) {
			free(config.save_dir);
			config.save_dir = xstrdup(ini_string(&ini[i])->text);
		} else if (!strcmp(ini[i].name->text, "save-format")) {
			if (!strcmp(ini_string(&ini[i])->text, "json")) {
				config.save_format = SAVE_FORMAT_JSON;
			} else if (!strcmp(ini_string(&ini[i])->text, "rsm")) {
				config.save_format = SAVE_FORMAT_RSM;
			} else {
				WARNING("Invalid value for save-format in config: \"%s\"",
						ini_string(&ini[i])->text);
			}
		}
		ini_free_entry(&ini[i]);
	}
	free(ini);
}

static void read_user_config(void)
{
	char *path;

	// global config
	path = xmalloc(PATH_MAX);
	snprintf(path, PATH_MAX-1, "%s/.xsys4rc", config.home_dir);
	read_user_config_file(path);
	free(path);

	// game-specific config
	path = gamedir_path(".xsys4rc");
	read_user_config_file(path);
	free(path);
}

static char *get_xsystem4_home(void)
{
	// $XSYSTEM4_HOME
	char *env_home = getenv("XSYSTEM4_HOME");
	if (env_home && *env_home) {
		return xstrdup(env_home);
	}

	// $XDG_DATA_HOME/xsystem4
	env_home = getenv("XDG_DATA_HOME");
	if (env_home && *env_home) {
		char *home = xmalloc(strlen(env_home) + strlen("/xsystem4") + 1);
		strcpy(home, env_home);
		strcat(home, "/xsystem4");
		return home;
	}

	// $HOME/.xsystem4
	env_home = getenv("HOME");
	if (env_home && *env_home) {
		char *home = xmalloc(strlen(env_home) + strlen("/.xsystem4") + 1);
		strcpy(home, env_home);
		strcat(home, "/.xsystem4");
		return home;
	}

#ifdef _WIN32
	// %APPDATA%/xsystem4  (e.g. C:/Users/user/AppData/Roaming/xsystem4)
	env_home = getenv("APPDATA");
	if (!env_home || !*env_home)
		env_home = getenv("USERPROFILE");
	if (env_home && *env_home) {
		char buf[PATH_MAX];
		snprintf(buf, PATH_MAX - 1, "%s/xsystem4", env_home);
		return realpath_utf8(buf);
	}
#endif

	// If all else fails, use the current directory
	return realpath_utf8(".");
}

static char *get_save_path(const char *dir_name)
{
	if (!dir_name)
		dir_name = "SaveData";

#ifdef _WIN32
	/* On Windows, save directly inside the game directory to avoid
	 * issues with non-ASCII characters in the home path. */
	char *save_dir = xmalloc(strlen(config.game_dir) + 1 + strlen(dir_name) + 1);
	strcpy(save_dir, config.game_dir);
	strcat(save_dir, "/");
	strcat(save_dir, dir_name);
	return save_dir;
#else
	char *utf8_game_name = sjis2utf(config.game_name, strlen(config.game_name));
	char *utf8_dir_name  = sjis2utf(dir_name, strlen(dir_name));
	char *save_dir = xmalloc(strlen(config.home_dir) + 1 + strlen(utf8_game_name) + 1 + strlen(utf8_dir_name) + 1);
	strcpy(save_dir, config.home_dir);
	strcat(save_dir, "/");
	strcat(save_dir, utf8_game_name);
	strcat(save_dir, "/");
	strcat(save_dir, utf8_dir_name);
	free(utf8_game_name);
	free(utf8_dir_name);
	return save_dir;
#endif
}

static void config_init(void)
{
	config.home_dir = get_xsystem4_home();
	if (!config.game_name)
		config.game_name = strdup(config.ain_filename);

	char *new_save_dir = get_save_path(config.save_dir);
	free(config.save_dir);
	config.save_dir = new_save_dir;
}

static bool config_init_with_ini(const char *ini_path)
{
	if (!read_config(ini_path))
		return false;
	if (!config.ain_filename)
		ERROR("No AIN filename specified in %s", ini_path);
	char *tmp = strdup(ini_path);
	config.game_dir = strdup(path_dirname(tmp));
	free(tmp);
	char *resolved_ain_filename = resolve_ini_ain_filename(config.game_dir, config.ain_filename);
	free(config.ain_filename);
	config.ain_filename = resolved_ain_filename;
	config_init();
	return true;
}

static bool config_init_with_dir(const char *dir)
{
	char path[PATH_MAX];
	snprintf(path, PATH_MAX, "%s/System40.ini", dir);
	if (!file_exists(path)) {
		snprintf(path, PATH_MAX, "%s/AliceStart.ini", dir);
		if (!file_exists(path))
			return false;
	}
	return config_init_with_ini(path);
}

static void config_init_with_ain(const char *ain_path)
{
	config.ain_filename = utf8_to_game_text(path_basename(ain_path));
	config.game_dir = strdup(path_dirname(ain_path));
	config_init();
}

static void ain_audit(FILE *f, struct ain *ain)
{
	init_libraries();

	for (size_t addr = 0; addr < ain->code_size;) {
		uint16_t opcode = LittleEndian_getW(ain->code, addr);
		const struct instruction *instr = &instructions[opcode];
		if (opcode >= NR_OPCODES) {
			ERROR("0x%08zx: Invalid/unknown opcode: %x", opcode);
		}
		if (!instr->implemented) {
			fprintf(f, "0x%08zx: %s (unimplemented instruction)\n", addr, instr->name);
		}
		if (opcode == CALLSYS) {
			uint32_t syscode = LittleEndian_getDW(ain->code, addr + 2);
			if (syscode >= NR_SYSCALLS) {
				ERROR("0x%08zx: CALLSYS system.(0x%x)\n", addr, syscode);
			}
			const char * const name = syscalls[syscode].name;
			if (!name) {
				fprintf(f, "0x%08zx: CALLSYS system.(0x%x)\n", addr, syscode);
			} else if (!syscalls[syscode].implemented) {
				fprintf(f, "0x%08zx: CALLSYS %s (unimplemented system call)\n", addr, name);
			}

		}
		if (opcode == CALLHLL) {
			uint32_t lib = LittleEndian_getDW(ain->code, addr+2);
			uint32_t fun = LittleEndian_getDW(ain->code, addr+6);
			if (!library_exists(lib)) {
				fprintf(f, "0x%08zx: CALLHLL %s.%s (unimplemented library)\n", addr,
					ain->libraries[lib].name, ain->libraries[lib].functions[fun].name);
			} else if (!library_function_exists(lib, fun)) {
				fprintf(f, "0x%08zx: CALLHLL %s.%s (unimplemented function)\n", addr,
					ain->libraries[lib].name, ain->libraries[lib].functions[fun].name);
			}
		}
		// TODO: audit library calls
		addr += instruction_width(opcode);
	}
	fflush(f);
}

static void usage(void)
{
	puts("Usage: xsystem4 [options] [inifile-or-directory]");
	puts("    -h, --help           Display this message and exit");
	puts("    -v, --version        Display the version and exit");
	puts("    -a, --audit          Audit AIN file for xsystem4 compatibility");
	puts("    -e, --echo-message   Echo in-game messages to standard output");
	puts("        --font-mincho    Specify the path to the mincho font to use");
	puts("        --font-gothic    Specify the path to the gothic font to use");
	puts("        --font-fnl       Specify the path to a .fnl font library to use");
	puts("        --font-x-scale   Specify the x scale for text rendering (1.0 = default scale)");
	puts("    -j, --joypad         Enable joypad");
	puts("        --msgskip-delay  Specify the delay in ms to add when skipping messages with CTRL");
	puts("        --save-folder    Override save folder location");
	puts("        --save-format    Specify the resume save file format. json (default) or rsm");
#ifdef DEBUGGER_ENABLED
	puts("        --nodebug        Disable debugger");
	puts("        --debug          Start in debugger");
	puts("        --debug-info     Specify the path to the debug information file");
#endif
}

static _Noreturn void _usage_error(const char *fmt, ...)
{
	usage();
	puts("");

	va_list ap;
	va_start(ap, fmt);
	sys_verror(fmt, ap);
}
#define usage_error(fmt, ...) _usage_error("Error: " fmt "\n", ##__VA_ARGS__)

enum {
	LOPT_HELP = 256,
	LOPT_VERSION,
	LOPT_AUDIT,
	LOPT_ECHO_MESSAGE,
	LOPT_FONT_MINCHO,
	LOPT_FONT_GOTHIC,
	LOPT_FONT_FNL,
	LOPT_FONT_X_SCALE,
	LOPT_JOYPAD,
	LOPT_MSGSKIP_DELAY,
	LOPT_SAVE_FOLDER,
	LOPT_SAVE_FORMAT,
#ifdef DEBUGGER_ENABLED
	LOPT_NODEBUG,
	LOPT_DEBUG,
	LOPT_DEBUG_API,
	LOPT_DEBUG_INFO,
#endif
};

static void error_handler(const char *msg)
{
	SDL_ShowSimpleMessageBox(SDL_MESSAGEBOX_ERROR, "xsystem4", msg, NULL);
}

int main(int argc, char *argv[])
{
	sys_error_handler = error_handler;

#ifdef _WIN32
	char **utf8_argv = normalize_argv_utf8(&argc);
	if (utf8_argv)
		argv = utf8_argv;
#endif

	char *ainfile;
	int err = AIN_SUCCESS;
	bool audit = false;

	char *font_mincho = NULL;
	char *font_gothic = NULL;
	char *font_fnl = NULL;
	char *joypad = NULL;
	char *savedir = NULL;
	char *debug_info_path = NULL;

	while (1) {
		static struct option long_options[] = {
			{ "help",          no_argument,       0, LOPT_HELP },
			{ "version",       no_argument,       0, LOPT_VERSION },
			{ "audit",         no_argument,       0, LOPT_AUDIT },
			{ "echo-message",  no_argument,       0, LOPT_ECHO_MESSAGE },
			{ "font-mincho",   required_argument, 0, LOPT_FONT_MINCHO },
			{ "font-gothic",   required_argument, 0, LOPT_FONT_GOTHIC },
			{ "font-fnl",      required_argument, 0, LOPT_FONT_FNL },
			{ "font-x-scale",  required_argument, 0, LOPT_FONT_X_SCALE },
			{ "joypad",        optional_argument, 0, LOPT_JOYPAD },
			{ "msgskip-delay", required_argument, 0, LOPT_MSGSKIP_DELAY },
			{ "save-folder",   required_argument, 0, LOPT_SAVE_FOLDER },
			{ "save-format",   required_argument, 0, LOPT_SAVE_FORMAT },
#ifdef DEBUGGER_ENABLED
			{ "nodebug",       no_argument,       0, LOPT_NODEBUG },
			{ "debug",         no_argument,       0, LOPT_DEBUG },
			{ "debug-api",     no_argument,       0, LOPT_DEBUG_API },
			{ "debug-info",    required_argument, 0, LOPT_DEBUG_INFO },
#endif
			{ 0 }
		};
		int option_index = 0;
		int c = getopt_long(argc, argv, "haej::v", long_options, &option_index);
		if (c == -1)
			break;

		switch (c) {
		case 'h':
		case LOPT_HELP:
			usage();
			return 0;
		case 'v':
		case LOPT_VERSION:
			NOTICE("xsystem4 " XSYSTEM4_VERSION);
			return 0;
		case 'a':
		case LOPT_AUDIT:
			audit = true;
			break;
		case 'e':
		case LOPT_ECHO_MESSAGE:
			config.echo = true;
			break;
		case LOPT_FONT_MINCHO:
			font_mincho = optarg;
			break;
		case LOPT_FONT_GOTHIC:
			font_gothic = optarg;
			break;
		case LOPT_FONT_FNL:
			font_fnl = optarg;
			break;
		case LOPT_FONT_X_SCALE:
			config.manual_text_x_scale = true;
			config.text_x_scale = strtof(optarg, NULL);
			if (fabsf(config.text_x_scale) < 0.01) {
				WARNING("Invalid value for --font-x-scale option: \"%s\"", optarg);
				config.manual_text_x_scale = false;
				config.text_x_scale = 1.0;
			}
			break;
		case 'j':
		case LOPT_JOYPAD:
			joypad = optarg ? optarg : "on";
			break;
		case LOPT_MSGSKIP_DELAY:
			config.msgskip_delay = atoi(optarg);
			if (config.msgskip_delay <= 0) {
				WARNING("Invalid value for --msgskip-delay: \"%s\"", optarg);
				config.msgskip_delay = 0;
			}
			break;
		case LOPT_SAVE_FOLDER:
			savedir = optarg;
			break;
		case LOPT_SAVE_FORMAT:
			if (!strcmp(optarg, "json")) {
				config.save_format = SAVE_FORMAT_JSON;
			} else if (!strcmp(optarg, "rsm")) {
				config.save_format = SAVE_FORMAT_RSM;
			} else {
				WARNING("Invalid value for --save-format option: \"%s\"", optarg);
			}
			break;
#ifdef DEBUGGER_ENABLED
		case LOPT_NODEBUG:
			dbg_enabled = false;
			break;
		case LOPT_DEBUG:
			dbg_start_in_debugger = true;
			break;
		case LOPT_DEBUG_API:
			dbg_dap = true;
			sys_silent = true;
			break;
		case LOPT_DEBUG_INFO:
			debug_info_path = optarg;
			break;
#endif
		}
	}
	argc -= optind;
	argv += optind;

	if (argc < 1) {
		if (!config_init_with_dir(".")) {
			if (!config_init_with_dir(".."))
				usage_error("Failed to find game in current or parent directory");
		}
	} else if (argc > 1) {
		usage_error("Too many arguments");
	} else if (is_directory(argv[0])) {
		if (!config_init_with_dir(argv[0]))
			usage_error("Failed to find game in '%s'", argv[0]);
	} else if (!strcasecmp(file_extension(argv[0]), "ini")) {
		if (!config_init_with_ini(argv[0]))
			usage_error("Failed to read .ini file '%s'", argv[0]);
	} else if (!strcasecmp(file_extension(argv[0]), "ain")) {
		config_init_with_ain(argv[0]);
	} else {
		usage_error("Can't initialize game with argument '%s'", argv[0]);
	}
	ainfile = gamedir_path(config.ain_filename);

	read_user_config();

	// NOTE: some command line options are handled here so that they
	//       will override settings from .xsys4rc files
	if (font_mincho)
		config.font_paths[FONT_MINCHO] = font_mincho;
	if (font_gothic)
		config.font_paths[FONT_GOTHIC] = font_gothic;
	if (font_fnl)
		config.fnl_path = font_fnl;
	if (joypad) {
		if (!strcmp(joypad, "on"))
			config.joypad = true;
		else if (!strcmp(joypad, "off"))
			config.joypad = false;
		else
			WARNING("Invalid value for 'joypad' option (must be 'on' or 'off')");
	}
	if (savedir) {
		free(config.save_dir);
		config.save_dir = strdup(savedir);
	}

#ifdef _WIN32
	if (!(ain = ain_open_conv(ainfile, game_text_to_utf8, &err))) {
#else
	if (!(ain = ain_open(ainfile, &err))) {
#endif
		ERROR("%s: %s", ain_strerror(err), display_utf0(ainfile));
	}

	if (audit) {
		ain_audit(stdout, ain);
		ain_free(ain);
		return 0;
	}

	mkdir_p(config.save_dir);
	apply_game_specific_hacks(ain);
	if (config.msgskip_delay)
		set_msgskip_delay(ain, config.msgskip_delay);
	asset_manager_init();
	dbg_init(debug_info_path);
	sys_exit(vm_execute_ain(ain));
}
