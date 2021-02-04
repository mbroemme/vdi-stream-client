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
#include <SDL2/SDL_ttf.h>

/* parsec includes. */
#include <parsec/parsec.h>

/* font include. */
#include "../include/font.h"

/* vdi-stream-client header includes. */
#include "vdi-stream-client.h"
#include "engine-parsec.h"

/* parsec configuration. */
struct parsec_context_s {

	/* parsec. */
	SDL_bool done;
	SDL_bool connection;
	SDL_bool decoder;
	SDL_bool mode_update;
	Parsec *parsec;
	ParsecClientStatus client_status;

	/* video. */
	SDL_Window *window;
	SDL_GLContext *gl;
	SDL_Cursor *cursor;
	Sint32 window_width;
	Sint32 window_height;

	/* opengl texture for ttf rendering. */
	SDL_Surface *surface_ttf;
	GLuint texture_ttf;
	GLfloat texture_min_x;
	GLfloat texture_min_y;
	GLfloat texture_max_x;
	GLfloat texture_max_y;

	/* audio. */
	SDL_AudioDeviceID audio;
	Sint32 playing;
	Uint32 min_buffer;
	Uint32 max_buffer;
};

/* quick utility function for texture creation. */
static Sint32 vdi_stream__power_of_two(Sint32 input) {
	Sint32 value = 1;
	while (value < input) {
		value <<= 1;
	}
	return value;
}

