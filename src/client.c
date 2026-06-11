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
 *
 *  Additional permission under GNU GPL version 3 section 7 is described in
 *  COPYING.EXCEPTION, allowing this program to link with the Parsec SDK.
 */

/* configuration includes. */
#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

/* sdl main include. */
#include <SDL3/SDL_main.h>

/* internal includes. */
#include "client.h"
#include "parsec.h"

/* system includes. */
#include <getopt.h>
#include <stdint.h>
#include <string.h>

/* this function show the usage. */
Sint32
vdi_stream_client__usage(char *program_name)
{

    /* show the help. */
    SDL_LogInfo(
        SDL_LOG_CATEGORY_APPLICATION,
        "Usage: %s [session] [peer] (options)...\n"
        "GPU accelerated remote host graphical console\n"
        "\n"
        "Help Options:\n"
        "  -h, --help               show this help screen\n"
        "  -v, --version            show the version information\n"
        "\n"
        "Debug Options:\n"
        "      --stats <seconds>    show render stats every <seconds> seconds\n"
        "\n"
        "Parsec Options:\n"
        "      --session <id>       session id for connection (mandatory)\n"
        "      --peer <id>          peer id for connection (mandatory)\n"
        "      --timeout <seconds>  connection timeout (default: 5 seconds)\n"
        "      --speed <speed>      mouse wheel sensitivity: 0 to 500 (default: 100)\n"
        "      --width <width>      horizontal resolution (default: host resolution)\n"
        "      --height <height>    vertical resolution (default: host resolution)\n"
        "\n"
        "Parsec Warp Options:\n"
        "      --no-subsampling     request 4:4:4 video without chroma subsampling\n"
        "\n"
        "Client Options:\n"
        "      --no-acceleration    disable hardware accelerated decoding\n"
        "      --no-upnp            disable upnp nat traversal\n"
        "      --no-reconnect       disable automatic reconnect in case of failures\n"
        "      --no-grab            disable exclusive mouse grab\n"
        "      --no-decoration      disable client window decorations\n"
        "      --no-screensaver     disable screen saver and lockers\n"
        "      --no-clipboard       disable clipboard sharing\n"
        "      --no-audio           disable audio streaming\n"
        "      --no-hevc            disable H.265/HEVC video codec\n"
        "\n"
        "USB Options:\n"
        "      --redirect <vendorID>:<productID>@<address>#<port>,[...]\n"
        "                           redirect single or multiple local USB devices\n"
        "\n"
        "Please report bugs to the appropriate authors, which can be found in the\n"
        "version information. All other things can be sent to <%s>\n",
        program_name, PACKAGE_BUGREPORT
    );

    /* if no error was found, return zero. */
    return VDI_STREAM_CLIENT_SUCCESS;
}

/* this function shows the version information. */
Sint32
vdi_stream_client__version(char *program_name)
{

    /* show the version. */
    SDL_LogInfo(
        SDL_LOG_CATEGORY_APPLICATION,
        "%s version %s Copyright (c) 2020-2026 The VDI Stream developers\n"
        "License GPLv3+: GNU GPL version 3 or later with Parsec SDK linking exception\n"
        "Written by %s\n"
        "\n"
        "This is free software; see the source for copying conditions.  There is NO\n"
        "warranty; not even for MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.\n",
        program_name, VERSION, AUTHOR
    );

    /* if no error was found, return zero. */
    return VDI_STREAM_CLIENT_SUCCESS;
}

