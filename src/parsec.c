/*
 *  parsec.c -- desktop streaming with parsec sdk
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
#include "audio.h"
#include "client.h"
#include "ffmpeg.h"
#include "input.h"
#include "parsec.h"
#include "redirect.h"
#include "video.h"

/* font include. */
#include "../include/font.h"

/* system includes. */
#include <limits.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>

#ifdef HAVE_LIBPARSEC
static ParsecStatus
vdi_stream_client__parsec_init(struct parsec_context_s *parsec_context, ParsecConfig *network_cfg)
{
    return ParsecInit(PARSEC_VER, network_cfg, NULL, &parsec_context->parsec);
}
#else
static ParsecStatus
vdi_stream_client__parsec_init(struct parsec_context_s *parsec_context, ParsecConfig *network_cfg)
{
    ParsecStatus e = ParsecInit(network_cfg, NULL, "libparsec.so", &parsec_context->parsec);

    /* The vendored DSO wrapper can return PARSEC_OK even if the loaded SDK
     * initialization failed. Treat a missing inner context as init failure. */
    if (e == PARSEC_OK && (parsec_context->parsec == NULL || parsec_context->parsec->ps == NULL)) {
        e = ERR_DEFAULT;
    }
    return e;
}
#endif

static double
vdi_stream_client__stats_mbps(Uint64 bytes, Uint64 elapsed_ms)
{
    if (elapsed_ms == 0) {
        return 0.0;
    }

    return (double)bytes * 8.0 / ((double)elapsed_ms * 1000.0);
}

static double
vdi_stream_client__stats_ms(Uint64 ns)
{
    return (double)ns / 1000000.0;
}

static double
vdi_stream_client__stats_avg_ms(Uint64 ns, Uint64 calls)
{
    if (calls == 0) {
        return 0.0;
    }

    return vdi_stream_client__stats_ms(ns) / (double)calls;
}

static void
vdi_stream_client__render_stats_reset(struct parsec_context_s *parsec_context)
{
    parsec_context->stats_loops = 0;
    parsec_context->stats_parsec_events = 0;
    parsec_context->stats_frames = 0;
    parsec_context->stats_presents = 0;
    parsec_context->stats_uploads = 0;
    parsec_context->stats_upload_ns = 0;
    parsec_context->stats_renders = 0;
    parsec_context->stats_render_ns = 0;
    parsec_context->stats_present_calls = 0;
    parsec_context->stats_present_ns = 0;
    parsec_context->stats_zero_copy_calls = 0;
    parsec_context->stats_zero_copy_ns = 0;
    parsec_context->stats_zero_copy_fallbacks = 0;
    parsec_context->stats_idle_waits = 0;
    parsec_context->stats_idle_wait_ms = 0;
}

static ParsecStatus
vdi_stream_client__parsec_reconnect(
    struct parsec_context_s *parsec_context, ParsecClientConfig *cfg,
    struct vdi_config_s *vdi_config
)
{
    ParsecStatus e;

    vdi_stream_client__context_set_connection(parsec_context, false);
    parsec_context->requested_width = 0;
    parsec_context->requested_height = 0;
    while (vdi_stream_client__context_audio_polling(parsec_context)) {
        SDL_Delay(1);
    }
    while (vdi_stream_client__context_input_polling(parsec_context)) {
        SDL_Delay(1);
    }

    ParsecClientDisconnect(parsec_context->parsec);
    e = ParsecClientConnect(parsec_context->parsec, cfg, vdi_config->session, vdi_config->peer);
    if (e != PARSEC_OK) {
        SDL_LogError(SDL_LOG_CATEGORY_APPLICATION, "Reconnect failed with code: %d\n", e);
    }
    return e;
}

/* parsec clipboard event. */
static void
vdi_stream_client__clipboard(struct parsec_context_s *parsec_context, Uint32 id, Uint32 buffer_key)
{
    char *msg = ParsecGetBuffer(parsec_context->parsec, buffer_key);

    if (msg && id == PARSEC_CLIPBOARD_MSG) {
        SDL_SetClipboardText(msg);
    }

#ifdef HAVE_LIBPARSEC
    ParsecFree(msg);
#else
    ParsecFree(parsec_context->parsec, msg);
#endif
}