/* convert text into opengl texture. */
static GLuint vdi_stream__gl_load_texture(SDL_Surface *surface, GLfloat *texture_coord) {
	GLuint texture;
	Sint32 w, h;
	SDL_Surface *image;
	SDL_Rect area;
	Uint8 saved_alpha;
	SDL_BlendMode saved_mode;

	/* use the surface width and height expanded to powers of 2. */
	w = vdi_stream__power_of_two(surface->w);
	h = vdi_stream__power_of_two(surface->h);
	texture_coord[0] = 0.0f; /* min x */
	texture_coord[1] = 0.0f; /* min y */
	texture_coord[2] = (GLfloat)surface->w / w; /* max x */
	texture_coord[3] = (GLfloat)surface->h / h; /* max y */

	image = SDL_CreateRGBSurface(
		SDL_SWSURFACE,
		w, h,
		32,
#if SDL_BYTEORDER == SDL_LIL_ENDIAN /* opengl rgba masks. */
		0x000000FF,
		0x0000FF00,
		0x00FF0000,
		0xFF000000
#else
		0xFF000000,
		0x00FF0000,
		0x0000FF00,
		0x000000FF
#endif
	);
	if (image == NULL) {
		return 0;
	}

	/* save the alpha blending attributes. */
	SDL_GetSurfaceAlphaMod(surface, &saved_alpha);
	SDL_SetSurfaceAlphaMod(surface, 0xFF);
	SDL_GetSurfaceBlendMode(surface, &saved_mode);
	SDL_SetSurfaceBlendMode(surface, SDL_BLENDMODE_NONE);

	/* copy the surface into the gl texture image. */
	area.x = 0;
	area.y = 0;
	area.w = surface->w;
	area.h = surface->h;
	SDL_BlitSurface(surface, &area, image, &area);

	/* restore the alpha blending attributes. */
	SDL_SetSurfaceAlphaMod(surface, saved_alpha);
	SDL_SetSurfaceBlendMode(surface, saved_mode);

	/* create an opengl texture for the image. */
	glGenTextures(1, &texture);
	glBindTexture(GL_TEXTURE_2D, texture);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
	glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, w, h, 0, GL_RGBA, GL_UNSIGNED_BYTE, image->pixels);

	/* no longer needed. */
	SDL_FreeSurface(image);

	return texture;
}

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

			ParsecFree(image);
		}
	}

	if (cursor->modeUpdate == SDL_TRUE && parsec_context->mode_update == SDL_TRUE) {
		if (SDL_GetRelativeMouseMode() == SDL_TRUE && cursor->relative == SDL_FALSE) {
			SDL_SetRelativeMouseMode(SDL_FALSE);
			SDL_WarpMouseInWindow(parsec_context->window, cursor->positionX, cursor->positionY);
		}
		if (SDL_GetRelativeMouseMode() == SDL_FALSE && cursor->relative == SDL_TRUE) {
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

	if (parsec_context->playing == SDL_TRUE && queued_packets > parsec_context->max_buffer) {
		SDL_ClearQueuedAudio(parsec_context->audio);
		SDL_PauseAudioDevice(parsec_context->audio, 1);
		parsec_context->playing = SDL_FALSE;
	} else if (parsec_context->playing == SDL_FALSE && queued_packets >= parsec_context->min_buffer) {
		SDL_PauseAudioDevice(parsec_context->audio, 0);
		parsec_context->playing = SDL_TRUE;
	}

	SDL_QueueAudio(parsec_context->audio, pcm, frames * VDI_AUDIO_CHANNELS * sizeof(Sint16));
}

/* sdl audio thread. */
static Sint32 vdi_stream_client__audio_thread(void *opaque) {
	struct parsec_context_s *parsec_context = (struct parsec_context_s *) opaque;

	while (parsec_context->done == SDL_FALSE) {

		/* poll audio only if connected. */
		if (parsec_context->connection == SDL_TRUE) {
			ParsecClientPollAudio(parsec_context->parsec, vdi_stream_client__audio, 100, parsec_context);
		}

		/* delay loop if in reconnect state. */
		if (parsec_context->connection == SDL_FALSE) {
			SDL_Delay(100);
		}
	}

	return VDI_STREAM_CLIENT_SUCCESS;
}

/* opengl frame text event. */
static void vdi_stream_client__frame_text(Uint32 timeout, void *opaque) {
	struct parsec_context_s *parsec_context = (struct parsec_context_s *) opaque;
	Sint32 x, y, w, h;

	/* calculate position and size to center of window. */
	x = (parsec_context->window_width - parsec_context->surface_ttf->w) / 2;
	y = (parsec_context->window_height - parsec_context->surface_ttf->h) / 2;
	w = parsec_context->surface_ttf->w;
	h = parsec_context->surface_ttf->h;

	/* reset drawable area. */
	glViewport(0, 0, parsec_context->window_width, parsec_context->window_height);
	glClearColor(0.0f, 0.0f, 0.0f, 1.0f);
	glClear(GL_COLOR_BUFFER_BIT);

	/* attributes. */
	glPushAttrib(GL_ENABLE_BIT);
	glDisable(GL_DEPTH_TEST);
	glDisable(GL_CULL_FACE);
	glEnable(GL_TEXTURE_2D);

	/* this allows alpha blending of 2d textures with the scene. */
	glEnable(GL_BLEND);
	glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);

	glMatrixMode(GL_PROJECTION);
	glPushMatrix();
	glLoadIdentity();

	glOrtho(0.0, parsec_context->window_width, parsec_context->window_height, 0.0, 0.0, 1.0);

	glMatrixMode(GL_MODELVIEW);
	glPushMatrix();
	glLoadIdentity();

	glTexEnvf(GL_TEXTURE_ENV, GL_TEXTURE_ENV_MODE, GL_MODULATE);

	/* show the text in center of window. */
	glBindTexture(GL_TEXTURE_2D, parsec_context->texture_ttf);
	glBegin(GL_TRIANGLE_STRIP);
	glTexCoord2f(parsec_context->texture_min_x, parsec_context->texture_min_y); glVertex2i(x,     y    );
	glTexCoord2f(parsec_context->texture_max_x, parsec_context->texture_min_y); glVertex2i(x + w, y    );
	glTexCoord2f(parsec_context->texture_min_x, parsec_context->texture_max_y); glVertex2i(x,     y + h);
	glTexCoord2f(parsec_context->texture_max_x, parsec_context->texture_max_y); glVertex2i(x + w, y + h);
	glEnd();

	/* stop text rendering. */
	glMatrixMode(GL_MODELVIEW);
	glPopMatrix();

	glMatrixMode(GL_PROJECTION);
	glPopMatrix();

	/* remove attributes. */
	glPopAttrib();

	/* static text and no need to render it frequently. */
	SDL_Delay(timeout);
}

/* opengl frame video event. */
static void vdi_stream_client__frame_video(void *opaque) {
	struct parsec_context_s *parsec_context = (struct parsec_context_s *) opaque;

	/* reset drawable area. */
	glViewport(0, 0, parsec_context->window_width, parsec_context->window_height);
	glClearColor(0.0f, 0.0f, 0.0f, 1.0f);
	glClear(GL_COLOR_BUFFER_BIT);

	ParsecClientSetDimensions(parsec_context->parsec, DEFAULT_STREAM, parsec_context->window_width, parsec_context->window_height, 1);
	ParsecClientGLRenderFrame(parsec_context->parsec, DEFAULT_STREAM, NULL, NULL, 100);
}

