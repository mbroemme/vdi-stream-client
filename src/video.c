/*
 *  video.c -- video rendering thread via sdl
 *
 *  Copyright (c) 2020-2026 Maik Broemme <mbroemme@libmpq.org>
 *
 *  This program is free software: you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License as published by
 *  the Free Software Foundation, either version 3 of the License, or
 *  (at your option) any later version.
 *
 *  This program is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *  GNU General Public License for more details.
 *
 *  You should have received a copy of the GNU General Public License
 *  along with this program.  If not, see <http://www.gnu.org/licenses/>.
 *
 *  Additional permission under GNU GPL version 3 section 7 is described in
 *  COPYING.EXCEPTION, allowing this program to link with the Parsec SDK.
 */

/* internal includes. */
#include "client.h"
#include "parsec.h"

/* system includes. */
#include <limits.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>



static int vdi_stream_client__video_env_int(const char *name, int default_value, int min_value, int max_value) {
	const char *value = getenv(name);
	char *endptr = NULL;
	long parsed;

	if (value == NULL || value[0] == '\0') {
		return default_value;
	}
	parsed = strtol(value, &endptr, 10);
	if (endptr == value || *endptr != '\0') {
		return default_value;
	}
	if (parsed < min_value) {
		return min_value;
	}
	if (parsed > max_value) {
		return max_value;
	}
	return (int) parsed;
}

static const char *vdi_stream_client__video_format_name(ParsecColorFormat format) {
	switch (format) {
		case FORMAT_NV12:
			return "NV12";
		case FORMAT_I420:
			return "I420";
		case FORMAT_NV16:
			return "NV16";
		case FORMAT_I422:
			return "I422";
		case FORMAT_BGRA:
			return "BGRA";
		case FORMAT_RGBA:
			return "RGBA";
		case FORMAT_I444:
			return "I444";
		default:
			return "UNKNOWN";
	}
}

static bool vdi_stream_client__video_format(ParsecColorFormat format, SDL_PixelFormat *pixel_format) {
	switch (format) {
		case FORMAT_NV12:
			*pixel_format = SDL_PIXELFORMAT_NV12;
			return true;
		case FORMAT_I420:
			*pixel_format = SDL_PIXELFORMAT_IYUV;
			return true;
		case FORMAT_BGRA:
			*pixel_format = SDL_PIXELFORMAT_BGRA32;
			return true;
		case FORMAT_RGBA:
		case FORMAT_I444:
			/* SDL does not provide a streaming planar I444 texture format, so I444 is
			 * converted to RGBA immediately before SDL_UpdateTexture(). */
			*pixel_format = SDL_PIXELFORMAT_RGBA32;
			return true;
		default:
			return false;
	}
}


bool vdi_stream_client__video_opengl_requested(void) {
#ifdef HAVE_OPENGL_RENDERER
	const char *renderer = getenv("VDI_STREAM_CLIENT_VIDEO_RENDERER");
	const char *disable = getenv("VDI_STREAM_CLIENT_DISABLE_OPENGL");

	if (renderer != NULL && renderer[0] != '\0') {
		return strcmp(renderer, "sdl") != 0 && strcmp(renderer, "SDL") != 0;
	}
	if (disable != NULL && disable[0] != '\0' && strcmp(disable, "0") != 0) {
		return false;
	}
	return true;
#else
	return false;
#endif
}

#ifdef HAVE_OPENGL_RENDERER
static bool vdi_stream_client__video_gl_check(const char *what) {
	GLenum err = glGetError();

	if (err == GL_NO_ERROR) {
		return true;
	}
	vdi_stream_client__log_error("OpenGL %s failed: 0x%04x\n", what, (unsigned int) err);
	return false;
}

static GLuint vdi_stream_client__video_gl_compile_shader(GLenum type, const char *source) {
	GLuint shader;
	GLint ok = GL_FALSE;
	GLchar log[1024];

	shader = glCreateShader(type);
	if (shader == 0) {
		vdi_stream_client__log_error("OpenGL shader creation failed\n");
		return 0;
	}
	glShaderSource(shader, 1, &source, NULL);
	glCompileShader(shader);
	glGetShaderiv(shader, GL_COMPILE_STATUS, &ok);
	if (!ok) {
		log[0] = '\0';
		glGetShaderInfoLog(shader, sizeof(log), NULL, log);
		vdi_stream_client__log_error("OpenGL shader compilation failed: %s\n", log);
		glDeleteShader(shader);
		return 0;
	}
	return shader;
}

