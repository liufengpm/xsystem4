/* Copyright (C) 2019 Nunuhara Cabbage <nunuhara@haniwa.technology>
 * Copyright (C) 2000- Fumihiko Murata <fmurata@p1.tcnet.ne.jp>
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

#include <SDL.h>
#include "gfx/gl.h"
#include <cglm/cglm.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <limits.h>

#include "system4.h"
#include "system4/cg.h"
#include "system4/file.h"
#include "system4/utfsjis.h"

#include "audio.h"
#include "gfx/gfx.h"
#include "gfx/private.h"
#include "icon.h"
#include "xsystem4.h"

struct sdl_private sdl;

static const GLchar glsl_preamble[] =
#ifdef USE_GLES
	"#version 300 es\n"
	"precision highp float;\n"
	"precision highp int;\n";
#else
	"#version 140\n";
#endif

/*
 * Transform from the window coordinate system -> clip-space.
 *
 * Window CS:  x= 0..w, y= 0..h (+y down)
 * clip-space: x=-1..1, y=-1..1 (+y up)
 *
 * +-                 -+
 * | 2/w   0    0   -1 |
 * |  0   -2/h  0    1 |
 * |  0    0    2   -1 |
 * |  0    0    0    1 |
 * +-                 -+
 */
static mat4 world_view_transform = MAT4(
	0, 0, 0, -1,
	0, 0, 0, -1,
	0, 0, 1, -1,
	0, 0, 0,  1);

static struct shader default_shader;

static GLuint main_surface_fb;
/* Persistent scratch FBOs reused across gfx_set/reset_framebuffer calls.
 * On BiSheng/Maleoon GPUs, creating and deleting an FBO per draw operation
 * triggers UAF crashes because GPU work runs on a background thread.
 * Using persistent FBOs eliminates the entire create/delete cycle. */
static GLuint scratch_draw_fbo;
static GLuint scratch_read_fbo;
static struct texture main_surface;
static struct texture *view = &main_surface;
static GLint max_texture_size;
static SDL_Color clear_color = { 0, 0, 0, 255 };
static float frame_rate;
static bool wait_vsync = false;

#ifdef USE_GLES
#define COLOR_RENDER_TARGET_INTERNAL_FORMAT GL_RGBA8
#else
#define COLOR_RENDER_TARGET_INTERNAL_FORMAT GL_RGBA
#endif

static const char *framebuffer_status_name(GLenum status)
{
	switch (status) {
	case GL_FRAMEBUFFER_COMPLETE:
		return "GL_FRAMEBUFFER_COMPLETE";
	case GL_FRAMEBUFFER_UNDEFINED:
		return "GL_FRAMEBUFFER_UNDEFINED";
	case GL_FRAMEBUFFER_INCOMPLETE_ATTACHMENT:
		return "GL_FRAMEBUFFER_INCOMPLETE_ATTACHMENT";
	case GL_FRAMEBUFFER_INCOMPLETE_MISSING_ATTACHMENT:
		return "GL_FRAMEBUFFER_INCOMPLETE_MISSING_ATTACHMENT";
	case GL_FRAMEBUFFER_UNSUPPORTED:
		return "GL_FRAMEBUFFER_UNSUPPORTED";
	case GL_FRAMEBUFFER_INCOMPLETE_MULTISAMPLE:
		return "GL_FRAMEBUFFER_INCOMPLETE_MULTISAMPLE";
	default:
		return "GL_FRAMEBUFFER_STATUS_UNKNOWN";
	}
}

static void alloc_color_render_target(int w, int h)
{
	glTexImage2D(GL_TEXTURE_2D, 0, COLOR_RENDER_TARGET_INTERNAL_FORMAT, w, h, 0,
			 GL_RGBA, GL_UNSIGNED_BYTE, NULL);
}

static GLchar *read_shader_file(const char *path)
{
	GLchar *source = SDL_LoadFile(path, NULL);
	if (!source) {
		char *full_path = xsystem4_data_path(path);
		source = SDL_LoadFile(full_path, NULL);
		if (!source)
			ERROR("Failed to load shader file %s", full_path, strerror(errno));
		free(full_path);
	}
	return source;
}

static void release_shader_source(GLchar *source)
{
#ifdef __OHOS__
	/* Some Harmony GPU drivers keep compiling on background threads even after
	 * glCompileShader/glLinkProgram return. Keep source buffers alive for the
	 * process lifetime to avoid vendor compiler UAF crashes.
	 */
	(void)source;
#else
	SDL_free(source);
#endif
}

static void sync_shader_compiler(void)
{
#ifdef __OHOS__
	/* Force vendor drivers to complete deferred compiler work before moving on
	 * to the next shader stage or releasing intermediate objects.
	 */
	glFinish();
#endif
}

