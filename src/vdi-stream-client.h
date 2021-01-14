/*
 *  vdi-stream-client.h -- default types and defines
 *
 *  Copyright (c) 2020 Maik Broemme <mbroemme@libmpq.org>
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

#ifndef _VDI_STREAM_CLIENT_H
#define _VDI_STREAM_CLIENT_H

/* define return values. */
#define VDI_STREAM_CLIENT_SUCCESS	0	/* return value for all functions which success. */
#define VDI_STREAM_CLIENT_ERROR		-1	/* generic error. */

/* stored command line options. */
typedef struct {
	uint16_t	audio;		/* audio support. (0 = disable audio streaming, 1 = enable audio streaming) */
	uint16_t	screensaver;	/* screen saver support. (0 = disable screen saver, 1 = enable screen saver) */
	uint16_t	clipboard;	/* clipboard sharing support. (0 = disable clipboard sharing, 1 = enable clipboard sharing) */
	uint16_t	timeout;	/* connection timeout in milliseconds. */
	uint16_t	codec;		/* streaming codec to use. (1 = h264 and 2 = h265) */
	uint16_t	mode;		/* color mode to use. (1 = 4:2:0 and 2 = 4:4:4) */
	uint16_t	speed;		/* mouse wheel sensitivity. (0 - 255) */
	uint16_t	grab;		/* keyboard and mouse grabbing. (0 = grab only keyboard on mouse focus, 1 = grab keyboard and mouse) */
	uint16_t	width;		/* screen width in pixel. (host resolution is used if not specified) */
	uint16_t	height;		/* screen height in pixel. (host resolution is used if not specified) */
	char		session[129];	/* session id for connection. (last character must be '\0') */
	char		peer[33];	/* peer id for connection. (last character must be '\0') */
} vdi_config_s;

#endif /* _VDI_STREAM_CLIENT_H */
