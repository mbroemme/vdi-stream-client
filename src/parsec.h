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
#include <stdatomic.h>
#include <stdbool.h>
#include <stdint.h>

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
struct vdi_stream_client__placebo_s;

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
    atomic_bool done;
    atomic_bool connection;
    atomic_bool input_polling;
    atomic_bool input_local_interaction;
    atomic_bool input_force_redraw;
    atomic_bool input_relative;
    atomic_bool input_relative_mouse;
    atomic_bool input_pressed;
    atomic_bool input_grab_forced;
    bool decoder;
    bool focus;
    bool hidden;
    bool hidden_drag;
    bool relative;
    bool cursor_grab;
#ifdef HAVE_LIBPARSEC
    Parsec *parsec;
#else
    ParsecDSO *parsec;
#endif
    ParsecClientStatus client_status;
    ParsecStatus stream_error;

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
    char overlay_text[32];
    SDL_Texture *texture_video;
    SDL_Texture *frame_video_texture;
    struct vdi_stream_client__placebo_s *placebo;
    bool frame_video_updated;
    SDL_PixelFormat pixel_format_video;
    Sint32 texture_width;
    Sint32 texture_height;
    TTF_Font *font;

    /* audio. */
    SDL_AudioStream *audio;
    atomic_bool playing;
    atomic_bool audio_polling;
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
    atomic_uint_fast64_t stats_sdl_events;
    Uint64 stats_parsec_events;
    Uint64 stats_frames;
    Uint64 stats_presents;
    Uint64 stats_uploads;
    Uint64 stats_upload_ns;
    Uint64 stats_renders;
    Uint64 stats_render_ns;
    Uint64 stats_present_calls;
    Uint64 stats_present_ns;
    Uint64 stats_zero_copy_calls;
    Uint64 stats_zero_copy_ns;
    Uint64 stats_zero_copy_fallbacks;
    Uint64 stats_idle_waits;
    Uint64 stats_idle_wait_ms;
};

/* Read the shared shutdown flag with acquire ordering so worker threads observe
 * prior shutdown-side state updates before leaving their loops. */
static inline bool
vdi_stream_client__context_done(struct parsec_context_s *parsec_context)
{
    return atomic_load_explicit(&parsec_context->done, memory_order_acquire);
}

/* Publish the shared shutdown flag with release ordering. All worker threads
 * use this as the common signal to stop polling external APIs. */
static inline void
vdi_stream_client__context_set_done(struct parsec_context_s *parsec_context, bool done)
{
    atomic_store_explicit(&parsec_context->done, done, memory_order_release);
}

/* Read whether the Parsec client is currently considered connected. Audio,
 * input, and rendering use this to decide whether to poll or show overlays. */
static inline bool
vdi_stream_client__context_connected(struct parsec_context_s *parsec_context)
{
    return atomic_load_explicit(&parsec_context->connection, memory_order_acquire);
}

/* Publish the current Parsec connection state for worker threads and the main
 * loop without requiring callers to manage atomic ordering themselves. */
static inline void
vdi_stream_client__context_set_connection(struct parsec_context_s *parsec_context, bool connection)
{
    atomic_store_explicit(&parsec_context->connection, connection, memory_order_release);
}

/* Read whether SDL audio playback is actively running. The audio thread uses
 * this to avoid redundant pause/resume calls around buffer thresholds. */
static inline bool
vdi_stream_client__context_playing(struct parsec_context_s *parsec_context)
{
    return atomic_load_explicit(&parsec_context->playing, memory_order_acquire);
}

/* Publish the current SDL audio playback state after the audio stream is paused
 * or resumed. */
static inline void
vdi_stream_client__context_set_playing(struct parsec_context_s *parsec_context, bool playing)
{
    atomic_store_explicit(&parsec_context->playing, playing, memory_order_release);
}

/* Read whether the audio thread is inside a Parsec audio poll. Reconnect waits
 * for this to clear before disconnecting the shared Parsec client. */
static inline bool
vdi_stream_client__context_audio_polling(struct parsec_context_s *parsec_context)
{
    return atomic_load_explicit(&parsec_context->audio_polling, memory_order_acquire);
}

/* Mark the short section where the audio thread may call into the Parsec client
 * so reconnect and shutdown can avoid racing that API use. */
static inline void
vdi_stream_client__context_set_audio_polling(
    struct parsec_context_s *parsec_context, bool audio_polling
)
{
    atomic_store_explicit(&parsec_context->audio_polling, audio_polling, memory_order_release);
}

/* Read whether the input thread is inside a Parsec input send. Reconnect waits
 * on this flag before disconnecting the shared client. */
