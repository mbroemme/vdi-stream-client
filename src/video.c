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

/* Wrap SDL_RenderTexture so render timing and call counts are recorded in one
 * place whenever render statistics are enabled. */
static bool
vdi_stream_client__video_render_texture(
    struct vdi_stream_client__output_s *output, SDL_Texture *texture, const SDL_FRect *src,
    const SDL_FRect *dst
)
{
    struct parsec_context_s *parsec_context = output->parsec_context;
    Uint64 render_start_ns = parsec_context->stats_enabled ? SDL_GetTicksNS() : 0;
    bool rendered = SDL_RenderTexture(output->renderer, texture, src, dst);

    if (parsec_context->stats_enabled) {
        parsec_context->stats_renders++;
        parsec_context->stats_render_ns += SDL_GetTicksNS() - render_start_ns;
    }
    return rendered;
}

/* Present the SDL renderer and account for both attempted and successful
 * presents. The caller still logs SDL errors because it knows the context. */
static bool
vdi_stream_client__video_present(struct vdi_stream_client__output_s *output)
{
    struct parsec_context_s *parsec_context = output->parsec_context;
    Uint64 present_start_ns = parsec_context->stats_enabled ? SDL_GetTicksNS() : 0;
    bool presented = SDL_RenderPresent(output->renderer);

    if (parsec_context->stats_enabled) {
        parsec_context->stats_present_calls++;
        parsec_context->stats_present_ns += SDL_GetTicksNS() - present_start_ns;
        if (presented) {
            parsec_context->stats_presents++;
        }
    }
    return presented;
}

/* Resolve the SDL texture format for a Parsec frame. FFmpeg descriptor frames
 * are queried first because their real pixel layout lives in the retained
 * AVFrame rather than only in ParsecFrame::format. */
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

/* Ensure the streaming SDL texture exists and matches the current frame size
 * and format. Recreating here keeps the hot upload path focused on pixels. */
static bool
vdi_stream_client__video_texture(
    struct vdi_stream_client__output_s *output, const ParsecFrame *frame, const void *image
)
{
    SDL_PixelFormat pixel_format;
    bool format_changed;
    const char *pixel_format_name;

    if (!vdi_stream_client__video_format(frame, image, &pixel_format)) {
        SDL_LogError(SDL_LOG_CATEGORY_APPLICATION, "Unsupported video format: %d\n", frame->format);
        return false;
    }
    if (output->renderer == NULL) {
        return false;
    }
    if (frame->fullWidth == 0 || frame->fullHeight == 0 || frame->fullWidth > (Uint32)INT_MAX ||
        frame->fullHeight > (Uint32)INT_MAX) {
        SDL_LogWarn(
            SDL_LOG_CATEGORY_APPLICATION, "Ignore invalid video frame size %ux%u\n",
            frame->fullWidth, frame->fullHeight
        );
        return false;
    }

    if (output->texture_video != NULL && output->texture_width == (Sint32)frame->fullWidth &&
        output->texture_height == (Sint32)frame->fullHeight &&
        output->pixel_format_video == pixel_format) {
        return true;
    }

    format_changed = output->pixel_format_video != pixel_format;
    if (output->frame_video_texture == output->texture_video) {
        output->frame_video_texture = NULL;
    }
    SDL_DestroyTexture(output->texture_video);
    output->texture_video = SDL_CreateTexture(
        output->renderer, pixel_format, SDL_TEXTUREACCESS_STREAMING, frame->fullWidth,
        frame->fullHeight
    );
    if (output->texture_video == NULL) {
        SDL_LogError(
            SDL_LOG_CATEGORY_APPLICATION, "Video texture creation failed: %s\n", SDL_GetError()
        );
        return false;
    }

    output->texture_width = frame->fullWidth;
    output->texture_height = frame->fullHeight;
    output->pixel_format_video = pixel_format;
    if (format_changed && !output->parsec_context->log_video_pixel_format) {
        pixel_format_name = SDL_GetPixelFormatName(pixel_format);
        SDL_LogInfo(
            SDL_LOG_CATEGORY_APPLICATION, "Use %s video pixel format\n",
            pixel_format_name != NULL ? pixel_format_name : "unknown"
        );
        output->parsec_context->log_video_pixel_format = true;
    }
    return true;
}

