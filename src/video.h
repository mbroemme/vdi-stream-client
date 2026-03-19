/*
 *  video.h -- video rendering thread via sdl
 *
 *  Copyright (c) 2021 Maik Broemme <mbroemme@libmpq.org>
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

#ifndef _VIDEO_H
#define _VIDEO_H

/* sdl includes. */
#include <SDL3/SDL.h>

/* opengl includes. */
#include <GL/gl.h>

struct parsec_context_s;

/* video rendering. */
GLuint vdi_stream_client__gl_load_texture(SDL_Surface *surface, GLfloat *texture_coord);
void vdi_stream_client__video_init(struct parsec_context_s *parsec_context);
void vdi_stream_client__video_render(struct parsec_context_s *parsec_context, bool force_redraw);
void vdi_stream_client__video_destroy(struct parsec_context_s *parsec_context);

#endif /* _VIDEO_H */
