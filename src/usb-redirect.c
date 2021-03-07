/*
 *  usb-redirect.c -- usb redirection via libusb
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

/* configuration includes. */
#include "config.h"

/* system includes. */
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <stdlib.h>
#include <stdio.h>
#include <unistd.h>

/* opengl includes. */
#include <GL/gl.h>

/* sdl includes. */
#define SDL_MAIN_HANDLED
#include <SDL2/SDL.h>
#include <SDL2/SDL_syswm.h>
#include <SDL2/SDL_ttf.h>

/* network includes. */
#include <arpa/inet.h>

/* usb includes. */
#include <libusb.h>
#include <usbredirhost.h>

/* parsec includes. */
#ifdef HAVE_LIBPARSEC
#include <parsec/parsec.h>
#else
#include "../parsec-sdk/sdk/parsec-dso.h"
#endif

/* vdi-stream-client header includes. */
#include "vdi-stream-client.h"
#include "engine-parsec.h"

/* thread specific variables. */
static __thread Sint32 server_fd = -1;
static __thread libusb_device_handle *device_handle = NULL;
static __thread struct usbredirhost *host = NULL;

/* this must be defined, otherwise usbredir will crash. */
static void vdi_stream_client__usb_log(void *priv, Sint32 level, const char *msg) { }

/* read data from client. */
static Sint32 vdi_stream_client__usb_read(void *priv, Uint8 *data, Sint32 count) {
	Sint32 r = read(server_fd, data, count);
	if (r < 0) {
		if (errno == EAGAIN) {
			return 0;
		}
		return -1;
	}

	/* client disconnected. */
	if (r == 0) {
		close(server_fd);
		server_fd = -1;
	}
	return r;
}

/* write data to client. */
static Sint32 vdi_stream_client__usb_write(void *priv, Uint8 *data, Sint32 count) {
	Sint32 r = write(server_fd, data, count);
	if (r < 0) {
		if (errno == EAGAIN) {
			return 0;
		}

		/* client disconnected. */
		if (errno == EPIPE) {
			close(server_fd);
			server_fd = -1;
			return 0;
		}
		return -1;
	}
	return r;
}

/* usb hotplug event. */
static Sint32 vdi_stream_client__usb_remove(struct libusb_context *usb_context, struct libusb_device *device, libusb_hotplug_event event, void *user_data) {

	/* usb device removed. */
	if (device_handle != NULL) {
		if (server_fd != -1) {
			close(server_fd);
			server_fd = -1;
		}
	}

	return VDI_STREAM_CLIENT_SUCCESS;
}