/* log render stats at the configured interval. */
static void
vdi_stream_client__render_stats(struct parsec_context_s *parsec_context)
{
    Uint64 now;
    Uint64 period_start_ms;
    Uint64 elapsed_ms;
    Uint64 sdl_events;
    struct vdi_stream_client__parsec_ffmpeg_stats_s ffmpeg_stats = { 0 };
    double video_mbps;

    if (!parsec_context->stats_enabled) {
        return;
    }

    now = SDL_GetTicks();
    Uint64 last_frame_age_ms = parsec_context->stats_last_frame_tick == 0
                                   ? 0
                                   : now - parsec_context->stats_last_frame_tick;

    if (parsec_context->stats_next_tick == 0) {
        vdi_stream_client__parsec_ffmpeg_drain_stats(&ffmpeg_stats);
        (void)atomic_exchange_explicit(
            &parsec_context->stats_sdl_events, (uint_fast64_t)0, memory_order_relaxed
        );
        vdi_stream_client__render_stats_reset(parsec_context);
        parsec_context->stats_next_tick = now + parsec_context->stats_period_ms;
        return;
    }

    if (now < parsec_context->stats_next_tick) {
        return;
    }

    period_start_ms = parsec_context->stats_next_tick > parsec_context->stats_period_ms
                          ? parsec_context->stats_next_tick - parsec_context->stats_period_ms
                          : now;
    elapsed_ms = now > period_start_ms ? now - period_start_ms : parsec_context->stats_period_ms;
    vdi_stream_client__parsec_ffmpeg_drain_stats(&ffmpeg_stats);
    video_mbps = vdi_stream_client__stats_mbps(ffmpeg_stats.video_packet_bytes, elapsed_ms);
    sdl_events = (Uint64)atomic_exchange_explicit(
        &parsec_context->stats_sdl_events, (uint_fast64_t)0, memory_order_relaxed
    );

    SDL_LogInfo(
        SDL_LOG_CATEGORY_APPLICATION,
        "Render:\n"
        "  loop: loops=%llu, presents=%llu\n"
        "  events: sdl=%llu, parsec=%llu\n"
        "  frames: frames=%llu, age=%llums\n"
        "  idle: waits=%llu, ms=%llu\n"
        "  bandwidth: video=%.3fMbps\n"
        "  stages:\n"
        "    avcodec_send_packet: calls=%llu, total=%.3fms, avg=%.3fms\n"
        "    avcodec_receive_frame: calls=%llu, total=%.3fms, avg=%.3fms\n"
        "    av_hwframe_transfer_data: calls=%llu, total=%.3fms, avg=%.3fms\n"
        "    descriptor_fallback: calls=%llu, total=%.3fms, avg=%.3fms\n"
        "    vaapi_zero_copy: calls=%llu, total=%.3fms, avg=%.3fms, fallbacks=%llu\n"
        "    sdl_upload: calls=%llu, total=%.3fms, avg=%.3fms\n"
        "    render: calls=%llu, total=%.3fms, avg=%.3fms\n"
        "    present: calls=%llu, total=%.3fms, avg=%.3fms\n",
        (unsigned long long)parsec_context->stats_loops,
        (unsigned long long)parsec_context->stats_presents, (unsigned long long)sdl_events,
        (unsigned long long)parsec_context->stats_parsec_events,
        (unsigned long long)parsec_context->stats_frames, (unsigned long long)last_frame_age_ms,
        (unsigned long long)parsec_context->stats_idle_waits,
        (unsigned long long)parsec_context->stats_idle_wait_ms, video_mbps,
        (unsigned long long)ffmpeg_stats.send_packet_calls,
        vdi_stream_client__stats_ms(ffmpeg_stats.send_packet_ns),
        vdi_stream_client__stats_avg_ms(
            ffmpeg_stats.send_packet_ns, ffmpeg_stats.send_packet_calls
        ),
        (unsigned long long)ffmpeg_stats.receive_frame_calls,
        vdi_stream_client__stats_ms(ffmpeg_stats.receive_frame_ns),
        vdi_stream_client__stats_avg_ms(
            ffmpeg_stats.receive_frame_ns, ffmpeg_stats.receive_frame_calls
        ),
        (unsigned long long)ffmpeg_stats.hwframe_transfer_calls,
        vdi_stream_client__stats_ms(ffmpeg_stats.hwframe_transfer_ns),
        vdi_stream_client__stats_avg_ms(
            ffmpeg_stats.hwframe_transfer_ns, ffmpeg_stats.hwframe_transfer_calls
        ),
        (unsigned long long)ffmpeg_stats.descriptor_fallback_calls,
        vdi_stream_client__stats_ms(ffmpeg_stats.descriptor_fallback_ns),
        vdi_stream_client__stats_avg_ms(
            ffmpeg_stats.descriptor_fallback_ns, ffmpeg_stats.descriptor_fallback_calls
        ),
        (unsigned long long)parsec_context->stats_zero_copy_calls,
        vdi_stream_client__stats_ms(parsec_context->stats_zero_copy_ns),
        vdi_stream_client__stats_avg_ms(
            parsec_context->stats_zero_copy_ns, parsec_context->stats_zero_copy_calls
        ),
        (unsigned long long)parsec_context->stats_zero_copy_fallbacks,
        (unsigned long long)parsec_context->stats_uploads,
        vdi_stream_client__stats_ms(parsec_context->stats_upload_ns),
        vdi_stream_client__stats_avg_ms(
            parsec_context->stats_upload_ns, parsec_context->stats_uploads
        ),
        (unsigned long long)parsec_context->stats_renders,
        vdi_stream_client__stats_ms(parsec_context->stats_render_ns),
        vdi_stream_client__stats_avg_ms(
            parsec_context->stats_render_ns, parsec_context->stats_renders
        ),
        (unsigned long long)parsec_context->stats_present_calls,
        vdi_stream_client__stats_ms(parsec_context->stats_present_ns),
        vdi_stream_client__stats_avg_ms(
            parsec_context->stats_present_ns, parsec_context->stats_present_calls
        )
    );

    parsec_context->stats_next_tick = now + parsec_context->stats_period_ms;
    vdi_stream_client__render_stats_reset(parsec_context);
}

/* signal worker threads and wait until they stop using shared runtime resources. */
static void
vdi_stream_client__stop_threads(
    struct parsec_context_s *parsec_context, SDL_Thread **input_thread, SDL_Thread **audio_thread,
    SDL_Thread *network_thread[USB_MAX], Uint32 usb_count
)
{
    bool network_started = false;

    vdi_stream_client__context_set_done(parsec_context, true);

    if (usb_count > 0) {
        for (Uint32 count = 0; count < usb_count; count++) {
            if (network_thread[count] != NULL) {
                network_started = true;
                break;
            }
        }
    }

    if (network_started) {
        SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION, "Stop Network Thread\n");
        for (Uint32 count = 0; count < usb_count; count++) {
            if (network_thread[count] == NULL) {
                continue;
            }
            SDL_WaitThread(network_thread[count], NULL);
            network_thread[count] = NULL;
        }
    }

    if (*input_thread != NULL) {
        SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION, "Stop Input Thread\n");
        SDL_WaitThread(*input_thread, NULL);
        *input_thread = NULL;
    }

    if (*audio_thread != NULL) {
        SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION, "Stop Audio Thread\n");
        SDL_WaitThread(*audio_thread, NULL);
        *audio_thread = NULL;
    }
}

/* keep cursor-induced grabs separate from user-requested grabs. */
static void
vdi_stream_client__cursor_set_grab(
    struct parsec_context_s *parsec_context, bool enable, bool grab, bool grab_forced
)
{
    if (enable) {
        if (!SDL_GetWindowMouseGrab(parsec_context->window)) {
            SDL_SetWindowMouseGrab(parsec_context->window, true);
            parsec_context->cursor_grab = true;
        }
        return;
    }

    if (parsec_context->cursor_grab && !grab && !grab_forced) {
        SDL_SetWindowMouseGrab(parsec_context->window, false);
    }
    parsec_context->cursor_grab = false;
}

static void
vdi_stream_client__set_relative_mouse_mode(
    struct parsec_context_s *parsec_context, bool relative_mouse
)
{
    SDL_SetWindowRelativeMouseMode(parsec_context->window, relative_mouse);
    vdi_stream_client__context_set_input_relative_mouse(
        parsec_context, SDL_GetWindowRelativeMouseMode(parsec_context->window)
    );
}

