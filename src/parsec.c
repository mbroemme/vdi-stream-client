/*
 *  parsec.c -- desktop streaming with parsec sdk
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
#include "audio.h"
#include "client.h"
#include "parsec.h"
#include "redirect.h"
#include "video.h"

/* font include. */
#include "../include/font.h"

/* system includes. */
#include <limits.h>
#include <stdlib.h>
#include <stdio.h>
#include <unistd.h>

/* sdl includes. */

/* parsec clipboard event. */
static void vdi_stream_client__clipboard(struct parsec_context_s *parsec_context, Uint32 id, Uint32 buffer_key) {
	char *msg = ParsecGetBuffer(parsec_context->parsec, buffer_key);

	if (msg && id == PARSEC_CLIPBOARD_MSG) {
		SDL_SetClipboardText(msg);
	}

#ifdef HAVE_LIBPARSEC
	ParsecFree(msg);
#else
	ParsecFree(parsec_context->parsec, msg);
#endif
}

/* parsec cursor event. */
static void vdi_stream_client__cursor(struct parsec_context_s *parsec_context, ParsecCursor *cursor, Uint32 buffer_key, bool grab, bool grab_forced) {
	if (cursor->imageUpdate) {
		Uint8 *image = ParsecGetBuffer(parsec_context->parsec, buffer_key);

		if (image != NULL) {
			SDL_Surface *surface = SDL_CreateSurfaceFrom(cursor->width, cursor->height,
				SDL_PIXELFORMAT_RGBA32, image, cursor->width * 4);
			SDL_Cursor *sdlCursor = NULL;

			if (surface != NULL) {
				sdlCursor = SDL_CreateColorCursor(surface, cursor->hotX, cursor->hotY);
				SDL_DestroySurface(surface);
			}

			if (sdlCursor != NULL) {
				SDL_SetCursor(sdlCursor);
				SDL_DestroyCursor(parsec_context->cursor);
				parsec_context->cursor = sdlCursor;
			}

#ifdef HAVE_LIBPARSEC
			ParsecFree(image);
#else
			ParsecFree(parsec_context->parsec, image);
#endif
		}
	}

	if (!SDL_GetWindowRelativeMouseMode(parsec_context->window) &&
	   (cursor->relative || cursor->hidden)) {
		SDL_HideCursor();
		SDL_SetWindowRelativeMouseMode(parsec_context->window, true);
		if (!parsec_context->pressed && !grab_forced) {
			SDL_SetWindowTitle(parsec_context->window, "VDI Stream Client (Press Ctrl+Alt to release grab)");
		}
		parsec_context->relative = cursor->relative;
	} else if (SDL_GetWindowRelativeMouseMode(parsec_context->window) &&
	   (!cursor->relative || !cursor->hidden)) {
		SDL_SetWindowRelativeMouseMode(parsec_context->window, false);
		SDL_ShowCursor();
		if (!parsec_context->pressed && !grab_forced && !grab) {
			SDL_SetWindowTitle(parsec_context->window, "VDI Stream Client");
		}
		parsec_context->relative = cursor->relative;
	}
}

