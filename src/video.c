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
#include <unistd.h>

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
			*pixel_format = SDL_PIXELFORMAT_RGBA32;
			return true;
		default:
			return false;
	}
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
			return "unknown";
	}
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
	vdi_stream_client__log_info("Use %s video frame format (%ux%u, full %ux%u)\n",
		vdi_stream_client__video_format_name(frame->format),
		frame->width,
		frame->height,
		frame->fullWidth,
		frame->fullHeight
	);
	return true;
}

static void vdi_stream_client__frame_video_update(const ParsecFrame *frame, const void *image, void *opaque) {
	struct parsec_context_s *parsec_context = (struct parsec_context_s *) opaque;
	const Uint8 *pixels = (const Uint8 *) image;

	if (!vdi_stream_client__video_texture(parsec_context, frame)) {
		return;
	}

	parsec_context->frame_width = frame->width > 0 ? frame->width : frame->fullWidth;
	parsec_context->frame_height = frame->height > 0 ? frame->height : frame->fullHeight;

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

	parsec_context->frame_video_updated = false;
	ParsecClientPollFrame(parsec_context->parsec, DEFAULT_STREAM, vdi_stream_client__frame_video_update, parsec_context->render_timeout, parsec_context);

	if (!force_redraw && !parsec_context->frame_video_updated) {
		return false;
	}

	SDL_SetRenderDrawColor(parsec_context->renderer, 0x00, 0x00, 0x00, 0xFF);
	SDL_RenderClear(parsec_context->renderer);

	if (parsec_context->texture_video == NULL) {
		return force_redraw;
	}

	src.x = 0.0f;
	src.y = 0.0f;
	src.w = parsec_context->frame_width > 0 ? parsec_context->frame_width : parsec_context->window_width;
	src.h = parsec_context->frame_height > 0 ? parsec_context->frame_height : parsec_context->window_height;
	SDL_RenderTexture(parsec_context->renderer, parsec_context->texture_video, &src, NULL);
	return true;
}

/* initialize video rendering on the main thread. */
void vdi_stream_client__video_init(struct parsec_context_s *parsec_context) {
	if (!SDL_SetRenderVSync(parsec_context->renderer, 1)) {
		vdi_stream_client__log_error("SDL_SetRenderVSync failed: %s\n", SDL_GetError());
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

	SDL_DestroyRenderer(parsec_context->renderer);
	parsec_context->renderer = NULL;
}