/* parsec cursor event. */
static void
vdi_stream_client__cursor(
    struct parsec_context_s *parsec_context, ParsecCursor *cursor, Uint32 buffer_key, bool grab,
    bool grab_forced
)
{
    bool pressed = vdi_stream_client__context_input_pressed(parsec_context);
    bool need_cursor_grab = cursor->relative || (cursor->hidden && pressed);

    if (cursor->hidden && !cursor->relative && pressed) {
        parsec_context->hidden_drag = true;
    } else if (!cursor->hidden || cursor->relative) {
        parsec_context->hidden_drag = false;
    }

    if (cursor->imageUpdate) {
        Uint8 *image = ParsecGetBuffer(parsec_context->parsec, buffer_key);

        if (image != NULL) {
            SDL_Surface *surface = SDL_CreateSurfaceFrom(
                cursor->width, cursor->height, SDL_PIXELFORMAT_RGBA32, image, cursor->width * 4
            );
            SDL_Cursor *sdlCursor = NULL;

            if (surface != NULL) {
                sdlCursor = SDL_CreateColorCursor(surface, cursor->hotX, cursor->hotY);
                SDL_DestroySurface(surface);
            }

            if (sdlCursor != NULL) {
                SDL_SetCursor(sdlCursor);
                SDL_DestroyCursor(parsec_context->cursor);
                parsec_context->cursor = sdlCursor;
            }

#ifdef HAVE_LIBPARSEC
            ParsecFree(image);
#else
            ParsecFree(parsec_context->parsec, image);
#endif
        }
    }

    if (need_cursor_grab) {
        vdi_stream_client__cursor_set_grab(parsec_context, true, grab, grab_forced);
    }

    if (cursor->hidden && (!parsec_context->hidden_drag || pressed)) {
        SDL_HideCursor();
    } else {
        SDL_ShowCursor();
    }

    if (!SDL_GetWindowRelativeMouseMode(parsec_context->window) && cursor->relative) {
        vdi_stream_client__set_relative_mouse_mode(parsec_context, true);
        if (!pressed && !grab_forced) {
            SDL_SetWindowTitle(
                parsec_context->window, "VDI Stream Client (Press Ctrl+Alt to release grab)"
            );
        }
    } else if (SDL_GetWindowRelativeMouseMode(parsec_context->window) && !cursor->relative) {
        vdi_stream_client__set_relative_mouse_mode(parsec_context, false);
        if (!cursor->hidden) {
            SDL_ShowCursor();
        }
        if (!pressed && !grab_forced && !grab) {
            SDL_SetWindowTitle(parsec_context->window, "VDI Stream Client");
        }
    }

    if (!need_cursor_grab) {
        vdi_stream_client__cursor_set_grab(parsec_context, false, grab, grab_forced);
    }

    parsec_context->hidden = cursor->hidden;
    parsec_context->relative = cursor->relative;
    vdi_stream_client__context_set_input_relative(parsec_context, cursor->relative);
}

/* render text. */
Sint32
vdi_stream_client__render_text(void *opaque, const char *text)
{
    struct parsec_context_s *parsec_context = (struct parsec_context_s *)opaque;
    SDL_Color color = { 0x88, 0x88, 0x88, 0xFF };

    SDL_DestroyTexture(parsec_context->texture_ttf);
    parsec_context->texture_ttf = NULL;

    if (parsec_context->surface_ttf != NULL) {
        SDL_DestroySurface(parsec_context->surface_ttf);
        parsec_context->surface_ttf = NULL;
    }

    /* create the text surface. */
    parsec_context->surface_ttf = TTF_RenderText_Blended(parsec_context->font, text, 0, color);
    if (parsec_context->surface_ttf == NULL) {
        SDL_LogError(
            SDL_LOG_CATEGORY_APPLICATION, "TTF surface creation failed: %s\n", SDL_GetError()
        );
        return VDI_STREAM_CLIENT_ERROR;
    }

    /* convert the text into an sdl texture. */
    parsec_context->texture_ttf =
        SDL_CreateTextureFromSurface(parsec_context->renderer, parsec_context->surface_ttf);
    if (parsec_context->texture_ttf == NULL) {
        SDL_LogError(
            SDL_LOG_CATEGORY_APPLICATION, "TTF texture creation failed: %s\n", SDL_GetError()
        );
        return VDI_STREAM_CLIENT_ERROR;
    }

    /* no error. */
    return VDI_STREAM_CLIENT_SUCCESS;
}

static void
vdi_stream_client__release_grab(struct parsec_context_s *parsec_context)
{
    if (SDL_GetWindowRelativeMouseMode(parsec_context->window)) {
        vdi_stream_client__set_relative_mouse_mode(parsec_context, false);
        SDL_ShowCursor();
    }

    if (SDL_GetWindowMouseGrab(parsec_context->window)) {
        SDL_SetWindowMouseGrab(parsec_context->window, false);
    }
    parsec_context->cursor_grab = false;
    SDL_SetWindowTitle(parsec_context->window, "VDI Stream Client");
}

static void
vdi_stream_client__handle_mouse_button_down(
    struct parsec_context_s *parsec_context, struct vdi_config_s *vdi_config, bool grab_forced
)
{
    if (grab_forced) {
        return;
    }

    if (vdi_config->grab == 1 && !SDL_GetWindowMouseGrab(parsec_context->window)) {
        SDL_SetWindowMouseGrab(parsec_context->window, true);
        SDL_SetWindowTitle(
            parsec_context->window, "VDI Stream Client (Press Ctrl+Alt to release grab)"
        );
    }

    if (parsec_context->relative && !SDL_GetWindowRelativeMouseMode(parsec_context->window)) {
        SDL_HideCursor();
        vdi_stream_client__cursor_set_grab(parsec_context, true, vdi_config->grab, grab_forced);
        vdi_stream_client__set_relative_mouse_mode(parsec_context, true);
        SDL_SetWindowTitle(
            parsec_context->window, "VDI Stream Client (Press Ctrl+Alt to release grab)"
        );
    }

    if (parsec_context->hidden && !parsec_context->relative) {
        vdi_stream_client__cursor_set_grab(parsec_context, true, vdi_config->grab, grab_forced);
    }
}

static void
vdi_stream_client__handle_mouse_button_up(
    struct parsec_context_s *parsec_context, struct vdi_config_s *vdi_config, bool grab_forced
)
{
    if (parsec_context->hidden_drag && parsec_context->hidden && !parsec_context->relative) {
        SDL_ShowCursor();
    }

    if (parsec_context->hidden && !parsec_context->relative) {
        vdi_stream_client__cursor_set_grab(parsec_context, false, vdi_config->grab, grab_forced);
    }
}

