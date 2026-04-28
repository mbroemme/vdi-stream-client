/*
 *  video.c -- video rendering thread via sdl
 *
 *  Copyright (c) 2020-2021 Maik Broemme <mbroemme@libmpq.org>
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
	return true;
}

static void vdi_stream_client__frame_video_update(const ParsecFrame *frame, const void *image, void *opaque) {
	struct parsec_context_s *parsec_context = (struct parsec_context_s *) opaque;
	const Uint8 *pixels = (const Uint8 *) image;

	if (!vdi_stream_client__video_texture(parsec_context, frame)) {
		return;
	}

	switch (frame->format) {
		case FORMAT_NV12:
			SDL_UpdateNVTexture(parsec_context->texture_video, NULL,
				pixels, frame->fullWidth,
				pixels + frame->fullWidth * frame->fullHeight, frame->fullWidth);
			break;
		case FORMAT_I420:
			SDL_UpdateYUVTexture(parsec_context->texture_video, NULL,
				pixels, frame->fullWidth,
				pixels + frame->fullWidth * frame->fullHeight, frame->fullWidth / 2,
				pixels + frame->fullWidth * frame->fullHeight + (frame->fullWidth / 2) * (frame->fullHeight / 2), frame->fullWidth / 2);
			break;
		case FORMAT_BGRA:
		case FORMAT_RGBA:
			SDL_UpdateTexture(parsec_context->texture_video, NULL, pixels, frame->fullWidth * 4);
			break;
		default:
			break;
	}
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
static void vdi_stream_client__frame_video(void *opaque) {
	struct parsec_context_s *parsec_context = (struct parsec_context_s *) opaque;
	SDL_FRect src;

	ParsecClientSetDimensions(parsec_context->parsec, DEFAULT_STREAM, parsec_context->window_width, parsec_context->window_height, 1);
	ParsecClientPollFrame(parsec_context->parsec, DEFAULT_STREAM, vdi_stream_client__frame_video_update, parsec_context->render_timeout, parsec_context);

	SDL_SetRenderDrawColor(parsec_context->renderer, 0x00, 0x00, 0x00, 0xFF);
	SDL_RenderClear(parsec_context->renderer);

	if (parsec_context->texture_video == NULL) {
		return;
	}

	src.x = 0.0f;
	src.y = 0.0f;
	src.w = parsec_context->window_width;
	src.h = parsec_context->window_height;
	SDL_RenderTexture(parsec_context->renderer, parsec_context->texture_video, &src, NULL);
}

/* initialize video rendering on the main thread. */
void vdi_stream_client__video_init(struct parsec_context_s *parsec_context) {
	if (!SDL_SetRenderVSync(parsec_context->renderer, 1)) {
		vdi_stream_client__log_error("SDL_SetRenderVSync failed: %s\n", SDL_GetError());
	}
}

/* render a single frame on the main thread. */
void vdi_stream_client__video_render(struct parsec_context_s *parsec_context, bool force_redraw) {

	/* show parsec frame. */
	if (parsec_context->connection) {
		vdi_stream_client__frame_video(parsec_context);
		if (!SDL_RenderPresent(parsec_context->renderer)) {
			vdi_stream_client__log_error("SDL_RenderPresent failed: %s\n", SDL_GetError());
		}
		return;
	}

	/* show reconnecting/shutdown text if available. */
	if (parsec_context->surface_ttf != NULL &&
	    (force_redraw || SDL_GetTicks() >= parsec_context->next_overlay_tick)) {
		vdi_stream_client__frame_text(parsec_context);
		if (!SDL_RenderPresent(parsec_context->renderer)) {
			vdi_stream_client__log_error("SDL_RenderPresent failed: %s\n", SDL_GetError());
		}
		parsec_context->next_overlay_tick = SDL_GetTicks() + parsec_context->timeout;
	}
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
