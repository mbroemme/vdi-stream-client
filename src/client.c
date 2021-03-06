/*
 *  client.c -- gpu accelerated streaming client
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
#include <getopt.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>

/* this function show the usage. */
Sint32 vdi_stream_client__usage(char *program_name) {

	/* show the help. */
	printf("Usage: %s [session] [peer] (options)...\n", program_name);
	printf("GPU accelerated remote host graphical console\n");
	printf("\n");
	printf("Help Options:\n");
	printf("  -h, --help               show this help screen\n");
	printf("  -v, --version            show the version information\n");
	printf("\n");
	printf("Parsec Options:\n");
	printf("      --session <id>       session id for connection (mandatory)\n");
	printf("      --peer <id>          peer id for connection (mandatory)\n");
	printf("      --timeout <seconds>  connection timeout (default: 5 seconds)\n");
	printf("      --speed <speed>      mouse wheel sensitivity: 0 to 500 (default: 100)\n");
	printf("      --width <width>      horizontal resolution (default: host resolution)\n");
	printf("      --height <height>    vertical resolution (default: host resolution)\n");
	printf("\n");
	printf("Parsec Warp Options:\n");
	printf("      --no-subsampling     disable compression with chroma subsampling\n");
	printf("\n");
	printf("Client Options:\n");
	printf("      --no-acceleration    disable hardware accelerated decoding\n");
	printf("      --no-upnp            disable upnp nat traversal\n");
	printf("      --no-reconnect       disable automatic reconnect in case of failures\n");
	printf("      --no-grab            disable exclusive mouse grab\n");
	printf("      --no-screensaver     disable screen saver and lockers\n");
	printf("      --no-clipboard       disable clipboard sharing\n");
	printf("      --no-audio           disable audio streaming\n");
	printf("      --no-hevc            disable H.265/HEVC video codec\n");
	printf("\n");
	printf("USB Options:\n");
	printf("      --redirect <vendorID>:<productID>@<address>#<port>,[...]\n");
	printf("                           redirect single or multiple local USB devices\n");
	printf("\n");
	printf("Please report bugs to the appropriate authors, which can be found in the\n");
	printf("version information. All other things can be send to <%s>\n", PACKAGE_BUGREPORT);

	/* if no error was found, return zero. */
	return VDI_STREAM_CLIENT_SUCCESS;
}

/* this function shows the version information. */
Sint32 vdi_stream_client__version(char *program_name) {

	/* show the version. */
	printf("%s version %s Copyright (c) 2020 The VDI Stream developers\n", program_name, VERSION);
	printf("Written by %s\n", AUTHOR);
	printf("\n");
	printf("This is free software; see the source for copying conditions.  There is NO\n");
	printf("warranty; not even for MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.\n");

	/* if no error was found, return zero. */
	return VDI_STREAM_CLIENT_SUCCESS;
}