/* main function to initialize structs and parse command line options. */
int
main(int argc, char **argv)
{

    /* variables. */
    Sint32 option_index = 0;
    Sint32 opt;
    char *program_name;
    vdi_config_s *vdi_config = NULL;
    static const vdi_server_addr_u zero_server_addr = { 0 };

    /* temp variables for command line parser. */
    Uint32 device;
    Sint32 type;
    char *redirect_list;
    char *redirect;
    char *item;
    char *delim;
    char *endptr;
    Sint64 port;
    Sint64 timeout;
    Sint64 speed;
    Sint64 width;
    Sint64 height;
    Sint64 stats_period;

    /* command line options. */
    enum
    {
        OPTION_HELP = 1,
        OPTION_VERSION = 2,
        OPTION_SESSION = 3,
        OPTION_PEER = 4,
        OPTION_TIMEOUT = 5,
        OPTION_SPEED = 6,
        OPTION_WIDTH = 7,
        OPTION_HEIGHT = 8,
        OPTION_NO_SUBSAMPLING = 9,
        OPTION_NO_ACCELERATION = 10,
        OPTION_NO_UPNP = 11,
        OPTION_NO_RECONNECT = 12,
        OPTION_NO_GRAB = 13,
        OPTION_NO_SCREENSAVER = 14,
        OPTION_NO_CLIPBOARD = 15,
        OPTION_NO_AUDIO = 16,
        OPTION_NO_HEVC = 17,
        OPTION_REDIRECT = 18,
        OPTION_STATS = 19,
        OPTION_NO_DECORATION = 20,
    };

    struct option long_options[] = {

        /* help options. */
        { "help", no_argument, NULL, OPTION_HELP },
        { "version", no_argument, NULL, OPTION_VERSION },

        /* debug options. */
        { "stats", required_argument, NULL, OPTION_STATS },

        /* parsec options. */
        { "session", required_argument, NULL, OPTION_SESSION },
        { "peer", required_argument, NULL, OPTION_PEER },
        { "timeout", required_argument, NULL, OPTION_TIMEOUT },
        { "speed", required_argument, NULL, OPTION_SPEED },
        { "width", required_argument, NULL, OPTION_WIDTH },
        { "height", required_argument, NULL, OPTION_HEIGHT },

        /* parsec warp options. */
        { "no-subsampling", no_argument, NULL, OPTION_NO_SUBSAMPLING },

        /* client options. */
        { "no-acceleration", no_argument, NULL, OPTION_NO_ACCELERATION },
        { "no-upnp", no_argument, NULL, OPTION_NO_UPNP },
        { "no-reconnect", no_argument, NULL, OPTION_NO_RECONNECT },
        { "no-grab", no_argument, NULL, OPTION_NO_GRAB },
        { "no-decoration", no_argument, NULL, OPTION_NO_DECORATION },
        { "no-screensaver", no_argument, NULL, OPTION_NO_SCREENSAVER },
        { "no-clipboard", no_argument, NULL, OPTION_NO_CLIPBOARD },
        { "no-audio", no_argument, NULL, OPTION_NO_AUDIO },
        { "no-hevc", no_argument, NULL, OPTION_NO_HEVC },

        /* usb options. */
        { "redirect", required_argument, NULL, OPTION_REDIRECT },

        /* end options. */
        { 0, 0, 0, 0 },
    };

    /* default values. */
    optind = 0;
    opterr = 0;

    /* allocate memory defaults. */
    if ((vdi_config = SDL_calloc(1, sizeof(vdi_config_s))) == NULL) {
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
    vdi_config->decoration = 1;
    vdi_config->screensaver = 1;
    vdi_config->clipboard = 1;
    vdi_config->audio = 1;
    vdi_config->hevc = 1;
    vdi_config->stats = 0;
    vdi_config->stats_period = 0;

    program_name = argv[0];
    if (program_name && SDL_strrchr(program_name, '/')) {
        program_name = SDL_strrchr(program_name, '/') + 1;
    }

    if (argc <= 1) {
        vdi_stream_client__usage(program_name);
        goto done;
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
        case OPTION_HELP:
            vdi_stream_client__usage(program_name);
            goto done;
        case 'v':
        case OPTION_VERSION:
            vdi_stream_client__version(program_name);
            goto done;

        /* parsec options. */
        case OPTION_SESSION:
            SDL_free(vdi_config->session);
            vdi_config->session = SDL_strdup(optarg);
            if (vdi_config->session == NULL) {
                goto error;
            }
            continue;
        case OPTION_PEER:
            SDL_free(vdi_config->peer);
            vdi_config->peer = SDL_strdup(optarg);
            if (vdi_config->peer == NULL) {
                goto error;
            }
            continue;
        case OPTION_TIMEOUT:
            timeout = SDL_strtoll(optarg, &endptr, 10);
            if (endptr == optarg || *endptr != '\0' || timeout <= 0 ||
                timeout > UINT32_MAX / 1000) {
                SDL_LogError(
                    SDL_LOG_CATEGORY_APPLICATION, "%s: invalid timeout: %s\n", program_name, optarg
                );
                SDL_LogError(
                    SDL_LOG_CATEGORY_APPLICATION, "Try `%s --help' for more information.\n",
                    program_name
                );
                goto error;
            }
            vdi_config->timeout = timeout * 1000;
            continue;
        case OPTION_SPEED:
            speed = SDL_strtol(optarg, &endptr, 10);
            if (endptr == optarg || *endptr != '\0') {
                SDL_LogError(
                    SDL_LOG_CATEGORY_APPLICATION, "%s: invalid speed: %s\n", program_name, optarg
                );
                SDL_LogError(
                    SDL_LOG_CATEGORY_APPLICATION, "Try `%s --help' for more information.\n",
                    program_name
                );
                goto error;
            }
            if (speed < 0) {
                speed = 0;
            }
            if (speed > 500) {
                speed = 500;
            }
            vdi_config->speed = speed;
            continue;
        case OPTION_WIDTH:
            width = SDL_strtol(optarg, &endptr, 10);
            if (endptr == optarg || *endptr != '\0' || width < 0 || width > UINT16_MAX) {
                SDL_LogError(
                    SDL_LOG_CATEGORY_APPLICATION, "%s: invalid width: %s\n", program_name, optarg
                );
                SDL_LogError(
                    SDL_LOG_CATEGORY_APPLICATION, "Try `%s --help' for more information.\n",
                    program_name
                );
                goto error;
            }
            vdi_config->width = width;
            continue;
        case OPTION_HEIGHT:
            height = SDL_strtol(optarg, &endptr, 10);
            if (endptr == optarg || *endptr != '\0' || height < 0 || height > UINT16_MAX) {
                SDL_LogError(
                    SDL_LOG_CATEGORY_APPLICATION, "%s: invalid height: %s\n", program_name, optarg
                );
                SDL_LogError(
                    SDL_LOG_CATEGORY_APPLICATION, "Try `%s --help' for more information.\n",
                    program_name
                );
                goto error;
            }
            vdi_config->height = height;
            continue;

        /* parsec warp options. */
        case OPTION_NO_SUBSAMPLING:
            vdi_config->subsampling = 0;
            continue;

        /* client options. */
        case OPTION_NO_ACCELERATION:
            vdi_config->acceleration = 0;
            continue;
        case OPTION_NO_UPNP:
            vdi_config->upnp = 0;
            continue;
        case OPTION_NO_RECONNECT:
            vdi_config->reconnect = 0;
            continue;
        case OPTION_NO_GRAB:
            vdi_config->grab = 0;
            continue;
        case OPTION_NO_DECORATION:
            vdi_config->decoration = 0;
            continue;
        case OPTION_NO_SCREENSAVER:
            vdi_config->screensaver = 0;
            continue;
        case OPTION_NO_CLIPBOARD:
            vdi_config->clipboard = 0;
            continue;
        case OPTION_NO_AUDIO:
            vdi_config->audio = 0;
            continue;
        case OPTION_NO_HEVC:
            vdi_config->hevc = 0;
            continue;
        case OPTION_STATS:
            stats_period = SDL_strtol(optarg, &endptr, 10);
            if (*endptr != '\0' || stats_period <= 0) {
                SDL_LogError(
                    SDL_LOG_CATEGORY_APPLICATION, "%s: invalid stats period: %s\n", program_name,
                    optarg
                );
                SDL_LogError(
                    SDL_LOG_CATEGORY_APPLICATION, "Try `%s --help' for more information.\n",
                    program_name
                );
                goto error;
            }
            vdi_config->stats = 1;
            vdi_config->stats_period = stats_period;
            continue;

        /* usb options. */
        case OPTION_REDIRECT:

            /* loop through multiple redirect configs. */
            device = vdi_config->usb_count;
            redirect_list = optarg;
            while ((redirect = strsep(&redirect_list, ",")) != NULL) {

                /* check if number of usb redirects are out of range. */
                if (device >= USB_MAX) {
                    SDL_LogError(
                        SDL_LOG_CATEGORY_APPLICATION, "%s: usb redirection limit of %d reached\n",
                        program_name, USB_MAX
                    );
                    SDL_LogError(
                        SDL_LOG_CATEGORY_APPLICATION, "Try `%s --help' for more information.\n",
                        program_name
                    );
                    goto error;
                }

                /* initialize. */
                vdi_config->server_addrs[device] = zero_server_addr;

                /* loop through redirect categroy. (usb vendor, product, address and port) */
                type = 0;
                while ((item = strsep(&redirect, "@#")) != NULL) {

                    /* reject additional fields after port. */
                    if (type > 2) {
                        SDL_LogError(
                            SDL_LOG_CATEGORY_APPLICATION,
                            "%s: invalid usb redirection format: too many fields\n", program_name
                        );
                        SDL_LogError(
                            SDL_LOG_CATEGORY_APPLICATION, "Try `%s --help' for more information.\n",
                            program_name
                        );
                        goto error;
                    }

                    /* usb device. */
                    if (type == 0) {

                        /* check if usb vendor is out of range. */
                        vdi_config->usb_devices[device].vendor = SDL_strtol(item, &endptr, 16);
                        if (vdi_config->usb_devices[device].vendor <= 0 ||
                            vdi_config->usb_devices[device].vendor > 0xffff) {
                            SDL_LogError(
                                SDL_LOG_CATEGORY_APPLICATION,
                                "%s: invalid vendor identifier in usb device: %s\n", program_name,
                                item
                            );
                            SDL_LogError(
                                SDL_LOG_CATEGORY_APPLICATION,
                                "Try `%s --help' for more information.\n", program_name
                            );
                            goto error;
                        }

                        /* check if usb product is given. */
                        delim = SDL_strchr(item, ':');
                        if (*endptr != ':' || delim == NULL || SDL_strlen(delim) == 1) {
                            SDL_LogError(
                                SDL_LOG_CATEGORY_APPLICATION,
                                "%s: no product identifier in usb device\n", program_name
                            );
                            SDL_LogError(
                                SDL_LOG_CATEGORY_APPLICATION,
                                "Try `%s --help' for more information.\n", program_name
                            );
                            goto error;
                        }

                        /* check if usb product is out of range. (product id 0000 is valid) */
                        vdi_config->usb_devices[device].product =
                            SDL_strtol(delim + 1, &endptr, 16);
                        if (*endptr != '\0' || vdi_config->usb_devices[device].product < 0 ||
                            vdi_config->usb_devices[device].product > 0xffff) {
                            SDL_LogError(
                                SDL_LOG_CATEGORY_APPLICATION,
                                "%s: invalid product identifier in usb device: %s\n", program_name,
                                item
                            );
                            SDL_LogError(
                                SDL_LOG_CATEGORY_APPLICATION,
                                "Try `%s --help' for more information.\n", program_name
                            );
                            goto error;
                        }
                    }

                    /* address. */
                    if (type == 1) {

                        /* check if empty ip address given. */
                        if (item[0] == '\0') {
                            SDL_LogError(
                                SDL_LOG_CATEGORY_APPLICATION,
                                "%s: no ip address in usb redirection\n", program_name
                            );
                            SDL_LogError(
                                SDL_LOG_CATEGORY_APPLICATION,
                                "Try `%s --help' for more information.\n", program_name
                            );
                            goto error;
                        }

                        /* check if ipv4 address. */
                        if (inet_pton(
                                AF_INET, item, &vdi_config->server_addrs[device].v4.sin_addr
                            ) == 1) {
                            vdi_config->server_addrs[device].v4.sin_family = AF_INET;
                        }

                        /* check if ipv6 address. */
                        if (inet_pton(
                                AF_INET6, item, &vdi_config->server_addrs[device].v6.sin6_addr
                            ) == 1) {
                            vdi_config->server_addrs[device].v6.sin6_family = AF_INET6;
                        }

                        /* check if bad formatted address. */
                        if (vdi_config->server_addrs[device].v4.sin_family != AF_INET &&
                            vdi_config->server_addrs[device].v6.sin6_family != AF_INET6) {
                            SDL_LogError(
                                SDL_LOG_CATEGORY_APPLICATION,
                                "%s: invalid address in usb redirection: %s\n", program_name, item
                            );
                            SDL_LogError(
                                SDL_LOG_CATEGORY_APPLICATION,
                                "Try `%s --help' for more information.\n", program_name
                            );
                            goto error;
                        }
                    }

                    /* port. */
                    if (type == 2) {

                        /* check if empty port given. */
                        if (item[0] == '\0') {
                            SDL_LogError(
                                SDL_LOG_CATEGORY_APPLICATION, "%s: no port in usb redirection\n",
                                program_name
                            );
                            SDL_LogError(
                                SDL_LOG_CATEGORY_APPLICATION,
                                "Try `%s --help' for more information.\n", program_name
                            );
                            goto error;
                        }

                        /* convert port string to integer. */
                        port = SDL_strtol(item, &endptr, 10);

                        /* check if port is out of range. */
                        if (endptr == item || *endptr != '\0' || port <= 0 || port >= 65536) {
                            SDL_LogError(
                                SDL_LOG_CATEGORY_APPLICATION,
                                "%s: invalid port in usb redirection: %s\n", program_name, item
                            );
                            SDL_LogError(
                                SDL_LOG_CATEGORY_APPLICATION,
                                "Try `%s --help' for more information.\n", program_name
                            );
                            goto error;
                        }

                        /* check if ipv4 address has been assigned previosuly. */
                        if (vdi_config->server_addrs[device].v4.sin_family == AF_INET) {
                            vdi_config->server_addrs[device].v4.sin_port = htons(port);
                        }

                        /* check if ipv6 address has been assigned previosuly. */
                        if (vdi_config->server_addrs[device].v6.sin6_family == AF_INET6) {
                            vdi_config->server_addrs[device].v6.sin6_port = htons(port);
                        }
                    }
                    type++;
                }

                /* check if no ip address was given. */
                if (type == 1) {
                    SDL_LogError(
                        SDL_LOG_CATEGORY_APPLICATION, "%s: no ip address in usb redirection\n",
                        program_name
                    );
                    SDL_LogError(
                        SDL_LOG_CATEGORY_APPLICATION, "Try `%s --help' for more information.\n",
                        program_name
                    );
                    goto error;
                }

                /* check if no port was given. */
                if (type == 2) {
                    SDL_LogError(
                        SDL_LOG_CATEGORY_APPLICATION, "%s: no port in usb redirection\n",
                        program_name
                    );
                    SDL_LogError(
                        SDL_LOG_CATEGORY_APPLICATION, "Try `%s --help' for more information.\n",
                        program_name
                    );
                    goto error;
                }

                /* exactly usb device, ip address and port must be given. */
                if (type != 3) {
                    SDL_LogError(
                        SDL_LOG_CATEGORY_APPLICATION, "%s: invalid usb redirection format\n",
                        program_name
                    );
                    SDL_LogError(
                        SDL_LOG_CATEGORY_APPLICATION, "Try `%s --help' for more information.\n",
                        program_name
                    );
                    goto error;
                }

                device++;
                vdi_config->usb_count = device;
            }
            continue;

        /* missing arguments. */
        case ':':
            SDL_LogError(
                SDL_LOG_CATEGORY_APPLICATION, "%s: option `%s' requires an argument\n",
                program_name, argv[optind - 1]
            );
            SDL_LogError(
                SDL_LOG_CATEGORY_APPLICATION, "Try `%s --help' for more information.\n",
                program_name
            );
            goto error;

        /* unknown switches. */
        case '?':
            SDL_LogError(
                SDL_LOG_CATEGORY_APPLICATION, "%s: unrecognized option `%s'\n", program_name,
                argv[optind - 1]
            );
            SDL_LogError(
                SDL_LOG_CATEGORY_APPLICATION, "Try `%s --help' for more information.\n",
                program_name
            );
            goto error;
        }
    }

    /* mandatory arguments not given. */
    if (vdi_config->session == NULL || vdi_config->peer == NULL ||
        SDL_strlen(vdi_config->session) == 0 || SDL_strlen(vdi_config->peer) == 0) {
        SDL_LogError(
            SDL_LOG_CATEGORY_APPLICATION, "%s: mandatory arguments missing\n", program_name
        );
        SDL_LogError(
            SDL_LOG_CATEGORY_APPLICATION, "Try `%s --help' for more information.\n", program_name
        );
        goto error;
    }

    /* width and height must be specified together. */
    if ((vdi_config->width == 0) != (vdi_config->height == 0)) {
        SDL_LogError(
            SDL_LOG_CATEGORY_APPLICATION, "%s: --width and --height must be specified together\n",
            program_name
        );
        SDL_LogError(
            SDL_LOG_CATEGORY_APPLICATION, "Try `%s --help' for more information.\n", program_name
        );
        goto error;
    }

    /* additional non-option arguments given. */
    if (argc > optind) {
        SDL_LogError(
            SDL_LOG_CATEGORY_APPLICATION, "%s: non-option argument `%s'\n", program_name,
            argv[optind]
        );
        SDL_LogError(
            SDL_LOG_CATEGORY_APPLICATION, "Try `%s --help' for more information.\n", program_name
        );
        goto error;
    }

    /* main event loop. */
    if (vdi_stream_client__event_loop(vdi_config) != 0) {
        goto error;
    }

    /* quit application. */
    goto done;

error:

    /* free allocated memory and quit application with error code. */
    if (vdi_config != NULL) {
        SDL_free(vdi_config->session);
        SDL_free(vdi_config->peer);
        SDL_free(vdi_config);
    }
    return VDI_STREAM_CLIENT_ERROR;

done:

    /* free allocated memory and quit application. */
    if (vdi_config != NULL) {
        SDL_free(vdi_config->session);
        SDL_free(vdi_config->peer);
        SDL_free(vdi_config);
    }
    return VDI_STREAM_CLIENT_SUCCESS;
}
