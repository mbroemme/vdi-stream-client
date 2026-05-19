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

/* internal includes. */
#include "parsec.h"
#include "audio.h"
#include "client.h"
#include "redirect.h"
#include "video.h"

/* font include. */
#include "../include/font.h"

/* system includes. */
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>

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

    if (!parsec_context->stats_enabled) {
        return;
    }

    now = SDL_GetTicks();
    Uint64 last_frame_age_ms = parsec_context->stats_last_frame_tick == 0
                                   ? 0
                                   : now - parsec_context->stats_last_frame_tick;

    if (parsec_context->stats_next_tick == 0) {
        parsec_context->stats_next_tick = now + parsec_context->stats_period_ms;
        return;
    }

    if (now < parsec_context->stats_next_tick) {
        return;
    }

    SDL_LogInfo(
        SDL_LOG_CATEGORY_APPLICATION,
        "Render:\n"
        "  loop: loops=%llu, presents=%llu\n"
        "  events: sdl=%llu, parsec=%llu\n"
        "  frames: frames=%llu, age=%llums\n"
        "  idle: waits=%llu, ms=%llu\n",
        (unsigned long long)parsec_context->stats_loops,
        (unsigned long long)parsec_context->stats_presents,
        (unsigned long long)parsec_context->stats_sdl_events,
        (unsigned long long)parsec_context->stats_parsec_events,
        (unsigned long long)parsec_context->stats_frames, (unsigned long long)last_frame_age_ms,
        (unsigned long long)parsec_context->stats_idle_waits,
        (unsigned long long)parsec_context->stats_idle_wait_ms
    );

    parsec_context->stats_next_tick = now + parsec_context->stats_period_ms;
    parsec_context->stats_loops = 0;
    parsec_context->stats_sdl_events = 0;
    parsec_context->stats_parsec_events = 0;
    parsec_context->stats_frames = 0;
    parsec_context->stats_presents = 0;
    parsec_context->stats_idle_waits = 0;
    parsec_context->stats_idle_wait_ms = 0;
}

/* parsec cursor event. */
static void
vdi_stream_client__cursor(
    struct parsec_context_s *parsec_context, ParsecCursor *cursor, Uint32 buffer_key, bool grab,
    bool grab_forced
)
{
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

    if (!SDL_GetWindowRelativeMouseMode(parsec_context->window) &&
        (cursor->relative || cursor->hidden)) {
        SDL_HideCursor();
        SDL_SetWindowRelativeMouseMode(parsec_context->window, true);
        if (!parsec_context->pressed && !grab_forced) {
            SDL_SetWindowTitle(
                parsec_context->window, "VDI Stream Client (Press Ctrl+Alt to release grab)"
            );
        }
        parsec_context->relative = cursor->relative;
    } else if (SDL_GetWindowRelativeMouseMode(parsec_context->window) &&
               (!cursor->relative || !cursor->hidden)) {
        SDL_SetWindowRelativeMouseMode(parsec_context->window, false);
        SDL_ShowCursor();
        if (!parsec_context->pressed && !grab_forced && !grab) {
            SDL_SetWindowTitle(parsec_context->window, "VDI Stream Client");
        }
        parsec_context->relative = cursor->relative;
    }
}

/* render text. */
Sint32
vdi_stream_client__render_text(void *opaque, char *text)
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

/* parsec event loop. */
Sint32
vdi_stream_client__event_loop(struct vdi_config_s *vdi_config)
{
    struct parsec_context_s parsec_context = { 0 };
    struct redirect_context_s redirect_context[USB_MAX] = { 0 };
    bool grab_forced = false;
    Uint32 wait_time = 0;
    Uint64 last_time = 0;
    bool force_redraw = false;
    float x = 0.0f;
    float y = 0.0f;
    SDL_AudioSpec want = { 0 };
    ParsecStatus e;
    ParsecConfig network_cfg = PARSEC_DEFAULTS;
    ParsecClientConfig cfg = PARSEC_CLIENT_DEFAULTS;
    Uint32 device;
    Uint32 count;
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

    /* parsec init. */
    SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION, "Initialize Parsec\n");
