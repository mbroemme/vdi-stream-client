/*
 *  parsec.h -- parsec default types and defines
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

#ifndef VDI_STREAM_CLIENT_PARSEC_H
#define VDI_STREAM_CLIENT_PARSEC_H

/* configuration includes. */
#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

/* system includes. */
#include <stdbool.h>

/* parsec includes. */
#ifdef HAVE_LIBPARSEC
#include <parsec/parsec.h>
#else
#include "../parsec-sdk/sdk/parsec-dso.h"
#endif

/* sdl includes. */
#include <SDL3/SDL.h>
#include <SDL3_ttf/SDL_ttf.h>

/* network includes. */
#include <arpa/inet.h>

/* forward declarations. */
struct vdi_config_s;

/* define audio defaults. */
#define PARSEC_AUDIO_CHANNELS 2
#define PARSEC_AUDIO_SAMPLE_RATE 48000
#define PARSEC_AUDIO_FRAMES_PER_PACKET 960

/* define parsec messages. */
#define PARSEC_CLIPBOARD_MSG 7

/* parsec configuration. */
struct parsec_context_s
{

    /* parsec. */
    bool done;
    bool connection;
    bool decoder;
    bool focus;
    bool hidden;
    bool hidden_drag;
    bool relative;
    bool cursor_grab;
    bool pressed;
#ifdef HAVE_LIBPARSEC
    Parsec *parsec;
#else
    ParsecDSO *parsec;
#endif
    ParsecClientStatus client_status;

    /* video. */
    SDL_Window *window;
    SDL_Renderer *renderer;
    SDL_Cursor *cursor;
    Sint32 window_width;
    Sint32 window_height;
    Sint32 requested_width;
    Sint32 requested_height;

    /* sdl textures for rendering. */
    SDL_Surface *surface_ttf;
    SDL_Texture *texture_ttf;
    SDL_Texture *texture_video;
    bool frame_video_updated;
    ParsecColorFormat format_video;
    Sint32 texture_width;
    Sint32 texture_height;
    TTF_Font *font;

    /* audio. */
    SDL_AudioStream *audio;
    bool playing;
    Uint32 min_buffer;
    Uint32 max_buffer;

    /* timeouts. */
    Uint32 timeout;
    Uint32 render_timeout;
    Uint64 next_overlay_tick;

    /* render stats. */
    Uint16 stats_enabled;
    Uint64 stats_period_ms;
    Uint64 stats_next_tick;
    Uint64 stats_last_frame_tick;
    Uint64 stats_loops;
    Uint64 stats_sdl_events;
    Uint64 stats_parsec_events;
    Uint64 stats_frames;
    Uint64 stats_presents;
    Uint64 stats_idle_waits;
    Uint64 stats_idle_wait_ms;
};

/* usb redirect. */
struct redirect_context_s
{

    /* parsec context. */
    struct parsec_context_s *parsec_context;

    /* network. */
    union
    {
        struct sockaddr_in v4;
        struct sockaddr_in6 v6;
    } server_addr;

    /* usb. */
    struct
    {
        Sint32 vendor;
        Sint32 product;
    } usb_device;
};

/* parsec event loop. */
Sint32 vdi_stream_client__event_loop(struct vdi_config_s *vdi_config);

#endif /* VDI_STREAM_CLIENT_PARSEC_H */
