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

/* configuration includes. */
#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

/* internal includes. */
#include "client.h"
#include "ffmpeg.h"
#include "parsec.h"
#include "placebo.h"

/* system includes. */
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>

static bool
vdi_stream_client__video_render_texture(
    struct parsec_context_s *parsec_context, SDL_Texture *texture, const SDL_FRect *src,
    const SDL_FRect *dst
)
{
    Uint64 render_start_ns = parsec_context->stats_enabled ? SDL_GetTicksNS() : 0;
    bool rendered = SDL_RenderTexture(parsec_context->renderer, texture, src, dst);

    if (parsec_context->stats_enabled) {
        parsec_context->stats_renders++;
        parsec_context->stats_render_ns += SDL_GetTicksNS() - render_start_ns;
    }
    return rendered;
}

static bool
vdi_stream_client__video_present(struct parsec_context_s *parsec_context)
{
    Uint64 present_start_ns = parsec_context->stats_enabled ? SDL_GetTicksNS() : 0;
    bool presented = SDL_RenderPresent(parsec_context->renderer);

    if (parsec_context->stats_enabled) {
        parsec_context->stats_present_calls++;
        parsec_context->stats_present_ns += SDL_GetTicksNS() - present_start_ns;
        if (presented) {
            parsec_context->stats_presents++;
        }
    }
    return presented;
}