/* main function to initialize structs and parse command line options. */
Sint32 main(Sint32 argc, char **argv) {

	/* variables. */
	Sint32 option_index = 0;
	Sint32 opt;
	char *program_name;
	vdi_config_s *vdi_config = NULL;

	/* temp variables for command line parser. */
	Sint32 device;
	Sint32 type;
	char *redirect;
	char *item;
	char *delim;
	char *endptr;
	Uint64 port;

	/* command line options. */
	struct option long_options[] = {

		/* help options. */
		{"help", no_argument, NULL, 'h'},
		{"version", no_argument, NULL, 'v'},

		/* parsec options. */
		{"session", required_argument, NULL, 'x'},
		{"peer", required_argument, NULL, 'y'},
		{"timeout", required_argument, NULL, 't'},
		{"speed", required_argument, NULL, 's'},
		{"width", required_argument, NULL, 'w'},
		{"height", required_argument, NULL, 'u'},

		/* parsec warp options. */
		{"no-subsampling", no_argument, NULL, 'm'},

		/* client options. */
		{"no-acceleration", no_argument, NULL, 'd'},
		{"no-upnp", no_argument, NULL, 'b'},
		{"no-reconnect", no_argument, NULL, 'r'},
		{"no-grab", no_argument, NULL, 'g'},
		{"no-screensaver", no_argument, NULL, 'z'},
		{"no-clipboard", no_argument, NULL, 'p'},
		{"no-audio", no_argument, NULL, 'a'},
		{"no-hevc", no_argument, NULL, 'c'},

		/* usb options. */
		{"redirect", required_argument, NULL, 'f'},

		/* end options. */
		{0, 0, 0, 0},
	};

	/* default values. */
	optind = 0;
	opterr = 0;

	/* allocate memory defaults. */
	if ((vdi_config = calloc(1, sizeof(vdi_config_s))) == NULL) {
		goto error;
	}
	if ((vdi_config->session = calloc(0, sizeof(vdi_config->session))) == NULL ||
	    (vdi_config->peer = calloc(0, sizeof(vdi_config->peer))) == NULL) {
		goto error;
	}

	/* parsec defaults. */
	vdi_config->timeout = 5000;
	vdi_config->speed = 100;

	/* parsec warp defaults. */
	vdi_config->subsampling = 1;

	/* client defaults. */
	vdi_config->acceleration = 1;
	vdi_config->upnp = 1;
	vdi_config->reconnect = 1;
	vdi_config->grab = 1;
	vdi_config->screensaver = 1;
	vdi_config->clipboard = 1;
	vdi_config->audio = 1;
	vdi_config->hevc = 1;

	program_name = argv[0];
	if (program_name && strrchr(program_name, '/')) {
		program_name = strrchr(program_name, '/') + 1;
	}

	if (argc <= 1) {
		vdi_stream_client__usage(program_name);
		return VDI_STREAM_CLIENT_SUCCESS;
	}

	/* parse command line. */
	while ((opt = getopt_long(argc, argv, ":hv", long_options, &option_index)) != -1) {

		/* check if all command line options are parsed. */
		if (opt == -1) {
			break;
		}

		/* parse option. */
		switch (opt) {

			/* help options. */
			case 'h':
				vdi_stream_client__usage(program_name);
                                return VDI_STREAM_CLIENT_SUCCESS;
			case 'v':
				vdi_stream_client__version(program_name);
                                return VDI_STREAM_CLIENT_SUCCESS;

			/* parsec options. */
			case 'x':
				vdi_config->session = strdup(argv[optind - 1]);
				continue;
			case 'y':
				vdi_config->peer = strdup(argv[optind - 1]);
				continue;
			case 't':
				vdi_config->timeout = strtol(argv[optind - 1], NULL, 10) * 1000;
				continue;
			case 's':
				vdi_config->speed = strtol(argv[optind - 1], NULL, 10);
				if (vdi_config->speed < 0) {
					vdi_config->speed = 0;
				}
				if (vdi_config->speed > 500) {
					vdi_config->speed = 500;
				}
				continue;
			case 'w':
				vdi_config->width = strtol(argv[optind - 1], NULL, 10);
				continue;
			case 'u':
				vdi_config->height = strtol(argv[optind - 1], NULL, 10);
				continue;

			/* parsec warp options. */
			case 'm':
				vdi_config->subsampling = 0;
				continue;

			/* client options. */
			case 'd':
				vdi_config->acceleration = 0;
				continue;
			case 'b':
				vdi_config->upnp = 0;
				continue;
			case 'r':
				vdi_config->reconnect = 0;
				continue;
			case 'g':
				vdi_config->grab = 0;
				continue;
			case 'z':
				vdi_config->screensaver = 0;
				continue;
			case 'p':
				vdi_config->clipboard = 0;
				continue;
			case 'a':
				vdi_config->audio = 0;
				continue;
			case 'c':
				vdi_config->hevc = 0;
				continue;

			/* usb options. */
			case 'f':

				/* loop through multiple redirect configs. */
				device = 0;
				while ((redirect = strsep(&argv[optind - 1], ",")) != NULL) {

					/* check if number of usb redirects are out of range. */
					if (device >= USB_MAX) {
						fprintf(stderr, "%s: usb redirection limit of %d reached\n", program_name, USB_MAX);
						fprintf(stderr, "Try `%s --help' for more information.\n", program_name);
						goto error;
					}

					/* initialize. */
					memset(&vdi_config->server_addrs[device], 0, sizeof(vdi_config->server_addrs[device]));

					/* loop through redirect categroy. (usb vendor, product, address and port) */
					type = 0;
					while ((item = strsep(&redirect, "@#")) != NULL) {

						/* usb device. */
						if (type == 0) {

							/* check if usb vendor is out of range. */
							vdi_config->usb_devices[device].vendor = strtol(item, &endptr, 16);
							if (vdi_config->usb_devices[device].vendor <= 0 || vdi_config->usb_devices[device].vendor > 0xffff) {
								fprintf(stderr, "%s: invalid vendor identifier in usb device: %s\n", program_name, item);
								fprintf(stderr, "Try `%s --help' for more information.\n", program_name);
								goto error;
							}

							/* check if usb product is given. */
							delim = strchr(item, ':');
							if (*endptr != ':' || delim == NULL || strlen(delim) == 1) {
								fprintf(stderr, "%s: no product identifier in usb device\n", program_name);
								fprintf(stderr, "Try `%s --help' for more information.\n", program_name);
								goto error;
							}

							/* check if usb product is out of range. (product id 0000 is valid) */
							vdi_config->usb_devices[device].product = strtol(delim + 1, &endptr, 16);
							if (*endptr != '\0' || vdi_config->usb_devices[device].product < 0 || vdi_config->usb_devices[device].product > 0xffff) {
								fprintf(stderr, "%s: invalid product identifier in usb device: %s\n", program_name, item);
								fprintf(stderr, "Try `%s --help' for more information.\n", program_name);
								goto error;
							}
						}

						/* address. */
						if (type == 1) {

							/* check if empty ip address given. */
							if (item[0] == '\0') {
								fprintf(stderr, "%s: no ip address in usb redirection\n", program_name);
								fprintf(stderr, "Try `%s --help' for more information.\n", program_name);
								goto error;
							}

							/* check if ipv4 address. */
							if (inet_pton(AF_INET, item, &vdi_config->server_addrs[device].v4.sin_addr) == 1) {
								vdi_config->server_addrs[device].v4.sin_family = AF_INET;
							}

							/* check if ipv6 address. */
							if (inet_pton(AF_INET6, item, &vdi_config->server_addrs[device].v6.sin6_addr) == 1) {
								vdi_config->server_addrs[device].v6.sin6_family = AF_INET6;
							}

							/* check if bad formatted address. */
							if (vdi_config->server_addrs[device].v4.sin_family != AF_INET &&
							    vdi_config->server_addrs[device].v6.sin6_family != AF_INET6) {
								fprintf(stderr, "%s: invalid address in usb redirection: %s\n", program_name, item);
								fprintf(stderr, "Try `%s --help' for more information.\n", program_name);
								goto error;
							}
						}

						/* port. */
						if (type == 2) {

							/* check if empty port given. */
							if (item[0] == '\0') {
								fprintf(stderr, "%s: no port in usb redirection\n", program_name);
								fprintf(stderr, "Try `%s --help' for more information.\n", program_name);
								goto error;
							}

							/* convert port string to integer. */
							port = strtol(item, NULL, 10);

							/* check if port is out of range. */
							if (port <= 0 || port >= 65536) {
								fprintf(stderr, "%s: invalid port in usb redirection: %s\n", program_name, item);
								fprintf(stderr, "Try `%s --help' for more information.\n", program_name);
								goto error;
							}

							/* check if ipv4 address has been assigned previosuly. */
							if (vdi_config->server_addrs[device].v4.sin_family != AF_INET) {
								vdi_config->server_addrs[device].v4.sin_port = htons(port);
							}

							/* check if ipv6 address has been assigned previosuly. */
							if (vdi_config->server_addrs[device].v6.sin6_family != AF_INET6) {
								vdi_config->server_addrs[device].v6.sin6_port = htons(port);
							}
						}
						type++;
					}

					/* check if no ip address was given. */
					if (type == 1) {
						fprintf(stderr, "%s: no ip address in usb redirection\n", program_name);
						fprintf(stderr, "Try `%s --help' for more information.\n", program_name);
						goto error;
					}

					/* check if no port was given. */
					if (type == 2) {
						fprintf(stderr, "%s: no port in usb redirection\n", program_name);
						fprintf(stderr, "Try `%s --help' for more information.\n", program_name);
						goto error;
					}
					device++;
				}
				continue;

			/* missing arguments. */
			case ':':
				fprintf(stderr, "%s: option `%s' requires an argument\n", program_name, argv[optind - 1]);
				fprintf(stderr, "Try `%s --help' for more information.\n", program_name);
				goto error;

			/* unknown switches. */
			case '?':
				fprintf(stderr, "%s: unrecognized option `%s'\n", program_name, argv[optind - 1]);
				fprintf(stderr, "Try `%s --help' for more information.\n", program_name);
				goto error;
                }
	}

	/* mandatory arguments not given. */
	if (strlen(vdi_config->session) == 0 || strlen(vdi_config->peer) == 0) {
		fprintf(stderr, "%s: mandatory arguments missing\n", program_name);
		fprintf(stderr, "Try `%s --help' for more information.\n", program_name);
		goto error;
	}

	/* additional non-option arguments given. */
	if (argc > optind) {
		fprintf(stderr, "%s: non-option argument `%s'\n", program_name, argv[optind]);
		fprintf(stderr, "Try `%s --help' for more information.\n", program_name);
		goto error;
	}

	/* main event loop. */
	if (vdi_stream_client__event_loop(vdi_config) != 0) {
		goto error;
	}

	/* free allocated memory. */
	free(vdi_config->session);
	free(vdi_config->peer);
	free(vdi_config);

	/* quit application. */
	return VDI_STREAM_CLIENT_SUCCESS;

error:

	/* free allocated memory. */
	free(vdi_config->session);
	free(vdi_config->peer);
	free(vdi_config);

	/* quit application with error code. */
	return VDI_STREAM_CLIENT_ERROR;
}