static GLuint vdi_stream_client__video_gl_link_program(const char *fragment_source) {
	static const char *vertex_source =
		"#version 120\n"
		"void main() {\n"
		"\tgl_Position = gl_Vertex;\n"
		"\tgl_TexCoord[0] = gl_MultiTexCoord0;\n"
		"}\n";
	GLuint vertex;
	GLuint fragment;
	GLuint program;
	GLint ok = GL_FALSE;
	GLchar log[1024];

	vertex = vdi_stream_client__video_gl_compile_shader(GL_VERTEX_SHADER, vertex_source);
	if (vertex == 0) {
		return 0;
	}
	fragment = vdi_stream_client__video_gl_compile_shader(GL_FRAGMENT_SHADER, fragment_source);
	if (fragment == 0) {
		glDeleteShader(vertex);
		return 0;
	}

	program = glCreateProgram();
	if (program == 0) {
		glDeleteShader(vertex);
		glDeleteShader(fragment);
		return 0;
	}
	glAttachShader(program, vertex);
	glAttachShader(program, fragment);
	glLinkProgram(program);
	glGetProgramiv(program, GL_LINK_STATUS, &ok);
	glDeleteShader(vertex);
	glDeleteShader(fragment);
	if (!ok) {
		log[0] = '\0';
		glGetProgramInfoLog(program, sizeof(log), NULL, log);
		vdi_stream_client__log_error("OpenGL shader link failed: %s\n", log);
		glDeleteProgram(program);
		return 0;
	}
	return program;
}

static bool vdi_stream_client__video_gl_create_programs(struct parsec_context_s *parsec_context) {
	static const char *rgba_fragment =
		"#version 120\n"
		"uniform sampler2D tex0;\n"
		"void main() {\n"
		"\tgl_FragColor = texture2D(tex0, gl_TexCoord[0].st);\n"
		"}\n";
	static const char *yuv_fragment =
		"#version 120\n"
		"uniform sampler2D tex_y;\n"
		"uniform sampler2D tex_u;\n"
		"uniform sampler2D tex_v;\n"
		"uniform int yuv_mode;\n"
		"void main() {\n"
		"\tvec2 tc = gl_TexCoord[0].st;\n"
		"\tfloat y = texture2D(tex_y, tc).r;\n"
		"\tfloat u;\n"
		"\tfloat v;\n"
		"\tif (yuv_mode == 1) {\n"
		"\t\tvec4 uv = texture2D(tex_u, tc);\n"
		"\t\tu = uv.r - 0.5;\n"
		"\t\tv = uv.a - 0.5;\n"
		"\t} else {\n"
		"\t\tu = texture2D(tex_u, tc).r - 0.5;\n"
		"\t\tv = texture2D(tex_v, tc).r - 0.5;\n"
		"\t}\n"
		"\tfloat yy = 1.164383 * (y - 0.0625);\n"
		"\tvec3 rgb;\n"
		"\trgb.r = yy + 1.792741 * v;\n"
		"\trgb.g = yy - 0.213249 * u - 0.532909 * v;\n"
		"\trgb.b = yy + 2.112402 * u;\n"
		"\tgl_FragColor = vec4(clamp(rgb, 0.0, 1.0), 1.0);\n"
		"}\n";

	parsec_context->gl_program_rgba = vdi_stream_client__video_gl_link_program(rgba_fragment);
	parsec_context->gl_program_yuv = vdi_stream_client__video_gl_link_program(yuv_fragment);
	if (parsec_context->gl_program_rgba == 0 || parsec_context->gl_program_yuv == 0) {
		return false;
	}

	glUseProgram(parsec_context->gl_program_rgba);
	glUniform1i(glGetUniformLocation(parsec_context->gl_program_rgba, "tex0"), 0);

	glUseProgram(parsec_context->gl_program_yuv);
	glUniform1i(glGetUniformLocation(parsec_context->gl_program_yuv, "tex_y"), 0);
	glUniform1i(glGetUniformLocation(parsec_context->gl_program_yuv, "tex_u"), 1);
	glUniform1i(glGetUniformLocation(parsec_context->gl_program_yuv, "tex_v"), 2);
	parsec_context->gl_uniform_yuv_mode = glGetUniformLocation(parsec_context->gl_program_yuv, "yuv_mode");
	glUseProgram(0);
	return vdi_stream_client__video_gl_check("program setup");
}

static void vdi_stream_client__video_gl_setup_texture(GLuint texture) {
	glBindTexture(GL_TEXTURE_2D, texture);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
}