static void
vdi_stream_client__handle_force_grab_enable(
    struct parsec_context_s *parsec_context, struct vdi_config_s *vdi_config
)
{
    if (vdi_config->screensaver == 1) {
        SDL_DisableScreenSaver();
    }

    if (parsec_context->relative && !SDL_GetWindowRelativeMouseMode(parsec_context->window)) {
        SDL_HideCursor();
        vdi_stream_client__cursor_set_grab(parsec_context, true, vdi_config->grab, true);
        vdi_stream_client__set_relative_mouse_mode(parsec_context, true);
    }

    SDL_SetWindowMouseGrab(parsec_context->window, true);
    SDL_SetWindowTitle(
        parsec_context->window, "VDI Stream Client (Press Shift+F12 to release forced grab)"
    );
}

static void
vdi_stream_client__handle_clipboard_update(struct parsec_context_s *parsec_context)
{
    char *clipboard;

    if (!vdi_stream_client__context_connected(parsec_context)) {
        return;
    }

    clipboard = SDL_GetClipboardText();
    if (clipboard != NULL) {
        ParsecClientSendUserData(parsec_context->parsec, PARSEC_CLIPBOARD_MSG, clipboard);
        SDL_free(clipboard);
    }
}

static void vdi_stream_client__window_enforce_size(SDL_Window *window, Sint32 width, Sint32 height);

static void
vdi_stream_client__handle_input_command(
    struct parsec_context_s *parsec_context, struct vdi_config_s *vdi_config,
    const vdi_stream_client__input_command_s *command, bool *force_redraw
)
{
    switch (command->type) {
    case VDI_STREAM_CLIENT_INPUT_COMMAND_QUIT:
        vdi_stream_client__context_set_done(parsec_context, true);
        vdi_stream_client__render_text(parsec_context, "Closing...");
        *force_redraw = true;
        break;
    case VDI_STREAM_CLIENT_INPUT_COMMAND_RELEASE_GRAB:
        vdi_stream_client__release_grab(parsec_context);
        break;
    case VDI_STREAM_CLIENT_INPUT_COMMAND_FORCE_GRAB_ENABLE:
        vdi_stream_client__handle_force_grab_enable(parsec_context, vdi_config);
        break;
    case VDI_STREAM_CLIENT_INPUT_COMMAND_FORCE_GRAB_DISABLE:
        if (vdi_config->screensaver == 1) {
            SDL_EnableScreenSaver();
        }
        vdi_stream_client__release_grab(parsec_context);
        break;
    case VDI_STREAM_CLIENT_INPUT_COMMAND_MOUSE_BUTTON_DOWN:
        vdi_stream_client__handle_mouse_button_down(
            parsec_context, vdi_config, command->grab_forced
        );
        break;
    case VDI_STREAM_CLIENT_INPUT_COMMAND_MOUSE_BUTTON_UP:
        vdi_stream_client__handle_mouse_button_up(parsec_context, vdi_config, command->grab_forced);
        break;
    case VDI_STREAM_CLIENT_INPUT_COMMAND_CLIPBOARD_UPDATE:
        if (vdi_config->clipboard == 1) {
            vdi_stream_client__handle_clipboard_update(parsec_context);
        }
        break;
    case VDI_STREAM_CLIENT_INPUT_COMMAND_MOUSE_ENTER:
        SDL_SetEventEnabled(SDL_EVENT_KEY_DOWN, true);
        SDL_SetEventEnabled(SDL_EVENT_KEY_UP, true);
        SDL_SetWindowKeyboardGrab(parsec_context->window, true);
        break;
    case VDI_STREAM_CLIENT_INPUT_COMMAND_MOUSE_LEAVE:
        SDL_SetEventEnabled(SDL_EVENT_KEY_DOWN, false);
        SDL_SetEventEnabled(SDL_EVENT_KEY_UP, false);
        SDL_SetWindowKeyboardGrab(parsec_context->window, false);
        break;
    case VDI_STREAM_CLIENT_INPUT_COMMAND_WINDOW_RESIZED:
        vdi_stream_client__window_enforce_size(
            parsec_context->window, parsec_context->window_width, parsec_context->window_height
        );
        *force_redraw = true;
        break;
    default:
        break;
    }
}

static void
vdi_stream_client__handle_input_commands(
    vdi_stream_client__input_context_s *input_context, bool *force_redraw
)
{
    vdi_stream_client__input_command_s command;

    while (vdi_stream_client__input_next_command(input_context, &command)) {
        vdi_stream_client__handle_input_command(
            input_context->parsec_context, input_context->vdi_config, &command, force_redraw
        );
    }
}

static void
vdi_stream_client__show_connection_overlay(
    struct parsec_context_s *parsec_context, bool *force_redraw, const char *text
)
{
    vdi_stream_client__render_text(parsec_context, text);
    *force_redraw = true;
}

static void
vdi_stream_client__use_h264_fallback(
    ParsecClientConfig *cfg, bool *hevc_attempt_active, bool *h264_fallback_done
)
{
    if (!*hevc_attempt_active || *h264_fallback_done) {
        return;
    }

    cfg->video[DEFAULT_STREAM].decoderH265 = 0;
    cfg->video[DEFAULT_STREAM].decoderCompatibility = 0;
    *hevc_attempt_active = false;
    *h264_fallback_done = true;
}

static void
vdi_stream_client__handle_connection_status(
    struct parsec_context_s *parsec_context, struct vdi_config_s *vdi_config,
    ParsecClientConfig *cfg, Uint64 *last_time, bool *force_redraw, bool *hevc_attempt_active,
    bool *h264_fallback_done
)
{
    ParsecStatus e = ParsecClientGetStatus(parsec_context->parsec, &parsec_context->client_status);

    if (vdi_config->reconnect == 0 && e != PARSEC_CONNECTING && e != PARSEC_OK) {
        vdi_stream_client__show_connection_overlay(parsec_context, force_redraw, "Closing...");
        SDL_LogError(SDL_LOG_CATEGORY_APPLICATION, "Parsec disconnected\n");
        vdi_stream_client__context_set_done(parsec_context, true);
    }
    if (vdi_config->reconnect == 1 && e != PARSEC_CONNECTING && e != PARSEC_OK &&
        SDL_GetTicks() > *last_time + vdi_config->timeout) {
        vdi_stream_client__show_connection_overlay(parsec_context, force_redraw, "Reconnecting...");
        vdi_stream_client__use_h264_fallback(cfg, hevc_attempt_active, h264_fallback_done);
        e = vdi_stream_client__parsec_reconnect(parsec_context, cfg, vdi_config);
        *last_time = SDL_GetTicks();
    }

    if (vdi_config->reconnect == 0 && parsec_context->client_status.networkFailure == 1) {
        vdi_stream_client__show_connection_overlay(parsec_context, force_redraw, "Closing...");
        SDL_LogError(SDL_LOG_CATEGORY_APPLICATION, "Network disconnected\n");
        vdi_stream_client__context_set_done(parsec_context, true);
    }
    if (vdi_config->reconnect == 1 && parsec_context->client_status.networkFailure == 1 &&
        SDL_GetTicks() > *last_time + vdi_config->timeout) {
        vdi_stream_client__show_connection_overlay(parsec_context, force_redraw, "Reconnecting...");
        e = vdi_stream_client__parsec_reconnect(parsec_context, cfg, vdi_config);
        *last_time = SDL_GetTicks();
    }

    if (vdi_config->reconnect == 1 && parsec_context->client_status.networkFailure == 0 &&
        e == PARSEC_OK && !vdi_stream_client__context_connected(parsec_context)) {
        vdi_stream_client__context_set_connection(parsec_context, true);
    }
}

