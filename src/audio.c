/*
 *  audio.c -- audio rendering thread via sdl
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

/* parsec audio event. */
static void vdi_stream_client__audio(const Sint16 *pcm, Uint32 frames, void *opaque) {
	struct parsec_context_s *parsec_context = (struct parsec_context_s *) opaque;

	Uint32 size = SDL_GetQueuedAudioSize(parsec_context->audio);
	Uint32 queued_frames = size / (VDI_AUDIO_CHANNELS * sizeof(Sint16));
	Uint32 queued_packets = queued_frames / VDI_AUDIO_FRAMES_PER_PACKET;

	if (parsec_context->playing == SDL_TRUE && queued_packets > parsec_context->max_buffer) {
		SDL_ClearQueuedAudio(parsec_context->audio);
		SDL_PauseAudioDevice(parsec_context->audio, 1);
		parsec_context->playing = SDL_FALSE;
	} else if (parsec_context->playing == SDL_FALSE && queued_packets >= parsec_context->min_buffer) {
		SDL_PauseAudioDevice(parsec_context->audio, 0);
		parsec_context->playing = SDL_TRUE;
	}

	SDL_QueueAudio(parsec_context->audio, pcm, frames * VDI_AUDIO_CHANNELS * sizeof(Sint16));
}

/* sdl audio thread. */
Sint32 vdi_stream_client__audio_thread(void *opaque) {
	struct parsec_context_s *parsec_context = (struct parsec_context_s *) opaque;

	while (parsec_context->done == SDL_FALSE) {

		/* poll audio only if connected. */
		if (parsec_context->connection == SDL_TRUE) {
			ParsecClientPollAudio(parsec_context->parsec, vdi_stream_client__audio, 100, parsec_context);
		}

		/* delay loop if in reconnect state. */
		if (parsec_context->connection == SDL_FALSE) {

			/* clear queue and close audio device. */
			if (parsec_context->playing == SDL_TRUE) {
				SDL_ClearQueuedAudio(parsec_context->audio);
				SDL_PauseAudioDevice(parsec_context->audio, 1);
				parsec_context->playing = SDL_FALSE;
			}
			SDL_Delay(parsec_context->timeout);
		}
	}

	return VDI_STREAM_CLIENT_SUCCESS;
}