/* render text. */
Sint32 vdi_stream_client__render_text(void *opaque, char *text) {
	struct parsec_context_s *parsec_context = (struct parsec_context_s *) opaque;
	SDL_Color color = { 0x88, 0x88, 0x88, 0xFF };
	GLfloat texture_coord[4];
	GLenum gl_error;

	if (parsec_context->texture_ttf != 0) {
		glDeleteTextures(1, &parsec_context->texture_ttf);
		parsec_context->texture_ttf = 0;
	}

	if (parsec_context->surface_ttf != NULL) {
		SDL_DestroySurface(parsec_context->surface_ttf);
		parsec_context->surface_ttf = NULL;
	}

	/* create the text surface. */
	parsec_context->surface_ttf = TTF_RenderText_Blended(parsec_context->font, text, 0, color);
	if (parsec_context->surface_ttf == NULL) {
		vdi_stream_client__log_error("TTF surface creation failed: %s\n", SDL_GetError());
		return VDI_STREAM_CLIENT_ERROR;
	}

	/* convert the text into an opengl texture. */
	parsec_context->texture_ttf = vdi_stream_client__gl_load_texture(parsec_context->surface_ttf, texture_coord);
	if ((gl_error = glGetError()) != GL_NO_ERROR) {
		vdi_stream_client__log_error("TTF OpenGL texture creation failed: 0x%x\n", gl_error);
		return VDI_STREAM_CLIENT_ERROR;
	}

	/* make texture coordinates easy to understand. */
	parsec_context->texture_min_x = texture_coord[0];
	parsec_context->texture_min_y = texture_coord[1];
	parsec_context->texture_max_x = texture_coord[2];
	parsec_context->texture_max_y = texture_coord[3];

	/* no error. */
	return VDI_STREAM_CLIENT_SUCCESS;
}