static bool vdi_stream_client__video_gl_upload_texture(GLuint texture, bool recreate, GLsizei width, GLsizei height, GLenum internal_format, GLenum format, const Uint8 *pixels) {
	glBindTexture(GL_TEXTURE_2D, texture);
	if (recreate) {
		glTexImage2D(GL_TEXTURE_2D, 0, internal_format, width, height, 0, format, GL_UNSIGNED_BYTE, pixels);
	} else {
		glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, width, height, format, GL_UNSIGNED_BYTE, pixels);
	}
	return vdi_stream_client__video_gl_check("texture upload");
}

static bool vdi_stream_client__video_gl_texture(struct parsec_context_s *parsec_context, const ParsecFrame *frame, const Uint8 *pixels) {
	bool recreate;
	Uint32 y_size;
	Uint32 uv_size;
	GLenum rgba_format;

	if (frame == NULL || pixels == NULL || frame->fullWidth == 0 || frame->fullHeight == 0) {
		return false;
	}

	recreate = parsec_context->texture_width != (Sint32) frame->fullWidth ||
	    parsec_context->texture_height != (Sint32) frame->fullHeight ||
	    parsec_context->format_video != frame->format;
	if (recreate) {
		vdi_stream_client__log_info("Use %s video frame format (%ux%u, full %ux%u) via OpenGL\n",
			vdi_stream_client__video_format_name(frame->format),
			frame->width, frame->height, frame->fullWidth, frame->fullHeight);
		parsec_context->texture_width = frame->fullWidth;
		parsec_context->texture_height = frame->fullHeight;
		parsec_context->format_video = frame->format;
	}

	parsec_context->gl_tex_s = frame->fullWidth != 0 ? (float) frame->width / (float) frame->fullWidth : 1.0f;
	parsec_context->gl_tex_t = frame->fullHeight != 0 ? (float) frame->height / (float) frame->fullHeight : 1.0f;
	glPixelStorei(GL_UNPACK_ALIGNMENT, 1);

	switch (frame->format) {
		case FORMAT_RGBA:
		case FORMAT_BGRA:
			rgba_format = frame->format == FORMAT_BGRA ? GL_BGRA : GL_RGBA;
			return vdi_stream_client__video_gl_upload_texture(parsec_context->gl_textures[0], recreate,
			    (GLsizei) frame->fullWidth, (GLsizei) frame->fullHeight, GL_RGBA, rgba_format, pixels);
		case FORMAT_NV12:
			if ((frame->fullWidth & 1u) != 0 || (frame->fullHeight & 1u) != 0) {
				return false;
			}
			y_size = frame->fullWidth * frame->fullHeight;
			return vdi_stream_client__video_gl_upload_texture(parsec_context->gl_textures[0], recreate,
			    (GLsizei) frame->fullWidth, (GLsizei) frame->fullHeight, GL_LUMINANCE, GL_LUMINANCE, pixels) &&
			    vdi_stream_client__video_gl_upload_texture(parsec_context->gl_textures[1], recreate,
			    (GLsizei) (frame->fullWidth / 2u), (GLsizei) (frame->fullHeight / 2u), GL_LUMINANCE_ALPHA, GL_LUMINANCE_ALPHA, pixels + y_size);
		case FORMAT_I420:
			if ((frame->fullWidth & 1u) != 0 || (frame->fullHeight & 1u) != 0) {
				return false;
			}
			y_size = frame->fullWidth * frame->fullHeight;
			uv_size = (frame->fullWidth / 2u) * (frame->fullHeight / 2u);
			return vdi_stream_client__video_gl_upload_texture(parsec_context->gl_textures[0], recreate,
			    (GLsizei) frame->fullWidth, (GLsizei) frame->fullHeight, GL_LUMINANCE, GL_LUMINANCE, pixels) &&
			    vdi_stream_client__video_gl_upload_texture(parsec_context->gl_textures[1], recreate,
			    (GLsizei) (frame->fullWidth / 2u), (GLsizei) (frame->fullHeight / 2u), GL_LUMINANCE, GL_LUMINANCE, pixels + y_size) &&
			    vdi_stream_client__video_gl_upload_texture(parsec_context->gl_textures[2], recreate,
			    (GLsizei) (frame->fullWidth / 2u), (GLsizei) (frame->fullHeight / 2u), GL_LUMINANCE, GL_LUMINANCE, pixels + y_size + uv_size);
		case FORMAT_I444:
			y_size = frame->fullWidth * frame->fullHeight;
			return vdi_stream_client__video_gl_upload_texture(parsec_context->gl_textures[0], recreate,
			    (GLsizei) frame->fullWidth, (GLsizei) frame->fullHeight, GL_LUMINANCE, GL_LUMINANCE, pixels) &&
			    vdi_stream_client__video_gl_upload_texture(parsec_context->gl_textures[1], recreate,
			    (GLsizei) frame->fullWidth, (GLsizei) frame->fullHeight, GL_LUMINANCE, GL_LUMINANCE, pixels + y_size) &&
			    vdi_stream_client__video_gl_upload_texture(parsec_context->gl_textures[2], recreate,
			    (GLsizei) frame->fullWidth, (GLsizei) frame->fullHeight, GL_LUMINANCE, GL_LUMINANCE, pixels + y_size * 2u);
		default:
			vdi_stream_client__log_error("Unsupported OpenGL video format: %d\n", frame->format);
			return false;
	}
}