/* sdl video thread. */
static Sint32 vdi_stream_client__video_thread(void *opaque) {
	struct parsec_context_s *parsec_context = (struct parsec_context_s *) opaque;

	SDL_GL_MakeCurrent(parsec_context->window, parsec_context->gl);
	SDL_GL_SetSwapInterval(1);

	while (parsec_context->done == SDL_FALSE) {

		/* show parsec frame. */
		if (parsec_context->connection == SDL_TRUE) {
			vdi_stream_client__frame_video(parsec_context);
		}

		/* show reconnecting text. */
		if (parsec_context->connection == SDL_FALSE) {
			vdi_stream_client__frame_text(100, parsec_context);
		}

		SDL_GL_SwapWindow(parsec_context->window);
	}

	ParsecClientGLDestroy(parsec_context->parsec, DEFAULT_STREAM);
	SDL_GL_DeleteContext(parsec_context->gl);

	return VDI_STREAM_CLIENT_SUCCESS;
}

/* parsec event loop. */
Sint32 vdi_stream_client__event_loop(vdi_config_s *vdi_config) {
	struct parsec_context_s parsec_context = {0};
	const Uint8 *keys;
	SDL_bool focus = SDL_FALSE;
	Uint32 wait_time = 0;
	Uint32 last_time = 0;
	Sint32 error = 0;
	GLenum gl_error;
	GLfloat texture_coord[4];
	SDL_AudioSpec want = {0};
	SDL_AudioSpec have = {0};
	SDL_SysWMinfo wm_info;
	SDL_Color color = { 0x88, 0x88, 0x88, 0xFF };
	TTF_Font *font;
	ParsecStatus e;
	ParsecConfig network_cfg = PARSEC_DEFAULTS;
	ParsecClientConfig cfg = PARSEC_CLIENT_DEFAULTS;

	/* sdl init. */
	vdi_stream__log_info("Initialize SDL\n");
	error = SDL_Init(SDL_INIT_VIDEO | SDL_INIT_AUDIO);
	if (error != 0) {
		vdi_stream__log_error("Initialization failed: %s\n", SDL_GetError());
		goto error;
	}
	SDL_VERSION(&wm_info.version);

	/* ttf init. */
	vdi_stream__log_info("Initialize TTF\n");
	error = TTF_Init();
	if (error != 0) {
		vdi_stream__log_error("Initialization failed: %s\n", TTF_GetError());
		goto error;
	}

	/* load font. */
	font = TTF_OpenFontRW(SDL_RWFromMem(MorePerfectDOSVGA_ttf, MorePerfectDOSVGA_ttf_len), 1, 16);
	if (font == NULL) {
		vdi_stream__log_error("Loading font failed: %s\n", TTF_GetError());
		goto error;
	}

	/* parsec init. */
	vdi_stream__log_info("Initialize Parsec\n");
	e = ParsecInit(PARSEC_VER, &network_cfg, NULL, &parsec_context.parsec);
	if (e != PARSEC_OK) {
		vdi_stream__log_error("Initialization failed with code: %d\n", e);
		goto error;
	}

	/* parsec configuration. */
	cfg.video[DEFAULT_STREAM].decoderH265 = (vdi_config->codec == 2) ? SDL_TRUE : SDL_FALSE;
	cfg.video[DEFAULT_STREAM].decoder444 = (vdi_config->mode == 2) ? SDL_TRUE : SDL_FALSE;

	/* use client resolution if specified. */
	if (vdi_config->width > 0 && vdi_config->height > 0) {
		vdi_stream__log_info("Override resolution %dx%d\n", vdi_config->width, vdi_config->height);
		cfg.video[DEFAULT_STREAM].resolutionX = vdi_config->width;
		cfg.video[DEFAULT_STREAM].resolutionY = vdi_config->height;
	}

	/* configure client decoding acceleration. */
	if (vdi_config->acceleration == 1) {
		cfg.video[DEFAULT_STREAM].decoderIndex = 1;
	}
	if (vdi_config->acceleration == 0) {
		vdi_stream__log_info("Disable Hardware Accelerated Video Decoding\n");
		cfg.video[DEFAULT_STREAM].decoderIndex = 0;
	}

	/* configure upnp. */
	if (vdi_config->upnp == 1) {
		network_cfg.upnp = 1;
	}
	if (vdi_config->upnp == 0) {
		vdi_stream__log_info("Disable UPnP\n");
		network_cfg.upnp = 0;
	}

	/* check if reconnect should be disabled. */
	if (vdi_config->reconnect == 0) {
		vdi_stream__log_info("Disable automatic reconnect\n");
	}

	/* check if exclusive mouse grab should be disabled. */
	if (vdi_config->grab == 0) {
		vdi_stream__log_info("Disable exclusive mouse grab\n");
	}

	/* configure screen saver. */
	if (vdi_config->screensaver == 1) {
		SDL_EnableScreenSaver();
	}
	if (vdi_config->screensaver == 0) {
		vdi_stream__log_info("Disable screen saver\n");
		SDL_DisableScreenSaver();
	}

	/* check if clipboard should be disabled. */
	if (vdi_config->clipboard == 0) {
		vdi_stream__log_info("Disable clipboard sharing\n");
	}

	/* check if audio should be streamed. */
	if (vdi_config->audio == 0) {
		vdi_stream__log_info("Disable audio streaming\n");
	}

	/* parsec connect. */
	vdi_stream__log_info("Connect to Parsec service\n");
	e = ParsecClientConnect(parsec_context.parsec, &cfg, vdi_config->session, vdi_config->peer);
	if (e != PARSEC_OK) {
		vdi_stream__log_error("Connection failed with code: %d\n", e);
		goto error;
	}

	/* wait until connection is established. */
	vdi_stream__log_info("Connect to Parsec host\n");
	while (parsec_context.decoder == SDL_FALSE) {

		/* get client status. */
		e = ParsecClientGetStatus(parsec_context.parsec, &parsec_context.client_status);

		/* connection established */
		if (e == PARSEC_OK) {

			/* decoder not yet initialized. */
			if (parsec_context.client_status.decoder->width == 0 &&
			    parsec_context.client_status.decoder->height == 0 &&
			    parsec_context.connection == SDL_FALSE) {
				vdi_stream__log_info("Initialize Video Decoder\n");
				parsec_context.connection = SDL_TRUE;
			}

			/* decoder initialized. */
			if (parsec_context.client_status.decoder->width > 0 &&
			    parsec_context.client_status.decoder->height > 0) {
				vdi_stream__log_info("Use resolution %dx%d\n", parsec_context.client_status.decoder->width, parsec_context.client_status.decoder->height);
				parsec_context.window_width = parsec_context.client_status.decoder->width;
				parsec_context.window_height = parsec_context.client_status.decoder->height;
				parsec_context.decoder = SDL_TRUE;
			}
		}

		/* unknown error. */
		if (e != PARSEC_CONNECTING && e != PARSEC_OK) {
			vdi_stream__log_error("Connection failed with code: %d\n", e);
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
	if (parsec_context.connection == SDL_FALSE &&
	    parsec_context.decoder == SDL_FALSE) {
		vdi_stream__log_error("Connection timed out after %d seconds\n", (vdi_config->timeout / 1000));
		goto error;
	}

	/* check if connected but decoder initialization failed. */
	if (parsec_context.connection == SDL_TRUE &&
	    parsec_context.decoder == SDL_FALSE) {
		parsec_context.window_width = 640;
		parsec_context.window_height = 480;
	}

	parsec_context.window = SDL_CreateWindow("VDI Stream Client",
					SDL_WINDOWPOS_UNDEFINED,
					SDL_WINDOWPOS_UNDEFINED,
					parsec_context.window_width,
					parsec_context.window_height,
					(parsec_context.decoder == SDL_TRUE) ?
						SDL_WINDOW_OPENGL | SDL_WINDOW_ALLOW_HIGHDPI | SDL_WINDOW_INPUT_FOCUS :

						/* TODO: workaround if decoder initialization failed. (workaround for buggy parsec sdk) */
						SDL_WINDOW_OPENGL | SDL_WINDOW_ALLOW_HIGHDPI | SDL_WINDOW_INPUT_FOCUS | SDL_WINDOW_HIDDEN
				);
	if (parsec_context.window == NULL) {
		vdi_stream__log_error("Window creation failed: %s\n", SDL_GetError());
		goto error;
	}

	parsec_context.gl = SDL_GL_CreateContext(parsec_context.window);
	if (parsec_context.gl == NULL) {
		vdi_stream__log_error("OpenGL context creation failed: %s\n", SDL_GetError());
		goto error;
	}

	parsec_context.surface_ttf = TTF_RenderUTF8_Blended(font, "Reconnecting...", color);
	if (parsec_context.surface_ttf == NULL) {
		vdi_stream__log_error("TTF surface creation failed: %s\n", TTF_GetError());
		goto error;
	}

	/* convert the text into an opengl texture. */
	parsec_context.texture_ttf = vdi_stream__gl_load_texture(parsec_context.surface_ttf, texture_coord);
	if ((gl_error = glGetError()) != GL_NO_ERROR) {
		vdi_stream__log_error("TTF OpenGL texture creation failed: 0x%x\n", gl_error);
		goto error;
	}

	/* make texture coordinates easy to understand. */
	parsec_context.texture_min_x = texture_coord[0];
	parsec_context.texture_min_y = texture_coord[1];
	parsec_context.texture_max_x = texture_coord[2];
	parsec_context.texture_max_y = texture_coord[3];

	/* sdl video thread. */
	SDL_Thread *video_thread = SDL_CreateThread(vdi_stream_client__video_thread, "vdi_stream_client__video_thread", &parsec_context);
	if (video_thread == NULL) {
		vdi_stream__log_error("Video thread creation failed: %s\n", SDL_GetError());
		goto error;
	}

	/* check if audio should be streamed. */
	if (vdi_config->audio == 1) {
		want.freq = VDI_AUDIO_SAMPLE_RATE;
		want.format = AUDIO_S16;
		want.channels = VDI_AUDIO_CHANNELS;
		want.samples = 2048;

		/* the number of audio packets (960 frames) to buffer before we begin playing. */
		parsec_context.min_buffer = 1;

		/* the number of audio packets (960 frames) to buffer before overflow and clear. */
		parsec_context.max_buffer = 6;

		/* sdl audio device. */
		parsec_context.audio = SDL_OpenAudioDevice(NULL, 0, &want, &have, 0);
		if (parsec_context.audio == 0) {
			vdi_stream__log_error("Failed to open audio: %s\n", SDL_GetError());
			goto error;
		}

		/* sdl audio thread. */
		SDL_Thread *audio_thread = SDL_CreateThread(vdi_stream_client__audio_thread, "vdi_stream_client__audio_thread", &parsec_context);
		if (audio_thread == NULL) {
			vdi_stream__log_error("Audio thread creation failed: %s\n", SDL_GetError());
			goto error;
		}
	}

	SDL_GetWindowWMInfo(parsec_context.window, &wm_info);
	keys = SDL_GetKeyboardState(NULL);

	/* event loop. */
	while (parsec_context.done == SDL_FALSE) {
		for (SDL_Event msg; SDL_PollEvent(&msg);) {
			ParsecMessage pmsg = {0};

			switch (msg.type) {
				case SDL_QUIT:
					parsec_context.done = SDL_TRUE;
					break;
				case SDL_KEYUP:
					pmsg.type = MESSAGE_KEYBOARD;
					pmsg.keyboard.code = (ParsecKeycode) msg.key.keysym.scancode;
					pmsg.keyboard.mod = msg.key.keysym.mod;
					pmsg.keyboard.pressed = SDL_FALSE;

					/* TODO: we need to re-grab keyboard later. (workaround for buggy x11 and sdl) */
					focus = SDL_FALSE;

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
				case SDL_MOUSEBUTTONUP:
					pmsg.type = MESSAGE_MOUSE_BUTTON;
					pmsg.mouseButton.button = msg.button.button;
					pmsg.mouseButton.pressed = SDL_FALSE;
					break;
				case SDL_MOUSEBUTTONDOWN:

					/* check if we need to grab mouse. */
					if (vdi_config->grab == 1 && SDL_GetWindowGrab(parsec_context.window) == SDL_FALSE) {
						SDL_SetWindowGrab(parsec_context.window, SDL_TRUE);
						SDL_SetWindowTitle(parsec_context.window, "VDI Stream Client (Press Ctrl+Alt to release grab)");
					}
					pmsg.type = MESSAGE_MOUSE_BUTTON;
					pmsg.mouseButton.button = msg.button.button;
					pmsg.mouseButton.pressed = SDL_TRUE;
					break;
				case SDL_MOUSEWHEEL:
					pmsg.type = MESSAGE_MOUSE_WHEEL;
					pmsg.mouseWheel.x = msg.wheel.x * vdi_config->speed;
					pmsg.mouseWheel.y = msg.wheel.y * vdi_config->speed;
					break;
				case SDL_CLIPBOARDUPDATE:
					if (vdi_config->clipboard == 1) {
						ParsecClientSendUserData(parsec_context.parsec, PARSEC_CLIPBOARD_MSG, SDL_GetClipboardText());
					}
					break;
				case SDL_WINDOWEVENT:
					switch (msg.window.event) {
						case SDL_WINDOWEVENT_FOCUS_GAINED:
							SDL_EventState(SDL_KEYDOWN, SDL_ENABLE);
							SDL_EventState(SDL_KEYUP, SDL_ENABLE);
							XGrabKeyboard(wm_info.info.x11.display, wm_info.info.x11.window, False, GrabModeAsync, GrabModeAsync, CurrentTime);

							/* TODO: copy client to host clipboard. (workaround for buggy x11 and sdl) */
							if (vdi_config->clipboard == 1 && SDL_HasClipboardText() == SDL_TRUE) {
								ParsecClientSendUserData(parsec_context.parsec, PARSEC_CLIPBOARD_MSG, SDL_GetClipboardText());
							}

							break;
						case SDL_WINDOWEVENT_FOCUS_LOST:
							SDL_EventState(SDL_KEYDOWN, SDL_DISABLE);
							SDL_EventState(SDL_KEYUP, SDL_DISABLE);
							XUngrabKeyboard(wm_info.info.x11.display, CurrentTime);
							break;
						case SDL_WINDOWEVENT_ENTER:
							parsec_context.mode_update = SDL_TRUE;
							break;
						case SDL_WINDOWEVENT_LEAVE:
							parsec_context.mode_update = SDL_FALSE;
							break;
					}
					break;
			}

			if (pmsg.type != 0) {
				ParsecClientSendMessage(parsec_context.parsec, &pmsg);
			}
		}

		/* check parsec connection status. */
		e = ParsecClientGetStatus(parsec_context.parsec, &parsec_context.client_status);
		if (vdi_config->reconnect == 0 && e != PARSEC_CONNECTING && e != PARSEC_OK) {
			vdi_stream__log_error("Parsec disconnected\n");
			parsec_context.done = SDL_TRUE;
		}
		if (vdi_config->reconnect == 1 && e != PARSEC_CONNECTING && e != PARSEC_OK &&
		    SDL_GetTicks() > last_time + vdi_config->timeout) {
			ParsecClientDisconnect(parsec_context.parsec);
			ParsecClientConnect(parsec_context.parsec, &cfg, vdi_config->session, vdi_config->peer);
			parsec_context.connection = SDL_FALSE;
			last_time = SDL_GetTicks();
		}

		/* check network connection status. */
		if (vdi_config->reconnect == 0 && parsec_context.client_status.networkFailure == 1) {
			vdi_stream__log_error("Network disconnected\n");
			parsec_context.done = SDL_TRUE;
		}
		if (vdi_config->reconnect == 1 && parsec_context.client_status.networkFailure == 1 &&
		    SDL_GetTicks() > last_time + vdi_config->timeout) {
			ParsecClientDisconnect(parsec_context.parsec);
			ParsecClientConnect(parsec_context.parsec, &cfg, vdi_config->session, vdi_config->peer);
			parsec_context.connection = SDL_FALSE;
			last_time = SDL_GetTicks();
		}

		/* set connection status if reconnected. */
		if (vdi_config->reconnect == 1 && parsec_context.client_status.networkFailure == 0 &&
		    e == PARSEC_OK && parsec_context.connection == SDL_FALSE) {
			parsec_context.connection = SDL_TRUE;
		}

		for (ParsecClientEvent event; ParsecClientPollEvents(parsec_context.parsec, 0, &event);) {
			switch (event.type) {
				case CLIENT_EVENT_CURSOR:
					vdi_stream_client__cursor(&parsec_context, &event.cursor.cursor, event.cursor.key);
					break;
				case CLIENT_EVENT_USER_DATA:
					if (vdi_config->clipboard == 1) {
						vdi_stream_client__clipboard(&parsec_context, event.userData.id, event.userData.key);
					}
					break;
				default:
					break;
			}

			/* TODO: we need to re-grab keyboard later. (workaround for buggy x11 and sdl) */
			focus = SDL_FALSE;
		}
		
		/* check if we need to release mouse. */
		if (vdi_config->grab == 1 && SDL_GetWindowGrab(parsec_context.window) == SDL_TRUE) {
			if (keys[SDL_SCANCODE_LCTRL] != 0 &&
			    keys[SDL_SCANCODE_LALT] != 0) {
				SDL_SetWindowGrab(parsec_context.window, SDL_FALSE);
				SDL_SetWindowTitle(parsec_context.window, "VDI Stream Client");
			}
		}

		/* check if we need to resize and unhide window. */
		if (parsec_context.decoder == SDL_FALSE && (SDL_GetWindowFlags(parsec_context.window) & SDL_WINDOW_HIDDEN) != 0 &&
		    parsec_context.client_status.decoder->width > 0 &&
		    parsec_context.client_status.decoder->height > 0) {
			vdi_stream__log_info("Use resolution %dx%d\n",
				parsec_context.client_status.decoder->width,
				parsec_context.client_status.decoder->height
			);
			SDL_SetWindowSize(parsec_context.window, parsec_context.client_status.decoder->width, parsec_context.client_status.decoder->height);
			SDL_ShowWindow(parsec_context.window);
			parsec_context.window_width = parsec_context.client_status.decoder->width;
			parsec_context.window_height = parsec_context.client_status.decoder->height;
			parsec_context.decoder = SDL_TRUE;
		}

		/* check if we need to resize window due to client resolution change. */
		if ((parsec_context.window_width != parsec_context.client_status.decoder->width || parsec_context.window_height != parsec_context.client_status.decoder->height) &&
		    parsec_context.client_status.decoder->width > 0 &&
		    parsec_context.client_status.decoder->height > 0) {
			vdi_stream__log_info("Change resolution from %dx%d to %dx%d\n",
				parsec_context.window_width,
				parsec_context.window_height,
				parsec_context.client_status.decoder->width,
				parsec_context.client_status.decoder->height
			);
			SDL_SetWindowSize(parsec_context.window, parsec_context.client_status.decoder->width, parsec_context.client_status.decoder->height);
			parsec_context.window_width = parsec_context.client_status.decoder->width;
			parsec_context.window_height = parsec_context.client_status.decoder->height;
		}

		/* check if we need to regrab keyboard on modifier keypress. */
		if (focus == SDL_FALSE && (SDL_GetWindowFlags(parsec_context.window) & SDL_WINDOW_INPUT_FOCUS) != 0) {
			XGrabKeyboard(wm_info.info.x11.display, wm_info.info.x11.window, False, GrabModeAsync, GrabModeAsync, CurrentTime);
			focus = SDL_TRUE;
		}

		SDL_Delay(1);
	}

	/* parsec destroy. */
	ParsecDestroy(parsec_context.parsec);

	/* ttf destroy. */
	TTF_CloseFont(font);
	TTF_Quit();

	/* sdl destroy. */
	if (vdi_config->audio == 1) {
		SDL_CloseAudioDevice(parsec_context.audio);
	}
	SDL_FreeSurface(parsec_context.surface_ttf);
	SDL_DestroyWindow(parsec_context.window);
	SDL_Quit();

	/* terminate loop. */
	return VDI_STREAM_CLIENT_SUCCESS;

error:

	/* parsec destroy. */
	ParsecDestroy(parsec_context.parsec);

	/* ttf destroy. */
	TTF_CloseFont(font);
	TTF_Quit();

	/* sdl destroy. */
	if (vdi_config->audio == 1) {
		SDL_CloseAudioDevice(parsec_context.audio);
	}
	SDL_FreeSurface(parsec_context.surface_ttf);
	SDL_DestroyWindow(parsec_context.window);
	SDL_Quit();

	/* return with error. */
	return VDI_STREAM_CLIENT_ERROR;
}
