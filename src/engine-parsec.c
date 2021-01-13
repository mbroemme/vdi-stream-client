/*
 *  engine-parsec.c -- desktop streaming with parsec sdk
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
#include <limits.h>
#include <stdint.h>
#include <stdlib.h>
#include <stdio.h>
#include <unistd.h>

/* x11 includes. */
#include <X11/Xlib.h>
#include <X11/keysym.h>

/* opengl includes. */
#include <GL/gl.h>

/* sdl includes. */
#define SDL_MAIN_HANDLED
#include <SDL2/SDL.h>
#include <SDL2/SDL_syswm.h>

/* parsec includes. */
#include <parsec/parsec.h>

/* vdi-stream-client header includes. */
#include "vdi-stream-client.h"
#include "engine-parsec.h"

/* parsec configuration. */
struct parsec_context_s {

	/* parsec. */
	Sint32 done;
	Sint32 connection;
	Parsec *parsec;
	ParsecClientStatus client_status;

	/* video. */
	SDL_Window *window;
	SDL_GLContext *gl;
	SDL_Surface *surface;
	SDL_Cursor *cursor;
	Sint32 old_width;
	Sint32 old_height;

	/* audio. */
	SDL_AudioDeviceID audio;
	Sint32 playing;
	Uint32 min_buffer;
	Uint32 max_buffer;
};

/* parsec clipboard event. */
static void vdi_stream_client__clipboard(struct parsec_context_s *parsec_context, Uint32 id, Uint32 buffer_key) {
	char *msg = ParsecGetBuffer(parsec_context->parsec, buffer_key);

	if (msg && id == PARSEC_CLIPBOARD_MSG) {
		SDL_SetClipboardText(msg);
	}

	ParsecFree(msg);
}

/* parsec cursor event. */
static void vdi_stream_client__cursor(struct parsec_context_s *parsec_context, ParsecCursor *cursor, Uint32 buffer_key) {
	if (cursor->imageUpdate == SDL_TRUE) {
		Uint8 *image = ParsecGetBuffer(parsec_context->parsec, buffer_key);

		if (image != NULL) {
			SDL_Surface *surface = SDL_CreateRGBSurfaceFrom(image, cursor->width, cursor->height,
				32, cursor->width * 4, 0xff, 0xff00, 0xff0000, 0xff000000);
			SDL_Cursor *sdlCursor = SDL_CreateColorCursor(surface, cursor->hotX, cursor->hotY);
			SDL_SetCursor(sdlCursor);

			SDL_FreeCursor(parsec_context->cursor);
			parsec_context->cursor = sdlCursor;

			SDL_FreeSurface(parsec_context->surface);
			parsec_context->surface = surface;

			ParsecFree(image);
		}
	}

	if (cursor->modeUpdate == SDL_TRUE) {
		if (SDL_GetRelativeMouseMode() == SDL_TRUE && cursor->relative == SDL_FALSE) {
			SDL_ShowCursor(SDL_ENABLE);
			SDL_SetRelativeMouseMode(SDL_FALSE);
			SDL_WarpMouseInWindow(parsec_context->window, cursor->positionX, cursor->positionY);
		} else if (SDL_GetRelativeMouseMode() == SDL_FALSE && cursor->relative == SDL_TRUE) {
			SDL_ShowCursor(SDL_DISABLE);
			SDL_SetRelativeMouseMode(SDL_TRUE);
		}
	}
}

/* parsec audio event. */
static void vdi_stream_client__audio(const Sint16 *pcm, Uint32 frames, void *opaque) {
	struct parsec_context_s *parsec_context = (struct parsec_context_s *) opaque;

	Uint32 size = SDL_GetQueuedAudioSize(parsec_context->audio);
	Uint32 queued_frames = size / (VDI_AUDIO_CHANNELS * sizeof(Sint16));
	Uint32 queued_packets = queued_frames / VDI_AUDIO_FRAMES_PER_PACKET;

	if (parsec_context->playing == 1 && queued_packets > parsec_context->max_buffer) {
		SDL_ClearQueuedAudio(parsec_context->audio);
		SDL_PauseAudioDevice(parsec_context->audio, 1);
		parsec_context->playing = 0;
	} else if (parsec_context->playing == 0 && queued_packets >= parsec_context->min_buffer) {
		SDL_PauseAudioDevice(parsec_context->audio, 0);
		parsec_context->playing = 1;
	}

	SDL_QueueAudio(parsec_context->audio, pcm, frames * VDI_AUDIO_CHANNELS * sizeof(Sint16));
}