static void
vdi_stream_client__window_lock_size(SDL_Window *window, Sint32 width, Sint32 height)
{
    if (!SDL_SetWindowResizable(window, false)) {
        SDL_LogWarn(
            SDL_LOG_CATEGORY_APPLICATION, "Disabling window resize failed: %s\n", SDL_GetError()
        );
    }
    if (!SDL_SetWindowMinimumSize(window, width, height)) {
        SDL_LogWarn(
            SDL_LOG_CATEGORY_APPLICATION, "Setting minimum window size failed: %s\n", SDL_GetError()
        );
    }
    if (!SDL_SetWindowMaximumSize(window, width, height)) {
        SDL_LogWarn(
            SDL_LOG_CATEGORY_APPLICATION, "Setting maximum window size failed: %s\n", SDL_GetError()
        );
    }
}

static void
vdi_stream_client__window_unlock_size(SDL_Window *window)
{
    if (!SDL_SetWindowMinimumSize(window, 0, 0)) {
        SDL_LogWarn(
            SDL_LOG_CATEGORY_APPLICATION, "Clearing minimum window size failed: %s\n",
            SDL_GetError()
        );
    }
    if (!SDL_SetWindowMaximumSize(window, 0, 0)) {
        SDL_LogWarn(
            SDL_LOG_CATEGORY_APPLICATION, "Clearing maximum window size failed: %s\n",
            SDL_GetError()
        );
    }
}

static void
vdi_stream_client__window_enforce_size(SDL_Window *window, Sint32 width, Sint32 height)
{
    int current_width = 0;
    int current_height = 0;
    bool maximized = (SDL_GetWindowFlags(window) & SDL_WINDOW_MAXIMIZED) != 0;

    if (!SDL_GetWindowSize(window, &current_width, &current_height)) {
        SDL_LogWarn(
            SDL_LOG_CATEGORY_APPLICATION, "Reading window size failed: %s\n", SDL_GetError()
        );
    }
    if (maximized) {
        if (!SDL_RestoreWindow(window)) {
            SDL_LogWarn(
                SDL_LOG_CATEGORY_APPLICATION, "Restoring window failed: %s\n", SDL_GetError()
            );
        }
        if (!SDL_SyncWindow(window)) {
            SDL_LogWarn(
                SDL_LOG_CATEGORY_APPLICATION, "Window restore synchronization failed: %s\n",
                SDL_GetError()
            );
        }
    }
    if (maximized || current_width != width || current_height != height) {
        if (!SDL_SetWindowSize(window, width, height)) {
            SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION, "Window resize failed: %s\n", SDL_GetError());
        }
        if (!SDL_SyncWindow(window)) {
            SDL_LogWarn(
                SDL_LOG_CATEGORY_APPLICATION, "Window resize synchronization failed: %s\n",
                SDL_GetError()
            );
        }
    }
    vdi_stream_client__window_lock_size(window, width, height);
}

static bool
vdi_stream_client__video_setup(
    struct parsec_context_s *parsec_context, SDL_WindowFlags window_flags, bool acceleration
)
{
    parsec_context->window = SDL_CreateWindow(
        "VDI Stream Client", parsec_context->window_width, parsec_context->window_height,
        window_flags
    );
    if (parsec_context->window == NULL) {
        SDL_LogError(SDL_LOG_CATEGORY_APPLICATION, "Window creation failed: %s\n", SDL_GetError());
        return false;
    }
    if (vdi_stream_client__video_init(parsec_context, acceleration)) {
        vdi_stream_client__window_lock_size(
            parsec_context->window, parsec_context->window_width, parsec_context->window_height
        );
        return true;
    }

    vdi_stream_client__video_destroy(parsec_context);
    SDL_DestroyWindow(parsec_context->window);
    parsec_context->window = NULL;
    return false;
}