GLuint gfx_load_shader_file(const char *path, GLenum type, const char *defines)
{
	GLint shader_compiled;
	GLuint shader;
	GLchar *file_source = read_shader_file(path);
#ifdef __OHOS__
	/* Huawei BiSheng GPU compiler (Mate 80 / Kirin X90) crashes in a background
	 * thread when glShaderSource is given multiple source-string arrays: the
	 * driver's internal "version" string parser dereferences a NULL pointer when
	 * iterating over fragmented source chunks.  Work around it by concatenating
	 * all pieces into one allocation before calling glShaderSource.
	 */
	const char *defines_str = defines ? defines : "";
	size_t preamble_len = strlen(glsl_preamble);
	size_t defines_len  = strlen(defines_str);
	size_t file_len     = strlen(file_source);
	GLchar *merged = xmalloc(preamble_len + defines_len + file_len + 1);
	memcpy(merged, glsl_preamble, preamble_len);
	memcpy(merged + preamble_len, defines_str, defines_len);
	memcpy(merged + preamble_len + defines_len, file_source, file_len + 1);
	release_shader_source(file_source);
	const GLchar *source = merged;
	shader = glCreateShader(type);
	glShaderSource(shader, 1, &source, NULL);
#else
	const GLchar *sources[3] = {
		glsl_preamble,
		defines ? defines : "",
		file_source
	};
	shader = glCreateShader(type);
	glShaderSource(shader, 3, sources, NULL);
#endif
	glCompileShader(shader);
	sync_shader_compiler();
	glGetShaderiv(shader, GL_COMPILE_STATUS, &shader_compiled);
	if (!shader_compiled) {
		GLint len;
		glGetShaderiv(shader, GL_INFO_LOG_LENGTH, &len);
		char *infolog = xmalloc(len + 1);
		glGetShaderInfoLog(shader, len, NULL, infolog);
		ERROR("Failed to compile shader %s: %s", path, infolog);
	}
#ifdef __OHOS__
	release_shader_source(merged);
#else
	release_shader_source(file_source);
#endif
	return shader;
}

void gfx_load_shader(struct shader *dst, const char *vertex_shader_path, const char *fragment_shader_path)
{
	GLuint program = glCreateProgram();
	GLuint vertex_shader = gfx_load_shader_file(vertex_shader_path, GL_VERTEX_SHADER, NULL);
	GLuint fragment_shader = gfx_load_shader_file(fragment_shader_path, GL_FRAGMENT_SHADER, NULL);

	glAttachShader(program, vertex_shader);
	glAttachShader(program, fragment_shader);
	glLinkProgram(program);
	sync_shader_compiler();

	GLint link_success;
	glGetProgramiv(program, GL_LINK_STATUS, &link_success);
	if (!link_success)
		ERROR("Failed to link shader: %s, %s", vertex_shader_path, fragment_shader_path);

	glDetachShader(program, vertex_shader);
	glDetachShader(program, fragment_shader);
	glDeleteShader(vertex_shader);
	glDeleteShader(fragment_shader);

	dst->program = program;
	dst->world_transform = glGetUniformLocation(program, "world_transform");
	dst->view_transform = glGetUniformLocation(program, "view_transform");
	dst->texture = glGetUniformLocation(program, "tex");
	dst->vertex_pos = glGetAttribLocation(program, "vertex_pos");
	dst->vertex_uv = glGetAttribLocation(program, "vertex_uv");
	dst->prepare = NULL;
}

static int gl_initialize(void)
{
	gfx_load_shader(&default_shader, "shaders/render.v.glsl", "shaders/render.f.glsl");
	NOTICE("xsystem4 GL vendor: %s", (const char *)glGetString(GL_VENDOR));
	NOTICE("xsystem4 GL renderer: %s", (const char *)glGetString(GL_RENDERER));
	NOTICE("xsystem4 GL version: %s", (const char *)glGetString(GL_VERSION));
	const struct gfx_vertex vertex_data[] = {
		//  x,   y,   z,   w,   u,   v
		{ 0.f, 0.f, 0.f, 1.f, 0.f, 0.f },
		{ 1.f, 0.f, 0.f, 1.f, 1.f, 0.f },
		{ 0.f, 1.f, 0.f, 1.f, 0.f, 1.f },
		{ 1.f, 1.f, 0.f, 1.f, 1.f, 1.f }
	};

	const GLuint rect_index_data[] = { 0, 1, 3, 2 };
	const GLuint line_index_data[] = { 0, 3 };

	glGenVertexArrays(1, &sdl.gl.vao);
	glBindVertexArray(sdl.gl.vao);

	glGenBuffers(1, &sdl.gl.vbo);
	glBindBuffer(GL_ARRAY_BUFFER, sdl.gl.vbo);
	glBufferData(GL_ARRAY_BUFFER, sizeof(vertex_data), vertex_data, GL_STATIC_DRAW);
	glGenBuffers(1, &sdl.gl.quad_vbo);

	glGenBuffers(1, &sdl.gl.rect_ibo);
	glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, sdl.gl.rect_ibo);
	glBufferData(GL_ELEMENT_ARRAY_BUFFER, 4 * sizeof(GLuint), rect_index_data, GL_STATIC_DRAW);

	glGenBuffers(1, &sdl.gl.line_ibo);
	glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, sdl.gl.line_ibo);
	glBufferData(GL_ELEMENT_ARRAY_BUFFER, 2 * sizeof(GLuint), line_index_data, GL_STATIC_DRAW);

	glEnable(GL_BLEND);
	glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);

	glPixelStorei(GL_UNPACK_ALIGNMENT, 1);

	glBindVertexArray(0);

	glGenFramebuffers(1, &scratch_draw_fbo);
	glGenFramebuffers(1, &scratch_read_fbo);

	glGetIntegerv(GL_MAX_TEXTURE_SIZE, &max_texture_size);
	if (max_texture_size <= 0) {
		WARNING("GL_MAX_TEXTURE_SIZE reported %d, falling back to 4096", max_texture_size);
		max_texture_size = 4096;
	}

	return 0;
}

