/*
 *  audio.c -- audio rendering thread via sdl
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
#include "parsec.h"

/* system includes. */
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>

/* parsec audio event. */
static void
vdi_stream_client__audio(const Sint16 *pcm, Uint32 frames, void *opaque)
{
    struct parsec_context_s *parsec_context = (struct parsec_context_s *)opaque;
    int size = SDL_GetAudioStreamQueued(parsec_context->audio);
    Uint32 queued_frames;
    Uint32 queued_packets;

    if (size < 0) {
        SDL_LogError(
            SDL_LOG_CATEGORY_APPLICATION, "Failed to query queued audio: %s\n", SDL_GetError()
        );
        return;
    }

    queued_frames = (Uint32)size / (PARSEC_AUDIO_CHANNELS * sizeof(Sint16));
    queued_packets = queued_frames / PARSEC_AUDIO_FRAMES_PER_PACKET;

    if (vdi_stream_client__context_playing(parsec_context) &&
        queued_packets > parsec_context->max_buffer) {
        SDL_ClearAudioStream(parsec_context->audio);
        SDL_PauseAudioStreamDevice(parsec_context->audio);
        vdi_stream_client__context_set_playing(parsec_context, false);
    } else if (!vdi_stream_client__context_playing(parsec_context) &&
               queued_packets >= parsec_context->min_buffer) {
        SDL_ResumeAudioStreamDevice(parsec_context->audio);
        vdi_stream_client__context_set_playing(parsec_context, true);
    }

    if (!SDL_PutAudioStreamData(
            parsec_context->audio, pcm, frames * PARSEC_AUDIO_CHANNELS * sizeof(Sint16)
        )) {
        SDL_LogError(SDL_LOG_CATEGORY_APPLICATION, "Failed to queue audio: %s\n", SDL_GetError());
    }
}

/* sdl audio thread. */
Sint32
vdi_stream_client__audio_thread(void *opaque)
{
    struct parsec_context_s *parsec_context = (struct parsec_context_s *)opaque;

    while (!vdi_stream_client__context_done(parsec_context)) {

        /* poll audio only if connected. */
        if (vdi_stream_client__context_connected(parsec_context)) {
            ParsecClientPollAudio(
                parsec_context->parsec, vdi_stream_client__audio, 100, parsec_context
            );
        }

        /* delay loop if in reconnect state. */
        if (!vdi_stream_client__context_connected(parsec_context)) {

            /* clear queue and pause audio device. */
            if (vdi_stream_client__context_playing(parsec_context)) {
                SDL_ClearAudioStream(parsec_context->audio);
                SDL_PauseAudioStreamDevice(parsec_context->audio);
                vdi_stream_client__context_set_playing(parsec_context, false);
            }
            SDL_Delay(parsec_context->timeout);
        }
    }

    return VDI_STREAM_CLIENT_SUCCESS;
}