/* sdl network thread. */
Sint32 vdi_stream_client__network_thread(void *opaque) {
	struct redirect_context_s *redirect_context = (struct redirect_context_s *) opaque;
	struct libusb_device **devices;
	struct libusb_device *device;
	libusb_context *usb_context;
	libusb_hotplug_callback_handle callback_handle;
	struct libusb_device_descriptor desc;
	size_t count;

	/* variables for data processing loop. */
	const struct libusb_pollfd **pollfds = NULL;
	fd_set readfds;
	fd_set writefds;
	Sint32 i;
	Sint32 n;
	Sint32 nfds;
	struct timeval timeout;

	/* initial values. */
	Uint32 delay = 100;
	Sint32 option = 1;
	Sint32 error = 0;

	/* user output. */
	vdi_stream__log_info("Start USB Redirect %04x:%04x\n", redirect_context->usb_device.vendor, redirect_context->usb_device.product);

	/* usb init. */
	error = libusb_init(&usb_context);
	if (error != 0) {
		vdi_stream__log_error("USB Device %04x:%04x redirect initialization failed: %s\n", redirect_context->usb_device.vendor, redirect_context->usb_device.product, libusb_strerror(error));
		goto error;
	}

	/* register events. */
	error = libusb_hotplug_register_callback(
		usb_context,
		LIBUSB_HOTPLUG_EVENT_DEVICE_LEFT,
		0,
		redirect_context->usb_device.vendor,
		redirect_context->usb_device.product,
		LIBUSB_HOTPLUG_MATCH_ANY,
		vdi_stream_client__usb_remove,
		NULL,
		&callback_handle
	);
	if (error != 0) {
		vdi_stream__log_error("USB Device %04x:%04x redirect initialization failed: %s\n", redirect_context->usb_device.vendor, redirect_context->usb_device.product, libusb_strerror(error));
		goto error;
	}

	while (redirect_context->parsec_context->done == SDL_FALSE) {
		memset(&timeout, 0, sizeof(timeout));
		timeout.tv_sec = 1;
		timeout.tv_usec = 0;

		/* set ipv4 or ipv6 socket. */
		if (redirect_context->server_addr.v4.sin_family == AF_INET) {
			server_fd = socket(AF_INET, SOCK_STREAM, 0);
		}
		if (redirect_context->server_addr.v6.sin6_family == AF_INET6) {
			server_fd = socket(AF_INET6, SOCK_STREAM, 0);
		}

		/* set socket connection timeout. */
		setsockopt(server_fd, SOL_SOCKET, SO_SNDTIMEO, &timeout, sizeof(timeout));

		/* connect to qemu usbredir guest service. */
		n = connect(server_fd, (struct sockaddr *)&redirect_context->server_addr, sizeof(redirect_context->server_addr));
		if (n == -1) {
			if (errno == EINTR) {
				continue;
			}
		}

		/* non-blocking client socket. */
		fcntl(server_fd, F_SETFL, O_NONBLOCK);

		/* get list of usb devices. */
		libusb_get_device_list(usb_context, &devices);

		/* find device to open. */
		count = 0;
		while ((device = devices[count++]) != NULL) {
			libusb_get_device_descriptor(device, &desc);
			if (desc.idVendor == redirect_context->usb_device.vendor && desc.idProduct == redirect_context->usb_device.product) {
				break;
			}
		}

		/* usb device not attached. */
		if (device == NULL) {

			/* free list of usb devices after successful open. */
			libusb_free_device_list(devices, 1);

			close(server_fd);
			server_fd = -1;
			continue;
		}

		/* open usb device. */
		error = libusb_open(device, &device_handle);
		if (error != 0) {

			/* check if permission denied. */
			if (error == LIBUSB_ERROR_ACCESS) {
				vdi_stream__log_info("USB Device %04x:%04x redirect failed: %s\n", redirect_context->usb_device.vendor, redirect_context->usb_device.product, libusb_strerror(error));
			}

			/* free list of usb devices after successful open. */
			libusb_free_device_list(devices, 1);

			close(server_fd);
			server_fd = -1;
			continue;
		}

		/* free list of usb devices after successful open. */
		libusb_free_device_list(devices, 1);

		/* setup host. */
		host = usbredirhost_open(usb_context, device_handle, vdi_stream_client__usb_log, vdi_stream_client__usb_read, vdi_stream_client__usb_write, NULL, NULL, 0, 0);
		if (host == NULL) {
			close(server_fd);
			server_fd = -1;
			continue;
		}

		/* user output. */
		vdi_stream__log_info("USB Device %04x:%04x connected\n", redirect_context->usb_device.vendor, redirect_context->usb_device.product);

		/* data processing loop. */
		while (server_fd != -1) {

			/* check if main thread is still running. */
			if (redirect_context->parsec_context->done == SDL_TRUE) {
				break;
			}

			/* remove all socket descriptors from set. */
			FD_ZERO(&readfds);
			FD_ZERO(&writefds);

			/* adding client socket descriptor to set. */
			FD_SET(server_fd, &readfds);
			if (usbredirhost_has_data_to_write(host)) {
				FD_SET(server_fd, &writefds);
			}
			nfds = server_fd + 1;

			/* free not cleared sockets. */
			if (pollfds != NULL) {
				libusb_free_pollfds(pollfds);
				pollfds = NULL;
			}

			/* poll usb devices for data. */
			pollfds = libusb_get_pollfds(usb_context);
			for (i = 0; pollfds && pollfds[i]; i++) {
				if (pollfds[i]->events & POLLIN) {
					FD_SET(pollfds[i]->fd, &readfds);
				}
				if (pollfds[i]->events & POLLOUT) {
					FD_SET(pollfds[i]->fd, &writefds);
				}
				if (pollfds[i]->fd >= nfds) {
					nfds = pollfds[i]->fd + 1;
				}
			}

			/* get next timeout. */
			memset(&timeout, 0, sizeof(timeout));
			if (libusb_get_next_timeout(usb_context, &timeout) == 0) {
				timeout.tv_sec = 1;
				timeout.tv_usec = 0;
			}

			/* select will wait for data to arrive until timeout. */
			n = select(nfds, &readfds, &writefds, NULL, &timeout);
			if (n == -1) {
				if (errno == EINTR) {
					continue;
				}
				break;
			}

			/* wait for usb events and cleanup timeout structure for re-use. */
			memset(&timeout, 0, sizeof(timeout));
			if (n == 0) {
				libusb_handle_events_timeout(usb_context, &timeout);
				continue;
			}

			/* read data from usb device. */
			if (FD_ISSET(server_fd, &readfds) != 0 && usbredirhost_read_guest_data(host) != 0) {
				break;
			}

			/* usbredirhost_read_guest_data may have detected client disconnect. */
			if (server_fd == -1) {
				break;
			}

			/* write data to usb device. */
			if (FD_ISSET(server_fd, &writefds) != 0 && usbredirhost_write_guest_data(host) != 0) {
				break;
			}

			/* wait until first timeout either for read or write happens. */
			for (i = 0; pollfds && pollfds[i]; i++) {
				if (FD_ISSET(pollfds[i]->fd, &readfds) ||
				    FD_ISSET(pollfds[i]->fd, &writefds)) {
					libusb_handle_events_timeout(usb_context, &timeout);
					break;
				}
			}
		}

		/* loop terminated but socket still open. */
		if (server_fd != -1) {
			close(server_fd);
			server_fd = -1;
		}

		/* free memory. */
		if (pollfds != NULL) {
			libusb_free_pollfds(pollfds);
			pollfds = NULL;
		}

		/* user output. */
		vdi_stream__log_info("USB Device %04x:%04x removed\n", redirect_context->usb_device.vendor, redirect_context->usb_device.product);

		/* usbredirhost_close() calls libusb_close() so no need to do it again. */
		usbredirhost_close(host);
		device_handle = NULL;
	}

	/* close client socket. */
	if (server_fd != -1) {
		close(server_fd);
	}

	/* usb destroy. */
	libusb_hotplug_deregister_callback(usb_context, callback_handle);
	libusb_exit(usb_context);

	/* user output. */
	vdi_stream__log_info("Stop USB Redirect %04x:%04x\n", redirect_context->usb_device.vendor, redirect_context->usb_device.product);

	/* terminate loop. */
	return VDI_STREAM_CLIENT_SUCCESS;

error:

	/* stop main thread. */
	redirect_context->parsec_context->done = SDL_TRUE;

	/* close client socket. */
	if (server_fd != -1) {
		close(server_fd);
	}

	/* usb destroy. */
	libusb_hotplug_deregister_callback(usb_context, callback_handle);
	libusb_exit(usb_context);

	/* return with error. */
	return VDI_STREAM_CLIENT_ERROR;
}
