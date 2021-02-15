/*
 *  vdi-stream-client.h -- default types and defines
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
	uint16_t	reconnect;	/* automatic reconnect support in case of failures. (0 = disable reconnect, 1 = enable reconnect). */
	uint16_t	timeout;	/* connection timeout in milliseconds. */
	uint16_t	acceleration;	/* client decoding type. (0 = software, 1 = hardware) */
	uint16_t	hevc;		/* streaming codec to use. (0 = h264, 1 = h265) */
	uint16_t	subsampling;	/* color mode to use. (0 = 4:4:4, 1 = 4:2:0) */
	uint16_t	speed;		/* mouse wheel sensitivity. (0 - 255) */
	uint16_t	grab;		/* keyboard and mouse grabbing. (0 = grab only keyboard on mouse focus, 1 = grab keyboard and mouse) */
	uint16_t	relative;	/* relative mouse grabbing. (0 = disable relative mode, 1 = enable relative mode when application requests) */
	uint16_t	width;		/* screen width in pixel. (host resolution is used if not specified) */
	uint16_t	height;		/* screen height in pixel. (host resolution is used if not specified) */
	uint16_t	upnp;		/* upnp nat traversal support. (0 = disable upnp, 1 = enable upnp) */
	char		session[129];	/* session id for connection. (last character must be '\0') */
	char		peer[33];	/* peer id for connection. (last character must be '\0') */
} vdi_config_s;

/* define new print functions for logging. */
#define vdi_stream__log_info(...) printf(__VA_ARGS__);
#define vdi_stream__log_error(...) fprintf(stderr, __VA_ARGS__);

#endif /* _VDI_STREAM_CLIENT_H */