static void set_window_title(void)
{
	char title[1024] = { [1023] = 0 };
	char *game_name = vm_str_to_utf8(config.game_name, 0);
	snprintf(title, 1023, "%s - XSystem4", game_name);
	free(game_name);

	SDL_SetWindowTitle(sdl.window, title);
}

static bool gfx_initialized = false;

/*
 * Reduce window size if the display resolution is smaller than the game
 * resolution.
 */
static void init_window_size(void)
{
#ifndef __ANDROID__
	int display_id = SDL_GetWindowDisplayIndex(sdl.window);
	if (display_id < 0)
		return;

	SDL_Rect bounds;
	if (SDL_GetDisplayUsableBounds(display_id, &bounds) < 0)
		return;

	int w, h;
	SDL_GetWindowSize(sdl.window, &w, &h);
	if (bounds.w > w && bounds.h > h)
		return;

	int border_top, border_bot;
	if (SDL_GetWindowBordersSize(sdl.window, &border_top, NULL, &border_bot, NULL) < 0)
		return;

	int scaled_h = bounds.h - (border_top + border_bot);
	int scaled_w = ((float)scaled_h / h) * w;
	int scaled_x = bounds.w / 2 - scaled_w / 2;

	SDL_SetWindowSize(sdl.window, scaled_w, scaled_h);
	SDL_SetWindowPosition(sdl.window, bounds.x + scaled_x, bounds.y + border_top);
#endif
}

static int get_portrait_top_offset_pct(void)
{
	const char *env = getenv("TAPIR_PORTRAIT_TOP_OFFSET");
	int pct = env ? atoi(env) : 0;
	if (pct < 0)
		return 0;
	if (pct > 100)
		return 100;
	return pct;
}

