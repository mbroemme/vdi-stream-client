/*
 *  vdi-stream-client.c -- gpu accelerated streaming client
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

/* system includes. */
#include <getopt.h>
#include <stdlib.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

/* vdi-stream-client header includes. */
#include "vdi-stream-client.h"
#include "engine-parsec.h"

/* vdi-stream-client configuration includes. */
#include "config.h"

/* this function show the usage. */
int32_t vdi_stream_client__usage(char *program_name) {

	/* show the help. */
	printf("Usage: %s [session] [peer] (options)...\n", program_name);
	printf("GPU accelerated remote host graphical console\n", program_name);
	printf("\n");
	printf("Help Options:\n");
	printf("  -h, --help               show this help screen\n");
	printf("  -v, --version            show the version information\n");
	printf("\n");
	printf("Parsec Options:\n");
	printf("      --session <id>       session id for connection (mandatory)\n");
	printf("      --peer <id>          peer id for connection (mandatory)\n");
	printf("      --timeout <seconds>  connection timeout (default: 5 seconds)\n");
	printf("      --codec <codec>      streaming codec: h264 or h265 (default: h264)\n");
	printf("      --mode <color>       color mode: 4:2:0 or 4:4:4 (default 4:2:0)\n");
	printf("      --speed <speed>      mouse wheel sensitivity: 0 to 500 (default: 100)\n");
	printf("      --width <width>      horizontal resolution (default: host resolution)\n");
	printf("      --height <height>    vertical resolution (default: host resolution)\n");
	printf("      --no-grab            disable exclusive mouse grab\n");
	printf("      --no-screensaver     disable screen saver and lockers\n");
	printf("      --no-clipboard       disable clipboard sharing\n");
	printf("      --no-audio           disable audio streaming\n");
	printf("\n");
	printf("Please report bugs to the appropriate authors, which can be found in the\n");
	printf("version information. All other things can be send to <%s>\n", PACKAGE_BUGREPORT);

	/* if no error was found, return zero. */
	return VDI_STREAM_CLIENT_SUCCESS;
}

/* this function shows the version information. */
int32_t vdi_stream_client__version(char *program_name) {

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
int32_t main(int32_t argc, char **argv) {

	/* variables. */
	int32_t option_index = 0;
	int32_t opt;
	int32_t speed;
	char *program_name;
	vdi_config_s *vdi_config = NULL;

	/* command line options. */
	struct option long_options[] = {
		{"codec", required_argument, NULL, 'c'},
		{"height", required_argument, NULL, 'u'},
		{"help", no_argument, NULL, 'h'},
		{"mode", required_argument, NULL, 'm'},
		{"no-audio", no_argument, NULL, 'a'},
		{"no-clipboard", no_argument, NULL, 'p'},
		{"no-grab", no_argument, NULL, 'g'},
		{"no-screensaver", no_argument, NULL, 'z'},
		{"peer", required_argument, NULL, 'y'},
		{"session", required_argument, NULL, 'x'},
		{"speed", required_argument, NULL, 's'},
		{"timeout", required_argument, NULL, 't'},
		{"version", no_argument, NULL, 'v'},
		{"width", required_argument, NULL, 'w'},
		{0, 0, 0, 0},
	};

	/* default values. */
	optind = 0;
	opterr = 0;

	if ((vdi_config = calloc(1, sizeof(vdi_config_s))) == NULL) {
		return VDI_STREAM_CLIENT_ERROR;
	}
	vdi_config->audio = 1;
	vdi_config->clipboard = 1;
	vdi_config->screensaver = 1;
	vdi_config->grab = 1;
	vdi_config->timeout = 5000;
	vdi_config->codec = 1;
	vdi_config->mode = 1;
	vdi_config->speed = 100;

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
			case 'u':
				vdi_config->height = strtol(argv[optind - 1], NULL, 10);
				continue;
			case 'w':
				vdi_config->width = strtol(argv[optind - 1], NULL, 10);
				continue;
			case 'a':
				vdi_config->audio = 0;
				continue;
			case 'p':
				vdi_config->clipboard = 0;
				continue;
			case 'z':
				vdi_config->screensaver = 0;
				continue;
			case 't':
				vdi_config->timeout = strtol(argv[optind - 1], NULL, 10) * 1000;
				continue;
			case 'c':
				if (strcmp(argv[optind - 1], "h265") == 0) {
					vdi_config->codec = 2;
				}
				continue;
			case 'm':
				if (strcmp(argv[optind - 1], "4:4:4") == 0) {
					printf("Parsec SDK bug and color mode 4:4:4 not working yet, details at:\n");
					printf("https://github.com/parsec-cloud/parsec-sdk/issues/36\n");
					vdi_config->mode = 2;
				}
				continue;
			case 's':
				speed = strtol(argv[optind - 1], NULL, 10);
				if (speed < 0) {
					vdi_config->speed = 0;
				} else if (speed > 500) {
					vdi_config->speed = 500;
				} else {
					vdi_config->speed = speed;
				}
				continue;
			case 'g':
				vdi_config->grab = 0;
				continue;
			case 'x':
				strncpy(vdi_config->session, argv[optind - 1], sizeof(vdi_config->session));
				vdi_config->session[128] = '\0';
				continue;
			case 'y':
				strncpy(vdi_config->peer, argv[optind - 1], sizeof(vdi_config->peer));
				vdi_config->peer[32] = '\0';
				continue;
			case 'h':
				vdi_stream_client__usage(program_name);
                                return VDI_STREAM_CLIENT_SUCCESS;
			case 'v':
				vdi_stream_client__version(program_name);
                                return VDI_STREAM_CLIENT_SUCCESS;
			case ':':
				fprintf(stderr, "%s: option `%s' requires an argument\n", program_name, argv[optind - 1]);
				fprintf(stderr, "Try `%s --help' for more information.\n", program_name);
				return VDI_STREAM_CLIENT_ERROR;
			case '?':
				fprintf(stderr, "%s: unrecognized option `%s'\n", program_name, argv[optind]);
				fprintf(stderr, "Try `%s --help' for more information.\n", program_name);
				return VDI_STREAM_CLIENT_ERROR;
                }
	}

	/* mandatory arguments not given. */
	if (strlen(vdi_config->session) == 0 || strlen(vdi_config->peer) == 0) {
		fprintf(stderr, "%s: mandatory arguments missing\n", program_name, argv[optind]);
		fprintf(stderr, "Try `%s --help' for more information.\n", program_name);
		return VDI_STREAM_CLIENT_ERROR;
	}

	/* additional non-option arguments given. */
	if (argc > optind) {
		fprintf(stderr, "%s: non-option arguments given\n", program_name, argv[optind]);
		fprintf(stderr, "Try `%s --help' for more information.\n", program_name);
		return VDI_STREAM_CLIENT_ERROR;
	}

	/* main event loop. */
	if (vdi_stream_client__event_loop(vdi_config) != 0) {
		return VDI_STREAM_CLIENT_ERROR;
	}

	/* quit application. */
	return VDI_STREAM_CLIENT_SUCCESS;
}