#ifdef HAVE_LIBPARSEC
    e = ParsecInit(PARSEC_VER, &network_cfg, NULL, &parsec_context.parsec);
#else
    e = ParsecInit(NULL, &network_cfg, "libparsec.so", &parsec_context.parsec);
#endif
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
    if (vdi_config->acceleration == 1) {
        cfg.video[DEFAULT_STREAM].decoderIndex = 1;
    }
    if (vdi_config->acceleration == 0) {
        SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION, "Disable Hardware Accelerated Video Decoding\n");
        cfg.video[DEFAULT_STREAM].decoderIndex = 0;
    }

    /* configure upnp. */
    if (vdi_config->upnp == 1) {
        network_cfg.upnp = 1;
    }
    if (vdi_config->upnp == 0) {
        SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION, "Disable UPnP\n");
        network_cfg.upnp = 0;
    }

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
                !parsec_context.connection) {
                SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION, "Initialize Video Decoder\n");
                parsec_context.connection = true;
            }

            /* decoder initialized. */
            if (parsec_context.client_status.decoder[DEFAULT_STREAM].width > 0 &&
                parsec_context.client_status.decoder[DEFAULT_STREAM].height > 0) {
                SDL_LogInfo(
                    SDL_LOG_CATEGORY_APPLICATION, "Use %s decoder\n",
                    parsec_context.client_status.decoder[DEFAULT_STREAM].name[0] != '\0'
                        ? parsec_context.client_status.decoder[DEFAULT_STREAM].name
                        : "unknown"
                );
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

    /* detect sdl video driver. */
    video_driver = SDL_GetCurrentVideoDriver();
    SDL_LogInfo(
        SDL_LOG_CATEGORY_APPLICATION, "Use %s video\n",
        video_driver != NULL ? video_driver : "unknown"
    );

    /* check if connected and decoder initialized. */
    if (!parsec_context.connection && !parsec_context.decoder) {
        SDL_LogError(SDL_LOG_CATEGORY_APPLICATION, "Connection failed with code: %d\n", e);
        goto error;
    }

    parsec_context.window = SDL_CreateWindow(
        "VDI Stream Client", parsec_context.window_width, parsec_context.window_height, window_flags
    );
    if (parsec_context.window == NULL) {
        SDL_LogError(SDL_LOG_CATEGORY_APPLICATION, "Window creation failed: %s\n", SDL_GetError());
        goto error;
    }

    parsec_context.renderer = SDL_CreateRenderer(parsec_context.window, NULL);
    if (parsec_context.renderer == NULL) {
        SDL_LogError(
            SDL_LOG_CATEGORY_APPLICATION, "Renderer creation failed: %s\n", SDL_GetError()
        );
        goto error;
    }

    vdi_stream_client__video_init(&parsec_context);

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
    if (vdi_config->usb_devices[0].vendor != 0) {
        SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION, "Initialize USB\n");

        /* one thread per one usb device redirect. */
        for (device = 0; vdi_config->usb_devices[device].vendor != 0; device++) {

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

    /* event loop. */
    while (!parsec_context.done) {
        Uint64 loop_sdl_events = 0;
        Uint64 loop_parsec_events = 0;
        Uint64 loop_frames = 0;
        Uint64 loop_presents = 0;
        Uint64 idle_start = 0;
        bool local_interaction = false;
        bool rendered = false;

        force_redraw = false;
        if (parsec_context.stats_enabled) {
            loop_sdl_events = parsec_context.stats_sdl_events;
            loop_parsec_events = parsec_context.stats_parsec_events;
            loop_frames = parsec_context.stats_frames;
            loop_presents = parsec_context.stats_presents;
            parsec_context.stats_loops++;
        }

        for (SDL_Event msg; SDL_PollEvent(&msg);) {
            if (parsec_context.stats_enabled) {
                parsec_context.stats_sdl_events++;
            }
            ParsecMessage pmsg = { 0 };
            local_interaction = true;

            /* Only the overlay needs a forced redraw for arbitrary SDL events; the
             * connected video path should present on new frames instead of repainting
             * the same texture for every keyboard/mouse event. */
            if (!parsec_context.connection) {
                force_redraw = true;
            }

            switch (msg.type) {
            case SDL_EVENT_QUIT:
                parsec_context.done = true;

                /* render shutdown text. */
                vdi_stream_client__render_text(&parsec_context, "Closing...");
                force_redraw = true;
                break;
            case SDL_EVENT_KEY_UP:
                pmsg.type = MESSAGE_KEYBOARD;
                pmsg.keyboard.code = (ParsecKeycode)msg.key.scancode;
                pmsg.keyboard.mod = msg.key.mod;
                pmsg.keyboard.pressed = false;
                break;
            case SDL_EVENT_KEY_DOWN:

                /* check if we need to switch window grab state. */
                if ((msg.key.mod & SDL_KMOD_LCTRL) != 0 && (msg.key.mod & SDL_KMOD_LALT) != 0) {

                    /* check if no forced grab. */
                    if (!grab_forced) {

                        /* check if no mouse button is hold down. */
                        SDL_MouseButtonFlags mouse_buttons = SDL_GetMouseState(NULL, NULL);
                        if ((mouse_buttons & SDL_BUTTON_LMASK) == 0 &&
                            (mouse_buttons & SDL_BUTTON_MMASK) == 0 &&
                            (mouse_buttons & SDL_BUTTON_RMASK) == 0) {

                            /* check if we need to release mouse grab. */
                            if (vdi_config->grab == 1 &&
                                SDL_GetWindowMouseGrab(parsec_context.window)) {
                                SDL_SetWindowMouseGrab(parsec_context.window, false);
                            }

                            /* check if we need to release relative grab. */
                            if (SDL_GetWindowRelativeMouseMode(parsec_context.window)) {
                                SDL_SetWindowRelativeMouseMode(parsec_context.window, false);
                                SDL_ShowCursor();
                            }

                            /* remove grab information from window title. */
                            SDL_SetWindowTitle(parsec_context.window, "VDI Stream Client");

                            /* don't send hotkey to host and break execution. */
                            break;
                        }
                    }
                }

                /* check if we need to toggle runtime configuration. */
                if ((msg.key.mod & SDL_KMOD_LSHIFT) != 0) {

                    /* check if we need to toggle forced grab. */
                    if (msg.key.key == SDLK_F12) {
                        if (grab_forced) {

                            /* re-enable screensaver if leaving forced lock. */
                            if (vdi_config->screensaver == 1) {
                                SDL_EnableScreenSaver();
                            }

                            /* check if we need to release relative grab. */
                            if (SDL_GetWindowRelativeMouseMode(parsec_context.window)) {
                                SDL_SetWindowRelativeMouseMode(parsec_context.window, false);
                                SDL_ShowCursor();
                            }

                            SDL_SetWindowMouseGrab(parsec_context.window, false);
                            SDL_SetWindowTitle(parsec_context.window, "VDI Stream Client");
                            grab_forced = false;
                        } else {

                            /* disable screensaver if entering forced lock. */
                            if (vdi_config->screensaver == 1) {
                                SDL_DisableScreenSaver();
                            }

                            /* check if we need to grab mouse in relative mode. */
                            if (parsec_context.relative &&
                                !SDL_GetWindowRelativeMouseMode(parsec_context.window)) {
                                SDL_HideCursor();
                                SDL_SetWindowRelativeMouseMode(parsec_context.window, true);
                            }

                            SDL_SetWindowMouseGrab(parsec_context.window, true);
                            SDL_SetWindowTitle(
                                parsec_context.window,
                                "VDI Stream Client (Press Shift+F12 to release forced grab)"
                            );
                            grab_forced = true;
                        }

                        /* don't send hotkey to host and break execution. */
                        break;
                    }
                }

                pmsg.type = MESSAGE_KEYBOARD;
                pmsg.keyboard.code = (ParsecKeycode)msg.key.scancode;
                pmsg.keyboard.mod = msg.key.mod;
                pmsg.keyboard.pressed = true;
                break;
            case SDL_EVENT_MOUSE_MOTION:
            {
                bool relative_mouse = SDL_GetWindowRelativeMouseMode(parsec_context.window);

                /* check if we released relative mouse grab. */
                if (parsec_context.relative && !relative_mouse) {

                    /* no mouse motion events should be forwarded. */
                    break;
                }

                pmsg.type = MESSAGE_MOUSE_MOTION;
                pmsg.mouseMotion.relative = relative_mouse;
                pmsg.mouseMotion.x =
                    relative_mouse ? (Sint32)msg.motion.xrel : (Sint32)msg.motion.x + 1;
                pmsg.mouseMotion.y =
                    relative_mouse ? (Sint32)msg.motion.yrel : (Sint32)msg.motion.y + 1;
                break;
            }
            case SDL_EVENT_MOUSE_BUTTON_UP:

                /* store mouse button state for use in cursor update. */
                parsec_context.pressed = false;

                pmsg.type = MESSAGE_MOUSE_BUTTON;
                pmsg.mouseButton.button = msg.button.button;
                pmsg.mouseButton.pressed = false;
                break;
            case SDL_EVENT_MOUSE_BUTTON_DOWN:

                /* check if no forced grab. */
                if (!grab_forced) {

                    /* check if we need to grab mouse. */
                    if (vdi_config->grab == 1 && !SDL_GetWindowMouseGrab(parsec_context.window)) {
                        SDL_SetWindowMouseGrab(parsec_context.window, true);
                        SDL_SetWindowTitle(
                            parsec_context.window,
                            "VDI Stream Client (Press Ctrl+Alt to release grab)"
                        );
                    }

                    /* check if we need to grab mouse in relative mode. */
                    if (parsec_context.relative &&
                        !SDL_GetWindowRelativeMouseMode(parsec_context.window)) {
                        SDL_HideCursor();
                        SDL_SetWindowRelativeMouseMode(parsec_context.window, true);
                        SDL_SetWindowTitle(
                            parsec_context.window,
                            "VDI Stream Client (Press Ctrl+Alt to release grab)"
                        );
                    }
                }

                /* store mouse button state for use in cursor update. */
                parsec_context.pressed = true;

                pmsg.type = MESSAGE_MOUSE_BUTTON;
                pmsg.mouseButton.button = msg.button.button;
                pmsg.mouseButton.pressed = true;
                break;
            case SDL_EVENT_MOUSE_WHEEL:
                pmsg.type = MESSAGE_MOUSE_WHEEL;
                pmsg.mouseWheel.x = msg.wheel.x * vdi_config->speed;
                pmsg.mouseWheel.y = msg.wheel.y * vdi_config->speed;
                break;
            case SDL_EVENT_CLIPBOARD_UPDATE:
                if (vdi_config->clipboard == 1) {
                    char *clipboard = SDL_GetClipboardText();

                    if (clipboard != NULL) {
                        ParsecClientSendUserData(
                            parsec_context.parsec, PARSEC_CLIPBOARD_MSG, clipboard
                        );
                        SDL_free(clipboard);
                    }
                }
                break;
            case SDL_EVENT_WINDOW_MOUSE_ENTER:
                SDL_SetEventEnabled(SDL_EVENT_KEY_DOWN, true);
                SDL_SetEventEnabled(SDL_EVENT_KEY_UP, true);
                SDL_SetWindowKeyboardGrab(parsec_context.window, true);
                break;
            case SDL_EVENT_WINDOW_MOUSE_LEAVE:
                SDL_SetEventEnabled(SDL_EVENT_KEY_DOWN, false);
                SDL_SetEventEnabled(SDL_EVENT_KEY_UP, false);
                SDL_SetWindowKeyboardGrab(parsec_context.window, false);
                break;
            }

            if (pmsg.type != 0) {
                ParsecClientSendMessage(parsec_context.parsec, &pmsg);
            }
        }

        /* prioritize SDL responsiveness after local interaction without forcing
         * redundant presents of the last video frame. */
        parsec_context.render_timeout = local_interaction ? 0 : 5;

        /* check parsec connection status. */
        e = ParsecClientGetStatus(parsec_context.parsec, &parsec_context.client_status);
        if (vdi_config->reconnect == 0 && e != PARSEC_CONNECTING && e != PARSEC_OK) {

            /* render shutdown text. */
            vdi_stream_client__render_text(&parsec_context, "Closing...");
            force_redraw = true;
            SDL_LogError(SDL_LOG_CATEGORY_APPLICATION, "Parsec disconnected\n");
            parsec_context.done = true;
        }
        if (vdi_config->reconnect == 1 && e != PARSEC_CONNECTING && e != PARSEC_OK &&
            SDL_GetTicks() > last_time + vdi_config->timeout) {

            /* render reconnect text. */
            vdi_stream_client__render_text(&parsec_context, "Reconnecting...");
            force_redraw = true;
            ParsecClientDisconnect(parsec_context.parsec);
            ParsecClientConnect(parsec_context.parsec, &cfg, vdi_config->session, vdi_config->peer);
            parsec_context.connection = false;
            last_time = SDL_GetTicks();
        }

        /* check network connection status. */
        if (vdi_config->reconnect == 0 && parsec_context.client_status.networkFailure == 1) {

            /* render shutdown text. */
            vdi_stream_client__render_text(&parsec_context, "Closing...");
            force_redraw = true;
            SDL_LogError(SDL_LOG_CATEGORY_APPLICATION, "Network disconnected\n");
            parsec_context.done = true;
        }
        if (vdi_config->reconnect == 1 && parsec_context.client_status.networkFailure == 1 &&
            SDL_GetTicks() > last_time + vdi_config->timeout) {

            /* render reconnect text. */
            vdi_stream_client__render_text(&parsec_context, "Reconnecting...");
            force_redraw = true;
            ParsecClientDisconnect(parsec_context.parsec);
            ParsecClientConnect(parsec_context.parsec, &cfg, vdi_config->session, vdi_config->peer);
            parsec_context.connection = false;
            last_time = SDL_GetTicks();
        }

        /* set connection status if reconnected. */
        if (vdi_config->reconnect == 1 && parsec_context.client_status.networkFailure == 0 &&
            e == PARSEC_OK && !parsec_context.connection) {
            parsec_context.connection = true;
        }

        for (ParsecClientEvent event; ParsecClientPollEvents(parsec_context.parsec, 0, &event);) {
            if (parsec_context.stats_enabled) {
                parsec_context.stats_parsec_events++;
            }

            switch (event.type) {
            case CLIENT_EVENT_CURSOR:
                vdi_stream_client__cursor(
                    &parsec_context, &event.cursor.cursor, event.cursor.key, vdi_config->grab,
                    grab_forced
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
            SDL_SetWindowSize(
                parsec_context.window, parsec_context.client_status.decoder[DEFAULT_STREAM].width,
                parsec_context.client_status.decoder[DEFAULT_STREAM].height
            );
            SDL_SyncWindow(parsec_context.window);
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
        if (!parsec_context.connection && !rendered && parsec_context.render_timeout > 0) {
            SDL_Delay(parsec_context.render_timeout);
        }

        if (parsec_context.stats_enabled && parsec_context.stats_sdl_events == loop_sdl_events &&
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

    /* stop network threads for usb redirection. */
    if (vdi_config->usb_devices[0].vendor != 0) {
        SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION, "Stop Network Thread\n");
        for (count = 0; count < USB_MAX; count++) {
            if (network_thread[count] == NULL) {
                continue;
            }
            SDL_WaitThread(network_thread[count], NULL);
        }
    }

    /* stop audio thread. */
    if (vdi_config->audio == 1) {
        SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION, "Stop Audio Thread\n");
        SDL_WaitThread(audio_thread, NULL);
    }

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