static void vdi_stream_client__video_gl_draw(struct parsec_context_s *parsec_context) {
	GLuint program;
	int yuv_mode = 0;

	glViewport(0, 0, parsec_context->window_width, parsec_context->window_height);
	glClearColor(0.0f, 0.0f, 0.0f, 1.0f);
	glClear(GL_COLOR_BUFFER_BIT);

	if (parsec_context->texture_width <= 0 || parsec_context->texture_height <= 0) {
		return;
	}

	if (parsec_context->format_video == FORMAT_RGBA || parsec_context->format_video == FORMAT_BGRA) {
		program = parsec_context->gl_program_rgba;
		glUseProgram(program);
		glActiveTexture(GL_TEXTURE0);
		glBindTexture(GL_TEXTURE_2D, parsec_context->gl_textures[0]);
	} else {
		program = parsec_context->gl_program_yuv;
		if (parsec_context->format_video == FORMAT_NV12) {
			yuv_mode = 1;
		}
		glUseProgram(program);
		if (parsec_context->gl_uniform_yuv_mode >= 0) {
			glUniform1i(parsec_context->gl_uniform_yuv_mode, yuv_mode);
		}
		glActiveTexture(GL_TEXTURE0);
		glBindTexture(GL_TEXTURE_2D, parsec_context->gl_textures[0]);
		glActiveTexture(GL_TEXTURE1);
		glBindTexture(GL_TEXTURE_2D, parsec_context->gl_textures[1]);
		if (parsec_context->format_video != FORMAT_NV12) {
			glActiveTexture(GL_TEXTURE2);
			glBindTexture(GL_TEXTURE_2D, parsec_context->gl_textures[2]);
		}
	}

	glBegin(GL_TRIANGLE_STRIP);
	glTexCoord2f(0.0f, parsec_context->gl_tex_t); glVertex2f(-1.0f, -1.0f);
	glTexCoord2f(parsec_context->gl_tex_s, parsec_context->gl_tex_t); glVertex2f(1.0f, -1.0f);
	glTexCoord2f(0.0f, 0.0f); glVertex2f(-1.0f, 1.0f);
	glTexCoord2f(parsec_context->gl_tex_s, 0.0f); glVertex2f(1.0f, 1.0f);
	glEnd();
	glUseProgram(0);
}

static bool vdi_stream_client__video_gl_init(struct parsec_context_s *parsec_context, int vsync) {
	const GLubyte *renderer;
	const GLubyte *version;
	GLuint texture;

	SDL_GL_SetAttribute(SDL_GL_CONTEXT_MAJOR_VERSION, 2);
	SDL_GL_SetAttribute(SDL_GL_CONTEXT_MINOR_VERSION, 1);
	SDL_GL_SetAttribute(SDL_GL_DOUBLEBUFFER, 1);

	parsec_context->gl_context = SDL_GL_CreateContext(parsec_context->window);
	if (parsec_context->gl_context == NULL) {
		vdi_stream_client__log_error("OpenGL context creation failed: %s\n", SDL_GetError());
		return false;
	}
	if (!SDL_GL_MakeCurrent(parsec_context->window, parsec_context->gl_context)) {
		vdi_stream_client__log_error("OpenGL MakeCurrent failed: %s\n", SDL_GetError());
		return false;
	}
	if (!SDL_GL_SetSwapInterval(vsync)) {
		vdi_stream_client__log_error("SDL_GL_SetSwapInterval failed: %s\n", SDL_GetError());
	}

	renderer = glGetString(GL_RENDERER);
	version = glGetString(GL_VERSION);
	if (renderer != NULL && version != NULL) {
		vdi_stream_client__log_info("Use OpenGL video renderer (%s, %s)\n", (const char *) renderer, (const char *) version);
	} else {
		vdi_stream_client__log_info("Use OpenGL video renderer\n");
	}

	if (!vdi_stream_client__video_gl_create_programs(parsec_context)) {
		return false;
	}
	glGenTextures(3, parsec_context->gl_textures);
	for (texture = 0; texture < 3; texture++) {
		vdi_stream_client__video_gl_setup_texture(parsec_context->gl_textures[texture]);
	}
	glDisable(GL_DEPTH_TEST);
	glDisable(GL_CULL_FACE);
	glClearColor(0.0f, 0.0f, 0.0f, 1.0f);
	parsec_context->gl_tex_s = 1.0f;
	parsec_context->gl_tex_t = 1.0f;
	parsec_context->video_opengl = true;
	return vdi_stream_client__video_gl_check("initialization");
}
#endif

