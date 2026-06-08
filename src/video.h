/*
 *  video.h -- video rendering thread via sdl
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

#ifndef VDI_STREAM_CLIENT_VIDEO_H
#define VDI_STREAM_CLIENT_VIDEO_H

/* system includes. */
#include <stdbool.h>

/* sdl includes. */
#include <SDL3/SDL.h>

struct parsec_context_s;

/* video rendering. */
SDL_WindowFlags vdi_stream_client__video_window_flags(bool acceleration);
bool vdi_stream_client__video_init(struct parsec_context_s *parsec_context, bool acceleration);
bool vdi_stream_client__video_render(struct parsec_context_s *parsec_context, bool force_redraw);
void vdi_stream_client__video_destroy(struct parsec_context_s *parsec_context);

#endif /* VDI_STREAM_CLIENT_VIDEO_H */