int gfx_init(void)
{
	if (gfx_initialized)
		return true;

#if defined(USE_GLES) && defined(_WIN32)
	SDL_SetHint(SDL_HINT_OPENGL_ES_DRIVER, "1");
#endif

	uint32_t flags = SDL_INIT_VIDEO | SDL_INIT_AUDIO;
	if (config.joypad)
		flags |= SDL_INIT_GAMECONTROLLER;
	if (SDL_Init(flags) < 0)
		ERROR("SDL_Init failed: %s", SDL_GetError());

#ifdef __OHOS__
	/* The Harmony backend feeds SDL touch events directly. xsystem4 input
	 * semantics are still built around mouse buttons, so enable SDL's
	 * touch-to-mouse synthesis explicitly like the other Harmony engines.
	 */
	SDL_SetHint(SDL_HINT_TOUCH_MOUSE_EVENTS, "1");
	SDL_SetHint(SDL_HINT_MOUSE_TOUCH_EVENTS, "0");
#endif

#ifdef USE_GLES
	NOTICE("xsystem4 renderer backend: OpenGL ES 3.x");
	SDL_GL_SetAttribute(SDL_GL_CONTEXT_MAJOR_VERSION, 3);
	SDL_GL_SetAttribute(SDL_GL_CONTEXT_MINOR_VERSION, 0);
	SDL_GL_SetAttribute(SDL_GL_CONTEXT_PROFILE_MASK, SDL_GL_CONTEXT_PROFILE_ES);
#else
	NOTICE("xsystem4 renderer backend: OpenGL 3.1 core");
	SDL_GL_SetAttribute(SDL_GL_CONTEXT_MAJOR_VERSION, 3);
	SDL_GL_SetAttribute(SDL_GL_CONTEXT_MINOR_VERSION, 1);
	SDL_GL_SetAttribute(SDL_GL_CONTEXT_PROFILE_MASK, SDL_GL_CONTEXT_PROFILE_CORE);
#endif

	sdl.format = SDL_AllocFormat(SDL_PIXELFORMAT_RGBA32);
	int window_w = config.view_width;
	int window_h = config.view_height;
#ifdef __OHOS__
	/* Reuse the fullscreen root XComponent on Harmony instead of creating
	 * a fixed-size child surface at the game's logical resolution.
	 */
	window_w = 1;
	window_h = 1;
#endif
	sdl.window =  SDL_CreateWindow("XSystem4",
				       SDL_WINDOWPOS_UNDEFINED,
				       SDL_WINDOWPOS_UNDEFINED,
				       window_w,
				       window_h,
				       SDL_WINDOW_OPENGL | SDL_WINDOW_SHOWN | SDL_WINDOW_RESIZABLE);
	if (!sdl.window)
		ERROR("SDL_CreateWindow failed: %s", SDL_GetError());

	set_window_title();

#ifdef __OHOS__
	/* SDL_CreateWindow already waits for a native surface, but the root
	 * XComponent can still publish its final size immediately afterwards.
	 * Mirror the extra surface-ready sync used by the stable Harmony engines
	 * before creating the GL context and starting xsystem4's heavy GL init.
	 */
	{
		extern void OHOS_WaitForSurfaceReady(int timeout_ms);
		SDL_Event ev;
		int window_w = 0;
		int window_h = 0;
		OHOS_WaitForSurfaceReady(2000);
		SDL_PumpEvents();
		while (SDL_PeepEvents(&ev, 1, SDL_GETEVENT, SDL_WINDOWEVENT, SDL_WINDOWEVENT) > 0) {
			if (ev.window.event == SDL_WINDOWEVENT_RESIZED ||
			    ev.window.event == SDL_WINDOWEVENT_SIZE_CHANGED) {
				window_w = ev.window.data1;
				window_h = ev.window.data2;
			}
		}
		SDL_GetWindowSize(sdl.window, &window_w, &window_h);
		NOTICE("xsystem4 OHOS window size before GL context: %dx%d", window_w, window_h);
	}
#endif

	sdl.gl.context = SDL_GL_CreateContext(sdl.window);
	if (!sdl.gl.context)
		ERROR("SDL_GL_CreateContext failed: %s", SDL_GetError());
	if (SDL_GL_MakeCurrent(sdl.window, sdl.gl.context) < 0)
		ERROR("SDL_GL_MakeCurrent failed: %s", SDL_GetError());
	SDL_Log("[xsystem4] gfx_init: GL context ready");

#ifndef USE_GLES
	glewExperimental = GL_TRUE;
	GLenum err = glewInit();
	if (err != GLEW_OK && err != GLEW_ERROR_NO_GLX_DISPLAY)
		ERROR("glewInit failed");
#endif

	SDL_GL_SetSwapInterval(wait_vsync ? 1 : 0);
	gl_initialize();
	SDL_Log("[xsystem4] gfx_init: gl_initialize OK (vendor=%s renderer=%s)",
		(const char *)glGetString(GL_VENDOR), (const char *)glGetString(GL_RENDERER));
	gfx_draw_init();
	SDL_Log("[xsystem4] gfx_init: gfx_draw_init OK");
	/* Pre-warm shaders from non-draw.c modules before the first frame.
	 * This avoids late shader compilation that can crash certain GPU compilers
	 * (e.g. BiSheng on HiSilicon Kirin) and improves first-frame latency on
	 * all GLES backends.  sprite_prewarm_shaders() uses a "steal" mechanism
	 * so sprite_init_sact/chipmunk never recompile at game time.  The remaining
	 * module shaders are compiled into static holders that stay alive; drivers
	 * with program caching will reuse them when the real init calls happen. */
	{
		extern void sprite_prewarm_shaders(void);
		sprite_prewarm_shaders();
		/* Pre-warm all 14 TRANS effect shaders.  They are normally lazy-loaded
		 * by effect_init() at scene-transition time; on BiSheng (Maleoon GPU)
		 * that late glLinkProgram call crashes the compiler thread. */
		extern void effect_prewarm_shaders(void);
		effect_prewarm_shaders();

		/* Module shaders compiled here + kept alive to populate driver cache.
		 * NOTE: reign/reign_outline/reign_shadow are excluded because they
		 * require ENGINE/REIGN_ENGINE/TAPIR_ENGINE defines that are only
		 * known at 3d/renderer.c init time. */
		static Shader module_prewarmed[6];
		static const struct { const char *v; const char *f; } pairs[] = {
			{ "shaders/render.v.glsl",         "shaders/movie.f.glsl"          },
			{ "shaders/render.v.glsl",         "shaders/fill_angle.f.glsl"     },
			{ "shaders/parts.v.glsl",          "shaders/parts.f.glsl"          },
			{ "shaders/dungeon.v.glsl",        "shaders/dungeon.f.glsl"        },
			{ "shaders/render.v.glsl",         "shaders/dungeon_raster.f.glsl" },
			{ "shaders/dungeon_skybox.v.glsl", "shaders/dungeon_skybox.f.glsl" },
		};
		for (int i = 0; i < (int)(sizeof(pairs)/sizeof(pairs[0])); i++)
			gfx_load_shader(&module_prewarmed[i], pairs[i].v, pairs[i].f);
	}
	SDL_Log("[xsystem4] gfx_init: shader pre-warm complete");
	gfx_set_window_logical_size(config.view_width, config.view_height);
	init_window_size();
	atexit(gfx_fini);
	gfx_clear();
	icon_init();
	gfx_initialized = true;
	SDL_Log("[xsystem4] gfx_init: complete (%dx%d logical)", sdl.w, sdl.h);
	return 0;
}