static Uint8 vdi_stream_client__video_clip_u8(Sint32 value) {
	if (value < 0) {
		return 0;
	}
	if (value > 255) {
		return 255;
	}
	return (Uint8) value;
}

static bool vdi_stream_client__video_update_i444_texture(struct parsec_context_s *parsec_context, const ParsecFrame *frame, const Uint8 *pixels) {
	const Uint32 width = frame->fullWidth;
	const Uint32 height = frame->fullHeight;
	Uint32 plane_size;
	const Uint8 *y_plane;
	const Uint8 *u_plane;
	const Uint8 *v_plane;
	Uint8 *rgba;
	Uint32 required;
	Uint32 row;
	Uint32 col;

	if (width == 0 || height == 0 || pixels == NULL) {
		return false;
	}
	if (width > UINT32_MAX / height) {
		vdi_stream_client__log_error("I444 frame is too large for RGBA conversion (%ux%u)\n", width, height);
		return false;
	}
	plane_size = width * height;
	if (plane_size > UINT32_MAX / 4u) {
		vdi_stream_client__log_error("I444 frame is too large for RGBA conversion (%ux%u)\n", width, height);
		return false;
	}
	y_plane = pixels;
	u_plane = pixels + plane_size;
	v_plane = pixels + plane_size * 2u;
	required = plane_size * 4u;
	if (parsec_context->texture_i444_rgba_size < required) {
		Uint8 *tmp = SDL_realloc(parsec_context->texture_i444_rgba, required);
		if (tmp == NULL) {
			vdi_stream_client__log_error("I444 RGBA conversion buffer allocation failed\n");
			return false;
		}
		parsec_context->texture_i444_rgba = tmp;
		parsec_context->texture_i444_rgba_size = required;
	}

	rgba = parsec_context->texture_i444_rgba;
	for (row = 0; row < height; row++) {
		for (col = 0; col < width; col++) {
			const Uint32 i = row * width + col;
			const Sint32 c = (Sint32) y_plane[i] - 16;
			const Sint32 d = (Sint32) u_plane[i] - 128;
			const Sint32 e = (Sint32) v_plane[i] - 128;
			Uint8 *dst = rgba + i * 4u;

			/* BT.709 limited-range YUV to RGB. Parsec desktop streams are normally
			 * tagged as video-range YUV, and this keeps color close to the stock
			 * decoder while preserving full-resolution chroma for text. */
			dst[0] = vdi_stream_client__video_clip_u8((298 * c + 459 * e + 128) >> 8);
			dst[1] = vdi_stream_client__video_clip_u8((298 * c - 55 * d - 136 * e + 128) >> 8);
			dst[2] = vdi_stream_client__video_clip_u8((298 * c + 541 * d + 128) >> 8);
			dst[3] = 255;
		}
	}

	if (!SDL_UpdateTexture(parsec_context->texture_video, NULL, rgba, width * 4u)) {
		vdi_stream_client__log_error("Video texture update failed: %s\n", SDL_GetError());
		return false;
	}
	return true;
}

