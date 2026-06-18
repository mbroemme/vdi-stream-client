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

/* Initialize SDL audio output for Parsec PCM packets. When audio is disabled or
 * no playback device exists, the client continues without an audio stream. */
bool
vdi_stream_client__audio_init(struct parsec_context_s *parsec_context, bool enabled)
{
    SDL_AudioSpec want = { 0 };
    SDL_AudioDeviceID *devices;
    int device_count = 0;

    SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION, "Initialize Audio\n");
    if (!enabled) {
        SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION, "Disable audio streaming\n");
        return true;
    }

    devices = SDL_GetAudioPlaybackDevices(&device_count);
    if (devices == NULL) {
        SDL_LogError(
            SDL_LOG_CATEGORY_APPLICATION, "Failed to query audio devices: %s\n", SDL_GetError()
        );
        return false;
    }
    SDL_free(devices);

    if (device_count == 0) {
        SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION, "No audio device available\n");
        return true;
    }

    want.freq = PARSEC_AUDIO_SAMPLE_RATE;
    want.format = SDL_AUDIO_S16;
    want.channels = PARSEC_AUDIO_CHANNELS;

    /* The number of audio packets to buffer before playback and overflow. */
    parsec_context->min_buffer = 1;
    parsec_context->max_buffer = 6;
    parsec_context->audio =
        SDL_OpenAudioDeviceStream(SDL_AUDIO_DEVICE_DEFAULT_PLAYBACK, &want, NULL, NULL);
    if (parsec_context->audio == NULL) {
        SDL_LogError(SDL_LOG_CATEGORY_APPLICATION, "Failed to open audio: %s\n", SDL_GetError());
        return false;
    }

    return true;
}

/* Destroy the SDL audio stream and clear the shared context pointer so later
 * shutdown paths can call this safely after partial initialization failures. */
void
vdi_stream_client__audio_destroy(struct parsec_context_s *parsec_context)
{
    if (parsec_context->audio != NULL) {
        SDL_DestroyAudioStream(parsec_context->audio);
        parsec_context->audio = NULL;
    }
}

/* Receive decoded PCM from Parsec, maintain a small packet buffer, and start or
 * pause SDL playback when the queue crosses the configured thresholds. */
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

/* Poll Parsec audio on a worker thread while connected. During reconnect waits
 * it drains and pauses playback so stale audio does not resume after a gap. */
Sint32
vdi_stream_client__audio_thread(void *opaque)
{
    struct parsec_context_s *parsec_context = (struct parsec_context_s *)opaque;

    while (!vdi_stream_client__context_done(parsec_context)) {

        /* Poll audio only if connected. */
        if (vdi_stream_client__context_connected(parsec_context)) {
            vdi_stream_client__context_set_audio_polling(parsec_context, true);
            if (vdi_stream_client__context_connected(parsec_context) &&
                !vdi_stream_client__context_done(parsec_context)) {
                ParsecClientPollAudio(
                    parsec_context->parsec, vdi_stream_client__audio, 100, parsec_context
                );
            }
            vdi_stream_client__context_set_audio_polling(parsec_context, false);
        }

        /* Delay loop if in reconnect state. */
        if (!vdi_stream_client__context_connected(parsec_context)) {

            /* Clear queue and pause audio device. */
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