void gfx_fini(void)
{
	audio_shutdown();
	glDeleteProgram(default_shader.program);
	if (scratch_draw_fbo)
		glDeleteFramebuffers(1, &scratch_draw_fbo);
	if (scratch_read_fbo)
		glDeleteFramebuffers(1, &scratch_read_fbo);
	SDL_DestroyWindow(sdl.window);
	SDL_FreeFormat(sdl.format);
	SDL_Quit();
}

Texture *gfx_main_surface(void)
{
	return &main_surface;
}

static void main_surface_init(int w, int h)
{
	gfx_delete_texture(&main_surface);
	if (main_surface_fb)
		glDeleteFramebuffers(1, &main_surface_fb);

	glGenTextures(1, &main_surface.handle);
	glBindTexture(GL_TEXTURE_2D, main_surface.handle);
	/* GLES/ANGLE is stricter about FBO color attachments than desktop GL.
	 * Store render targets as RGBA so they remain framebuffer-complete. */
	alloc_color_render_target(w, h);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
	glBindTexture(GL_TEXTURE_2D, 0);

	main_surface.w = w;
	main_surface.h = h;

	glGenFramebuffers(1, &main_surface_fb);
	glBindFramebuffer(GL_FRAMEBUFFER, main_surface_fb);
	glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, main_surface.handle, 0);
	GLenum status = glCheckFramebufferStatus(GL_FRAMEBUFFER);
	if (status != GL_FRAMEBUFFER_COMPLETE)
		ERROR("Main surface framebuffer incomplete: status=%s(0x%04x) size=%dx%d tex=%u err=0x%04x",
			framebuffer_status_name(status), status, w, h, main_surface.handle, glGetError());
}

void gfx_set_window_logical_size(int w, int h)
{
	sdl.w = w;
	sdl.h = h;
	world_view_transform[0][0] = 2.0 / w;
	world_view_transform[1][1] = 2.0 / h;
	gfx_update_screen_scale();
	main_surface_init(w, h);
}

bool gfx_set_fullscreen(bool enable)
{
	uint32_t flags = enable ? SDL_WINDOW_FULLSCREEN_DESKTOP : 0;
	if (SDL_SetWindowFullscreen(sdl.window, flags) < 0) {
		WARNING("Failed to set fullscreen mode: %s", SDL_GetError());
		return false;
	}
	return true;
}

bool gfx_is_fullscreen(void)
{
	return SDL_GetWindowFlags(sdl.window) & SDL_WINDOW_FULLSCREEN_DESKTOP;
}

void gfx_toggle_fullscreen(void)
{
	gfx_set_fullscreen(!gfx_is_fullscreen());
}

void gfx_update_screen_scale(void)
{
	int display_w, display_h;
	SDL_GL_GetDrawableSize(sdl.window, &display_w, &display_h);

	if (display_w * sdl.h == display_h * sdl.w) {
		// The aspect ratios are the same, just scale appropriately.
		sdl.viewport.x = 0;
		sdl.viewport.y = 0;
		sdl.viewport.w = display_w;
		sdl.viewport.h = display_h;
		return;
	}

	double logical_aspect = (double)sdl.w / sdl.h;
	double display_aspect = (double)display_w / display_h;
	if (logical_aspect > display_aspect) {
		// letterbox
		sdl.viewport.w = display_w;
		sdl.viewport.h = sdl.h * display_w / sdl.w;
		sdl.viewport.x = 0;
		int remaining_y = display_h - sdl.viewport.h;
#ifdef __OHOS__
		if (remaining_y > 0 && display_h > display_w)
			sdl.viewport.y = remaining_y * get_portrait_top_offset_pct() / 100;
		else
#endif
			sdl.viewport.y = remaining_y / 2;
	} else {
		// pillarbox (side bars)
		sdl.viewport.w = sdl.w * display_h / sdl.h;
		sdl.viewport.h = display_h;
		sdl.viewport.x = (display_w - sdl.viewport.w) / 2;
		sdl.viewport.y = 0;
	}
}

void gfx_set_wait_vsync(bool wait)
{
	wait_vsync = wait;
	if (gfx_initialized)
		SDL_GL_SetSwapInterval(wait ? 1 : 0);
}

void gfx_set_clear_color(int r, int g, int b, int a)
{
	clear_color.r = max(0, min(255, r));
	clear_color.g = max(0, min(255, g));
	clear_color.b = max(0, min(255, b));
	clear_color.a = max(0, min(255, a));
}