static bool vdi_stream_client__video_texture(struct parsec_context_s *parsec_context, const ParsecFrame *frame) {
	SDL_PixelFormat pixel_format;

	if (!vdi_stream_client__video_format(frame->format, &pixel_format)) {
		vdi_stream_client__log_error("Unsupported video format: %d\n", frame->format);
		return false;
	}

	if (parsec_context->texture_video != NULL &&
	    parsec_context->texture_width == (Sint32) frame->fullWidth &&
	    parsec_context->texture_height == (Sint32) frame->fullHeight &&
	    parsec_context->format_video == frame->format) {
		return true;
	}

	vdi_stream_client__log_info("Use %s video frame format (%ux%u, full %ux%u)\n",
		vdi_stream_client__video_format_name(frame->format),
		frame->width,
		frame->height,
		frame->fullWidth,
		frame->fullHeight
	);

	SDL_DestroyTexture(parsec_context->texture_video);
	parsec_context->texture_video = SDL_CreateTexture(parsec_context->renderer,
		pixel_format, SDL_TEXTUREACCESS_STREAMING, frame->fullWidth, frame->fullHeight);
	if (parsec_context->texture_video == NULL) {
		vdi_stream_client__log_error("Video texture creation failed: %s\n", SDL_GetError());
		return false;
	}

	parsec_context->texture_width = frame->fullWidth;
	parsec_context->texture_height = frame->fullHeight;
	parsec_context->format_video = frame->format;
	return true;
}

static void vdi_stream_client__frame_video_update(const ParsecFrame *frame, const void *image, void *opaque) {
	struct parsec_context_s *parsec_context = (struct parsec_context_s *) opaque;
	const Uint8 *pixels = (const Uint8 *) image;

	if (parsec_context->texture_video == NULL &&
	    frame->width > 0 && frame->height > 0 &&
	    (parsec_context->window_width != (Sint32) frame->width ||
	     parsec_context->window_height != (Sint32) frame->height)) {
		vdi_stream_client__log_info("Change resolution from %dx%d to %ux%u\n",
			parsec_context->window_width,
			parsec_context->window_height,
			frame->width,
			frame->height
		);
		SDL_SetWindowSize(parsec_context->window, frame->width, frame->height);
		SDL_SyncWindow(parsec_context->window);
		parsec_context->window_width = frame->width;
		parsec_context->window_height = frame->height;
	}

#ifdef HAVE_OPENGL_RENDERER
	if (parsec_context->video_opengl) {
		if (!vdi_stream_client__video_gl_texture(parsec_context, frame, pixels)) {
			return;
		}
		if (parsec_context->stats_enabled) {
			parsec_context->stats_frames++;
			parsec_context->stats_last_frame_tick = SDL_GetTicks();
		}
		parsec_context->frame_video_updated = true;
		return;
	}
#endif

	if (!vdi_stream_client__video_texture(parsec_context, frame)) {
		return;
	}

	switch (frame->format) {
		case FORMAT_NV12:
			if (!SDL_UpdateNVTexture(parsec_context->texture_video, NULL,
			    pixels, frame->fullWidth,
			    pixels + frame->fullWidth * frame->fullHeight, frame->fullWidth)) {
				vdi_stream_client__log_error("Video texture update failed: %s\n", SDL_GetError());
				return;
			}
			break;
		case FORMAT_I420:
			if (!SDL_UpdateYUVTexture(parsec_context->texture_video, NULL,
			    pixels, frame->fullWidth,
			    pixels + frame->fullWidth * frame->fullHeight, frame->fullWidth / 2,
			    pixels + frame->fullWidth * frame->fullHeight + (frame->fullWidth / 2) * (frame->fullHeight / 2), frame->fullWidth / 2)) {
				vdi_stream_client__log_error("Video texture update failed: %s\n", SDL_GetError());
				return;
			}
			break;
		case FORMAT_BGRA:
		case FORMAT_RGBA:
			if (!SDL_UpdateTexture(parsec_context->texture_video, NULL, pixels, frame->fullWidth * 4)) {
				vdi_stream_client__log_error("Video texture update failed: %s\n", SDL_GetError());
				return;
			}
			break;
		case FORMAT_I444:
			if (!vdi_stream_client__video_update_i444_texture(parsec_context, frame, pixels)) {
				return;
			}
			break;
		default:
			return;
	}

	if (parsec_context->stats_enabled) {
		parsec_context->stats_frames++;
		parsec_context->stats_last_frame_tick = SDL_GetTicks();
	}
	parsec_context->frame_video_updated = true;
}