/* sdl audio thread. */
static Sint32 vdi_stream_client__audio_thread(void *opaque) {
	struct parsec_context_s *parsec_context = (struct parsec_context_s *) opaque;

	while (parsec_context->done == 0) {
		ParsecClientPollAudio(parsec_context->parsec, vdi_stream_client__audio, 100, parsec_context);
	}

	return VDI_STREAM_CLIENT_SUCCESS;
}

/* sdl video thread. */
static Sint32 vdi_stream_client__video_thread(void *opaque) {
	struct parsec_context_s *parsec_context = (struct parsec_context_s *) opaque;

	SDL_GL_MakeCurrent(parsec_context->window, parsec_context->gl);
	SDL_GL_SetSwapInterval(1);

	while (parsec_context->done == 0) {
		ParsecClientSetDimensions(
			parsec_context->parsec,
			DEFAULT_STREAM,
			parsec_context->client_status.decoder->width,
			parsec_context->client_status.decoder->height,
			1
		);

		glViewport(0, 0, parsec_context->client_status.decoder->width, parsec_context->client_status.decoder->height);
		glClearColor(0.0f, 0.0f, 0.0f, 1.0f);
		glClear(GL_COLOR_BUFFER_BIT);

		ParsecClientGLRenderFrame(parsec_context->parsec, DEFAULT_STREAM, NULL, NULL, 100);
		SDL_GL_SwapWindow(parsec_context->window);
		glFinish();

		parsec_context->old_width = parsec_context->client_status.decoder->width;
		parsec_context->old_height = parsec_context->client_status.decoder->height;
	}

	ParsecClientGLDestroy(parsec_context->parsec, DEFAULT_STREAM);
	SDL_GL_DeleteContext(parsec_context->gl);

	return VDI_STREAM_CLIENT_SUCCESS;
}