/* parsec event loop. */
Sint32 vdi_stream_client__event_loop(vdi_config_s *vdi_config) {
	struct parsec_context_s parsec_context = {0};
	struct redirect_context_s redirect_context[USB_MAX] = {0};
	bool grab_forced = false;
	Uint32 wait_time = 0;
	Uint64 last_time = 0;
	bool force_redraw = false;
	float x = 0.0f;
	float y = 0.0f;
	SDL_AudioSpec want = {0};
	ParsecStatus e;
	ParsecConfig network_cfg = PARSEC_DEFAULTS;
	ParsecClientConfig cfg = PARSEC_CLIENT_DEFAULTS;
	Uint32 device;
	Uint32 count;
	SDL_Thread *audio_thread = NULL;
	SDL_Thread *network_thread[USB_MAX] = {0};

	/* default values. */
	parsec_context.timeout = 100;
	parsec_context.render_timeout = 5;
	parsec_context.next_overlay_tick = 0;

	/* sdl init. */
	vdi_stream_client__log_info("Initialize SDL\n");
	if (!SDL_Init(SDL_INIT_VIDEO | SDL_INIT_AUDIO)) {
		vdi_stream_client__log_error("Initialization failed: %s\n", SDL_GetError());
		goto error;
	}

	/* ttf init. */
	vdi_stream_client__log_info("Initialize TTF\n");
	if (!TTF_Init()) {
		vdi_stream_client__log_error("Initialization failed: %s\n", SDL_GetError());
		goto error;
	}

	/* load font. */
	parsec_context.font = TTF_OpenFontIO(SDL_IOFromMem(MorePerfectDOSVGA_ttf, MorePerfectDOSVGA_ttf_len), true, 16.0f);
	if (parsec_context.font == NULL) {
		vdi_stream_client__log_error("Loading font failed: %s\n", SDL_GetError());
		goto error;
	}

	/* parsec init. */
	vdi_stream_client__log_info("Initialize Parsec\n");
#ifdef HAVE_LIBPARSEC
	e = ParsecInit(PARSEC_VER, &network_cfg, NULL, &parsec_context.parsec);
#else
	e = ParsecInit(NULL, &network_cfg, "libparsec.so", &parsec_context.parsec);
#endif
	if (e != PARSEC_OK) {
		vdi_stream_client__log_error("Initialization failed with code: %d\n", e);
		goto error;
	}

	/* use client resolution if specified. */
	if (vdi_config->width > 0 && vdi_config->height > 0) {
		vdi_stream_client__log_info("Override resolution %dx%d\n", vdi_config->width, vdi_config->height);
		cfg.video[DEFAULT_STREAM].resolutionX = vdi_config->width;
		cfg.video[DEFAULT_STREAM].resolutionY = vdi_config->height;
	}

	/* configure host video codec. */
	if (vdi_config->hevc == 1) {
		cfg.video[DEFAULT_STREAM].decoderH265 = 1;
	}
	if (vdi_config->hevc == 0) {
		vdi_stream_client__log_info("Disable H.265 (HEVC) Video Codec\n");
		cfg.video[DEFAULT_STREAM].decoderH265 = 0;
	}

	/* configure host color mode. */
	if (vdi_config->subsampling == 1) {
		cfg.video[DEFAULT_STREAM].decoder444 = 0;
	}
	if (vdi_config->subsampling == 0) {
		vdi_stream_client__log_info("Disable Chroma Subsampling\n");
		cfg.video[DEFAULT_STREAM].decoder444 = 1;

		/* TODO: parsec sdk bug. */
		vdi_stream_client__log_info("WARNING: Parsec SDK bug and color mode 4:4:4 not working yet, details at:\n");
		vdi_stream_client__log_info("WARNING: https://github.com/parsec-cloud/parsec-sdk/issues/36\n");
	}

	/* configure client decoding acceleration. */
	if (vdi_config->acceleration == 1) {
		cfg.video[DEFAULT_STREAM].decoderIndex = 1;
	}
	if (vdi_config->acceleration == 0) {
		vdi_stream_client__log_info("Disable Hardware Accelerated Video Decoding\n");
		cfg.video[DEFAULT_STREAM].decoderIndex = 0;
	}

	/* configure upnp. */
	if (vdi_config->upnp == 1) {
		network_cfg.upnp = 1;
	}
	if (vdi_config->upnp == 0) {
		vdi_stream_client__log_info("Disable UPnP\n");
		network_cfg.upnp = 0;
	}

	/* check if reconnect should be disabled. */
	if (vdi_config->reconnect == 0) {
		vdi_stream_client__log_info("Disable automatic reconnect\n");
	}

	/* check if exclusive mouse grab should be disabled. */
	if (vdi_config->grab == 0) {
		vdi_stream_client__log_info("Disable exclusive mouse grab\n");
	}

	/* configure screen saver. */
	if (vdi_config->screensaver == 1) {
		SDL_EnableScreenSaver();
	}
	if (vdi_config->screensaver == 0) {
		vdi_stream_client__log_info("Disable screen saver\n");
		SDL_DisableScreenSaver();
	}

	/* check if clipboard should be disabled. */
	if (vdi_config->clipboard == 0) {
		vdi_stream_client__log_info("Disable clipboard sharing\n");
	}

	/* check if audio should be streamed. */
	if (vdi_config->audio == 0) {
		vdi_stream_client__log_info("Disable audio streaming\n");
	}

	/* parsec connect. */
	vdi_stream_client__log_info("Connect to Parsec service\n");
	e = ParsecClientConnect(parsec_context.parsec, &cfg, vdi_config->session, vdi_config->peer);
	if (e != PARSEC_OK) {
		vdi_stream_client__log_error("Connection failed with code: %d\n", e);
		goto error;
	}

	/* wait until connection is established. */
	vdi_stream_client__log_info("Connect to Parsec host\n");
	while (!parsec_context.decoder) {

		/* get client status. */
		e = ParsecClientGetStatus(parsec_context.parsec, &parsec_context.client_status);

		/* connection established */
		if (e == PARSEC_OK) {

			/* decoder not yet initialized. */
			if (parsec_context.client_status.decoder->width == 0 &&
			    parsec_context.client_status.decoder->height == 0 &&
			    !parsec_context.connection) {
				vdi_stream_client__log_info("Initialize Video Decoder\n");
				parsec_context.connection = true;
			}

			/* decoder initialized. */
			if (parsec_context.client_status.decoder->width > 0 &&
			    parsec_context.client_status.decoder->height > 0) {
				vdi_stream_client__log_info("Use resolution %dx%d\n", parsec_context.client_status.decoder->width, parsec_context.client_status.decoder->height);
				parsec_context.window_width = parsec_context.client_status.decoder->width;
				parsec_context.window_height = parsec_context.client_status.decoder->height;
				parsec_context.decoder = true;
			}
		}

		/* unknown error. */
		if (e != PARSEC_CONNECTING && e != PARSEC_OK) {
			break;
		}

		/* check if timeout reached. */
		if (wait_time >= vdi_config->timeout) {
			break;
		}

		/* wait some time and re-check. */
		SDL_Delay(250);
		wait_time = wait_time + 250;
	}

	/* check if connected and decoder initialized. */
	if (!parsec_context.connection &&
	    !parsec_context.decoder) {
		vdi_stream_client__log_error("Connection failed with code: %d\n", e);
		goto error;
	}

	/* check if connected but decoder initialization failed. */
	if (parsec_context.connection &&
	    !parsec_context.decoder) {

		/* TODO: workaround if decoder initialization failed. (workaround for buggy parsec sdk) */
		parsec_context.window_width = 640;
		parsec_context.window_height = 480;
	}

	parsec_context.window = SDL_CreateWindow("VDI Stream Client",
					parsec_context.window_width,
					parsec_context.window_height,
					SDL_WINDOW_OPENGL | SDL_WINDOW_HIGH_PIXEL_DENSITY
				);
	if (parsec_context.window == NULL) {
		vdi_stream_client__log_error("Window creation failed: %s\n", SDL_GetError());
		goto error;
	}

	parsec_context.gl = SDL_GL_CreateContext(parsec_context.window);
	if (parsec_context.gl == NULL) {
		vdi_stream_client__log_error("OpenGL context creation failed: %s\n", SDL_GetError());
		goto error;
	}

	if (!SDL_GL_MakeCurrent(parsec_context.window, parsec_context.gl)) {
		vdi_stream_client__log_error("OpenGL context activation failed: %s\n", SDL_GetError());
		goto error;
	}

	vdi_stream_client__video_init(&parsec_context);

	/* check if audio should be streamed. */
	if (vdi_config->audio == 1) {
		want.freq = PARSEC_AUDIO_SAMPLE_RATE;
		want.format = SDL_AUDIO_S16;
		want.channels = PARSEC_AUDIO_CHANNELS;

		/* the number of audio packets (960 frames) to buffer before we begin playing. */
		parsec_context.min_buffer = 1;

		/* the number of audio packets (960 frames) to buffer before overflow and clear. */
		parsec_context.max_buffer = 6;

		/* sdl audio device. */
		parsec_context.audio = SDL_OpenAudioDeviceStream(SDL_AUDIO_DEVICE_DEFAULT_PLAYBACK, &want, NULL, NULL);
		if (parsec_context.audio == NULL) {
			vdi_stream_client__log_error("Failed to open audio: %s\n", SDL_GetError());
			goto error;
		}

		/* sdl audio thread. */
		audio_thread = SDL_CreateThread(vdi_stream_client__audio_thread, "vdi_stream_client__audio_thread", &parsec_context);
		if (audio_thread == NULL) {
			vdi_stream_client__log_error("Audio thread creation failed: %s\n", SDL_GetError());
			goto error;
		}
	}

	/* configure usb. */
	if (vdi_config->usb_devices[0].vendor != 0) {
		vdi_stream_client__log_info("Initialize USB\n");

		/* one thread per one usb device redirect. */
		for (device = 0; vdi_config->usb_devices[device].vendor != 0 ; device++) {

			/* store main thread context in a pointer. */
			redirect_context[device].parsec_context = &parsec_context;

			/* prepare data for network thread. */
			redirect_context[device].server_addr.v4 = vdi_config->server_addrs[device].v4;
			redirect_context[device].server_addr.v6 = vdi_config->server_addrs[device].v6;
			redirect_context[device].usb_device.vendor = vdi_config->usb_devices[device].vendor;
			redirect_context[device].usb_device.product = vdi_config->usb_devices[device].product;

			/* sdl network thread. */
			network_thread[device] = SDL_CreateThread(vdi_stream_client__network_thread, "vdi_stream_client__network_thread", &redirect_context[device]);
			if (network_thread[device] == NULL) {
				vdi_stream_client__log_error("Network thread creation failed: %s\n", SDL_GetError());
				goto error;
			}
		}
	}

	/* event loop. */
	while (!parsec_context.done) {
		force_redraw = false;

		for (SDL_Event msg; SDL_PollEvent(&msg);) {
			ParsecMessage pmsg = {0};
			force_redraw = true;

			switch (msg.type) {
				case SDL_EVENT_QUIT:
					parsec_context.done = true;

					/* render shutdown text. */
					vdi_stream_client__render_text(&parsec_context, "Closing...");
					force_redraw = true;
					break;
				case SDL_EVENT_KEY_UP:
					pmsg.type = MESSAGE_KEYBOARD;
					pmsg.keyboard.code = (ParsecKeycode) msg.key.scancode;
					pmsg.keyboard.mod = msg.key.mod;
					pmsg.keyboard.pressed = false;
					break;
				case SDL_EVENT_KEY_DOWN:

					/* check if we need to switch window grab state. */
					if ((msg.key.mod & SDL_KMOD_LCTRL) != 0 &&
					    (msg.key.mod & SDL_KMOD_LALT) != 0) {

						/* check if no forced grab. */
						if (!grab_forced) {

							/* check if no mouse button is hold down. */
							if ((SDL_GetMouseState(NULL, NULL) & SDL_BUTTON_LMASK) == 0 &&
							    (SDL_GetMouseState(NULL, NULL) & SDL_BUTTON_MMASK) == 0 &&
							    (SDL_GetMouseState(NULL, NULL) & SDL_BUTTON_RMASK) == 0) {

								/* check if we need to release mouse grab. */
								if (vdi_config->grab == 1 && SDL_GetWindowMouseGrab(parsec_context.window)) {
									SDL_SetWindowMouseGrab(parsec_context.window, false);
								}

								/* check if we need to release relative grab. */
								if (SDL_GetWindowRelativeMouseMode(parsec_context.window)) {
									SDL_SetWindowRelativeMouseMode(parsec_context.window, false);
									SDL_ShowCursor();
								}

								/* remove grab information from window title. */
								SDL_SetWindowTitle(parsec_context.window, "VDI Stream Client");

								/* don't send hotkey to host and break execution. */
								break;
							}
						}
					}

					/* check if we need to toggle runtime configuration. */
					if ((msg.key.mod & SDL_KMOD_LSHIFT) != 0) {

						/* check if we need to toggle forced grab. */
						if (msg.key.key == SDLK_F12) {
							if (grab_forced) {

								/* re-enable screensaver if leaving forced lock. */
								if (vdi_config->screensaver == 1) {
									SDL_EnableScreenSaver();
								}

								/* check if we need to release relative grab. */
								if (SDL_GetWindowRelativeMouseMode(parsec_context.window)) {
									SDL_SetWindowRelativeMouseMode(parsec_context.window, false);
									SDL_ShowCursor();
								}

								SDL_SetWindowMouseGrab(parsec_context.window, false);
								SDL_SetWindowTitle(parsec_context.window, "VDI Stream Client");
								grab_forced = false;
							} else {

								/* disable screensaver if entering forced lock. */
								if (vdi_config->screensaver == 1) {
									SDL_DisableScreenSaver();
								}

								/* check if we need to grab mouse in relative mode. */
								if (parsec_context.relative && !SDL_GetWindowRelativeMouseMode(parsec_context.window)) {
									SDL_HideCursor();
									SDL_SetWindowRelativeMouseMode(parsec_context.window, true);
								}

								SDL_SetWindowMouseGrab(parsec_context.window, true);
								SDL_SetWindowTitle(parsec_context.window, "VDI Stream Client (Press Shift+F12 to release forced grab)");
								grab_forced = true;
							}

							/* don't send hotkey to host and break execution. */
							break;
						}
					}

					pmsg.type = MESSAGE_KEYBOARD;
					pmsg.keyboard.code = (ParsecKeycode) msg.key.scancode;
					pmsg.keyboard.mod = msg.key.mod;
					pmsg.keyboard.pressed = true;
					break;
				case SDL_EVENT_MOUSE_MOTION:

					/* check if we released relative mouse grab. */
					if (parsec_context.relative && !SDL_GetWindowRelativeMouseMode(parsec_context.window)) {

						/* no mouse motion events should be forwarded. */
						break;
					}

					pmsg.type = MESSAGE_MOUSE_MOTION;
					pmsg.mouseMotion.relative = SDL_GetWindowRelativeMouseMode(parsec_context.window);
					pmsg.mouseMotion.x = pmsg.mouseMotion.relative ? (Sint32) msg.motion.xrel : (Sint32) msg.motion.x + 1;
					pmsg.mouseMotion.y = pmsg.mouseMotion.relative ? (Sint32) msg.motion.yrel : (Sint32) msg.motion.y + 1;
					break;
				case SDL_EVENT_MOUSE_BUTTON_UP:

					/* store mouse button state for use in cursor update. */
					parsec_context.pressed = false;

					pmsg.type = MESSAGE_MOUSE_BUTTON;
					pmsg.mouseButton.button = msg.button.button;
					pmsg.mouseButton.pressed = false;
					break;
				case SDL_EVENT_MOUSE_BUTTON_DOWN:

					/* check if no forced grab. */
					if (!grab_forced) {

						/* check if we need to grab mouse. */
						if (vdi_config->grab == 1 && !SDL_GetWindowMouseGrab(parsec_context.window)) {
							SDL_SetWindowMouseGrab(parsec_context.window, true);
							SDL_SetWindowTitle(parsec_context.window, "VDI Stream Client (Press Ctrl+Alt to release grab)");
						}

						/* check if we need to grab mouse in relative mode. */
						if (parsec_context.relative && !SDL_GetWindowRelativeMouseMode(parsec_context.window)) {
							SDL_HideCursor();
							SDL_SetWindowRelativeMouseMode(parsec_context.window, true);
							SDL_SetWindowTitle(parsec_context.window, "VDI Stream Client (Press Ctrl+Alt to release grab)");
						}
					}

					/* store mouse button state for use in cursor update. */
					parsec_context.pressed = true;

					pmsg.type = MESSAGE_MOUSE_BUTTON;
					pmsg.mouseButton.button = msg.button.button;
					pmsg.mouseButton.pressed = true;
					break;
				case SDL_EVENT_MOUSE_WHEEL:
					pmsg.type = MESSAGE_MOUSE_WHEEL;
					pmsg.mouseWheel.x = msg.wheel.x * vdi_config->speed;
					pmsg.mouseWheel.y = msg.wheel.y * vdi_config->speed;
					break;
				case SDL_EVENT_CLIPBOARD_UPDATE:
					if (vdi_config->clipboard == 1) {
						char *clipboard = SDL_GetClipboardText();

						if (clipboard != NULL) {
							ParsecClientSendUserData(parsec_context.parsec, PARSEC_CLIPBOARD_MSG, clipboard);
							SDL_free(clipboard);
						}
					}
					break;
				case SDL_EVENT_WINDOW_MOUSE_ENTER:
					SDL_SetEventEnabled(SDL_EVENT_KEY_DOWN, true);
					SDL_SetEventEnabled(SDL_EVENT_KEY_UP, true);
					SDL_SetWindowKeyboardGrab(parsec_context.window, true);
					break;
				case SDL_EVENT_WINDOW_MOUSE_LEAVE:
					SDL_SetEventEnabled(SDL_EVENT_KEY_DOWN, false);
					SDL_SetEventEnabled(SDL_EVENT_KEY_UP, false);
					SDL_SetWindowKeyboardGrab(parsec_context.window, false);
					break;
			}

			if (pmsg.type != 0) {
				ParsecClientSendMessage(parsec_context.parsec, &pmsg);
			}
		}

		/* prioritize SDL responsiveness after local interaction. */
		parsec_context.render_timeout = force_redraw ? 0 : 5;

		/* check parsec connection status. */
		e = ParsecClientGetStatus(parsec_context.parsec, &parsec_context.client_status);
		if (vdi_config->reconnect == 0 && e != PARSEC_CONNECTING && e != PARSEC_OK) {

			/* render shutdown text. */
			vdi_stream_client__render_text(&parsec_context, "Closing...");
			force_redraw = true;
			vdi_stream_client__log_error("Parsec disconnected\n");
			parsec_context.done = true;
		}
		if (vdi_config->reconnect == 1 && e != PARSEC_CONNECTING && e != PARSEC_OK &&
		    SDL_GetTicks() > last_time + vdi_config->timeout) {

			/* render reconnect text. */
			vdi_stream_client__render_text(&parsec_context, "Reconnecting...");
			force_redraw = true;
			ParsecClientDisconnect(parsec_context.parsec);
			ParsecClientConnect(parsec_context.parsec, &cfg, vdi_config->session, vdi_config->peer);
			parsec_context.connection = false;
			last_time = SDL_GetTicks();
		}

		/* check network connection status. */
		if (vdi_config->reconnect == 0 && parsec_context.client_status.networkFailure == 1) {

			/* render shutdown text. */
			vdi_stream_client__render_text(&parsec_context, "Closing...");
			force_redraw = true;
			vdi_stream_client__log_error("Network disconnected\n");
			parsec_context.done = true;
		}
		if (vdi_config->reconnect == 1 && parsec_context.client_status.networkFailure == 1 &&
		    SDL_GetTicks() > last_time + vdi_config->timeout) {

			/* render reconnect text. */
			vdi_stream_client__render_text(&parsec_context, "Reconnecting...");
			force_redraw = true;
			ParsecClientDisconnect(parsec_context.parsec);
			ParsecClientConnect(parsec_context.parsec, &cfg, vdi_config->session, vdi_config->peer);
			parsec_context.connection = false;
			last_time = SDL_GetTicks();
		}

		/* set connection status if reconnected. */
		if (vdi_config->reconnect == 1 && parsec_context.client_status.networkFailure == 0 &&
		    e == PARSEC_OK && !parsec_context.connection) {
			parsec_context.connection = true;
		}

		for (ParsecClientEvent event; ParsecClientPollEvents(parsec_context.parsec, 0, &event);) {
			switch (event.type) {
				case CLIENT_EVENT_CURSOR:
					vdi_stream_client__cursor(&parsec_context, &event.cursor.cursor, event.cursor.key, vdi_config->grab, grab_forced);
					break;
				case CLIENT_EVENT_USER_DATA:
					if (vdi_config->clipboard == 1) {
						vdi_stream_client__clipboard(&parsec_context, event.userData.id, event.userData.key);
					}
					break;
				default:
					break;
			}
		}

		/* TODO: check if we need to grab input to force decoder initialization. (workaround for buggy parsec sdk) */
		if (vdi_config->grab == 0 && !SDL_GetWindowMouseGrab(parsec_context.window) &&
		    !parsec_context.decoder &&
		    parsec_context.client_status.decoder->width == 0 &&
		    parsec_context.client_status.decoder->height == 0) {
			SDL_HideCursor();
			SDL_GetGlobalMouseState(&x, &y);
			SDL_SetWindowMouseGrab(parsec_context.window, true);
		}

		/* TODO: check if we need to ungrab input due to forced decoder initialization. (workaround for buggy parsec sdk) */
		if (vdi_config->grab == 0 && SDL_GetWindowMouseGrab(parsec_context.window) &&
		    !parsec_context.decoder &&
		    parsec_context.client_status.decoder->width > 0 &&
		    parsec_context.client_status.decoder->height > 0) {
			vdi_stream_client__log_info("Use resolution %dx%d\n",
				parsec_context.client_status.decoder->width,
				parsec_context.client_status.decoder->height
			);
			SDL_SetWindowMouseGrab(parsec_context.window, false);
			SDL_WarpMouseGlobal(x, y);
			SDL_ShowCursor();
			SDL_SetWindowSize(parsec_context.window, parsec_context.client_status.decoder->width, parsec_context.client_status.decoder->height);
			SDL_SyncWindow(parsec_context.window);
			parsec_context.window_width = parsec_context.client_status.decoder->width;
			parsec_context.window_height = parsec_context.client_status.decoder->height;
			parsec_context.decoder = true;
		}

		vdi_stream_client__video_render(&parsec_context, force_redraw);

		/* check if we need to resize window due to client resolution change. */
		if ((parsec_context.window_width != parsec_context.client_status.decoder->width || parsec_context.window_height != parsec_context.client_status.decoder->height) &&
		    parsec_context.client_status.decoder->width > 0 &&
		    parsec_context.client_status.decoder->height > 0) {
			vdi_stream_client__log_info("Change resolution from %dx%d to %dx%d\n",
				parsec_context.window_width,
				parsec_context.window_height,
				parsec_context.client_status.decoder->width,
				parsec_context.client_status.decoder->height
			);
			SDL_SetWindowSize(parsec_context.window, parsec_context.client_status.decoder->width, parsec_context.client_status.decoder->height);
			SDL_SyncWindow(parsec_context.window);
			parsec_context.window_width = parsec_context.client_status.decoder->width;
			parsec_context.window_height = parsec_context.client_status.decoder->height;
		}

		SDL_Delay(1);
	}

	/* already release any grabbed keyboard because thread termination can take some time. */
	SDL_SetWindowMouseGrab(parsec_context.window, false);
	SDL_SetWindowKeyboardGrab(parsec_context.window, false);

	/* stop network threads for usb redirection. */
	if (vdi_config->usb_devices[0].vendor != 0) {
		vdi_stream_client__log_info("Stop Network Thread\n");
		for (count = 0; count < USB_MAX ; count++) {
			if (network_thread[count] == NULL) {
				continue;
			}
			SDL_WaitThread(network_thread[count], NULL);
		}
	}

	/* stop audio thread. */
	if (vdi_config->audio == 1) {
		vdi_stream_client__log_info("Stop Audio Thread\n");
		SDL_WaitThread(audio_thread, NULL);
	}


	/* destroy parsec GL resources before releasing the parsec client. */
	vdi_stream_client__video_destroy(&parsec_context);

	/* parsec destroy. */
	ParsecDestroy(parsec_context.parsec);

	/* ttf destroy. */
	TTF_CloseFont(parsec_context.font);
	TTF_Quit();

	/* sdl destroy. */
	if (vdi_config->audio == 1) {
		SDL_DestroyAudioStream(parsec_context.audio);
	}
	SDL_DestroySurface(parsec_context.surface_ttf);
	SDL_DestroyWindow(parsec_context.window);
	SDL_Quit();

	/* terminate loop. */
	return VDI_STREAM_CLIENT_SUCCESS;

error:

	/* destroy parsec GL resources before releasing the parsec client. */
	vdi_stream_client__video_destroy(&parsec_context);

	/* parsec destroy. */
	ParsecDestroy(parsec_context.parsec);

	/* ttf destroy. */
	TTF_CloseFont(parsec_context.font);
	TTF_Quit();

	/* sdl destroy. */
	if (vdi_config->audio == 1) {
		SDL_DestroyAudioStream(parsec_context.audio);
	}
	SDL_DestroySurface(parsec_context.surface_ttf);
	SDL_DestroyWindow(parsec_context.window);
	SDL_Quit();

	/* return with error. */
	return VDI_STREAM_CLIENT_ERROR;
}
