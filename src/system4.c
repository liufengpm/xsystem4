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
#if defined(XSYSTEM4_HOST_UTF8)
#include <errno.h>
#include <iconv.h>
#endif

#ifdef __OHOS__
#include <SDL.h>
#endif

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#define NOGDI
#include <windows.h>
#include <shellapi.h>
#include <psapi.h>
#include <dbghelp.h>
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
	DWORD flags = WC_NO_BEST_FIT_CHARS;
	BOOL *used_default_char_ptr = &used_default_char;

	if (codepage == CP_UTF8 || codepage == 54936) {
		flags = 0;
		used_default_char_ptr = NULL;
	}

	int nr_chars = WideCharToMultiByte(codepage, flags, text, -1,
			NULL, 0, NULL, used_default_char_ptr);
	if (nr_chars <= 0)
		return NULL;

	char *encoded = xmalloc(nr_chars);
	if (!WideCharToMultiByte(codepage, flags, text, -1,
			encoded, nr_chars, NULL, used_default_char_ptr)) {
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

enum game_text_encoding {
	GAME_TEXT_ENCODING_AUTO,
	GAME_TEXT_ENCODING_UTF8,
	GAME_TEXT_ENCODING_EUC_JP,
	GAME_TEXT_ENCODING_CP932,
	GAME_TEXT_ENCODING_GB18030,
};

static const UINT game_text_candidate_codepages[] = {
	CP_UTF8,
	54936,
	936,
	20932,
	932,
};

enum xsystem4_ain_conv_section_kind {
	XSYSTEM4_AIN_CONV_SECTION_GENERAL = 0,
	XSYSTEM4_AIN_CONV_SECTION_CODE = 1,
	XSYSTEM4_AIN_CONV_SECTION_MESSAGE = 2,
};

/* Preferred game text codepage for mixed-encoding titles. */
static UINT game_str_codepage = 932;
static bool game_text_encoding_forced = false;

/*
 * Per-section encoding hint set by ain.c during AIN loading.
 * CODE sections keep original JP identifiers in CP932.
 * MESSAGE sections may use the user-forced localized text encoding.
 * GENERAL sections (for example STR0 constants) stay heuristic-driven so
 * Japanese script keys are not corrupted by a blanket GB18030 override.
 */
extern int xsystem4_ain_conv_code_section;

static const char *game_text_encoding_label(enum game_text_encoding encoding)
{
	switch (encoding) {
	case GAME_TEXT_ENCODING_AUTO:
		return "auto";
	case GAME_TEXT_ENCODING_UTF8:
		return "UTF-8";
	case GAME_TEXT_ENCODING_EUC_JP:
		return "EUC-JP";
	case GAME_TEXT_ENCODING_CP932:
		return "CP932";
	case GAME_TEXT_ENCODING_GB18030:
		return "GB18030";
	default:
		return "unknown";
	}
}

static UINT game_text_encoding_codepage(enum game_text_encoding encoding)
{
	switch (encoding) {
	case GAME_TEXT_ENCODING_UTF8:
		return CP_UTF8;
	case GAME_TEXT_ENCODING_EUC_JP:
		return 20932;
	case GAME_TEXT_ENCODING_CP932:
		return 932;
	case GAME_TEXT_ENCODING_GB18030:
		return 54936;
	case GAME_TEXT_ENCODING_AUTO:
	default:
		return 0;
	}
}

static enum game_text_encoding game_text_encoding_from_codepage(UINT codepage)
{
	switch (codepage) {
	case CP_UTF8:
		return GAME_TEXT_ENCODING_UTF8;
	case 20932:
		return GAME_TEXT_ENCODING_EUC_JP;
	case 936:
	case 54936:
		return GAME_TEXT_ENCODING_GB18030;
	case 932:
	default:
		return GAME_TEXT_ENCODING_CP932;
	}
}

static bool parse_game_text_encoding(const char *text, enum game_text_encoding *encoding)
{
	if (!text || !*text)
		return false;

	if (!strcasecmp(text, "auto")) {
		*encoding = GAME_TEXT_ENCODING_AUTO;
		return true;
	}
	if (!strcasecmp(text, "utf8") || !strcasecmp(text, "utf-8")) {
		*encoding = GAME_TEXT_ENCODING_UTF8;
		return true;
	}
	if (!strcasecmp(text, "euc-jp") || !strcasecmp(text, "eucjp")) {
		*encoding = GAME_TEXT_ENCODING_EUC_JP;
		return true;
	}
	if (!strcasecmp(text, "cp932")
			|| !strcasecmp(text, "sjis")
			|| !strcasecmp(text, "shift-jis")
			|| !strcasecmp(text, "shift_jis")
			|| !strcasecmp(text, "windows-31j")
			|| !strcasecmp(text, "ms932")) {
		*encoding = GAME_TEXT_ENCODING_CP932;
		return true;
	}
	if (!strcasecmp(text, "gb18030")
			|| !strcasecmp(text, "gbk")
			|| !strcasecmp(text, "cp936")) {
		*encoding = GAME_TEXT_ENCODING_GB18030;
		return true;
	}
	return false;
}

static void force_game_text_encoding(enum game_text_encoding encoding, const char *source)
{
	if (encoding == GAME_TEXT_ENCODING_AUTO) {
		game_text_encoding_forced = false;
		game_str_codepage = 932;
		NOTICE("xsystem4 text encoding set to auto (%s)", source);
		return;
	}

	game_text_encoding_forced = true;
	game_str_codepage = game_text_encoding_codepage(encoding);
	NOTICE("xsystem4 text encoding forced to %s (%s)",
			game_text_encoding_label(encoding), source);
}

static void configure_game_text_encoding(const char *text, const char *source)
{
	enum game_text_encoding encoding;

	if (!parse_game_text_encoding(text, &encoding)) {
		WARNING("Invalid value for text encoding in %s: \"%s\"", source, text);
		return;
	}
	force_game_text_encoding(encoding, source);
}

static void apply_env_game_text_encoding_override(void)
{
	char *env = getenv("XSYSTEM4_TEXT_ENCODING");

	if (env && *env)
		configure_game_text_encoding(env, "XSYSTEM4_TEXT_ENCODING");
}

/* Conversion function passed to ain_open_conv(): converts game-native encoding -> UTF-8. */
static int score_decoded_string(const wchar_t *text)
{
	int score = 0;

	for (; *text; text++) {
		wchar_t c = *text;
		if (c < 0x20 && c != '\t' && c != '\r' && c != '\n') {
			score -= 20;
		} else if (c >= 0xE000 && c <= 0xF8FF) {
			score -= 20;
		} else if (c >= 0x3040 && c <= 0x30FF) {
			score += 7;
		} else if ((c >= 0x3400 && c <= 0x4DBF)
				|| (c >= 0x4E00 && c <= 0x9FFF)) {
			score += 3;
		} else if ((c >= 0x3001 && c <= 0x303F)
				|| (c >= 0xFF01 && c <= 0xFF60)
				|| c == 0x2014
				|| c == 0x2018
				|| c == 0x2019
				|| c == 0x201C
				|| c == 0x201D) {
			score += 2;
		} else if ((c >= 0xFF10 && c <= 0xFF19)
				|| (c >= 0xFF21 && c <= 0xFF3A)
				|| (c >= 0xFF41 && c <= 0xFF5A)) {
			score += 2;
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

static bool use_detected_text_codepage(UINT preferred_codepage, int preferred_score,
		UINT detected_codepage, int detected_score)
{
	if (!preferred_codepage || preferred_score == INT_MIN)
		return true;
	if (preferred_codepage == detected_codepage)
		return true;
	if (preferred_score < 0)
		return true;
	if (detected_codepage == CP_UTF8 && detected_score >= preferred_score)
		return true;
	return detected_score >= preferred_score + 8;
}

static char *detect_best_game_text_to_utf8(const char *text,
		UINT preferred_codepage, UINT *detected_codepage)
{
	char *best_utf8 = NULL;
	int best_score = INT_MIN;
	int preferred_score = INT_MIN;
	UINT best_codepage = 0;
	char *preferred_utf8 = NULL;

	for (size_t i = 0; i < sizeof(game_text_candidate_codepages) / sizeof(game_text_candidate_codepages[0]); i++) {
		UINT codepage = game_text_candidate_codepages[i];
		wchar_t *candidate = decode_multibyte_string(text, codepage);
		if (!candidate)
			continue;

		int score = score_decoded_string(candidate);
		char *candidate_utf8 = wchar_to_utf8(candidate);
		free(candidate);
		if (!candidate_utf8)
			continue;

		if (codepage == preferred_codepage) {
			free(preferred_utf8);
			preferred_utf8 = strdup(candidate_utf8);
			preferred_score = score;
		}

		if (!best_utf8 || score > best_score) {
			free(best_utf8);
			best_utf8 = candidate_utf8;
			best_score = score;
			best_codepage = codepage;
		} else {
			free(candidate_utf8);
		}
	}

	if (detected_codepage)
		*detected_codepage = best_codepage;

	if (!best_utf8)
		return NULL;
	if (preferred_utf8
			&& !use_detected_text_codepage(preferred_codepage, preferred_score,
					best_codepage, best_score)) {
		free(best_utf8);
		return preferred_utf8;
	}

	free(preferred_utf8);
	return best_utf8;
}

/* Conversion function passed to ain_open_conv(): converts game-native encoding -> UTF-8. */
static char *game_text_to_utf8(const char *str)
{
	UINT detected_codepage = 0;
	char *utf8;
	bool is_code_section = xsystem4_ain_conv_code_section == XSYSTEM4_AIN_CONV_SECTION_CODE;
	bool is_message_section = xsystem4_ain_conv_code_section == XSYSTEM4_AIN_CONV_SECTION_MESSAGE;

	if (!str)
		return NULL;
	if (is_ascii_text(str))
		return strdup(str);
	/*
	 * For mixed-encoding AIN files (e.g. Chinese-localized AliceSoft games),
	 * code identifiers in FUNC/STRT/GLOB/HLL0 are always in the original
	 * game encoding (CP932), while game text in STR0/MSG0 may be in a
	 * different encoding (e.g. GB18030).  Force CP932 for code sections
	 * to avoid misdetection when CJK kanji score equally in both codepages.
	 * This check must come before game_text_encoding_forced so that user-
	 * specified encoding (e.g. gb18030 from .xsys4rc) does not corrupt
	 * CP932 code identifiers.
	 */
	if (is_code_section)
		return decode_multibyte_to_utf8(str, 932);
	if (game_text_encoding_forced && is_message_section)
		return decode_multibyte_to_utf8(str, game_str_codepage);

	utf8 = detect_best_game_text_to_utf8(str, game_str_codepage, &detected_codepage);
	if (utf8)
		return utf8;
	return decode_multibyte_to_utf8(str, game_str_codepage);
}

static char *resource_text_to_utf8(const char *text)
{
	UINT preferred_codepage = game_text_encoding_forced ? 932 : game_str_codepage;
	char *utf8;

	if (!text)
		return NULL;
	if (is_ascii_text(text))
		return strdup(text);

	utf8 = detect_best_game_text_to_utf8(text, preferred_codepage, NULL);
	if (utf8)
		return utf8;
	return decode_multibyte_to_utf8(text, preferred_codepage);
}

static char *normalize_ini_game_text(const char *text)
{
	UINT detected_codepage = 0;
	char *utf8;

	if (!text)
		return NULL;
	if (is_ascii_text(text))
		return strdup(text);
	if (game_text_encoding_forced) {
		char *forced = decode_multibyte_to_utf8(text, game_str_codepage);
		return forced ? forced : strdup(text);
	}

	utf8 = detect_best_game_text_to_utf8(text, game_str_codepage, &detected_codepage);
	return utf8 ? utf8 : strdup(text);
}

static char *utf8_to_game_text(const char *text)
{
	return text ? strdup(text) : NULL;
}

char *xsystem4_utf8_to_ain_text(const char *text)
{
	return text ? strdup(text) : NULL;
}

char *xsystem4_game_text_to_utf8(const char *text)
{
	return game_text_to_utf8(text);
}

char *xsystem4_resource_text_to_utf8(const char *text)
{
	return resource_text_to_utf8(text);
}

char *xsystem4_resource_lookup_alias(const char *text)
{
	wchar_t *wide;
	char *encoded;
	char *alias;

	if (!text || !*text || !game_text_encoding_forced || game_str_codepage == 932)
		return NULL;

	wide = decode_multibyte_string(text, CP_UTF8);
	if (!wide)
		return NULL;
	encoded = encode_wide_string(wide, game_str_codepage);
	free(wide);
	if (!encoded)
		return NULL;
	alias = decode_multibyte_to_utf8(encoded, 932);
	free(encoded);
	if (!alias || !strcmp(alias, text)) {
		free(alias);
		return NULL;
	}
	return alias;
}

static char *resolve_ini_ain_filename(const char *game_dir, const char *raw_name)
{
	if (!raw_name)
		return NULL;
	if (is_ascii_text(raw_name))
		return strdup(raw_name);
	if (game_text_encoding_forced) {
		char *utf8_name = decode_multibyte_to_utf8(raw_name, game_str_codepage);
		if (utf8_name)
			return utf8_name;
	}

	/*
	 * The CodeName value in alicestart.ini may be encoded in EUC-JP (cp20932)
	 * rather than the expected SJIS, particularly in older Alice Soft titles.
	 * Try UTF-8, GB18030/GBK, system ANSI, EUC-JP (cp20932), SJIS in order;
	 * pick the first whose decoded name exists on the filesystem.
	 */
	static const UINT codepages[] = { CP_UTF8, 54936, 936, CP_ACP, 20932, 932 };
	for (size_t i = 0; i < sizeof(codepages) / sizeof(codepages[0]); i++) {
		char *utf8_name = decode_multibyte_to_utf8(raw_name, codepages[i]);
		if (!utf8_name)
			continue;

		char *candidate = path_join(game_dir, utf8_name);
		bool exists = file_exists(candidate);
		free(candidate);
		if (exists) {
			if (!game_text_encoding_forced) {
				game_str_codepage = codepages[i];
				NOTICE("xsystem4 AIN filename encoding detected as %s, "
						"setting game text preference to codepage %u (%s)",
						game_text_encoding_label(game_text_encoding_from_codepage(codepages[i])),
						codepages[i], "CodeName");
			}
			return utf8_name;
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
#elif defined(XSYSTEM4_HOST_UTF8)
enum game_text_encoding {
	GAME_TEXT_ENCODING_AUTO,
	GAME_TEXT_ENCODING_UTF8,
	GAME_TEXT_ENCODING_EUC_JP,
	GAME_TEXT_ENCODING_CP932,
	GAME_TEXT_ENCODING_GB18030,
	GAME_TEXT_ENCODING_NR,
};

static const enum game_text_encoding game_text_candidate_encodings[] = {
	GAME_TEXT_ENCODING_UTF8,
	GAME_TEXT_ENCODING_GB18030,
	GAME_TEXT_ENCODING_EUC_JP,
	GAME_TEXT_ENCODING_CP932,
};

enum xsystem4_ain_conv_section_kind {
	XSYSTEM4_AIN_CONV_SECTION_GENERAL = 0,
	XSYSTEM4_AIN_CONV_SECTION_CODE = 1,
	XSYSTEM4_AIN_CONV_SECTION_MESSAGE = 2,
};

/* Detected game text encoding for AIN strings on UTF-8 hosts. */
static enum game_text_encoding game_str_encoding = GAME_TEXT_ENCODING_AUTO;
static bool game_text_encoding_locked = false;
static bool game_text_encoding_forced = false;

/*
 * Per-section encoding hint set by ain.c during AIN loading.
 * CODE sections keep original JP identifiers in CP932.
 * MESSAGE sections may use the user-forced localized text encoding.
 * GENERAL sections (for example STR0 constants) stay heuristic-driven so
 * Japanese script keys are not corrupted by a blanket GB18030 override.
 */
extern int xsystem4_ain_conv_code_section;
static int game_text_auto_scores[GAME_TEXT_ENCODING_NR];
static int game_text_auto_samples = 0;

static const char *game_text_encoding_label(enum game_text_encoding encoding)
{
	switch (encoding) {
	case GAME_TEXT_ENCODING_AUTO:
		return "auto";
	case GAME_TEXT_ENCODING_UTF8:
		return "UTF-8";
	case GAME_TEXT_ENCODING_EUC_JP:
		return "EUC-JP";
	case GAME_TEXT_ENCODING_CP932:
		return "CP932";
	case GAME_TEXT_ENCODING_GB18030:
		return "GB18030";
	default:
		return "unknown";
	}
}

static const char *const *game_text_encoding_names(enum game_text_encoding encoding)
{
	static const char *const utf8_names[] = { "UTF-8", NULL };
	static const char *const euc_jp_names[] = { "EUC-JP", "eucJP", NULL };
	static const char *const cp932_names[] = {
		"CP932", "WINDOWS-31J", "MS932", "SHIFT-JIS", "SHIFT_JIS", "SJIS", NULL
	};
	static const char *const gb18030_names[] = {
		"GB18030", "GBK", "CP936", NULL
	};

	switch (encoding) {
	case GAME_TEXT_ENCODING_AUTO:
		return NULL;
	case GAME_TEXT_ENCODING_UTF8:
		return utf8_names;
	case GAME_TEXT_ENCODING_EUC_JP:
		return euc_jp_names;
	case GAME_TEXT_ENCODING_GB18030:
		return gb18030_names;
	case GAME_TEXT_ENCODING_CP932:
	default:
		return cp932_names;
	}
}

static void reset_game_text_auto_detection(void)
{
	game_str_encoding = GAME_TEXT_ENCODING_AUTO;
	game_text_encoding_locked = false;
	memset(game_text_auto_scores, 0, sizeof(game_text_auto_scores));
	game_text_auto_samples = 0;
}

static bool parse_game_text_encoding(const char *text, enum game_text_encoding *encoding)
{
	if (!text || !*text)
		return false;

	if (!strcasecmp(text, "auto")) {
		*encoding = GAME_TEXT_ENCODING_AUTO;
		return true;
	}
	if (!strcasecmp(text, "utf8") || !strcasecmp(text, "utf-8")) {
		*encoding = GAME_TEXT_ENCODING_UTF8;
		return true;
	}
	if (!strcasecmp(text, "euc-jp") || !strcasecmp(text, "eucjp")) {
		*encoding = GAME_TEXT_ENCODING_EUC_JP;
		return true;
	}
	if (!strcasecmp(text, "cp932")
			|| !strcasecmp(text, "sjis")
			|| !strcasecmp(text, "shift-jis")
			|| !strcasecmp(text, "shift_jis")
			|| !strcasecmp(text, "windows-31j")
			|| !strcasecmp(text, "ms932")) {
		*encoding = GAME_TEXT_ENCODING_CP932;
		return true;
	}
	if (!strcasecmp(text, "gb18030")
			|| !strcasecmp(text, "gbk")
			|| !strcasecmp(text, "cp936")) {
		*encoding = GAME_TEXT_ENCODING_GB18030;
		return true;
	}
	return false;
}

static void force_game_text_encoding(enum game_text_encoding encoding, const char *source)
{
	if (encoding == GAME_TEXT_ENCODING_AUTO) {
		game_text_encoding_forced = false;
		reset_game_text_auto_detection();
		NOTICE("xsystem4 text encoding set to auto (%s)", source);
		#ifdef __OHOS__
		SDL_Log("[xsystem4_text] encoding set to auto (%s)", source);
		#endif
		return;
	}

	game_text_encoding_forced = true;
	game_str_encoding = encoding;
	game_text_encoding_locked = true;
	memset(game_text_auto_scores, 0, sizeof(game_text_auto_scores));
	game_text_auto_samples = 0;
	NOTICE("xsystem4 text encoding forced to %s (%s)",
			game_text_encoding_label(encoding), source);
	#ifdef __OHOS__
	SDL_Log("[xsystem4_text] encoding forced to %s (%s)",
			game_text_encoding_label(encoding), source);
	#endif
}

static void configure_game_text_encoding(const char *text, const char *source)
{
	enum game_text_encoding encoding;

	if (!parse_game_text_encoding(text, &encoding)) {
		WARNING("Invalid value for text encoding in %s: \"%s\"", source, text);
		return;
	}
	force_game_text_encoding(encoding, source);
}

static void lock_game_text_encoding(enum game_text_encoding encoding, const char *source)
{
	if (game_text_encoding_forced || encoding == GAME_TEXT_ENCODING_AUTO || game_text_encoding_locked)
		return;

	game_str_encoding = encoding;
	game_text_encoding_locked = true;
	NOTICE("xsystem4 text encoding detected as %s (%s)",
			game_text_encoding_label(encoding), source);
	#ifdef __OHOS__
	SDL_Log("[xsystem4_text] encoding detected as %s (%s)",
			game_text_encoding_label(encoding), source);
	#endif
}

static void apply_env_game_text_encoding_override(void)
{
	char *env = getenv("XSYSTEM4_TEXT_ENCODING");

	if (env && *env)
		configure_game_text_encoding(env, "XSYSTEM4_TEXT_ENCODING");
}

#ifdef __OHOS__
static int xsystem4_text_trace_budget = 12;
static int xsystem4_text_passthrough_warn_budget = 8;

static void format_text_bytes_preview(const char *text, char *out, size_t out_size)
{
	size_t used = 0;
	size_t index = 0;

	if (!out_size)
		return;
	if (!text) {
		snprintf(out, out_size, "<null>");
		return;
	}

	while (text[index] && index < 12 && used + 4 < out_size) {
		used += snprintf(out + used, out_size - used,
				index ? " %02X" : "%02X", (unsigned char)text[index]);
		index++;
	}
	if (text[index] && used + 5 < out_size)
		snprintf(out + used, out_size - used, " ...");
	else if (used < out_size)
		out[used] = '\0';
}

static void format_text_utf8_preview(const char *text, char *out, size_t out_size)
{
	size_t in_index = 0;
	size_t out_index = 0;

	if (!out_size)
		return;
	if (!text) {
		snprintf(out, out_size, "<null>");
		return;
	}

	while (text[in_index] && in_index < 48 && out_index + 1 < out_size) {
		unsigned char c = (unsigned char)text[in_index++];
		if (c == '\r' || c == '\n' || c == '\t')
			out[out_index++] = ' ';
		else
			out[out_index++] = (char)c;
	}
	if (text[in_index] && out_index + 4 < out_size) {
		memcpy(out + out_index, "...", 4);
		return;
	}
	out[out_index] = '\0';
}

static void trace_text_conversion_sample(const char *label,
		const char *raw,
		enum game_text_encoding source_encoding,
		const char *utf8)
{
	char raw_hex[80];
	char utf8_preview[128];

	if (xsystem4_text_trace_budget <= 0 || !raw || !utf8 || is_ascii_text(raw))
		return;

	format_text_bytes_preview(raw, raw_hex, sizeof(raw_hex));
	format_text_utf8_preview(utf8, utf8_preview, sizeof(utf8_preview));
	NOTICE("xsystem4 text trace [%s]: source=%s raw=%s utf8=%s",
			label,
			game_text_encoding_label(source_encoding),
			raw_hex,
			utf8_preview);
	SDL_Log("[xsystem4_text] trace[%s] source=%s raw=%s utf8=%s",
			label,
			game_text_encoding_label(source_encoding),
			raw_hex,
			utf8_preview);
	xsystem4_text_trace_budget--;
}

static void warn_raw_text_passthrough(const char *label,
		const char *raw,
		enum game_text_encoding source_encoding)
{
	char raw_hex[80];

	if (xsystem4_text_passthrough_warn_budget <= 0 || !raw || is_ascii_text(raw))
		return;

	format_text_bytes_preview(raw, raw_hex, sizeof(raw_hex));
	WARNING("xsystem4 text trace [%s]: failed to decode %s on UTF-8 host; passing raw bytes through (raw=%s)",
			label,
			game_text_encoding_label(source_encoding),
			raw_hex);
	SDL_Log("[xsystem4_text] failed to decode %s for %s on UTF-8 host; passing raw bytes through (raw=%s)",
			label,
			game_text_encoding_label(source_encoding),
			raw_hex);
	xsystem4_text_passthrough_warn_budget--;
}
#else
static void trace_text_conversion_sample(const char *label,
		const char *raw,
		enum game_text_encoding source_encoding,
		const char *utf8)
{
	(void)label;
	(void)raw;
	(void)source_encoding;
	(void)utf8;
}

static void warn_raw_text_passthrough(const char *label,
		const char *raw,
		enum game_text_encoding source_encoding)
{
	(void)label;
	(void)raw;
	(void)source_encoding;
}
#endif

static char *convert_text_with_iconv(const char *text, const char *to_name, const char *from_name)
{
	if (!text || !to_name || !from_name)
		return NULL;

	iconv_t cd = iconv_open(to_name, from_name);
	if (cd == (iconv_t)-1)
		return NULL;

	size_t input_len = strlen(text);
	size_t output_capacity = input_len * 4 + 16;
	if (output_capacity < 32)
		output_capacity = 32;

	char *output = xmalloc(output_capacity);
	char *out_ptr = output;
	char *in_ptr = (char *)text;
	size_t in_left = input_len;

	while (true) {
		size_t out_used = (size_t)(out_ptr - output);
		size_t out_left = output_capacity - out_used;
		size_t result = iconv(cd, &in_ptr, &in_left, &out_ptr, &out_left);
		if (result != (size_t)-1)
			break;
		if (errno != E2BIG) {
			free(output);
			iconv_close(cd);
			return NULL;
		}

		output_capacity *= 2;
		output = xrealloc(output, output_capacity);
		out_ptr = output + out_used;
	}

	while (true) {
		size_t out_used = (size_t)(out_ptr - output);
		size_t out_left = output_capacity - out_used;
		size_t result = iconv(cd, NULL, NULL, &out_ptr, &out_left);
		if (result != (size_t)-1)
			break;
		if (errno != E2BIG) {
			free(output);
			iconv_close(cd);
			return NULL;
		}

		output_capacity *= 2;
		output = xrealloc(output, output_capacity);
		out_ptr = output + out_used;
	}

	*out_ptr = '\0';
	iconv_close(cd);
	return output;
}

static char *decode_multibyte_to_utf8(const char *text, enum game_text_encoding encoding)
{
	if (!text)
		return NULL;
	if (encoding == GAME_TEXT_ENCODING_AUTO)
		return NULL;
	if (encoding == GAME_TEXT_ENCODING_UTF8)
		return strdup(text);
	if (encoding == GAME_TEXT_ENCODING_CP932)
		return sjis2utf(text, 0);

	for (const char *const *name = game_text_encoding_names(encoding); *name; name++) {
		char *converted = convert_text_with_iconv(text, "UTF-8", *name);
		if (converted)
			return converted;
	}

	for (const char *const *name = game_text_encoding_names(encoding); *name; name++) {
		char *converted = SDL_iconv_string("UTF-8", *name, text, strlen(text) + 1);
		if (!converted)
			continue;
		char *copy = strdup(converted);
		SDL_free(converted);
		return copy;
	}
	return NULL;
}

static char *encode_utf8_to_multibyte(const char *text, enum game_text_encoding encoding)
{
	if (!text)
		return NULL;
	if (encoding == GAME_TEXT_ENCODING_AUTO)
		return NULL;
	if (encoding == GAME_TEXT_ENCODING_UTF8)
		return strdup(text);
	if (encoding == GAME_TEXT_ENCODING_CP932)
		return utf2sjis(text, 0);

	for (const char *const *name = game_text_encoding_names(encoding); *name; name++) {
		char *converted = convert_text_with_iconv(text, *name, "UTF-8");
		if (converted)
			return converted;
	}

	for (const char *const *name = game_text_encoding_names(encoding); *name; name++) {
		char *converted = SDL_iconv_string(*name, "UTF-8", text, strlen(text) + 1);
		if (!converted)
			continue;
		char *copy = strdup(converted);
		SDL_free(converted);
		return copy;
	}
	return NULL;
}

static bool utf8_decode_codepoint(const unsigned char **ptr, unsigned int *codepoint)
{
	const unsigned char *p = *ptr;
	unsigned int c;

	if (*p < 0x80) {
		*codepoint = *p;
		*ptr = p + 1;
		return true;
	}

	if ((p[0] & 0xE0) == 0xC0) {
		if ((p[1] & 0xC0) != 0x80)
			return false;
		c = ((p[0] & 0x1F) << 6) | (p[1] & 0x3F);
		if (c < 0x80)
			return false;
		*codepoint = c;
		*ptr = p + 2;
		return true;
	}

	if ((p[0] & 0xF0) == 0xE0) {
		if ((p[1] & 0xC0) != 0x80 || (p[2] & 0xC0) != 0x80)
			return false;
		c = ((p[0] & 0x0F) << 12) | ((p[1] & 0x3F) << 6) | (p[2] & 0x3F);
		if (c < 0x800 || (c >= 0xD800 && c <= 0xDFFF))
			return false;
		*codepoint = c;
		*ptr = p + 3;
		return true;
	}

	if ((p[0] & 0xF8) == 0xF0) {
		if ((p[1] & 0xC0) != 0x80 || (p[2] & 0xC0) != 0x80 || (p[3] & 0xC0) != 0x80)
			return false;
		c = ((p[0] & 0x07) << 18) | ((p[1] & 0x3F) << 12) | ((p[2] & 0x3F) << 6) | (p[3] & 0x3F);
		if (c < 0x10000 || c > 0x10FFFF)
			return false;
		*codepoint = c;
		*ptr = p + 4;
		return true;
	}

	return false;
}

static bool is_valid_utf8_text(const char *text)
{
	const unsigned char *p = (const unsigned char *)text;

	if (!p)
		return false;

	while (*p) {
		unsigned int codepoint;
		const unsigned char *next = p;
		if (!utf8_decode_codepoint(&next, &codepoint))
			return false;
		p = next;
	}

	return true;
}

static int score_utf8_string(const char *text)
{
	int score = 0;
	const unsigned char *p = (const unsigned char *)text;

	while (*p) {
		unsigned int c = 0;
		const unsigned char *next = p;
		if (!utf8_decode_codepoint(&next, &c)) {
			score -= 20;
			p++;
			continue;
		}
		p = next;

		if (c < 0x20 && c != '\t' && c != '\r' && c != '\n') {
			score -= 20;
		} else if (c == 0xFFFD) {
			score -= 20;
		} else if (c >= 0x3040 && c <= 0x30FF) {
			score += 7;
		} else if ((c >= 0x3400 && c <= 0x4DBF)
				|| (c >= 0x4E00 && c <= 0x9FFF)) {
			score += 3;
		} else if ((c >= 0x3001 && c <= 0x303F)
				|| (c >= 0xFF01 && c <= 0xFF60)
				|| c == 0x2014
				|| c == 0x2018
				|| c == 0x2019
				|| c == 0x201C
				|| c == 0x201D) {
			score += 2;
		} else if ((c >= 0xFF10 && c <= 0xFF19)
				|| (c >= 0xFF21 && c <= 0xFF3A)
				|| (c >= 0xFF41 && c <= 0xFF5A)) {
			score += 2;
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

static char *detect_best_game_text_to_utf8(const char *text,
		enum game_text_encoding *best_encoding, int scores[GAME_TEXT_ENCODING_NR])
{
	char *best = NULL;
	int best_score = INT_MIN;
	size_t nr_candidates = sizeof(game_text_candidate_encodings) / sizeof(game_text_candidate_encodings[0]);

	if (scores) {
		for (int i = 0; i < GAME_TEXT_ENCODING_NR; i++)
			scores[i] = INT_MIN;
	}
	if (best_encoding)
		*best_encoding = GAME_TEXT_ENCODING_AUTO;

	for (size_t i = 0; i < nr_candidates; i++) {
		enum game_text_encoding encoding = game_text_candidate_encodings[i];
		char *candidate = decode_multibyte_to_utf8(text, encoding);
		if (!candidate)
			continue;

		int score = score_utf8_string(candidate);
		if (scores)
			scores[encoding] = score;
		if (!best || score > best_score) {
			free(best);
			best = candidate;
			best_score = score;
			if (best_encoding)
				*best_encoding = encoding;
		} else {
			free(candidate);
		}
	}

	return best;
}

static void update_auto_game_text_detection(const int scores[GAME_TEXT_ENCODING_NR])
{
	int best_score = INT_MIN;
	int runner_up = INT_MIN;
	enum game_text_encoding best_encoding = GAME_TEXT_ENCODING_AUTO;
	size_t nr_candidates = sizeof(game_text_candidate_encodings) / sizeof(game_text_candidate_encodings[0]);
	bool saw_valid_score = false;

	if (game_text_encoding_forced || game_text_encoding_locked)
		return;

	for (size_t i = 0; i < nr_candidates; i++) {
		enum game_text_encoding encoding = game_text_candidate_encodings[i];
		if (scores[encoding] == INT_MIN)
			continue;
		saw_valid_score = true;
		game_text_auto_scores[encoding] += scores[encoding];
	}
	if (!saw_valid_score)
		return;

	game_text_auto_samples++;
	for (size_t i = 0; i < nr_candidates; i++) {
		enum game_text_encoding encoding = game_text_candidate_encodings[i];
		int score = game_text_auto_scores[encoding];
		if (score > best_score) {
			runner_up = best_score;
			best_score = score;
			best_encoding = encoding;
		} else if (score > runner_up) {
			runner_up = score;
		}
	}

	if (best_encoding == GAME_TEXT_ENCODING_AUTO || best_score <= 0)
		return;
	if (game_text_auto_samples >= 6
			|| (runner_up != INT_MIN && best_score - runner_up >= 40)) {
		lock_game_text_encoding(best_encoding, "AIN text sampling");
	}
}

/* Conversion function passed to ain_open_conv(): converts game-native encoding -> UTF-8. */
static char *game_text_to_utf8(const char *str)
{
	int scores[GAME_TEXT_ENCODING_NR];
	enum game_text_encoding detected_encoding = GAME_TEXT_ENCODING_AUTO;
	char *utf8;
	char *preferred = NULL;
	int preferred_score = INT_MIN;
	bool is_code_section = xsystem4_ain_conv_code_section == XSYSTEM4_AIN_CONV_SECTION_CODE;
	bool is_message_section = xsystem4_ain_conv_code_section == XSYSTEM4_AIN_CONV_SECTION_MESSAGE;

	if (!str)
		return NULL;
	if (is_ascii_text(str))
		return strdup(str);

	/* Code sections (FUNC, STRT, GLOB, etc.) always use CP932.
	 * Use libsys4's built-in sjis2utf() instead of SDL_iconv
	 * because HarmonyOS musl iconv may not support CP932/SJIS. */
	if (is_code_section) {
		char *converted = sjis2utf(str, 0);
		trace_text_conversion_sample("ain-code", str, GAME_TEXT_ENCODING_CP932, converted);
		return converted;
	}
	/*
	 * Match Windows behavior more closely: once the game has an explicit or
	 * inferred non-auto text encoding, do not let a byte sequence that merely
	 * happens to be valid UTF-8 bypass the intended decoder.
	 */
	if (!game_text_encoding_forced
			&& game_str_encoding == GAME_TEXT_ENCODING_AUTO
			&& is_valid_utf8_text(str))
		return strdup(str);

	if (game_str_encoding != GAME_TEXT_ENCODING_AUTO) {
		preferred = decode_multibyte_to_utf8(str, game_str_encoding);
		if (preferred)
			preferred_score = score_utf8_string(preferred);
		if (game_text_encoding_forced && is_message_section) {
			if (preferred) {
				trace_text_conversion_sample("ain-forced", str, game_str_encoding, preferred);
				return preferred;
			}
			warn_raw_text_passthrough("ain-forced", str, game_str_encoding);
			return strdup(str);
		}
	}

	utf8 = detect_best_game_text_to_utf8(str, &detected_encoding, scores);
	if (!utf8) {
		if (preferred) {
			trace_text_conversion_sample("ain-preferred-fallback", str, game_str_encoding, preferred);
			return preferred;
		}
		warn_raw_text_passthrough("ain-detected", str, detected_encoding);
		return strdup(str);
	}

	if (preferred) {
		int detected_score = scores[detected_encoding];
		if (detected_encoding == game_str_encoding
				|| (preferred_score >= 0 && detected_score < preferred_score + 8)) {
			free(utf8);
			trace_text_conversion_sample(game_text_encoding_forced ? "ain-forced-preferred" : "ain-preferred",
					str,
					game_str_encoding,
					preferred);
			return preferred;
		}
		free(preferred);
	}

	update_auto_game_text_detection(scores);
	trace_text_conversion_sample("ain-detected", str, detected_encoding, utf8);
	return utf8;
}

static char *resource_text_to_utf8(const char *text)
{
	enum game_text_encoding preferred_encoding =
		game_text_encoding_forced ? GAME_TEXT_ENCODING_CP932 : game_str_encoding;
	int scores[GAME_TEXT_ENCODING_NR];
	enum game_text_encoding detected_encoding = GAME_TEXT_ENCODING_AUTO;
	char *best;
	char *preferred = NULL;
	int preferred_score = INT_MIN;

	if (!text)
		return NULL;
	if (is_ascii_text(text))
		return strdup(text);
	if (preferred_encoding == GAME_TEXT_ENCODING_AUTO && is_valid_utf8_text(text))
		return strdup(text);

	if (preferred_encoding != GAME_TEXT_ENCODING_AUTO) {
		preferred = decode_multibyte_to_utf8(text, preferred_encoding);
		if (preferred)
			preferred_score = score_utf8_string(preferred);
	}

	best = detect_best_game_text_to_utf8(text, &detected_encoding, scores);
	if (!best) {
		if (preferred) {
			trace_text_conversion_sample("resource-preferred-fallback", text, preferred_encoding, preferred);
			return preferred;
		}
		warn_raw_text_passthrough("resource", text, preferred_encoding);
		return strdup(text);
	}

	if (preferred) {
		int detected_score = scores[detected_encoding];
		if (detected_encoding == preferred_encoding
				|| (preferred_score >= 0 && detected_score < preferred_score + 8)) {
			free(best);
			trace_text_conversion_sample("resource-preferred", text, preferred_encoding, preferred);
			return preferred;
		}
		free(preferred);
	}

	trace_text_conversion_sample("resource-detected", text, detected_encoding, best);
	return best;
}

static char *normalize_ini_game_text(const char *text)
{
	if (!text)
		return NULL;
	if (is_ascii_text(text))
		return strdup(text);
	if (!game_text_encoding_forced
			&& game_str_encoding == GAME_TEXT_ENCODING_AUTO
			&& is_valid_utf8_text(text))
		return strdup(text);

	if (game_str_encoding != GAME_TEXT_ENCODING_AUTO) {
		char *converted = decode_multibyte_to_utf8(text, game_str_encoding);
		if (converted) {
			trace_text_conversion_sample("ini-preferred", text, game_str_encoding, converted);
			return converted;
		}
		warn_raw_text_passthrough("ini", text, game_str_encoding);
	}

	char *best = detect_best_game_text_to_utf8(text, NULL, NULL);
	if (best)
		trace_text_conversion_sample("ini-detected", text, game_str_encoding, best);
	return best ? best : strdup(text);
}

static char *utf8_to_game_text(const char *text)
{
	return text ? strdup(text) : NULL;
}

char *xsystem4_utf8_to_ain_text(const char *text)
{
	return text ? strdup(text) : NULL;
}

char *xsystem4_game_text_to_utf8(const char *text)
{
	return game_text_to_utf8(text);
}

char *xsystem4_resource_text_to_utf8(const char *text)
{
	return resource_text_to_utf8(text);
}

char *xsystem4_resource_lookup_alias(const char *text)
{
	char *encoded;
	char *alias;

	if (!text || !*text || !game_text_encoding_forced
			|| game_str_encoding == GAME_TEXT_ENCODING_AUTO
			|| game_str_encoding == GAME_TEXT_ENCODING_CP932)
		return NULL;

	encoded = encode_utf8_to_multibyte(text, game_str_encoding);
	if (!encoded)
		return NULL;
	alias = decode_multibyte_to_utf8(encoded, GAME_TEXT_ENCODING_CP932);
	free(encoded);
	if (!alias || !strcmp(alias, text)) {
		free(alias);
		return NULL;
	}
	return alias;
}

static char *resolve_ini_ain_filename(const char *game_dir, const char *raw_name)
{
	if (!raw_name)
		return NULL;
	if (game_text_encoding_forced) {
		char *utf8_name = decode_multibyte_to_utf8(raw_name, game_str_encoding);
		if (utf8_name) {
			char *resolved = utf8_to_game_text(utf8_name);
			free(utf8_name);
			return resolved;
		}
	}
	if (is_ascii_text(raw_name))
		return normalize_ini_game_text(raw_name);

	for (size_t i = 0; i < sizeof(game_text_candidate_encodings) / sizeof(game_text_candidate_encodings[0]); i++) {
		enum game_text_encoding encoding = game_text_candidate_encodings[i];
		char *utf8_name = decode_multibyte_to_utf8(raw_name, encoding);
		if (!utf8_name)
			continue;

		char *candidate = path_join(game_dir, utf8_name);
		bool exists = file_exists(candidate);
		free(candidate);
		if (exists) {
			lock_game_text_encoding(encoding, "AIN filename");
			#ifdef __OHOS__
			SDL_Log("[xsystem4_text] AIN filename encoding detected as %s", game_text_encoding_label(encoding));
			#endif
			char *resolved = utf8_to_game_text(utf8_name);
			free(utf8_name);
			return resolved;
		}
		free(utf8_name);
	}

	return normalize_ini_game_text(raw_name);
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

char *xsystem4_utf8_to_ain_text(const char *text)
{
	return utf8_to_game_text(text);
}

char *xsystem4_game_text_to_utf8(const char *text)
{
	return text ? sjis2utf(text, strlen(text)) : NULL;
}

char *xsystem4_resource_text_to_utf8(const char *text)
{
	return xsystem4_game_text_to_utf8(text);
}

char *xsystem4_resource_lookup_alias(const char *text)
{
	(void)text;
	return NULL;
}

static char *resolve_ini_ain_filename(const char *game_dir, const char *raw_name)
{
	(void)game_dir;
	return normalize_ini_game_text(raw_name);
}
#endif

struct string *xsystem4_cstring_to_string(const char *text, size_t len)
{
	if (!text || !len)
		return make_string("", 0);

	char *raw = xmalloc(len + 1);
	memcpy(raw, text, len);
	raw[len] = '\0';

	char *normalized = normalize_ini_game_text(raw);
	free(raw);
	if (!normalized)
		return make_string("", 0);

	struct string *s = make_string(normalized, strlen(normalized));
	free(normalized);
	return s;
}

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
		if (!strcmp(ini[i].name->text, "TextEncoding")) {
			configure_game_text_encoding(ini_string(&ini[i])->text, path);
		}
	}

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
		if (!strcmp(ini[i].name->text, "text-encoding")) {
			configure_game_text_encoding(ini_string(&ini[i])->text, path);
		}
	}

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
#elif defined(XSYSTEM4_HOST_UTF8)
	char *save_dir = xmalloc(strlen(config.home_dir) + 1 + strlen(config.game_name) + 1 + strlen(dir_name) + 1);
	strcpy(save_dir, config.home_dir);
	strcat(save_dir, "/");
	strcat(save_dir, config.game_name);
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
	fprintf(stderr, "[error_handler] %s\n", msg);
	fflush(stderr);
	SDL_ShowSimpleMessageBox(SDL_MESSAGEBOX_ERROR, "xsystem4", msg, NULL);
}

#ifdef _WIN32
static LONG WINAPI crash_handler(EXCEPTION_POINTERS *ep)
{
	DWORD code = ep->ExceptionRecord->ExceptionCode;
	PVOID addr = ep->ExceptionRecord->ExceptionAddress;
	fprintf(stderr, "\n*** CRASH: ExceptionCode=0x%08lX at address %p ***\n", code, addr);

	/* Print the name of the module containing the crash address */
	HMODULE hMods[512];
	HANDLE hProc = GetCurrentProcess();
	DWORD cbNeeded;
	if (EnumProcessModules(hProc, hMods, sizeof(hMods), &cbNeeded)) {
		DWORD nMods = cbNeeded / sizeof(HMODULE);
		for (DWORD i = 0; i < nMods; i++) {
			MODULEINFO mi;
			if (GetModuleInformation(hProc, hMods[i], &mi, sizeof(mi))) {
				LPCVOID base = mi.lpBaseOfDll;
				LPCVOID end  = (const char *)mi.lpBaseOfDll + mi.SizeOfImage;
				if (addr >= base && addr < end) {
					char name[MAX_PATH] = "<unknown>";
					GetModuleFileNameA(hMods[i], name, MAX_PATH);
					fprintf(stderr, "  in module: %s (base=%p)\n", name, base);
					break;
				}
			}
		}
	}

	/* Walk the call stack using StackWalk64 */
	CONTEXT ctx = *ep->ContextRecord;
	STACKFRAME64 sf;
	memset(&sf, 0, sizeof(sf));
#ifdef _M_X64
	sf.AddrPC.Offset    = ctx.Rip;
	sf.AddrStack.Offset = ctx.Rsp;
	sf.AddrFrame.Offset = ctx.Rbp;
#else
	sf.AddrPC.Offset    = ctx.Eip;
	sf.AddrStack.Offset = ctx.Esp;
	sf.AddrFrame.Offset = ctx.Ebp;
#endif
	sf.AddrPC.Mode    = AddrModeFlat;
	sf.AddrStack.Mode = AddrModeFlat;
	sf.AddrFrame.Mode = AddrModeFlat;
	SymInitialize(hProc, NULL, TRUE);
	fprintf(stderr, "  Stack trace:\n");
	for (int frame = 0; frame < 20; frame++) {
		if (!StackWalk64(IMAGE_FILE_MACHINE_AMD64, hProc, GetCurrentThread(),
		                 &sf, &ctx, NULL, SymFunctionTableAccess64,
		                 SymGetModuleBase64, NULL))
			break;
		if (sf.AddrPC.Offset == 0)
			break;
		char buf[sizeof(SYMBOL_INFO) + MAX_SYM_NAME * sizeof(TCHAR)];
		PSYMBOL_INFO sym = (PSYMBOL_INFO)buf;
		sym->SizeOfStruct = sizeof(SYMBOL_INFO);
		sym->MaxNameLen   = MAX_SYM_NAME;
		DWORD64 disp = 0;
		if (SymFromAddr(hProc, sf.AddrPC.Offset, &disp, sym))
			fprintf(stderr, "    #%d 0x%016llX  %s+0x%llX\n", frame, sf.AddrPC.Offset, sym->Name, disp);
		else
			fprintf(stderr, "    #%d 0x%016llX  ???\n", frame, sf.AddrPC.Offset);
	}

	fflush(stderr);
	return EXCEPTION_EXECUTE_HANDLER;
}
#endif

int main(int argc, char *argv[])
{
	if (!sys_error_handler)
		sys_error_handler = error_handler;

#ifdef _WIN32
	SetUnhandledExceptionFilter(crash_handler);
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

	apply_env_game_text_encoding_override();

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

#if defined(_WIN32) || defined(XSYSTEM4_HOST_UTF8)
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
	return vm_execute_ain(ain);
}