static inline bool
vdi_stream_client__context_input_polling(struct parsec_context_s *parsec_context)
{
    return atomic_load_explicit(&parsec_context->input_polling, memory_order_acquire);
}

/* Mark the short section where the input thread may send a message through the
 * Parsec client. */
static inline void
vdi_stream_client__context_set_input_polling(
    struct parsec_context_s *parsec_context, bool input_polling
)
{
    atomic_store_explicit(&parsec_context->input_polling, input_polling, memory_order_release);
}

/* Consume and clear the local-interaction marker. The render loop uses the
 * one-shot signal to temporarily reduce frame-poll latency after SDL input. */
static inline bool
vdi_stream_client__context_input_local_interaction(struct parsec_context_s *parsec_context)
{
    return atomic_exchange_explicit(
        &parsec_context->input_local_interaction, false, memory_order_acq_rel
    );
}

/* Set the one-shot local-interaction marker when the input thread receives any
 * SDL event that should make the next render loop iteration more responsive. */
static inline void
vdi_stream_client__context_set_input_local_interaction(struct parsec_context_s *parsec_context)
{
    atomic_store_explicit(&parsec_context->input_local_interaction, true, memory_order_release);
}

/* Consume and clear the force-redraw marker. It lets worker-side events request
 * a main-thread redraw without repeatedly presenting the same frame. */
static inline bool
vdi_stream_client__context_input_force_redraw(struct parsec_context_s *parsec_context)
{
    return atomic_exchange_explicit(
        &parsec_context->input_force_redraw, false, memory_order_acq_rel
    );
}

/* Set the one-shot redraw marker when input events change visible state while
 * no new video frame may arrive, such as during reconnect overlays. */
static inline void
vdi_stream_client__context_set_input_force_redraw(struct parsec_context_s *parsec_context)
{
    atomic_store_explicit(&parsec_context->input_force_redraw, true, memory_order_release);
}

/* Read whether the remote cursor currently requests relative motion semantics.
 * The input thread uses this to decide how mouse motion should be encoded. */
static inline bool
vdi_stream_client__context_input_relative(struct parsec_context_s *parsec_context)
{
    return atomic_load_explicit(&parsec_context->input_relative, memory_order_acquire);
}

/* Publish whether the remote cursor state is relative so input event handling
 * can encode motion before the main thread sees the next cursor event. */
static inline void
vdi_stream_client__context_set_input_relative(
    struct parsec_context_s *parsec_context, bool relative
)
{
    atomic_store_explicit(&parsec_context->input_relative, relative, memory_order_release);
}

/* Read whether SDL relative mouse mode is currently enabled on the window. This
 * distinguishes remote relative cursor state from local window capture state. */
static inline bool
vdi_stream_client__context_input_relative_mouse(struct parsec_context_s *parsec_context)
{
    return atomic_load_explicit(&parsec_context->input_relative_mouse, memory_order_acquire);
}

/* Publish the actual SDL relative mouse mode after the main thread attempts to
 * enable or disable it. */
static inline void
vdi_stream_client__context_set_input_relative_mouse(
    struct parsec_context_s *parsec_context, bool relative_mouse
)
{
    atomic_store_explicit(
        &parsec_context->input_relative_mouse, relative_mouse, memory_order_release
    );
}

/* Read whether a mouse button is currently pressed. Cursor grab logic uses this
 * to keep hidden-cursor drags captured until the drag ends. */
static inline bool
vdi_stream_client__context_input_pressed(struct parsec_context_s *parsec_context)
{
    return atomic_load_explicit(&parsec_context->input_pressed, memory_order_acquire);
}

/* Publish the current mouse-button pressed state from the input thread for use
 * by cursor and grab handling on the main thread. */
static inline void
vdi_stream_client__context_set_input_pressed(struct parsec_context_s *parsec_context, bool pressed)
{
    atomic_store_explicit(&parsec_context->input_pressed, pressed, memory_order_release);
}

/* Read whether the user has forced mouse and keyboard grab mode. This overrides
 * automatic grab release rules until toggled off. */
static inline bool
vdi_stream_client__context_input_grab_forced(struct parsec_context_s *parsec_context)
{
    return atomic_load_explicit(&parsec_context->input_grab_forced, memory_order_acquire);
}

/* Publish forced-grab state after the input thread accepts the Shift+F12 toggle
 * and before the main thread applies the matching window state change. */
static inline void
vdi_stream_client__context_set_input_grab_forced(
    struct parsec_context_s *parsec_context, bool grab_forced
)
{
    atomic_store_explicit(&parsec_context->input_grab_forced, grab_forced, memory_order_release);
}

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
