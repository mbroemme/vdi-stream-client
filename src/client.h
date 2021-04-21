/*
 *  client.h -- default types and defines
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

#ifndef _CLIENT_H
#define _CLIENT_H

/* sdl includes. */
#define SDL_MAIN_HANDLED
#include <SDL2/SDL.h>

/* network includes. */
#include <arpa/inet.h>

/* define return values. */
#define VDI_STREAM_CLIENT_SUCCESS	0	/* return value for all functions which success. */
#define VDI_STREAM_CLIENT_ERROR		-1	/* generic error. */

/* define limits. */
#define USB_MAX				8	/* maximum number of usb redirects. */

/* define new print functions for logging. */
#define vdi_stream_client__log_info(...) printf(__VA_ARGS__);
#define vdi_stream_client__log_error(...) fprintf(stderr, __VA_ARGS__);

/* stored command line options. */
typedef struct {

	/* parsec options. */
	char		*session;		/* session id for connection. */
	char		*peer;			/* peer id for connection. */
	Uint16		timeout;		/* connection timeout in milliseconds. */
	Uint16		speed;			/* mouse wheel sensitivity. (0 - 255) */
	Uint16		width;			/* screen width in pixel. (host resolution is used if not specified) */
	Uint16		height;			/* screen height in pixel. (host resolution is used if not specified) */

	/* parsec warp options. */
	Uint16		subsampling;		/* color mode to use. (0 = 4:4:4, 1 = 4:2:0) */

	/* client options. */
	Uint16		acceleration;		/* client decoding type. (0 = software, 1 = hardware) */
	Uint16		upnp;			/* upnp nat traversal support. (0 = disable upnp, 1 = enable upnp) */
	Uint16		reconnect;		/* automatic reconnect support in case of failures. (0 = disable reconnect, 1 = enable reconnect). */
	Uint16		grab;			/* keyboard and mouse grabbing. (0 = grab only keyboard on mouse focus, 1 = grab keyboard and mouse) */
	Uint16		screensaver;		/* screen saver support. (0 = disable screen saver, 1 = enable screen saver) */
	Uint16		clipboard;		/* clipboard sharing support. (0 = disable clipboard sharing, 1 = enable clipboard sharing) */
	Uint16		audio;			/* audio support. (0 = disable audio streaming, 1 = enable audio streaming) */
	Uint16		hevc;			/* streaming codec to use. (0 = h264, 1 = h265) */

	/* usb options. */
	union {
		struct		sockaddr_in v4;		/* address (ipv4) to connect for usb redirection. */
		struct		sockaddr_in6 v6;	/* address (ipv6) to connect for usb redirection. */
	} server_addrs[USB_MAX];
	struct {
		Sint32		vendor;			/* vendor id of usb devices for redirection. */
		Sint32		product;		/* product id of usb devices for redirection. */
	} usb_devices[USB_MAX];
} vdi_config_s;

#endif /* _CLIENT_H */