static bool
vdi_stream_client__video_format(
    const ParsecFrame *frame, const void *image, SDL_PixelFormat *pixel_format
)
{
    if (vdi_stream_client__parsec_ffmpeg_frame_texture_format(frame, image, pixel_format)) {
        return true;
    }

    switch (frame->format) {
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

static bool
vdi_stream_client__video_texture(
    struct parsec_context_s *parsec_context, const ParsecFrame *frame, const void *image
)
{
    SDL_PixelFormat pixel_format;
    bool format_changed;
    const char *pixel_format_name;

    if (!vdi_stream_client__video_format(frame, image, &pixel_format)) {
        SDL_LogError(SDL_LOG_CATEGORY_APPLICATION, "Unsupported video format: %d\n", frame->format);
        return false;
    }

    if (parsec_context->texture_video != NULL &&
        parsec_context->texture_width == (Sint32)frame->fullWidth &&
        parsec_context->texture_height == (Sint32)frame->fullHeight &&
        parsec_context->pixel_format_video == pixel_format) {
        return true;
    }

    format_changed = parsec_context->pixel_format_video != pixel_format;
    if (parsec_context->frame_video_texture == parsec_context->texture_video) {
        parsec_context->frame_video_texture = NULL;
    }
    SDL_DestroyTexture(parsec_context->texture_video);
    parsec_context->texture_video = SDL_CreateTexture(
        parsec_context->renderer, pixel_format, SDL_TEXTUREACCESS_STREAMING, frame->fullWidth,
        frame->fullHeight
    );
    if (parsec_context->texture_video == NULL) {
        SDL_LogError(
            SDL_LOG_CATEGORY_APPLICATION, "Video texture creation failed: %s\n", SDL_GetError()
        );
        return false;
    }

    parsec_context->texture_width = frame->fullWidth;
    parsec_context->texture_height = frame->fullHeight;
    parsec_context->pixel_format_video = pixel_format;
    if (format_changed) {
        pixel_format_name = SDL_GetPixelFormatName(pixel_format);
        SDL_LogInfo(
            SDL_LOG_CATEGORY_APPLICATION, "Use %s video pixel format\n",
            pixel_format_name != NULL ? pixel_format_name : "unknown"
        );
    }
    return true;
}

static void
vdi_stream_client__frame_video_update(const ParsecFrame *frame, const void *image, void *opaque)
{
    struct parsec_context_s *parsec_context = (struct parsec_context_s *)opaque;
    const Uint8 *pixels = (const Uint8 *)image;
    Uint64 upload_elapsed_ns = 0;
    Uint64 upload_start_ns = 0;
    bool upload_attempted = false;
    bool updated = false;

    if (vdi_stream_client__placebo_render(parsec_context, frame, image)) {
        updated = true;
        goto done;
    }

    if (!vdi_stream_client__video_texture(parsec_context, frame, image)) {
        goto done;
    }

    if (vdi_stream_client__parsec_ffmpeg_frame_is_descriptor(frame, image)) {
        upload_attempted = true;
        updated = vdi_stream_client__parsec_ffmpeg_frame_update(
            parsec_context->texture_video, frame, image,
            parsec_context->stats_enabled ? &upload_elapsed_ns : NULL
        );
        goto done;
    }

    if (parsec_context->stats_enabled) {
        upload_start_ns = SDL_GetTicksNS();
    }
    upload_attempted = true;

    switch (frame->format) {
    case FORMAT_NV12:
        if (!SDL_UpdateNVTexture(
                parsec_context->texture_video, NULL, pixels, frame->fullWidth,
                pixels + frame->fullWidth * frame->fullHeight, frame->fullWidth
            )) {
            SDL_LogError(
                SDL_LOG_CATEGORY_APPLICATION, "Video texture update failed: %s\n", SDL_GetError()
            );
            goto done;
        }
        updated = true;
        break;
    case FORMAT_I420:
        if (!SDL_UpdateYUVTexture(
                parsec_context->texture_video, NULL, pixels, frame->fullWidth,
                pixels + frame->fullWidth * frame->fullHeight, frame->fullWidth / 2,
                pixels + frame->fullWidth * frame->fullHeight +
                    (frame->fullWidth / 2) * (frame->fullHeight / 2),
                frame->fullWidth / 2
            )) {
            SDL_LogError(
                SDL_LOG_CATEGORY_APPLICATION, "Video texture update failed: %s\n", SDL_GetError()
            );
            goto done;
        }
        updated = true;
        break;
    case FORMAT_BGRA:
    case FORMAT_RGBA:
        if (!SDL_UpdateTexture(parsec_context->texture_video, NULL, pixels, frame->fullWidth * 4)) {
            SDL_LogError(
                SDL_LOG_CATEGORY_APPLICATION, "Video texture update failed: %s\n", SDL_GetError()
            );
            goto done;
        }
        updated = true;
        break;
    default:
        goto done;
    }

done:
    if (updated && upload_attempted) {
        parsec_context->frame_video_texture = parsec_context->texture_video;
    }
    if (upload_attempted && parsec_context->stats_enabled) {
        parsec_context->stats_uploads++;
        parsec_context->stats_upload_ns +=
            upload_start_ns != 0 ? SDL_GetTicksNS() - upload_start_ns : upload_elapsed_ns;
    }
    if (updated && parsec_context->stats_enabled) {
        parsec_context->stats_frames++;
        parsec_context->stats_last_frame_tick = SDL_GetTicks();
    }
    if (updated) {
        parsec_context->frame_video_updated = true;
    }
    vdi_stream_client__parsec_ffmpeg_frame_release(frame, image);
}

/* sdl frame text event. */
static void
vdi_stream_client__frame_text(void *opaque)
{
    struct parsec_context_s *parsec_context = (struct parsec_context_s *)opaque;
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
    vdi_stream_client__video_render_texture(
        parsec_context, parsec_context->texture_ttf, NULL, &dst
    );
}

/* sdl frame video event. */
static bool
vdi_stream_client__frame_video(void *opaque, bool force_redraw)
{
    struct parsec_context_s *parsec_context = (struct parsec_context_s *)opaque;
    ParsecStatus e;
    SDL_FRect src;

    if (parsec_context->requested_width != parsec_context->window_width ||
        parsec_context->requested_height != parsec_context->window_height) {
        e = ParsecClientSetDimensions(
            parsec_context->parsec, DEFAULT_STREAM, parsec_context->window_width,
            parsec_context->window_height, 1
        );
        if (e != PARSEC_OK) {
            SDL_LogError(SDL_LOG_CATEGORY_APPLICATION, "Set dimensions failed with code: %d\n", e);
        } else {
            parsec_context->requested_width = parsec_context->window_width;
            parsec_context->requested_height = parsec_context->window_height;
        }
    }

    parsec_context->frame_video_updated = false;
    ParsecClientPollFrame(
        parsec_context->parsec, DEFAULT_STREAM, vdi_stream_client__frame_video_update,
        parsec_context->render_timeout, parsec_context
    );

    if (!force_redraw && !parsec_context->frame_video_updated) {
        return false;
    }

    SDL_SetRenderDrawColor(parsec_context->renderer, 0x00, 0x00, 0x00, 0xFF);
    SDL_RenderClear(parsec_context->renderer);

    if (parsec_context->frame_video_texture == NULL) {
        return force_redraw;
    }

    src.x = 0.0f;
    src.y = 0.0f;
    src.w = parsec_context->window_width;
    src.h = parsec_context->window_height;
    vdi_stream_client__video_render_texture(
        parsec_context, parsec_context->frame_video_texture, &src, NULL
    );
    return true;
}

SDL_WindowFlags
vdi_stream_client__video_window_flags(bool acceleration)
{
    return acceleration ? SDL_WINDOW_VULKAN : 0;
}

/* initialize video rendering on the main thread. */
bool
vdi_stream_client__video_init(struct parsec_context_s *parsec_context, bool acceleration)
{
    const char *renderer_name;

    if (acceleration && (SDL_GetWindowFlags(parsec_context->window) & SDL_WINDOW_VULKAN) != 0) {
        if (!vdi_stream_client__placebo_init(parsec_context)) {
            SDL_LogWarn(
                SDL_LOG_CATEGORY_APPLICATION,
                "VA-API zero-copy renderer initialization failed; use SDL upload fallback\n"
            );
        }
    }
    if (parsec_context->renderer == NULL) {
        parsec_context->renderer = SDL_CreateRenderer(
            parsec_context->window,
            (SDL_GetWindowFlags(parsec_context->window) & SDL_WINDOW_VULKAN) != 0 ? "vulkan" : NULL
        );
    }
    if (parsec_context->renderer == NULL) {
        SDL_LogError(
            SDL_LOG_CATEGORY_APPLICATION, "Renderer creation failed: %s\n", SDL_GetError()
        );
        return false;
    }

    renderer_name = SDL_GetRendererName(parsec_context->renderer);
    SDL_LogInfo(
        SDL_LOG_CATEGORY_APPLICATION, "Use %s renderer\n",
        renderer_name != NULL ? renderer_name : "unknown"
    );
    if (!SDL_SetRenderVSync(parsec_context->renderer, 1)) {
        SDL_LogError(
            SDL_LOG_CATEGORY_APPLICATION, "SDL_SetRenderVSync failed: %s\n", SDL_GetError()
        );
    }
    return true;
}

/* render a single frame on the main thread. */
bool
vdi_stream_client__video_render(struct parsec_context_s *parsec_context, bool force_redraw)
{

    /* show parsec frame. */
    if (vdi_stream_client__context_connected(parsec_context)) {
        if (!vdi_stream_client__frame_video(parsec_context, force_redraw)) {
            return false;
        }
        if (!vdi_stream_client__video_present(parsec_context)) {
            SDL_LogError(
                SDL_LOG_CATEGORY_APPLICATION, "SDL_RenderPresent failed: %s\n", SDL_GetError()
            );
        }
        return true;
    }

    /* show reconnecting/shutdown text if available. */
    if (parsec_context->surface_ttf != NULL &&
        (force_redraw || SDL_GetTicks() >= parsec_context->next_overlay_tick)) {
        vdi_stream_client__frame_text(parsec_context);
        if (!vdi_stream_client__video_present(parsec_context)) {
            SDL_LogError(
                SDL_LOG_CATEGORY_APPLICATION, "SDL_RenderPresent failed: %s\n", SDL_GetError()
            );
        }
        parsec_context->next_overlay_tick = SDL_GetTicks() + parsec_context->timeout;
        return true;
    }

    return false;
}

/* release video resources on the main thread. */
void
vdi_stream_client__video_destroy(struct parsec_context_s *parsec_context)
{
    SDL_DestroyTexture(parsec_context->texture_ttf);
    parsec_context->texture_ttf = NULL;

    SDL_DestroyTexture(parsec_context->texture_video);
    parsec_context->texture_video = NULL;
    parsec_context->frame_video_texture = NULL;

    vdi_stream_client__placebo_destroy(parsec_context);
    if (parsec_context->renderer != NULL) {
        SDL_DestroyRenderer(parsec_context->renderer);
        parsec_context->renderer = NULL;
    }
}