/* parsec event loop. */
Sint32 vdi_stream_client__event_loop(vdi_config_s *vdi_config) {
	struct parsec_context_s parsec_context = {0};
	const Uint8 *keys;
	Uint32 wait_time = 0;
	SDL_SysWMinfo wm_info;
	ParsecStatus e;
	ParsecClientConfig cfg = PARSEC_CLIENT_DEFAULTS;

	/* sdl init. */
	SDL_Init(SDL_INIT_VIDEO | SDL_INIT_AUDIO);
	SDL_VERSION(&wm_info.version);

	/* parsec configuration. */
	cfg.video[DEFAULT_STREAM].decoderH265 = (vdi_config->codec == 2) ? SDL_TRUE : SDL_FALSE;
	cfg.video[DEFAULT_STREAM].decoder444 = (vdi_config->mode == 2) ? SDL_TRUE : SDL_FALSE;

	/* parsec init. */
	e = ParsecInit(PARSEC_VER, NULL, NULL, &parsec_context.parsec);
	if (e != PARSEC_OK) {
		goto error;
	}

	/* parsec connect. */
	e = ParsecClientConnect(parsec_context.parsec, &cfg, vdi_config->session, vdi_config->peer);
	if (e != PARSEC_OK) {
		goto error;
	}

	/* wait until connection is established. */
	while (parsec_context.connection == 0) {

		/* get client status. */
		e = ParsecClientGetStatus(parsec_context.parsec, &parsec_context.client_status);

		/* connection established. */
		if (e == PARSEC_OK &&
		    parsec_context.client_status.decoder->width > 0 &&
		    parsec_context.client_status.decoder->height > 0) {
			parsec_context.connection = 1;
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

	/* check if connected. */
	if (parsec_context.connection == 0) {
		goto error;
	}

	SDL_AudioSpec want = {0}, have;
	want.freq = VDI_AUDIO_SAMPLE_RATE;
	want.format = AUDIO_S16;
	want.channels = VDI_AUDIO_CHANNELS;
	want.samples = 2048;

	/* the number of audio packets (960 frames) to buffer before we begin playing. */
	parsec_context.min_buffer = 1;

	/* the number of audio packets (960 frames) to buffer before overflow and clear. */
	parsec_context.max_buffer = 6;

	parsec_context.audio = SDL_OpenAudioDevice(NULL, 0, &want, &have, 0);
	parsec_context.window = SDL_CreateWindow("VDI Stream Client",
					SDL_WINDOWPOS_UNDEFINED,
					SDL_WINDOWPOS_UNDEFINED,
					parsec_context.client_status.decoder->width,
					parsec_context.client_status.decoder->height,
					SDL_WINDOW_OPENGL | SDL_WINDOW_ALLOW_HIGHDPI | SDL_WINDOW_INPUT_FOCUS
				);

	parsec_context.gl = SDL_GL_CreateContext(parsec_context.window);

	/* configure screen saver. */
	if (vdi_config->screensaver == 1) {
		SDL_EnableScreenSaver();
	}

	/* sdl render threads. */
	SDL_Thread *video_thread = SDL_CreateThread(vdi_stream_client__video_thread, "vdi_stream_client__video_thread", &parsec_context);
	SDL_Thread *audio_thread = SDL_CreateThread(vdi_stream_client__audio_thread, "vdi_stream_client__audio_thread", &parsec_context);

	SDL_GetWindowWMInfo(parsec_context.window, &wm_info);

	keys = SDL_GetKeyboardState(NULL);

	/* event loop. */
	while (parsec_context.done == 0) {
		for (SDL_Event msg; SDL_PollEvent(&msg);) {
			ParsecMessage pmsg = {0};

			switch (msg.type) {
				case SDL_QUIT:
					parsec_context.done = 1;
					break;
				case SDL_KEYUP:

					/* TODO: re-grab keyboard on every keyup event. (workaround for some window manager hotkey bugs) */
					XGrabKeyboard(wm_info.info.x11.display, wm_info.info.x11.window, False, GrabModeAsync, GrabModeAsync, CurrentTime);

					pmsg.type = MESSAGE_KEYBOARD;
					pmsg.keyboard.code = (ParsecKeycode) msg.key.keysym.scancode;
					pmsg.keyboard.mod = msg.key.keysym.mod;
					pmsg.keyboard.pressed = SDL_FALSE;
					break;
				case SDL_KEYDOWN:
					pmsg.type = MESSAGE_KEYBOARD;
					pmsg.keyboard.code = (ParsecKeycode) msg.key.keysym.scancode;
					pmsg.keyboard.mod = msg.key.keysym.mod;
					pmsg.keyboard.pressed = SDL_TRUE;
					break;
				case SDL_MOUSEMOTION:
					pmsg.type = MESSAGE_MOUSE_MOTION;
					pmsg.mouseMotion.relative = SDL_GetRelativeMouseMode();
					pmsg.mouseMotion.x = pmsg.mouseMotion.relative ? msg.motion.xrel : msg.motion.x + 1;
					pmsg.mouseMotion.y = pmsg.mouseMotion.relative ? msg.motion.yrel : msg.motion.y + 1;
					break;
				case SDL_MOUSEBUTTONDOWN:
				case SDL_MOUSEBUTTONUP:

					/* check if we need to grab mouse. */
					if (vdi_config->grab == 1 && SDL_GetWindowGrab(parsec_context.window) == SDL_FALSE) {
						SDL_SetWindowGrab(parsec_context.window, SDL_TRUE);
						SDL_SetWindowTitle(parsec_context.window, "VDI Stream Client (Press Ctrl+Alt to release grab)");
					}
					pmsg.type = MESSAGE_MOUSE_BUTTON;
					pmsg.mouseButton.button = msg.button.button;
					pmsg.mouseButton.pressed = msg.button.type == SDL_MOUSEBUTTONDOWN;
					break;
				case SDL_MOUSEWHEEL:
					pmsg.type = MESSAGE_MOUSE_WHEEL;
					pmsg.mouseWheel.x = msg.wheel.x * vdi_config->speed;
					pmsg.mouseWheel.y = msg.wheel.y * vdi_config->speed;
					break;
				case SDL_CLIPBOARDUPDATE:
					ParsecClientSendUserData(parsec_context.parsec, PARSEC_CLIPBOARD_MSG, SDL_GetClipboardText());
					break;
				case SDL_WINDOWEVENT:
					switch (msg.window.event) {
						case SDL_WINDOWEVENT_FOCUS_GAINED:
						case SDL_WINDOWEVENT_ENTER:

							/* TODO: copy client to host clipboard. (workaround for buggy x11 and sdl clipboard handling) */
							if (SDL_HasClipboardText() == SDL_TRUE) {
								ParsecClientSendUserData(parsec_context.parsec, PARSEC_CLIPBOARD_MSG, SDL_GetClipboardText());
							}
							SDL_EventState(SDL_KEYDOWN, SDL_ENABLE);
							SDL_EventState(SDL_KEYUP, SDL_ENABLE);
							XGrabKeyboard(wm_info.info.x11.display, wm_info.info.x11.window, False, GrabModeAsync, GrabModeAsync, CurrentTime);
							break;
						case SDL_WINDOWEVENT_FOCUS_LOST:
						case SDL_WINDOWEVENT_LEAVE:
							SDL_EventState(SDL_KEYDOWN, SDL_DISABLE);
							SDL_EventState(SDL_KEYUP, SDL_DISABLE);
							XUngrabKeyboard(wm_info.info.x11.display, CurrentTime);
							break;
					}
					break;
			}

			if (pmsg.type != 0) {
				ParsecClientSendMessage(parsec_context.parsec, &pmsg);
			}
		}

		e = ParsecClientGetStatus(parsec_context.parsec, &parsec_context.client_status);
		if (e != PARSEC_CONNECTING && e != PARSEC_OK) {
			parsec_context.done = 1;
		}

		for (ParsecClientEvent event; ParsecClientPollEvents(parsec_context.parsec, 0, &event);) {
			switch (event.type) {
				case CLIENT_EVENT_CURSOR:
					vdi_stream_client__cursor(&parsec_context, &event.cursor.cursor, event.cursor.key);
					break;
				case CLIENT_EVENT_USER_DATA:
					vdi_stream_client__clipboard(&parsec_context, event.userData.id, event.userData.key);
					break;
				default:
					break;
			}
		}
		
		/* check if we need to release mouse. */
		if (vdi_config->grab == 1 && SDL_GetWindowGrab(parsec_context.window) == SDL_TRUE) {
			if (keys[SDL_SCANCODE_LCTRL] != 0 &&
			    keys[SDL_SCANCODE_LALT] != 0) {
				SDL_SetWindowGrab(parsec_context.window, SDL_FALSE);
				SDL_SetWindowTitle(parsec_context.window, "VDI Stream Client");
			}
		}

		/* check if we need to change window size. */
		if (parsec_context.old_width != parsec_context.client_status.decoder->width ||
		    parsec_context.old_height != parsec_context.client_status.decoder->height) {
			SDL_SetWindowSize(parsec_context.window, parsec_context.client_status.decoder->width, parsec_context.client_status.decoder->height);
		}

		SDL_Delay(1);
	}

	/* parsec destroy. */
	ParsecDestroy(parsec_context.parsec);

	/* sdl destroy. */
	SDL_DestroyWindow(parsec_context.window);
	SDL_CloseAudioDevice(parsec_context.audio);
	SDL_Quit();

	/* terminate loop. */
	return VDI_STREAM_CLIENT_SUCCESS;

error:

	/* parsec destroy. */
	ParsecDestroy(parsec_context.parsec);

	/* sdl destroy. */
	SDL_Quit();

	/* return with error. */
	return VDI_STREAM_CLIENT_ERROR;
}
