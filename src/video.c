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
void vdi_stream_client__video_init(struct parsec_context_s *parsec_context) {
	const char *no_vsync = getenv("VDI_STREAM_CLIENT_NO_VSYNC");
	int vsync = (no_vsync != NULL && no_vsync[0] != '\0' && strcmp(no_vsync, "0") != 0) ? 0 : 1;

	if (!SDL_SetRenderVSync(parsec_context->renderer, vsync)) {
		vdi_stream_client__log_error("SDL_SetRenderVSync failed: %s\n", SDL_GetError());
	}
	if (!vsync) {
		vdi_stream_client__log_info("Disable SDL renderer vsync because VDI_STREAM_CLIENT_NO_VSYNC is set\n");
	}
	if (vdi_stream_client__video_env_int("VDI_STREAM_CLIENT_FRAME_DRAIN", 1, 0, 8) > 0) {
		vdi_stream_client__log_info("Use video frame drain limit %d for lower visual latency\n",
			vdi_stream_client__video_env_int("VDI_STREAM_CLIENT_FRAME_DRAIN", 1, 0, 8));
	}
}

/* render a single frame on the main thread. */
bool vdi_stream_client__video_render(struct parsec_context_s *parsec_context, bool force_redraw) {

	/* show parsec frame. */
	if (parsec_context->connection) {
		if (!vdi_stream_client__frame_video(parsec_context, force_redraw)) {
			return false;
		}
		if (!SDL_RenderPresent(parsec_context->renderer)) {
			vdi_stream_client__log_error("SDL_RenderPresent failed: %s\n", SDL_GetError());
		} else if (parsec_context->stats_enabled) {
			parsec_context->stats_presents++;
		}
		return true;
	}

	/* show reconnecting/shutdown text if available. */
	if (parsec_context->surface_ttf != NULL &&
	    (force_redraw || SDL_GetTicks() >= parsec_context->next_overlay_tick)) {
		vdi_stream_client__frame_text(parsec_context);
		if (!SDL_RenderPresent(parsec_context->renderer)) {
			vdi_stream_client__log_error("SDL_RenderPresent failed: %s\n", SDL_GetError());
		} else if (parsec_context->stats_enabled) {
			parsec_context->stats_presents++;
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

	SDL_DestroyRenderer(parsec_context->renderer);
	parsec_context->renderer = NULL;
}