void gfx_clear(void)
{
	glClearColor(clear_color.r / 255.f, clear_color.g / 255.f, clear_color.b / 255.f, clear_color.a / 255.f);
	glClear(GL_COLOR_BUFFER_BIT);
}

static mat4 mw_transform = GLM_MAT4_IDENTITY_INIT;

void gfx_set_view_offset(int x, int y)
{
	glm_mat4_identity(mw_transform);
	if (!x && !y)
		return;
	vec3 off = { (float)x / config.view_width, (float)y / config.view_height, 0 };
	glm_translate(mw_transform, off);
}

float gfx_get_frame_rate(void)
{
	return frame_rate;
}

static void gfx_update_frame_rate_counter(void)
{
	static uint64_t timestamp;
	static int frame_count;

	frame_count++;
	uint64_t current_time = SDL_GetTicks64();
	if (current_time > timestamp + 1000) {
		frame_rate = frame_count * 1000.f / (current_time - timestamp);
		timestamp = current_time;
		frame_count = 0;
	}
}

void gfx_swap(void)
{
	gfx_update_screen_scale();
	int display_w, display_h;
	SDL_GL_GetDrawableSize(sdl.window, &display_w, &display_h);
	glBindFramebuffer(GL_FRAMEBUFFER, 0);
	glViewport(sdl.viewport.x, display_h - sdl.viewport.y - sdl.viewport.h, sdl.viewport.w, sdl.viewport.h);
	gfx_clear();
	/* Present the composed scene as an opaque image. ANGLE/DWM can honor the
	 * default framebuffer alpha, which makes the whole game window appear washed
	 * out if we carry per-pixel sprite alpha into the final swapchain image.
	 */
	glBlendFuncSeparate(GL_ONE, GL_ZERO, GL_ZERO, GL_ONE);

	static mat4 wv_transform = MAT4(
		2,  0, 0, -1,
		0, -2, 0,  1,
		0,  0, 1,  0,
		0,  0, 0,  1);
	struct gfx_render_job job = {
		.shader = &default_shader,
		.shape = GFX_RECTANGLE,
		.texture = view->handle,
		.world_transform = mw_transform[0],
		.view_transform = wv_transform[0],
		.data = view
	};
	gfx_render(&job);
	glBlendFuncSeparate(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA, GL_ONE, GL_ZERO);

	SDL_GL_SwapWindow(sdl.window);
	glBindFramebuffer(GL_FRAMEBUFFER, main_surface_fb);
	glViewport(0, 0, sdl.w, sdl.h);

	gfx_update_frame_rate_counter();
}

void gfx_set_view(struct texture *t)
{
	view = t;
}

void gfx_reset_view(void)
{
	view = &main_surface;
}

/*
 * Set up mandatory shader arguments.
 */
void gfx_prepare_job(struct gfx_render_job *job)
{
	glUseProgram(job->shader->program);

	glUniformMatrix4fv(job->shader->world_transform, 1, GL_FALSE, job->world_transform);
	glUniformMatrix4fv(job->shader->view_transform, 1, GL_FALSE, job->view_transform);

	glActiveTexture(GL_TEXTURE0);
	glBindTexture(GL_TEXTURE_2D, job->texture);
	glUniform1i(job->shader->texture, 0);

	if (job->shader->prepare)
		job->shader->prepare(job, job->data);
}

/*
 * Execute a render job set up previously with gfx_prepare_job.
 */
void gfx_run_job(struct gfx_render_job *job)
{
	glBindVertexArray(sdl.gl.vao);
	glEnableVertexAttribArray(job->shader->vertex_pos);
	glEnableVertexAttribArray(job->shader->vertex_uv);

	glBindBuffer(GL_ARRAY_BUFFER, job->shape == GFX_QUADRILATERAL ? sdl.gl.quad_vbo : sdl.gl.vbo);
	glVertexAttribPointer(job->shader->vertex_pos, 4, GL_FLOAT, GL_FALSE, sizeof(struct gfx_vertex), NULL);
	glVertexAttribPointer(job->shader->vertex_uv, 2, GL_FLOAT, GL_FALSE, sizeof(struct gfx_vertex), (void*)offsetof(struct gfx_vertex, u));

	switch (job->shape) {
	case GFX_RECTANGLE:
	case GFX_QUADRILATERAL:
		glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, sdl.gl.rect_ibo);
		glDrawElements(GL_TRIANGLE_FAN, 4, GL_UNSIGNED_INT, NULL);
		break;
	case GFX_LINE:
		glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, sdl.gl.line_ibo);
		glDrawElements(GL_LINES, 2, GL_UNSIGNED_INT, NULL);
		break;
	}

	glDisableVertexAttribArray(job->shader->vertex_pos);
	glDisableVertexAttribArray(job->shader->vertex_uv);
	glBindVertexArray(0);
	glUseProgram(0);
}

