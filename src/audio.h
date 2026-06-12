/*
 *  audio.h -- audio rendering thread via sdl
 *
 *  Copyright (c) 2021-2026 Maik Broemme <mbroemme@libmpq.org>
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

#ifndef VDI_STREAM_CLIENT_AUDIO_H
#define VDI_STREAM_CLIENT_AUDIO_H

/* sdl includes. */
#include <SDL3/SDL.h>

struct parsec_context_s;

/* audio device. */
bool vdi_stream_client__audio_init(struct parsec_context_s *parsec_context, bool enabled);
void vdi_stream_client__audio_destroy(struct parsec_context_s *parsec_context);

/* audio thread. */
Sint32 vdi_stream_client__audio_thread(void *opaque);

#endif /* VDI_STREAM_CLIENT_AUDIO_H */