/* sdl frame text event. */
static void vdi_stream_client__frame_text(void *opaque) {
	struct parsec_context_s *parsec_context = (struct parsec_context_s *) opaque;
	SDL_FRect dst;

#ifdef HAVE_OPENGL_RENDERER
	if (parsec_context->video_opengl) {
		glViewport(0, 0, parsec_context->window_width, parsec_context->window_height);
		glClearColor(0.0f, 0.0f, 0.0f, 1.0f);
		glClear(GL_COLOR_BUFFER_BIT);
		return;
	}
#endif

	if (parsec_context->texture_ttf == NULL || parsec_context->surface_ttf == NULL) {
		return;
	}

	/* calculate position and size to center of window. */
	dst.x = (parsec_context->window_width - parsec_context->surface_ttf->w) / 2.0f;
	dst.y = (parsec_context->window_height - parsec_context->surface_ttf->h) / 2.0f;
	dst.w = parsec_context->surface_ttf->w;
	dst.h = parsec_context->surface_ttf->h;

	SDL_SetRenderDrawColor(parsec_context->renderer, 0x00, 0x00, 0x00, 0xFF);
	SDL_RenderClear(parsec_context->renderer);
	SDL_RenderTexture(parsec_context->renderer, parsec_context->texture_ttf, NULL, &dst);
}

/* sdl frame video event. */
static bool vdi_stream_client__frame_video(void *opaque, bool force_redraw) {
	struct parsec_context_s *parsec_context = (struct parsec_context_s *) opaque;
	SDL_FRect src;

	if (parsec_context->requested_width != parsec_context->window_width ||
	    parsec_context->requested_height != parsec_context->window_height) {
		ParsecClientSetDimensions(parsec_context->parsec, DEFAULT_STREAM, parsec_context->window_width, parsec_context->window_height, 1);
		parsec_context->requested_width = parsec_context->window_width;
		parsec_context->requested_height = parsec_context->window_height;
	}

	{
		int drain;
		int i;
		bool got_frame;

		parsec_context->frame_video_updated = false;
		ParsecClientPollFrame(parsec_context->parsec, DEFAULT_STREAM, vdi_stream_client__frame_video_update, parsec_context->render_timeout, parsec_context);
		got_frame = parsec_context->frame_video_updated;

		/* When conversion/rendering is slightly slower than the incoming stream,
		 * presenting every queued frame increases visual latency. Drain a small
		 * number of already queued frames with timeout 0 and present only the newest
		 * texture. This does not wait for future frames and can be disabled with
		 * VDI_STREAM_CLIENT_FRAME_DRAIN=0. */
		drain = vdi_stream_client__video_env_int("VDI_STREAM_CLIENT_FRAME_DRAIN", 1, 0, 8);
		for (i = 0; got_frame && i < drain; i++) {
			parsec_context->frame_video_updated = false;
			ParsecClientPollFrame(parsec_context->parsec, DEFAULT_STREAM, vdi_stream_client__frame_video_update, 0, parsec_context);
			if (!parsec_context->frame_video_updated) {
				break;
			}
		}
		parsec_context->frame_video_updated = got_frame;

		if (!force_redraw && !got_frame) {
			return false;
		}
	}

#ifdef HAVE_OPENGL_RENDERER
	if (parsec_context->video_opengl) {
		vdi_stream_client__video_gl_draw(parsec_context);
		return true;
	}
#endif

	SDL_SetRenderDrawColor(parsec_context->renderer, 0x00, 0x00, 0x00, 0xFF);
	SDL_RenderClear(parsec_context->renderer);

	if (parsec_context->texture_video == NULL) {
		return force_redraw;
	}

	src.x = 0.0f;
	src.y = 0.0f;
	src.w = parsec_context->window_width;
	src.h = parsec_context->window_height;
	SDL_RenderTexture(parsec_context->renderer, parsec_context->texture_video, &src, NULL);
	return true;
}