void gfx_render(struct gfx_render_job *job)
{
	if (!job->shader) {
		job->shader = &default_shader;
		gfx_prepare_job(job);
		gfx_run_job(job);
		job->shader = NULL;
	} else {
		gfx_prepare_job(job);
		gfx_run_job(job);
	}
}

void _gfx_render_texture(struct shader *s, struct texture *t, Rectangle *r, void *data)
{
	if (!t->handle) {
		WARNING("Attempted to render uninitialized texture");
		return;
	}

	mat4 world_transform = WORLD_TRANSFORM(t->w, t->h, r ? r->x : 0, r ? r->y : 0);

	struct gfx_render_job job = {
		.shader = s,
		.shape = GFX_RECTANGLE,
		.texture = t->handle,
		.world_transform = world_transform[0],
		.view_transform = world_view_transform[0],
		.data = data
	};
	gfx_render(&job);
}

void gfx_render_texture(struct texture *t, Rectangle *r)
{
	_gfx_render_texture(&default_shader, t, r, t);
}

void gfx_render_quadrilateral(struct texture *t, struct gfx_vertex vertices[4])
{
	if (!t->handle) {
		WARNING("Attempted to render uninitialized texture");
		return;
	}

	// Normalize the texture coordinates to [0, 1]
	struct gfx_vertex buf[4];
	memcpy(buf, vertices, sizeof(struct gfx_vertex) * 4);
	for (int i = 0; i < 4; i++) {
		buf[i].u /= t->w;
		buf[i].v /= t->h;
	}

	glBindBuffer(GL_ARRAY_BUFFER, sdl.gl.quad_vbo);
	glBufferData(GL_ARRAY_BUFFER, sizeof(buf), buf, GL_DYNAMIC_DRAW);

	mat4 world_transform = GLM_MAT4_IDENTITY_INIT;
	struct gfx_render_job job = {
		.shader = &default_shader,
		.shape = GFX_QUADRILATERAL,
		.texture = t->handle,
		.world_transform = world_transform[0],
		.view_transform = world_view_transform[0],
		.data = t
	};
	gfx_render(&job);
}

static void init_texture(struct texture *t, int w, int h)
{
	glGenTextures(1, &t->handle);
	glBindTexture(GL_TEXTURE_2D, t->handle);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);

	t->w = w;
	t->h = h;
}

void gfx_init_texture_with_pixels(struct texture *t, int w, int h, void *pixels)
{
	init_texture(t, w, h);
	glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, w, h, 0, GL_RGBA, GL_UNSIGNED_BYTE, pixels);
}

void gfx_update_texture_with_pixels(struct texture *t, void *pixels)
{
	glBindTexture(GL_TEXTURE_2D, t->handle);
	glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, t->w, t->h, 0, GL_RGBA, GL_UNSIGNED_BYTE, pixels);
}

void gfx_init_texture_with_cg(struct texture *t, struct cg *cg)
{
	gfx_init_texture_with_pixels(t, cg->metrics.w, cg->metrics.h, cg->pixels);
}

void gfx_init_texture_rgba(struct texture *t, int w, int h, SDL_Color color)
{
	if (w > max_texture_size) {
		WARNING("Texture width %d exceeds maximum texture size %d", w, max_texture_size);
		w = max_texture_size;
	}
	if (h > max_texture_size) {
		WARNING("Texture height %d exceeds maximum texture size %d", h, max_texture_size);
		h = max_texture_size;
	}
	gfx_init_texture_blank(t, w, h);
	if (w <= 0 || h <= 0)
		return;
	GLuint fbo = gfx_set_framebuffer(GL_DRAW_FRAMEBUFFER, t, 0, 0, w, h);
	glClearColor(color.r / 255.f, color.g / 255.f, color.b / 255.f, color.a / 255.f);
	glClear(GL_COLOR_BUFFER_BIT);
	gfx_reset_framebuffer(GL_DRAW_FRAMEBUFFER, fbo);
}

void gfx_init_texture_rgb(struct texture *t, int w, int h, SDL_Color color)
{
	if (w > max_texture_size) {
		WARNING("Texture width %d exceeds maximum texture size %d", w, max_texture_size);
		w = max_texture_size;
	}
	if (h > max_texture_size) {
		WARNING("Texture height %d exceeds maximum texture size %d", h, max_texture_size);
		h = max_texture_size;
	}
	init_texture(t, w, h);
	alloc_color_render_target(w, h);
	if (w <= 0 || h <= 0)
		return;
	GLuint fbo = gfx_set_framebuffer(GL_DRAW_FRAMEBUFFER, t, 0, 0, w, h);
	glClearColor(color.r / 255.f, color.g / 255.f, color.b / 255.f, 1.f);
	glClear(GL_COLOR_BUFFER_BIT);
	gfx_reset_framebuffer(GL_DRAW_FRAMEBUFFER, fbo);
}

