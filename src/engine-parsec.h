/*
 *  engine-parsec.h -- parsec default types and defines
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

#ifndef _ENGINE_PARSEC_H
#define _ENGINE_PARSEC_H

/* define audio defaults. */
#define VDI_AUDIO_CHANNELS		2
#define VDI_AUDIO_SAMPLE_RATE		48000
#define VDI_AUDIO_FRAMES_PER_PACKET	960

/* define parsec messages. */
#define PARSEC_CLIPBOARD_MSG		7

/* parsec configuration. */
struct parsec_context_s {

	/* parsec. */
	SDL_bool done;
	SDL_bool connection;
	SDL_bool decoder;
	SDL_bool focus;
	SDL_bool relative;
	SDL_bool pressed;
#ifdef HAVE_LIBPARSEC
	Parsec *parsec;
#else
	ParsecDSO *parsec;
#endif
	ParsecClientStatus client_status;

	/* video. */
	SDL_Window *window;
	SDL_GLContext *gl;
	SDL_Cursor *cursor;
	Sint32 window_width;
	Sint32 window_height;

	/* opengl texture for ttf rendering. */
	SDL_Surface *surface_ttf;
	GLuint texture_ttf;
	GLfloat texture_min_x;
	GLfloat texture_min_y;
	GLfloat texture_max_x;
	GLfloat texture_max_y;

	/* audio. */
	SDL_AudioDeviceID audio;
	Sint32 playing;
	Uint32 min_buffer;
	Uint32 max_buffer;
};

/* usb redirect. */
struct redirect_context_s {

	/* parsec context. */
	struct parsec_context_s *parsec_context;

	/* network. */
	union {
		struct sockaddr_in v4;
		struct sockaddr_in6 v6;
	} server_addr;

	/* usb. */
	struct {
		Sint32 vendor;
		Sint32 product;
	} usb_device;
};

/* parsec event loop. */
Sint32 vdi_stream_client__event_loop(vdi_config_s *vdi_config);

#endif /* _ENGINE_PARSEC_H */