/* parsec event loop. */
Sint32
vdi_stream_client__event_loop(struct vdi_config_s *vdi_config)
{
    struct parsec_context_s parsec_context = { 0 };
    vdi_stream_client__input_context_s input_context = { 0 };
    struct redirect_context_s redirect_context[USB_MAX] = { 0 };
    Uint32 wait_time = 0;
    Uint64 last_time = 0;
    bool force_redraw = false;
    SDL_AudioSpec want = { 0 };
    ParsecStatus e;
    ParsecConfig network_cfg = PARSEC_DEFAULTS;
    ParsecClientConfig cfg = PARSEC_CLIENT_DEFAULTS;
    Uint32 ffmpeg_decoder_index = UINT32_MAX;
    bool hevc_attempt_active = false;
    bool h264_fallback_done = false;
    bool hardware_decoding;
    Uint32 device;
    SDL_Thread *input_thread = NULL;
    SDL_Thread *audio_thread = NULL;
    SDL_Thread *network_thread[USB_MAX] = { 0 };
    SDL_WindowFlags window_flags = SDL_WINDOW_HIGH_PIXEL_DENSITY;
    const char *video_driver;

    /* default values. */
    parsec_context.timeout = 100;
    parsec_context.render_timeout = 5;
    parsec_context.next_overlay_tick = 0;
    parsec_context.stats_enabled = vdi_config->stats;
    parsec_context.stats_period_ms = vdi_config->stats_period * 1000;

    /* sdl init. */
    SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION, "Initialize SDL\n");
    if (!SDL_Init(SDL_INIT_VIDEO | SDL_INIT_AUDIO)) {
        SDL_LogError(SDL_LOG_CATEGORY_APPLICATION, "Initialization failed: %s\n", SDL_GetError());
        goto error;
    }

    /* ttf init. */
    SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION, "Initialize TTF\n");
    if (!TTF_Init()) {
        SDL_LogError(SDL_LOG_CATEGORY_APPLICATION, "Initialization failed: %s\n", SDL_GetError());
        goto error;
    }

    /* load font. */
    parsec_context.font = TTF_OpenFontIO(
        SDL_IOFromMem(MorePerfectDOSVGA_ttf, MorePerfectDOSVGA_ttf_len), true, 16.0f
    );
    if (parsec_context.font == NULL) {
        SDL_LogError(SDL_LOG_CATEGORY_APPLICATION, "Loading font failed: %s\n", SDL_GetError());
        goto error;
    }

    /* configure upnp before parsec init consumes the network configuration. */
    if (vdi_config->upnp == 1) {
        network_cfg.upnp = 1;
    }
    if (vdi_config->upnp == 0) {
        SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION, "Disable UPnP\n");
        network_cfg.upnp = 0;
    }

    /* parsec init. */
    SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION, "Initialize Parsec\n");
    e = vdi_stream_client__parsec_init(&parsec_context, &network_cfg);
    if (e != PARSEC_OK) {
        SDL_LogError(SDL_LOG_CATEGORY_APPLICATION, "Initialization failed with code: %d\n", e);
        goto error;
    }

    /* use client resolution if specified. */
    if (vdi_config->width > 0 && vdi_config->height > 0) {
        SDL_LogInfo(
            SDL_LOG_CATEGORY_APPLICATION, "Override resolution %dx%d\n", vdi_config->width,
            vdi_config->height
        );
        cfg.video[DEFAULT_STREAM].resolutionX = vdi_config->width;
        cfg.video[DEFAULT_STREAM].resolutionY = vdi_config->height;
    }

    /* configure host video codec. */
    if (vdi_config->hevc == 1) {
        cfg.video[DEFAULT_STREAM].decoderH265 = 1;
    }
    if (vdi_config->hevc == 0) {
        SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION, "Disable H.265 (HEVC) Video Codec\n");
        cfg.video[DEFAULT_STREAM].decoderH265 = 0;
    }

    /* configure host color mode. */
    if (vdi_config->subsampling == 1) {
        cfg.video[DEFAULT_STREAM].decoder444 = 0;
    }
    if (vdi_config->subsampling == 0) {
        SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION, "Disable Chroma Subsampling\n");
        cfg.video[DEFAULT_STREAM].decoder444 = 1;

        /* TODO: parsec sdk bug. */
        SDL_LogWarn(
            SDL_LOG_CATEGORY_APPLICATION,
            "Parsec SDK bug and color mode 4:4:4 not working yet, details at:\n"
        );
        SDL_LogWarn(
            SDL_LOG_CATEGORY_APPLICATION, "https://github.com/parsec-cloud/parsec-sdk/issues/36\n"
        );
    }

    /* configure client decoding acceleration. */
    if (vdi_config->acceleration == 0) {
        SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION, "Disable Hardware Accelerated Video Decoding\n");
    }
    if (vdi_config->acceleration == 1 && cfg.video[DEFAULT_STREAM].decoderH265 == 1) {
        bool h264 = false;
        bool hevc = false;

        if (vdi_stream_client__parsec_ffmpeg_vaapi_codecs(&h264, &hevc) && h264 && !hevc) {
            SDL_LogInfo(
                SDL_LOG_CATEGORY_APPLICATION,
                "VA-API H.265 decoding unsupported, use H.264 (AVC) hardware fallback\n"
            );
            cfg.video[DEFAULT_STREAM].decoderH265 = 0;
        }
    }

    /* Configure client-side FFmpeg for H.264 and H.265. The public Linux SDK
     * exposes a hidden FFmpeg decoder entry; replace that entry with the client
     * decoder so both codecs use the same owned VAAPI or software path. */
    if (!vdi_stream_client__parsec_ffmpeg_decoder_enable(
            &parsec_context, &ffmpeg_decoder_index, vdi_config->acceleration == 1
        )) {
        SDL_LogError(SDL_LOG_CATEGORY_APPLICATION, "FFmpeg decoder injection failed\n");
        goto error;
    }
    cfg.video[DEFAULT_STREAM].decoderIndex = ffmpeg_decoder_index;
    hevc_attempt_active = cfg.video[DEFAULT_STREAM].decoderH265 == 1;

    /* check if reconnect should be disabled. */
    if (vdi_config->reconnect == 0) {
        SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION, "Disable automatic reconnect\n");
    }

    /* check if exclusive mouse grab should be disabled. */
    if (vdi_config->grab == 0) {
        SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION, "Disable exclusive mouse grab\n");
    }

    /* check if window decorations should be disabled. */
    if (vdi_config->decoration == 0) {
        SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION, "Disable window decorations\n");
        window_flags |= SDL_WINDOW_BORDERLESS;
    }

    /* configure screen saver. */
    if (vdi_config->screensaver == 1) {
        SDL_EnableScreenSaver();
    }
    if (vdi_config->screensaver == 0) {
        SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION, "Disable screen saver\n");
        SDL_DisableScreenSaver();
    }

    /* check if clipboard should be disabled. */
    if (vdi_config->clipboard == 0) {
        SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION, "Disable clipboard sharing\n");
    }

    /* check if audio should be streamed. */
    if (vdi_config->audio == 0) {
        SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION, "Disable audio streaming\n");
    }

    for (;;) {
        wait_time = 0;
        vdi_stream_client__context_set_connection(&parsec_context, false);
        parsec_context.decoder = false;

        /* parsec connect. */
        SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION, "Connect to Parsec service\n");
        e = ParsecClientConnect(parsec_context.parsec, &cfg, vdi_config->session, vdi_config->peer);
        if (e != PARSEC_OK) {
            SDL_LogError(SDL_LOG_CATEGORY_APPLICATION, "Connection failed with code: %d\n", e);
            goto error;
        }

        /* wait until connection is established. */
        SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION, "Connect to Parsec host\n");
        while (!parsec_context.decoder) {

            /* get client status. */
            e = ParsecClientGetStatus(parsec_context.parsec, &parsec_context.client_status);

            /* connection established */
            if (e == PARSEC_OK) {

                /* decoder not yet initialized. */
                if (parsec_context.client_status.decoder[DEFAULT_STREAM].width == 0 &&
                    parsec_context.client_status.decoder[DEFAULT_STREAM].height == 0 &&
                    !vdi_stream_client__context_connected(&parsec_context)) {
                    SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION, "Initialize Video Decoder\n");
                    vdi_stream_client__context_set_connection(&parsec_context, true);
                }

                /* decoder initialized. */
                if (parsec_context.client_status.decoder[DEFAULT_STREAM].width > 0 &&
                    parsec_context.client_status.decoder[DEFAULT_STREAM].height > 0) {
                    SDL_LogInfo(
                        SDL_LOG_CATEGORY_APPLICATION, "Use %s codec\n",
                        parsec_context.client_status.decoder[DEFAULT_STREAM].h265 ? "H.265 (HEVC)"
                                                                                  : "H.264 (AVC)"
                    );
                    SDL_LogInfo(
                        SDL_LOG_CATEGORY_APPLICATION, "Use resolution %dx%d\n",
                        parsec_context.client_status.decoder[DEFAULT_STREAM].width,
                        parsec_context.client_status.decoder[DEFAULT_STREAM].height
                    );
                    parsec_context.window_width =
                        parsec_context.client_status.decoder[DEFAULT_STREAM].width;
                    parsec_context.window_height =
                        parsec_context.client_status.decoder[DEFAULT_STREAM].height;
                    parsec_context.decoder = true;
                }
            }

            /* unknown error. */
            if (e != PARSEC_CONNECTING && e != PARSEC_OK) {
                break;
            }

            /* check if timeout reached. */
            if (wait_time >= vdi_config->timeout) {
                break;
            }

            /* wait some time and re-check. */
            SDL_Delay(250);
            wait_time = wait_time + 250;
        }

        if (hevc_attempt_active && !h264_fallback_done && e != PARSEC_CONNECTING &&
            e != PARSEC_OK && !parsec_context.decoder) {
            SDL_LogWarn(
                SDL_LOG_CATEGORY_APPLICATION,
                "FFmpeg H.265 decoder failed, retry FFmpeg H.264 (AVC) fallback\n"
            );
            ParsecClientDisconnect(parsec_context.parsec);
            vdi_stream_client__use_h264_fallback(&cfg, &hevc_attempt_active, &h264_fallback_done);
            continue;
        }

        break;
    }

    /* detect sdl video driver. */
    video_driver = SDL_GetCurrentVideoDriver();
    SDL_LogInfo(
        SDL_LOG_CATEGORY_APPLICATION, "Use %s video\n",
        video_driver != NULL ? video_driver : "unknown"
    );

    /* check if connected and decoder initialized. */
    if (!parsec_context.decoder) {
        SDL_LogError(SDL_LOG_CATEGORY_APPLICATION, "Connection failed with code: %d\n", e);
        goto error;
    }

    hardware_decoding = vdi_stream_client__parsec_ffmpeg_decoder_is_hardware();
    window_flags |= vdi_stream_client__video_window_flags(hardware_decoding);
    if (!vdi_stream_client__video_setup(&parsec_context, window_flags, hardware_decoding)) {
        if ((window_flags & SDL_WINDOW_VULKAN) == 0) {
            goto error;
        }

        SDL_LogWarn(
            SDL_LOG_CATEGORY_APPLICATION,
            "Vulkan video setup failed; retry with default SDL renderer\n"
        );
        window_flags &= ~SDL_WINDOW_VULKAN;
        if (!vdi_stream_client__video_setup(&parsec_context, window_flags, false)) {
            goto error;
        }
    }

    /* check if audio should be streamed. */
    if (vdi_config->audio == 1) {
        want.freq = PARSEC_AUDIO_SAMPLE_RATE;
        want.format = SDL_AUDIO_S16;
        want.channels = PARSEC_AUDIO_CHANNELS;

        /* the number of audio packets (960 frames) to buffer before we begin playing. */
        parsec_context.min_buffer = 1;

        /* the number of audio packets (960 frames) to buffer before overflow and clear. */
        parsec_context.max_buffer = 6;

        /* sdl audio device. */
        parsec_context.audio =
            SDL_OpenAudioDeviceStream(SDL_AUDIO_DEVICE_DEFAULT_PLAYBACK, &want, NULL, NULL);
        if (parsec_context.audio == NULL) {
            SDL_LogError(
                SDL_LOG_CATEGORY_APPLICATION, "Failed to open audio: %s\n", SDL_GetError()
            );
            goto error;
        }

        /* sdl audio thread. */
        audio_thread = SDL_CreateThread(
            vdi_stream_client__audio_thread, "vdi_stream_client__audio_thread", &parsec_context
        );
        if (audio_thread == NULL) {
            SDL_LogError(
                SDL_LOG_CATEGORY_APPLICATION, "Audio thread creation failed: %s\n", SDL_GetError()
            );
            goto error;
        }
    }

    /* configure usb. */
    if (vdi_config->usb_count > 0) {
        SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION, "Initialize USB\n");

        /* one thread per one usb device redirect. */
        for (device = 0; device < vdi_config->usb_count; device++) {

            /* store main thread context in a pointer. */
            redirect_context[device].parsec_context = &parsec_context;

            /* prepare data for network thread. */
            redirect_context[device].server_addr.v4 = vdi_config->server_addrs[device].v4;
            redirect_context[device].server_addr.v6 = vdi_config->server_addrs[device].v6;
            redirect_context[device].usb_device.vendor = vdi_config->usb_devices[device].vendor;
            redirect_context[device].usb_device.product = vdi_config->usb_devices[device].product;

            /* sdl network thread. */
            network_thread[device] = SDL_CreateThread(
                vdi_stream_client__network_thread, "vdi_stream_client__network_thread",
                &redirect_context[device]
            );
            if (network_thread[device] == NULL) {
                SDL_LogError(
                    SDL_LOG_CATEGORY_APPLICATION, "Network thread creation failed: %s\n",
                    SDL_GetError()
                );
                goto error;
            }
        }
    }

    /* sdl input thread. SDL events are pumped on the main thread and handled
     * by this worker without running main-thread-only window APIs there. */
    if (!vdi_stream_client__input_init(&input_context, &parsec_context, vdi_config)) {
        goto error;
    }
    input_thread = SDL_CreateThread(
        vdi_stream_client__input_thread, "vdi_stream_client__input_thread", &input_context
    );
    if (input_thread == NULL) {
        SDL_LogError(
            SDL_LOG_CATEGORY_APPLICATION, "Input thread creation failed: %s\n", SDL_GetError()
        );
        goto error;
    }

    /* event loop. */
    while (!vdi_stream_client__context_done(&parsec_context)) {
        Uint64 loop_sdl_events = 0;
        Uint64 loop_parsec_events = 0;
        Uint64 loop_frames = 0;
        Uint64 loop_presents = 0;
        Uint64 idle_start = 0;
        bool local_interaction = false;
        bool rendered = false;

        force_redraw = vdi_stream_client__context_input_force_redraw(&parsec_context);
        if (parsec_context.stats_enabled) {
            loop_sdl_events = (Uint64)atomic_load_explicit(
                &parsec_context.stats_sdl_events, memory_order_relaxed
            );
            loop_parsec_events = parsec_context.stats_parsec_events;
            loop_frames = parsec_context.stats_frames;
            loop_presents = parsec_context.stats_presents;
            parsec_context.stats_loops++;
        }

        SDL_PumpEvents();
        local_interaction = vdi_stream_client__context_input_local_interaction(&parsec_context);
        vdi_stream_client__handle_input_commands(&input_context, &force_redraw);

        /* prioritize SDL responsiveness after local interaction without forcing
         * redundant presents of the last video frame. */
        parsec_context.render_timeout = local_interaction ? 0 : 5;

        vdi_stream_client__handle_connection_status(
            &parsec_context, vdi_config, &cfg, &last_time, &force_redraw, &hevc_attempt_active,
            &h264_fallback_done
        );

        for (ParsecClientEvent event; ParsecClientPollEvents(parsec_context.parsec, 0, &event);) {
            if (parsec_context.stats_enabled) {
                parsec_context.stats_parsec_events++;
            }

            switch (event.type) {
            case CLIENT_EVENT_CURSOR:
                vdi_stream_client__cursor(
                    &parsec_context, &event.cursor.cursor, event.cursor.key, vdi_config->grab,
                    vdi_stream_client__context_input_grab_forced(&parsec_context)
                );
                break;
            case CLIENT_EVENT_USER_DATA:
                if (vdi_config->clipboard == 1) {
                    vdi_stream_client__clipboard(
                        &parsec_context, event.userData.id, event.userData.key
                    );
                }
                break;
            default:
                break;
            }
        }

        if (parsec_context.stats_enabled) {
            idle_start = SDL_GetTicks();
        }
        rendered = vdi_stream_client__video_render(&parsec_context, force_redraw);

        /* check if we need to resize window due to client resolution change. */
        if ((parsec_context.window_width !=
                 parsec_context.client_status.decoder[DEFAULT_STREAM].width ||
             parsec_context.window_height !=
                 parsec_context.client_status.decoder[DEFAULT_STREAM].height) &&
            parsec_context.client_status.decoder[DEFAULT_STREAM].width > 0 &&
            parsec_context.client_status.decoder[DEFAULT_STREAM].height > 0) {
            SDL_LogInfo(
                SDL_LOG_CATEGORY_APPLICATION, "Change resolution from %dx%d to %dx%d\n",
                parsec_context.window_width, parsec_context.window_height,
                parsec_context.client_status.decoder[DEFAULT_STREAM].width,
                parsec_context.client_status.decoder[DEFAULT_STREAM].height
            );
            vdi_stream_client__window_unlock_size(parsec_context.window);
            vdi_stream_client__window_enforce_size(
                parsec_context.window, parsec_context.client_status.decoder[DEFAULT_STREAM].width,
                parsec_context.client_status.decoder[DEFAULT_STREAM].height
            );
            parsec_context.window_width =
                parsec_context.client_status.decoder[DEFAULT_STREAM].width;
            parsec_context.window_height =
                parsec_context.client_status.decoder[DEFAULT_STREAM].height;
        }

        /* Do not add a blanket SDL_Delay(1) to the streaming hot path. When
         * connected, ParsecClientPollFrame(timeout) and SDL_RenderPresent(vsync)
         * are the pacing points. Sleeping here adds latency and can cap/jitter
         * frame delivery. When disconnected, no frame source is blocking this
         * loop, so keep a small coarse idle sleep to avoid busy-spinning while
         * reconnecting or showing the overlay. */
        if (!vdi_stream_client__context_connected(&parsec_context) && !rendered &&
            parsec_context.render_timeout > 0) {
            SDL_Delay(parsec_context.render_timeout);
        }

        if (parsec_context.stats_enabled &&
            (Uint64)atomic_load_explicit(&parsec_context.stats_sdl_events, memory_order_relaxed) ==
                loop_sdl_events &&
            parsec_context.stats_parsec_events == loop_parsec_events &&
            parsec_context.stats_frames == loop_frames &&
            parsec_context.stats_presents == loop_presents && !rendered) {
            parsec_context.stats_idle_waits++;
            parsec_context.stats_idle_wait_ms += SDL_GetTicks() - idle_start;
        }

        vdi_stream_client__render_stats(&parsec_context);
    }

    /* already release any grabbed keyboard because thread termination can take some time. */
    SDL_SetWindowMouseGrab(parsec_context.window, false);
    SDL_SetWindowKeyboardGrab(parsec_context.window, false);

    vdi_stream_client__stop_threads(
        &parsec_context, &input_thread, &audio_thread, network_thread, vdi_config->usb_count
    );
    vdi_stream_client__input_destroy(&input_context);

    /* destroy video resources before releasing the parsec client. */
    vdi_stream_client__video_destroy(&parsec_context);

    /* parsec destroy. */
    ParsecDestroy(parsec_context.parsec);

    /* ttf destroy. */
    TTF_CloseFont(parsec_context.font);
    TTF_Quit();

    /* sdl destroy. */
    if (vdi_config->audio == 1) {
        SDL_DestroyAudioStream(parsec_context.audio);
    }
    SDL_DestroySurface(parsec_context.surface_ttf);
    SDL_DestroyWindow(parsec_context.window);
    SDL_Quit();

    /* terminate loop. */
    return VDI_STREAM_CLIENT_SUCCESS;

error:

    vdi_stream_client__stop_threads(
        &parsec_context, &input_thread, &audio_thread, network_thread, vdi_config->usb_count
    );
    vdi_stream_client__input_destroy(&input_context);

    /* destroy video resources before releasing the parsec client. */
    vdi_stream_client__video_destroy(&parsec_context);

    /* parsec destroy. */
    ParsecDestroy(parsec_context.parsec);

    /* ttf destroy. */
    TTF_CloseFont(parsec_context.font);
    TTF_Quit();

    /* sdl destroy. */
    if (vdi_config->audio == 1) {
        SDL_DestroyAudioStream(parsec_context.audio);
    }
    SDL_DestroySurface(parsec_context.surface_ttf);
    SDL_DestroyWindow(parsec_context.window);
    SDL_Quit();

    /* return with error. */
    return VDI_STREAM_CLIENT_ERROR;
}