void gfx_init_texture_amap(struct texture *t, int w, int h, uint8_t *amap, SDL_Color color)
{
	uint32_t *pixels = xmalloc(sizeof(uint32_t) * w * h);
	for (int i = 0; i < w*h; i++) {
		uint32_t c = SDL_MapRGBA(sdl.format, color.r, color.g, color.b, amap[i]);
		pixels[i] = c;
	}

	gfx_init_texture_with_pixels(t, w, h, pixels);
	free(pixels);
}

void gfx_init_texture_rmap(struct texture *t, int w, int h, uint8_t *rmap)
{
	init_texture(t, w, h);
	glTexImage2D(GL_TEXTURE_2D, 0, GL_R8, w, h, 0, GL_RED, GL_UNSIGNED_BYTE, rmap);
}

void gfx_init_texture_blank(struct texture *t, int w, int h)
{
	gfx_init_texture_with_pixels(t, w, h, NULL);
}

void gfx_copy_main_surface(struct texture *dst)
{
	init_texture(dst, main_surface.w, main_surface.h);
	alloc_color_render_target(main_surface.w, main_surface.h);
	#ifdef __OHOS__
	/* Harmony's Maleoon/BiSheng stack is unstable when effect_init snapshots the
	 * current main FBO via glCopyTexSubImage2D. Render-copy the main surface
	 * into the destination texture instead so transitions avoid the driver path
	 * that crashes in libbishenggpucompiler/libmaleoon. */
	gfx_copy_with_alpha_map(dst, 0, 0, &main_surface, 0, 0, main_surface.w, main_surface.h);
	#else
	glCopyTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, 0, 0, main_surface.w, main_surface.h);
	#endif
}

void gfx_delete_texture(struct texture *t)
{
	if (t->handle)
		glDeleteTextures(1, &t->handle);
	t->handle = 0;
}

GLuint gfx_set_framebuffer(GLenum target, Texture *t, int x, int y, int w, int h)
{
	GLuint fbo = (target == GL_READ_FRAMEBUFFER) ? scratch_read_fbo : scratch_draw_fbo;
	glBindFramebuffer(target, fbo);
	glFramebufferTexture2D(target, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, t->handle, 0);
	glViewport(x, y, w, h);

	if (glCheckFramebufferStatus(target) != GL_FRAMEBUFFER_COMPLETE)
		ERROR("Incomplete framebuffer");
	return fbo;
}

void gfx_reset_framebuffer(GLenum target, GLuint fbo)
{
	/* Detach the texture so the persistent scratch FBO keeps no stale
	 * references to textures that may be freed between calls. */
	glFramebufferTexture2D(target, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, 0, 0);
	glBindFramebuffer(target, main_surface_fb);
	/* The scratch FBO is persistent — glDelete is never called on it.
	 * This eliminates the BiSheng/Maleoon GPU UAF crash that occurred when
	 * glDeleteFramebuffers was called before the driver's background thread
	 * finished processing the FBO's deferred render commands. */
	glViewport(0, 0, sdl.w, sdl.h);
	(void)fbo;
}

SDL_Color gfx_get_pixel(Texture *t, int x, int y)
{
	GLuint fbo = gfx_set_framebuffer(GL_READ_FRAMEBUFFER, t, 0, 0, t->w, t->h);

	uint8_t pixel[4];
	glReadPixels(x, y, 1, 1, GL_RGBA, GL_UNSIGNED_BYTE, pixel);

	gfx_reset_framebuffer(GL_READ_FRAMEBUFFER, fbo);

	return (SDL_Color) {
		.r = pixel[0],
		.g = pixel[1],
		.b = pixel[2],
		.a = pixel[3],
	};
}

void *gfx_get_pixels(Texture *t)
{
	GLuint fbo = gfx_set_framebuffer(GL_READ_FRAMEBUFFER, t, 0, 0, t->w, t->h);
	void *pixels = xmalloc(t->w * t->h * 4);
	glReadPixels(0, 0, t->w, t->h, GL_RGBA, GL_UNSIGNED_BYTE, pixels);
	gfx_reset_framebuffer(GL_READ_FRAMEBUFFER, fbo);
	return pixels;
}

int gfx_save_texture(Texture *t, const char *path, enum cg_type format)
{
	void *pixels = gfx_get_pixels(t);
	struct cg cg = {
		.type = ALCG_UNKNOWN,
		.metrics = {
			.w = t->w,
			.h = t->h,
			.bpp = 24,
			.has_pixel = true,
			.has_alpha = true,
			.pixel_pitch = t->w * 3,
			.alpha_pitch = 1
		},
		.pixels = pixels
	};
	FILE *fp = file_open_utf8(path, "wb");
	if (!fp) {
		WARNING("Failed to open %s: %s", display_utf0(path), strerror(errno));
		free(pixels);
		return 0;
	}
	int r = cg_write(&cg, format, fp);
	fclose(fp);
	free(pixels);
	return r;
}
