/*
 *  video.c -- video rendering thread via sdl
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
#include <limits.h>
#include <stdlib.h>
#include <stdio.h>
#include <unistd.h>

/* quick utility function for texture creation. */
static Sint32 vdi_stream_client__power_of_two(Sint32 input) {
	Sint32 value = 1;
	while (value < input) {
		value <<= 1;
	}
	return value;
}

/* convert text into opengl texture. */
GLuint vdi_stream_client__gl_load_texture(SDL_Surface *surface, GLfloat *texture_coord) {
	GLuint texture;
	Sint32 w, h;
	SDL_Surface *image;
	SDL_Rect area;
	Uint8 saved_alpha;
	SDL_BlendMode saved_mode;

	/* use the surface width and height expanded to powers of 2. */
	w = vdi_stream_client__power_of_two(surface->w);
	h = vdi_stream_client__power_of_two(surface->h);
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
		return VDI_STREAM_CLIENT_SUCCESS;
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

/* opengl frame text event. */
static void vdi_stream_client__frame_text(void *opaque) {
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
	SDL_Delay(parsec_context->timeout);
}

/* opengl frame video event. */
static void vdi_stream_client__frame_video(void *opaque) {
	struct parsec_context_s *parsec_context = (struct parsec_context_s *) opaque;

	/* reset drawable area. */
	glViewport(0, 0, parsec_context->window_width, parsec_context->window_height);
	glClearColor(0.0f, 0.0f, 0.0f, 1.0f);
	glClear(GL_COLOR_BUFFER_BIT);

	ParsecClientSetDimensions(parsec_context->parsec, DEFAULT_STREAM, parsec_context->window_width, parsec_context->window_height, 1);
	ParsecClientGLRenderFrame(parsec_context->parsec, DEFAULT_STREAM, NULL, NULL, parsec_context->timeout);
}

/* sdl video thread. */
Sint32 vdi_stream_client__video_thread(void *opaque) {
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
			vdi_stream_client__frame_text(parsec_context);
		}

		SDL_GL_SwapWindow(parsec_context->window);
	}

	/* show closing text. */
	vdi_stream_client__frame_text(parsec_context);
	SDL_GL_SwapWindow(parsec_context->window);

	ParsecClientGLDestroy(parsec_context->parsec, DEFAULT_STREAM);
	SDL_GL_DeleteContext(parsec_context->gl);

	return VDI_STREAM_CLIENT_SUCCESS;
}
