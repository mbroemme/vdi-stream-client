/*
 *  client.h -- default types and defines
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

#ifndef VDI_STREAM_CLIENT_CLIENT_H
#define VDI_STREAM_CLIENT_CLIENT_H

/* sdl includes. */
#include <SDL3/SDL.h>

/* network includes. */
#include <arpa/inet.h>

/* define return values. */
#define VDI_STREAM_CLIENT_SUCCESS (0) /* return value for all functions which success. */
#define VDI_STREAM_CLIENT_ERROR (-1)  /* generic error. */

/* define limits. */
#define USB_MAX (8) /* maximum number of usb redirects. */

typedef union
{
    struct sockaddr_in v4;  /* address (ipv4) to connect for usb redirection. */
    struct sockaddr_in6 v6; /* address (ipv6) to connect for usb redirection. */
} vdi_server_addr_u;

/* stored command line options. */
typedef struct vdi_config_s
{

    /* parsec options. */
    char *session;  /* session id for connection. */
    char *peer;     /* peer id for connection. */
    Uint16 timeout; /* connection timeout in milliseconds. */
    Uint16 speed;   /* mouse wheel sensitivity. (0 - 255) */
    Uint16 width;   /* screen width in pixel. (host resolution is used if not specified) */
    Uint16 height;  /* screen height in pixel. (host resolution is used if not specified) */

    /* color mode to use. (0 = 4:4:4, 1 = 4:2:0) */
    Uint16 subsampling;

    /* client decoding type. (0 = software, 1 = hardware) */
    Uint16 acceleration;

    /* upnp nat traversal support. (0 = disable upnp, 1 = enable upnp) */
    Uint16 upnp;

    /* automatic reconnect support in case of failures. (0 = disable reconnect, 1 = enable
     * reconnect) */
    Uint16 reconnect;

    /* keyboard and mouse grabbing. (0 = grab only keyboard on mouse focus, 1 = grab keyboard and
     * mouse) */
    Uint16 grab;

    /* window manager decoration support. (0 = borderless window, 1 = decorated window) */
    Uint16 decoration;

    /* screen saver support. (0 = disable screen saver, 1 = enable screen saver) */
    Uint16 screensaver;

    /* clipboard sharing support. (0 = disable clipboard sharing, 1 = enable clipboard sharing) */
    Uint16 clipboard;

    /* audio support. (0 = disable audio streaming, 1 = enable audio streaming) */
    Uint16 audio;

    /* streaming codec to use. (0 = h264, 1 = h265) */
    Uint16 hevc;

    /* render stats logging. (0 = disable, 1 = enable) */
    Uint16 stats;

    /* render stats logging interval in seconds. */
    Uint64 stats_period;

    /* usb options. */
    vdi_server_addr_u server_addrs[USB_MAX];
    struct
    {
        Sint32 vendor;  /* vendor id of usb devices for redirection. */
        Sint32 product; /* product id of usb devices for redirection. */
    } usb_devices[USB_MAX];
} vdi_config_s;

#endif /* VDI_STREAM_CLIENT_CLIENT_H */
