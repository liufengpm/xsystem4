#define SDL_MAIN_HANDLED

#include <SDL.h>
#include <setjmp.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef __OHOS__
/* SDL internal storage path (filesDir); declared in SDL_ohosfile.h but not in
 * the public SDL headers, so forward-declare it here. */
extern const char *SDL_OHOSGetInternalStoragePath(void);
#endif

#include "audio.h"
#include "system4.h"
#include "xsystem4.h"

#ifdef __cplusplus
#define PLUGIN_EXTERN extern "C"
#else
#define PLUGIN_EXTERN
#endif

#ifdef _WIN32
#define PLUGIN_EXPORT PLUGIN_EXTERN __declspec(dllexport)
#else
#define PLUGIN_EXPORT PLUGIN_EXTERN __attribute__((visibility("default")))
#endif

extern int main(int argc, char *argv[]);

static volatile int g_xsystem4_shutdown_requested = 0;
static jmp_buf g_xsystem4_exit_jmp;
static int g_xsystem4_exit_jmp_valid = 0;
static int g_xsystem4_exit_code = 0;
static char g_xsystem4_last_error[4096] = "";

static void xsystem4_plugin_set_error(const char *msg)
{
	if (!msg)
		msg = "unknown xsystem4 error";
	strncpy(g_xsystem4_last_error, msg, sizeof(g_xsystem4_last_error) - 1);
	g_xsystem4_last_error[sizeof(g_xsystem4_last_error) - 1] = '\0';
	SDL_LogError(SDL_LOG_CATEGORY_APPLICATION, "[xsystem4_plugin] %s", g_xsystem4_last_error);
}

static void xsystem4_plugin_error_handler(const char *msg)
{
	xsystem4_plugin_set_error(msg);
}

static void xsystem4_plugin_exit_handler(int code)
{
	g_xsystem4_exit_code = code;
	if (g_xsystem4_exit_jmp_valid)
		longjmp(g_xsystem4_exit_jmp, 1);
	exit(code);
}

static void xsystem4_plugin_configure_paths(int argc, char **argv)
{
#ifdef __OHOS__
	(void)argc;
	(void)argv;
	/* XSYSTEM4_DATA_DIR: relative prefix used by SDL rawfile I/O on OHOS.
	 * SDL_RWFromFile("xsystem4/shaders/foo.glsl") → rawfile/xsystem4/shaders/foo.glsl. */
	setenv("XSYSTEM4_DATA_DIR", XSYS4_DATA_DIR, 1);
	SDL_Log("[xsystem4_plugin] Set XSYSTEM4_DATA_DIR=%s", XSYS4_DATA_DIR);

	/* XSYSTEM4_HOME: writable sandbox root for save data and user config.
	 * SDL_OHOSGetInternalStoragePath() returns the app filesDir (e.g.
	 * /data/storage/el2/base) which is always writable. Without this the
	 * fallback is realpath(".") = game dir, which works but mixes runtime
	 * assets with save data. */
	const char *internal_path = SDL_OHOSGetInternalStoragePath();
	if (internal_path && *internal_path) {
		setenv("XSYSTEM4_HOME", internal_path, 0); /* 0 = don't override if already set */
		SDL_Log("[xsystem4_plugin] Set XSYSTEM4_HOME=%s", internal_path);
	}
	return;
#else
	if (argc < 2 || !argv[1] || !argv[1][0])
		return;

	char data_dir[4096];
	snprintf(data_dir, sizeof(data_dir), "%s/xsystem4", argv[1]);
	setenv("XSYSTEM4_DATA_DIR", data_dir, 1);
	SDL_Log("[xsystem4_plugin] Set XSYSTEM4_DATA_DIR=%s", data_dir);
#endif
}

PLUGIN_EXPORT
int runner_main(int argc, char **argv)
{
	SDL_Log("[xsystem4_plugin] runner_main called, argc=%d", argc);
	for (int i = 0; i < argc; i++) {
		SDL_Log("[xsystem4_plugin]   argv[%d] = %s", i, argv[i] ? argv[i] : "(null)");
	}

	g_xsystem4_shutdown_requested = 0;
	g_xsystem4_exit_code = 0;
	g_xsystem4_last_error[0] = '\0';

	sys_error_handler = xsystem4_plugin_error_handler;
	sys_exit_handler = xsystem4_plugin_exit_handler;
	xsystem4_plugin_configure_paths(argc, argv);

	/* On OHOS, keep the GL/EGL context alive across game-session restarts.
	 * system.Exit(0) in AliceSoft AIN means "chapter finished, restart AIN
	 * to read updated globals and continue the next chapter" – it is NOT a
	 * real application exit.  gfx_init() already guards with gfx_initialized,
	 * sact_init() guards with engine_type, and sprite_init_sact() guards with
	 * sprite_shader.s.prepare, so re-entering main() skips all GL/SDL/shader
	 * work and only re-parses the AIN script + resets the VM heap.  This lets
	 * the game loop indefinitely without ever tearing down the EGL surface,
	 * which avoids the repeated shader-compilation cost (and BiSheng driver
	 * instability) on every chapter transition. */
	int result;
	do {
		if (setjmp(g_xsystem4_exit_jmp) == 0) {
			g_xsystem4_exit_jmp_valid = 1;
			result = main(argc, argv);
		} else {
			result = g_xsystem4_exit_code;
		}
		if (result != 0 || g_xsystem4_shutdown_requested)
			break;
		/* result == 0: game called system.Exit(0) for a chapter transition.
		 * Loop back and re-run main(); GL stays alive. */
		SDL_Log("[xsystem4_plugin] SYS_EXIT(0): restarting game session "
		        "with preserved GL context (chapter transition)");
	} while (true);
	audio_shutdown();
	g_xsystem4_exit_jmp_valid = 0;
	sys_exit_handler = NULL;

	SDL_Log("[xsystem4_plugin] runner_main returning %d", result);
	return result;
}

PLUGIN_EXPORT
void requestShutdown(void)
{
	SDL_Event event;
	SDL_zero(event);
	event.type = SDL_QUIT;
	g_xsystem4_shutdown_requested = 1;
	SDL_PushEvent(&event);
	SDL_Log("[xsystem4_plugin] shutdown requested");
}

PLUGIN_EXPORT
int isShutdownRequested(void)
{
	return g_xsystem4_shutdown_requested;
}

PLUGIN_EXPORT
void cleanupSDL(void)
{
	g_xsystem4_shutdown_requested = 0;
	sys_error_handler = NULL;
	sys_exit_handler = NULL;
	audio_shutdown();
	SDL_Quit();
	SDL_Log("[xsystem4_plugin] cleanupSDL done");
}

PLUGIN_EXPORT
const char *get_last_ruby_error(void)
{
	return g_xsystem4_last_error[0] ? g_xsystem4_last_error : NULL;
}