/* initialize video rendering on the main thread. */
bool vdi_stream_client__video_init(struct parsec_context_s *parsec_context) {
	const char *no_vsync = getenv("VDI_STREAM_CLIENT_NO_VSYNC");
	int vsync = (no_vsync != NULL && no_vsync[0] != '\0' && strcmp(no_vsync, "0") != 0) ? 0 : 1;

#ifdef HAVE_OPENGL_RENDERER
	if (parsec_context->video_opengl_requested) {
		if (!vdi_stream_client__video_gl_init(parsec_context, vsync)) {
			vdi_stream_client__log_error("OpenGL video renderer initialization failed; set VDI_STREAM_CLIENT_VIDEO_RENDERER=sdl to use the SDL renderer\n");
			return false;
		}
		if (!vsync) {
			vdi_stream_client__log_info("Disable OpenGL swap interval because VDI_STREAM_CLIENT_NO_VSYNC is set\n");
		}
	} else
#endif
	{
		parsec_context->renderer = SDL_CreateRenderer(parsec_context->window, NULL);
		if (parsec_context->renderer == NULL) {
			vdi_stream_client__log_error("Renderer creation failed: %s\n", SDL_GetError());
			return false;
		}
		if (!SDL_SetRenderVSync(parsec_context->renderer, vsync)) {
			vdi_stream_client__log_error("SDL_SetRenderVSync failed: %s\n", SDL_GetError());
		}
		if (!vsync) {
			vdi_stream_client__log_info("Disable SDL renderer vsync because VDI_STREAM_CLIENT_NO_VSYNC is set\n");
		}
		vdi_stream_client__log_info("Use SDL video renderer\n");
	}

	if (vdi_stream_client__video_env_int("VDI_STREAM_CLIENT_FRAME_DRAIN", 1, 0, 8) > 0) {
		vdi_stream_client__log_info("Use video frame drain limit %d for lower visual latency\n",
			vdi_stream_client__video_env_int("VDI_STREAM_CLIENT_FRAME_DRAIN", 1, 0, 8));
	}
	return true;
}

/* render a single frame on the main thread. */
bool vdi_stream_client__video_render(struct parsec_context_s *parsec_context, bool force_redraw) {

	/* show parsec frame. */
	if (parsec_context->connection) {
		if (!vdi_stream_client__frame_video(parsec_context, force_redraw)) {
			return false;
		}
#ifdef HAVE_OPENGL_RENDERER
		if (parsec_context->video_opengl) {
			SDL_GL_SwapWindow(parsec_context->window);
			if (parsec_context->stats_enabled) {
				parsec_context->stats_presents++;
			}
		} else
#endif
		{
			if (!SDL_RenderPresent(parsec_context->renderer)) {
				vdi_stream_client__log_error("SDL_RenderPresent failed: %s\n", SDL_GetError());
			} else if (parsec_context->stats_enabled) {
				parsec_context->stats_presents++;
			}
		}
		return true;
	}

	/* show reconnecting/shutdown text if available. */
	if (parsec_context->surface_ttf != NULL &&
	    (force_redraw || SDL_GetTicks() >= parsec_context->next_overlay_tick)) {
		vdi_stream_client__frame_text(parsec_context);
#ifdef HAVE_OPENGL_RENDERER
		if (parsec_context->video_opengl) {
			SDL_GL_SwapWindow(parsec_context->window);
			if (parsec_context->stats_enabled) {
				parsec_context->stats_presents++;
			}
		} else
#endif
		{
			if (!SDL_RenderPresent(parsec_context->renderer)) {
				vdi_stream_client__log_error("SDL_RenderPresent failed: %s\n", SDL_GetError());
			} else if (parsec_context->stats_enabled) {
				parsec_context->stats_presents++;
			}
		}
		parsec_context->next_overlay_tick = SDL_GetTicks() + parsec_context->timeout;
		return true;
	}

	return false;
}

/* release video resources on the main thread. */
void vdi_stream_client__video_destroy(struct parsec_context_s *parsec_context) {
	SDL_DestroyTexture(parsec_context->texture_ttf);
	parsec_context->texture_ttf = NULL;

	SDL_DestroyTexture(parsec_context->texture_video);
	parsec_context->texture_video = NULL;

	SDL_free(parsec_context->texture_i444_rgba);
	parsec_context->texture_i444_rgba = NULL;
	parsec_context->texture_i444_rgba_size = 0;

#ifdef HAVE_OPENGL_RENDERER
	if (parsec_context->gl_textures[0] != 0) {
		glDeleteTextures(3, parsec_context->gl_textures);
		parsec_context->gl_textures[0] = 0;
		parsec_context->gl_textures[1] = 0;
		parsec_context->gl_textures[2] = 0;
	}
	if (parsec_context->gl_program_rgba != 0) {
		glDeleteProgram(parsec_context->gl_program_rgba);
		parsec_context->gl_program_rgba = 0;
	}
	if (parsec_context->gl_program_yuv != 0) {
		glDeleteProgram(parsec_context->gl_program_yuv);
		parsec_context->gl_program_yuv = 0;
	}
	if (parsec_context->gl_context != NULL) {
		SDL_GL_DestroyContext(parsec_context->gl_context);
		parsec_context->gl_context = NULL;
	}
	parsec_context->video_opengl = false;
#endif

	SDL_DestroyRenderer(parsec_context->renderer);
	parsec_context->renderer = NULL;
}