/* Parsec frame callback used by the render loop. It first gives libplacebo a
 * chance to render VA-API hardware frames, then falls back to SDL texture
 * uploads for FFmpeg descriptor frames or raw Parsec image buffers. */
static void
vdi_stream_client__frame_video_update(const ParsecFrame *frame, const void *image, void *opaque)
{
    struct vdi_stream_client__output_s *output = (struct vdi_stream_client__output_s *)opaque;
    struct parsec_context_s *parsec_context = output->parsec_context;
    const Uint8 *pixels = (const Uint8 *)image;
    Uint64 upload_elapsed_ns = 0;
    Uint64 upload_start_ns = 0;
    bool upload_attempted = false;
    bool placebo_handled = false;
    bool updated = false;

    if (vdi_stream_client__placebo_render(output, frame, image, &placebo_handled)) {
        updated = true;
        goto done;
    }
    if (placebo_handled) {
        goto done;
    }

    if (!vdi_stream_client__video_texture(output, frame, image)) {
        goto done;
    }

    if (vdi_stream_client__parsec_ffmpeg_frame_is_descriptor(frame, image)) {
        upload_attempted = true;
        updated = vdi_stream_client__parsec_ffmpeg_frame_update(
            output->texture_video, frame, image,
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
                output->texture_video, NULL, pixels, frame->fullWidth,
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
                output->texture_video, NULL, pixels, frame->fullWidth,
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
        if (!SDL_UpdateTexture(output->texture_video, NULL, pixels, frame->fullWidth * 4)) {
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
        output->frame_video_texture = output->texture_video;
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
        output->frame_video_updated = true;
    }
    vdi_stream_client__parsec_ffmpeg_frame_release(frame, image);
}

/* Render the current text overlay centered in the window. This is used while
 * connecting, reconnecting, or shutting down when no fresh video frame exists. */
static void
vdi_stream_client__frame_text(void *opaque)
{
    struct vdi_stream_client__output_s *output = (struct vdi_stream_client__output_s *)opaque;
    SDL_FRect dst;

    if (output->texture_ttf == NULL || output->surface_ttf == NULL) {
        return;
    }

    /* Calculate position and size to center of window. */
    dst.x = (output->window_width - output->surface_ttf->w) / 2.0f;
    dst.y = (output->window_height - output->surface_ttf->h) / 2.0f;
    dst.w = output->surface_ttf->w;
    dst.h = output->surface_ttf->h;

    SDL_SetRenderDrawColor(output->renderer, 0x00, 0x00, 0x00, 0xFF);
    SDL_RenderClear(output->renderer);
    vdi_stream_client__video_render_texture(output, output->texture_ttf, NULL, &dst);
}

/* Poll one Parsec video frame, update the active frame texture, and draw it to
 * the renderer. The function returns false when nothing changed and no forced
 * redraw was requested. */
static bool
vdi_stream_client__frame_video(void *opaque, bool force_redraw)
{
    struct vdi_stream_client__output_s *output = (struct vdi_stream_client__output_s *)opaque;
    struct parsec_context_s *parsec_context = output->parsec_context;
    ParsecStatus e;
    SDL_FRect src;

    if (output->requested_width != output->window_width ||
        output->requested_height != output->window_height) {
        e = ParsecClientSetDimensions(
            parsec_context->parsec, output->stream, output->window_width, output->window_height, 1
        );
        if (e != PARSEC_OK) {
            if (e < 0) {
                parsec_context->stream_error = e;
                vdi_stream_client__context_set_connection(parsec_context, false);
                return false;
            }
            SDL_LogError(SDL_LOG_CATEGORY_APPLICATION, "Set dimensions failed with code: %d\n", e);
        } else {
            output->requested_width = output->window_width;
            output->requested_height = output->window_height;
        }
    }

    output->frame_video_updated = false;
    e = ParsecClientPollFrame(
        parsec_context->parsec, output->stream, vdi_stream_client__frame_video_update,
        parsec_context->render_timeout, output
    );
    if (e < 0) {
        parsec_context->stream_error = e;
        vdi_stream_client__context_set_connection(parsec_context, false);
        return false;
    }

    if (!force_redraw && !output->frame_video_updated) {
        return false;
    }

    SDL_SetRenderDrawColor(output->renderer, 0x00, 0x00, 0x00, 0xFF);
    SDL_RenderClear(output->renderer);

    if (output->frame_video_texture == NULL) {
        return force_redraw;
    }

    src.x = 0.0f;
    src.y = 0.0f;
    src.w = output->texture_width;
    src.h = output->texture_height;
    vdi_stream_client__video_render_texture(output, output->frame_video_texture, &src, NULL);
    return true;
}

/* Choose SDL window creation flags required by the selected rendering path.
 * Hardware decoding asks for a Vulkan-capable window for libplacebo interop. */
SDL_WindowFlags
vdi_stream_client__video_window_flags(bool acceleration)
{
    return acceleration ? SDL_WINDOW_VULKAN : 0;
}

/* Initialize the renderer for the already-created window. The Vulkan path tries
 * libplacebo first so VA-API frames can be sampled without CPU copies. */
bool
vdi_stream_client__video_init(struct vdi_stream_client__output_s *output, bool acceleration)
{
    const char *renderer_name;

    if (acceleration && (SDL_GetWindowFlags(output->window) & SDL_WINDOW_VULKAN) != 0) {
        if (!vdi_stream_client__placebo_init(output)) {
            SDL_LogWarn(
                SDL_LOG_CATEGORY_APPLICATION,
                "VA-API zero-copy renderer initialization failed: %s\n", SDL_GetError()
            );
            return false;
        }
    }
    if (output->renderer == NULL) {
        output->renderer = SDL_CreateRenderer(
            output->window,
            (SDL_GetWindowFlags(output->window) & SDL_WINDOW_VULKAN) != 0 ? "vulkan" : NULL
        );
    }
    if (output->renderer == NULL) {
        SDL_LogError(
            SDL_LOG_CATEGORY_APPLICATION, "Renderer creation failed: %s\n", SDL_GetError()
        );
        return false;
    }

    if (!output->silent_reinit && !output->parsec_context->log_renderer) {
        renderer_name = SDL_GetRendererName(output->renderer);
        SDL_LogInfo(
            SDL_LOG_CATEGORY_APPLICATION, "Use %s renderer\n",
            renderer_name != NULL ? renderer_name : "unknown"
        );
        output->parsec_context->log_renderer = true;
    }
    if (!SDL_SetRenderVSync(output->renderer, 1)) {
        SDL_LogError(
            SDL_LOG_CATEGORY_APPLICATION, "SDL_SetRenderVSync failed: %s\n", SDL_GetError()
        );
    }
    return true;
}

/* Render one main-thread video iteration. Connected sessions draw Parsec video;
 * disconnected sessions periodically redraw the current text overlay. */
bool
vdi_stream_client__video_render(struct vdi_stream_client__output_s *output, bool force_redraw)
{
    struct parsec_context_s *parsec_context = output->parsec_context;

    /* Show Parsec frame. */
    if (vdi_stream_client__context_connected(parsec_context)) {
        if (!vdi_stream_client__frame_video(output, force_redraw)) {
            return false;
        }
        if (!vdi_stream_client__video_present(output)) {
            SDL_LogError(
                SDL_LOG_CATEGORY_APPLICATION, "SDL_RenderPresent failed: %s\n", SDL_GetError()
            );
        }
        return true;
    }

    /* Show reconnecting or shutdown text if available. */
    if (output->surface_ttf != NULL &&
        (force_redraw || SDL_GetTicks() >= output->next_overlay_tick)) {
        vdi_stream_client__frame_text(output);
        if (!vdi_stream_client__video_present(output)) {
            SDL_LogError(
                SDL_LOG_CATEGORY_APPLICATION, "SDL_RenderPresent failed: %s\n", SDL_GetError()
            );
        }
        output->next_overlay_tick = SDL_GetTicks() + parsec_context->timeout;
        return true;
    }

    return false;
}

/* Release renderer-owned textures and the optional libplacebo Vulkan bridge.
 * The SDL window itself is destroyed by the higher-level event loop. */
void
vdi_stream_client__video_destroy(struct vdi_stream_client__output_s *output)
{
    SDL_DestroyTexture(output->texture_ttf);
    output->texture_ttf = NULL;

    SDL_DestroyTexture(output->texture_video);
    output->texture_video = NULL;
    output->frame_video_texture = NULL;

    vdi_stream_client__placebo_destroy(output);
    if (output->renderer != NULL) {
        SDL_DestroyRenderer(output->renderer);
        output->renderer = NULL;
    }
}
