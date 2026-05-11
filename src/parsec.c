/*
 *  parsec.c -- desktop streaming with parsec sdk
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

/* internal includes. */
#include "audio.h"
#include "client.h"
#include "parsec.h"
#include "redirect.h"
#include "video.h"

/* font include. */
#include "../include/font.h"

/* system includes. */
#include <dlfcn.h>
#include <limits.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>



/* stringify status codes that matter while diagnosing decoder negotiation. */
static const char *vdi_stream_client__parsec_status_name(ParsecStatus status) {
	switch (status) {
		case PARSEC_OK:
			return "PARSEC_OK";
		case PARSEC_CONNECTING:
			return "PARSEC_CONNECTING";
		case PARSEC_NOT_RUNNING:
			return "PARSEC_NOT_RUNNING";
		case PARSEC_ALREADY_RUNNING:
			return "PARSEC_ALREADY_RUNNING";
		case DECODE_ERR_INIT:
			return "DECODE_ERR_INIT";
		case DECODE_ERR_LOAD:
			return "DECODE_ERR_LOAD";
		case DECODE_ERR_MAP:
			return "DECODE_ERR_MAP";
		case DECODE_ERR_DECODE:
			return "DECODE_ERR_DECODE";
		case DECODE_ERR_CLEANUP:
			return "DECODE_ERR_CLEANUP";
		case DECODE_ERR_PARSE:
			return "DECODE_ERR_PARSE";
		case DECODE_ERR_NO_SUPPORT:
			return "DECODE_ERR_NO_SUPPORT";
		case DECODE_ERR_PIXEL_FORMAT:
			return "DECODE_ERR_PIXEL_FORMAT";
		case DECODE_ERR_BUFFER:
			return "DECODE_ERR_BUFFER";
		case DECODE_ERR_RESOLUTION:
			return "DECODE_ERR_RESOLUTION";
		case DECODE_ERR_OUT_OF_RANGE:
			return "DECODE_ERR_OUT_OF_RANGE";
		case DECODE_ERR_DEPENDENCY:
			return "DECODE_ERR_DEPENDENCY";
		case DECODE_ERR_SYMBOL:
			return "DECODE_ERR_SYMBOL";
		default:
			return "unknown";
	}
}

static void vdi_stream_client__parsec_log_callback(ParsecLogLevel level, const char *msg, void *opaque) {
	(void) opaque;

	if (msg == NULL || msg[0] == '\0') {
		return;
	}

	vdi_stream_client__log_info("Parsec SDK %s: %s%s", level == LOG_DEBUG ? "DEBUG" : "INFO", msg, msg[strlen(msg) - 1] == '\n' ? "" : "\n");
}

static bool vdi_stream_client__parsec_dlsym_available(void *handle, const char *symbol, char *missing, size_t missing_len) {
	dlerror();
	if (dlsym(handle, symbol) != NULL) {
		return true;
	}

	if (missing != NULL && missing_len > 0) {
		snprintf(missing, missing_len, "%s", symbol);
	}
	return false;
}

/*
 * The hidden Linux FFmpeg decoder in this Parsec SDK is built for FFmpeg 4.x
 * library SONAMEs. It is visible after clearing the hidden bit, but decoder
 * initialization fails later with DECODE_ERR_LOAD / DECODE_ERR_SYMBOL if these
 * compatibility libraries are not present. Guard the forced H.265 selection so
 * the client falls back to the normal H.264 path instead of entering a reconnect
 * loop.
 */
static bool vdi_stream_client__parsec_ffmpeg_dependencies_available(char *missing, size_t missing_len) {
	const char *codec_symbols[] = {
		"avcodec_open2",
		"avcodec_close",
		"avcodec_find_decoder",
		"avcodec_receive_frame",
		"avcodec_alloc_context3",
		"avcodec_free_context",
		"avcodec_send_packet",
	};
	const char *util_symbols[] = {
		"av_frame_alloc",
		"av_frame_free",
	};
	void *avcodec;
	void *avutil;
	size_t i;

	avutil = dlopen("libavutil.so.56", RTLD_LAZY | RTLD_LOCAL);
	if (avutil == NULL) {
		if (missing != NULL && missing_len > 0) {
			snprintf(missing, missing_len, "libavutil.so.56");
		}
		return false;
	}

	for (i = 0; i < sizeof(util_symbols) / sizeof(util_symbols[0]); i++) {
		if (!vdi_stream_client__parsec_dlsym_available(avutil, util_symbols[i], missing, missing_len)) {
			dlclose(avutil);
			return false;
		}
	}

	avcodec = dlopen("libavcodec.so.58", RTLD_LAZY | RTLD_LOCAL);
	if (avcodec == NULL) {
		if (missing != NULL && missing_len > 0) {
			snprintf(missing, missing_len, "libavcodec.so.58");
		}
		dlclose(avutil);
		return false;
	}

	for (i = 0; i < sizeof(codec_symbols) / sizeof(codec_symbols[0]); i++) {
		if (!vdi_stream_client__parsec_dlsym_available(avcodec, codec_symbols[i], missing, missing_len)) {
			dlclose(avcodec);
			dlclose(avutil);
			return false;
		}
	}

	dlclose(avcodec);
	dlclose(avutil);
	return true;
}

/* find a visible Parsec decoder through the public SDK API. */
static bool vdi_stream_client__parsec_find_decoder(struct parsec_context_s *parsec_context, const char *name, bool h265, Uint32 *index) {
	ParsecDecoder decoders[8] = {0};
	Uint32 decoder;
	Uint32 count;

#ifdef HAVE_LIBPARSEC
	(void) parsec_context;
	count = ParsecGetDecoders(decoders, sizeof(decoders) / sizeof(decoders[0]));
#else
	count = ParsecGetDecoders(parsec_context->parsec, decoders, sizeof(decoders) / sizeof(decoders[0]));
#endif

	for (decoder = 0; decoder < count; decoder++) {
		if (name != NULL && strncmp(decoders[decoder].name, name, sizeof(decoders[decoder].name)) != 0) {
			continue;
		}

		if (h265 && !decoders[decoder].h265) {
			continue;
		}

		if (index != NULL) {
			*index = decoders[decoder].index;
		}

		return true;
	}

	return false;
}


/* locate the SDK-internal decoder table used by ParsecGetDecoders(). */
static Uint8 *vdi_stream_client__parsec_decoder_table(struct parsec_context_s *parsec_context) {
#if defined(__linux__) && defined(__x86_64__)
	const Uint8 pattern[] = {0x48, 0x8d, 0x2d};
	const Uint32 scan_len = 128;
	const Uint8 *func;
	Uint32 offset;
	int32_t displacement;

#ifdef HAVE_LIBPARSEC
	(void) parsec_context;
	func = (const Uint8 *) (const void *) ParsecGetDecoders;
#else
	if (parsec_context->parsec == NULL || parsec_context->parsec->api.ParsecGetDecoders == NULL) {
		return NULL;
	}
	func = (const Uint8 *) (const void *) parsec_context->parsec->api.ParsecGetDecoders;
#endif

	for (offset = 0; offset + sizeof(pattern) + sizeof(displacement) < scan_len; offset++) {
		if (memcmp(func + offset, pattern, sizeof(pattern)) != 0) {
			continue;
		}

		memcpy(&displacement, func + offset + sizeof(pattern), sizeof(displacement));
		return (Uint8 *) (func + offset + sizeof(pattern) + sizeof(displacement) + displacement);
	}
#else
	(void) parsec_context;
#endif

	return NULL;
}

static bool vdi_stream_client__parsec_make_writable(void *address, size_t len, int prot) {
	long page_size;
	uintptr_t start;
	uintptr_t end;

	page_size = sysconf(_SC_PAGESIZE);
	if (page_size <= 0) {
		return false;
	}

	start = (uintptr_t) address & ~((uintptr_t) page_size - 1);
	end = ((uintptr_t) address + len + (uintptr_t) page_size - 1) & ~((uintptr_t) page_size - 1);
	return mprotect((void *) start, (size_t) (end - start), prot) == 0;
}

/*
 * Keep the normal decoder index for H.264 fallback, but make the selected
 * decoder advertise the hidden FFmpeg decoder's H.265 capability during the
 * SDK's connection capability negotiation. This avoids forcing decoder index 2
 * for sessions where the host still sends H.264; forcing index 2 makes the SDK
 * fall back into OpenH264 and can fail before video starts.
 */
static bool vdi_stream_client__parsec_advertise_ffmpeg_h265(struct parsec_context_s *parsec_context, Uint32 decoder_index) {
#if defined(__linux__) && defined(__x86_64__)
	const Uint32 decoder_entry_size = 0x28;
	const Uint32 decoder_probe_offset = 0x18;
	const Uint32 ffmpeg_decoder_index = 2;
	Uint8 *table;
	void **selected_probe;
	void *ffmpeg_probe;

	if (decoder_index == ffmpeg_decoder_index) {
		return true;
	}
	if (decoder_index > ffmpeg_decoder_index) {
		return false;
	}

	table = vdi_stream_client__parsec_decoder_table(parsec_context);
	if (table == NULL) {
		return false;
	}

	selected_probe = (void **) (void *) (table + decoder_index * decoder_entry_size + decoder_probe_offset);
	ffmpeg_probe = *(void **) (void *) (table + ffmpeg_decoder_index * decoder_entry_size + decoder_probe_offset);
	if (ffmpeg_probe == NULL) {
		return false;
	}
	if (*selected_probe == ffmpeg_probe) {
		return true;
	}

	if (!vdi_stream_client__parsec_make_writable(selected_probe, sizeof(*selected_probe), PROT_READ | PROT_WRITE)) {
		return false;
	}

	*selected_probe = ffmpeg_probe;
	(void) vdi_stream_client__parsec_make_writable(selected_probe, sizeof(*selected_probe), PROT_READ);
	return true;
#else
	(void) parsec_context;
	(void) decoder_index;
	return false;
#endif
}

/*
 * Unhide the SDK-internal FFMPEG decoder.
 *
 * Parsec SDK 6.0 ships the Linux FFmpeg decoder in the decoder table, but marks
 * it hidden. The public API has no switch for this. We locate the private table
 * via the RIP-relative load in ParsecGetDecoders() and clear the FFMPEG hidden
 * flag before connecting to the host. This enables the software H.265 decoder
 * path while keeping the normal hardware decoder preference intact.
 */
static bool vdi_stream_client__parsec_unhide_ffmpeg_decoder(struct parsec_context_s *parsec_context) {
#if defined(__linux__) && defined(__x86_64__)
	const Uint32 decoder_entry_size = 0x28;
	const Uint32 decoder_hidden_offset = 0x20;
	const Uint32 ffmpeg_decoder_index = 2;
	Uint8 *table;
	Uint8 *hidden;

	if (vdi_stream_client__parsec_find_decoder(parsec_context, "FFMPEG", false, NULL)) {
		return true;
	}

	table = vdi_stream_client__parsec_decoder_table(parsec_context);
	if (table == NULL) {
		return false;
	}

	hidden = table + ffmpeg_decoder_index * decoder_entry_size + decoder_hidden_offset;
	if (*hidden == 0) {
		return true;
	}
	if (*hidden != 1) {
		return false;
	}

	if (!vdi_stream_client__parsec_make_writable(hidden, sizeof(*hidden), PROT_READ | PROT_WRITE)) {
		return false;
	}

	*hidden = 0;
	(void) vdi_stream_client__parsec_make_writable(hidden, sizeof(*hidden), PROT_READ);
	return vdi_stream_client__parsec_find_decoder(parsec_context, "FFMPEG", false, NULL);
#else
	(void) parsec_context;
	return false;
#endif
}

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

/* log render stats at the configured interval. */
static void vdi_stream_client__render_stats(struct parsec_context_s *parsec_context) {
	Uint64 now;

	if (!parsec_context->stats_enabled) {
		return;
	}

	now = SDL_GetTicks();
	Uint64 last_frame_age_ms = parsec_context->stats_last_frame_tick == 0 ? 0 : now - parsec_context->stats_last_frame_tick;

	if (parsec_context->stats_next_tick == 0) {
		parsec_context->stats_next_tick = now + parsec_context->stats_period_ms;
		return;
	}

	if (now < parsec_context->stats_next_tick) {
		return;
	}

	vdi_stream_client__log_info(
		"Render:\n"
		"  loop: loops=%llu, presents=%llu\n"
		"  events: sdl=%llu, parsec=%llu\n"
		"  frames: frames=%llu, age=%llums\n"
		"  idle: waits=%llu, ms=%llu\n",
		(unsigned long long) parsec_context->stats_loops,
		(unsigned long long) parsec_context->stats_presents,
		(unsigned long long) parsec_context->stats_sdl_events,
		(unsigned long long) parsec_context->stats_parsec_events,
		(unsigned long long) parsec_context->stats_frames,
		(unsigned long long) last_frame_age_ms,
		(unsigned long long) parsec_context->stats_idle_waits,
		(unsigned long long) parsec_context->stats_idle_wait_ms
	);

	parsec_context->stats_next_tick = now + parsec_context->stats_period_ms;
	parsec_context->stats_loops = 0;
	parsec_context->stats_sdl_events = 0;
	parsec_context->stats_parsec_events = 0;
	parsec_context->stats_frames = 0;
	parsec_context->stats_presents = 0;
	parsec_context->stats_idle_waits = 0;
	parsec_context->stats_idle_wait_ms = 0;
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

	SDL_DestroyTexture(parsec_context->texture_ttf);
	parsec_context->texture_ttf = NULL;

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

	/* convert the text into an sdl texture. */
	parsec_context->texture_ttf = SDL_CreateTextureFromSurface(parsec_context->renderer, parsec_context->surface_ttf);
	if (parsec_context->texture_ttf == NULL) {
		vdi_stream_client__log_error("TTF texture creation failed: %s\n", SDL_GetError());
		return VDI_STREAM_CLIENT_ERROR;
	}

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
	SDL_WindowFlags window_flags = SDL_WINDOW_HIGH_PIXEL_DENSITY;
	const char *video_driver;
	Uint32 normal_decoder_index = 1;
	bool hevc_negotiation_enabled = false;
	bool ffmpeg_hevc_retry_done = false;

	/* default values. */
	parsec_context.timeout = 100;
	parsec_context.render_timeout = 5;
	parsec_context.next_overlay_tick = 0;
	parsec_context.stats_enabled = vdi_config->stats;
	parsec_context.stats_period_ms = vdi_config->stats_period * 1000;

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
#ifdef HAVE_LIBPARSEC
	ParsecSetLogCallback(vdi_stream_client__parsec_log_callback, NULL);
#else
	ParsecSetLogCallback(parsec_context.parsec, vdi_stream_client__parsec_log_callback, NULL);
#endif

	/* use client resolution if specified. */
	if (vdi_config->width > 0 && vdi_config->height > 0) {
		vdi_stream_client__log_info("Override resolution %dx%d\n", vdi_config->width, vdi_config->height);
		cfg.video[DEFAULT_STREAM].resolutionX = vdi_config->width;
		cfg.video[DEFAULT_STREAM].resolutionY = vdi_config->height;
	}

	/* configure host video codec. */
	if (vdi_config->hevc == 1) {
		cfg.video[DEFAULT_STREAM].decoderH265 = 1;
		if (vdi_stream_client__parsec_unhide_ffmpeg_decoder(&parsec_context)) {
			vdi_stream_client__log_info("Enable FFmpeg Video Decoder for H.265 (HEVC) fallback\n");
		} else {
			vdi_stream_client__log_info("WARNING: Unable to enable FFmpeg Video Decoder, H.265 (HEVC) may be unavailable\n");
		}
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
	normal_decoder_index = cfg.video[DEFAULT_STREAM].decoderIndex;

	/* The SDK only advertises H.265 to the host if the selected decoder reports
	 * H.265 support. Do not force decoder index 2 here: if the host still chooses
	 * H.264, forcing the hidden FFmpeg decoder makes the SDK fall back into its
	 * OpenH264 software path and can fail. Instead, keep the normal decoder index
	 * for H.264 fallback and borrow FFmpeg's H.265 probe for negotiation. */
	if (vdi_config->hevc == 1) {
		ParsecDecoder decoders[8] = {0};
		bool selected_h265 = false;
		char missing[64] = {0};
		Uint32 decoder;
		Uint32 count;

		if (!vdi_stream_client__parsec_ffmpeg_dependencies_available(missing, sizeof(missing))) {
			vdi_stream_client__log_info("WARNING: FFmpeg H.265 decoder dependencies unavailable (%s)\n", missing[0] != '\0' ? missing : "unknown dependency");
			vdi_stream_client__log_info("WARNING: Keep configured decoder index %u; host may fall back to H.264 (AVC)\n", cfg.video[DEFAULT_STREAM].decoderIndex);
		} else {
#ifdef HAVE_LIBPARSEC
			count = ParsecGetDecoders(decoders, sizeof(decoders) / sizeof(decoders[0]));
#else
			count = ParsecGetDecoders(parsec_context.parsec, decoders, sizeof(decoders) / sizeof(decoders[0]));
#endif
			for (decoder = 0; decoder < count; decoder++) {
				if (decoders[decoder].index == cfg.video[DEFAULT_STREAM].decoderIndex && decoders[decoder].h265) {
					selected_h265 = true;
					break;
				}
			}

			if (selected_h265) {
				vdi_stream_client__log_info("Use decoder index %u with native H.265 (HEVC) capability\n", cfg.video[DEFAULT_STREAM].decoderIndex);
				hevc_negotiation_enabled = true;
			} else if (vdi_stream_client__parsec_advertise_ffmpeg_h265(&parsec_context, cfg.video[DEFAULT_STREAM].decoderIndex)) {
				vdi_stream_client__log_info("Advertise FFmpeg H.265 (HEVC) capability through decoder index %u\n", cfg.video[DEFAULT_STREAM].decoderIndex);
				vdi_stream_client__log_info("Keep decoder index %u for H.264 (AVC) fallback\n", cfg.video[DEFAULT_STREAM].decoderIndex);
				hevc_negotiation_enabled = true;
			} else {
				vdi_stream_client__log_info("WARNING: No visible H.265 (HEVC) decoder found, host may fall back to H.264 (AVC)\n");
			}
		}
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

	/* check if window decorations should be disabled. */
	if (vdi_config->decoration == 0) {
		vdi_stream_client__log_info("Disable window decorations\n");
		window_flags |= SDL_WINDOW_BORDERLESS;
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

connect_parsec:
	/* parsec connect. */
	vdi_stream_client__log_info("Connect to Parsec service\n");
	e = ParsecClientConnect(parsec_context.parsec, &cfg, vdi_config->session, vdi_config->peer);
	if (e != PARSEC_OK) {
		vdi_stream_client__log_error("Connection failed with code: %d\n", e);
		goto error;
	}

	/* wait until connection is established. */
	vdi_stream_client__log_info("Connect to Parsec host\n");
	wait_time = 0;
	parsec_context.connection = false;
	parsec_context.decoder = false;
	while (!parsec_context.decoder) {

		/* get client status. */
		e = ParsecClientGetStatus(parsec_context.parsec, &parsec_context.client_status);

		/* connection established */
		if (e == PARSEC_OK) {

			/* decoder not yet initialized. */
			if (parsec_context.client_status.decoder[DEFAULT_STREAM].width == 0 &&
			    parsec_context.client_status.decoder[DEFAULT_STREAM].height == 0 &&
			    !parsec_context.connection) {
				vdi_stream_client__log_info("Initialize Video Decoder\n");
				parsec_context.connection = true;
			}

			/* decoder initialized. */
			if (parsec_context.client_status.decoder[DEFAULT_STREAM].width > 0 &&
			    parsec_context.client_status.decoder[DEFAULT_STREAM].height > 0) {
				vdi_stream_client__log_info("Use %s decoder\n", parsec_context.client_status.decoder[DEFAULT_STREAM].name[0] != '\0' ? parsec_context.client_status.decoder[DEFAULT_STREAM].name : "unknown");
				vdi_stream_client__log_info("Use %s codec\n", parsec_context.client_status.decoder[DEFAULT_STREAM].h265 ? "H.265 (HEVC)" : "H.264 (AVC)");
				vdi_stream_client__log_info("Use resolution %dx%d\n", parsec_context.client_status.decoder[DEFAULT_STREAM].width, parsec_context.client_status.decoder[DEFAULT_STREAM].height);
				parsec_context.window_width = parsec_context.client_status.decoder[DEFAULT_STREAM].width;
				parsec_context.window_height = parsec_context.client_status.decoder[DEFAULT_STREAM].height;
				parsec_context.decoder = true;
			}
		}

		/* unknown error. */
		if (e != PARSEC_CONNECTING && e != PARSEC_OK) {
			vdi_stream_client__log_error("Connection failed while waiting for decoder with status: %d (%s)\n", e, vdi_stream_client__parsec_status_name(e));
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

	if (hevc_negotiation_enabled && !ffmpeg_hevc_retry_done && e == DECODE_ERR_DECODE && !parsec_context.decoder) {
		vdi_stream_client__log_info("WARNING: FFmpeg H.265 (HEVC) decoder failed with %d (%s), retrying with normal H.264 decoder index %u\n",
			e,
			vdi_stream_client__parsec_status_name(e),
			normal_decoder_index
		);
		ffmpeg_hevc_retry_done = true;
		hevc_negotiation_enabled = false;
		cfg.video[DEFAULT_STREAM].decoderH265 = 0;
		cfg.video[DEFAULT_STREAM].decoderCompatibility = 0;
		cfg.video[DEFAULT_STREAM].decoderIndex = normal_decoder_index;
		ParsecClientDisconnect(parsec_context.parsec);
		goto connect_parsec;
	}

	/* detect sdl video driver. */
	video_driver = SDL_GetCurrentVideoDriver();
	vdi_stream_client__log_info("Use %s video\n", video_driver != NULL ? video_driver : "unknown");

	/* check if connected and decoder initialized. */
	if (!parsec_context.connection &&
	    !parsec_context.decoder) {
		vdi_stream_client__log_error("Connection failed with code: %d\n", e);
		goto error;
	}

	/* The SDK's hidden FFmpeg decoder can connect and deliver audio before
	 * ParsecClientGetStatus() reports decoder dimensions. Do not create a 0x0
	 * SDL window in that case. Start with a provisional visible size and resize
	 * to the first decoded frame below. */
	if (parsec_context.window_width <= 0 || parsec_context.window_height <= 0) {
		parsec_context.window_width = vdi_config->width > 0 ? vdi_config->width : 1280;
		parsec_context.window_height = vdi_config->height > 0 ? vdi_config->height : 720;
		parsec_context.requested_width = parsec_context.window_width;
		parsec_context.requested_height = parsec_context.window_height;
		vdi_stream_client__log_info("WARNING: Decoder dimensions unavailable, start with provisional resolution %dx%d\n",
			parsec_context.window_width,
			parsec_context.window_height
		);
	}

	parsec_context.window = SDL_CreateWindow("VDI Stream Client",
					parsec_context.window_width,
					parsec_context.window_height,
					window_flags
				);
	if (parsec_context.window == NULL) {
		vdi_stream_client__log_error("Window creation failed: %s\n", SDL_GetError());
		goto error;
	}

	parsec_context.renderer = SDL_CreateRenderer(parsec_context.window, NULL);
	if (parsec_context.renderer == NULL) {
		vdi_stream_client__log_error("Renderer creation failed: %s\n", SDL_GetError());
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
		Uint64 loop_sdl_events = 0;
		Uint64 loop_parsec_events = 0;
		Uint64 loop_frames = 0;
		Uint64 loop_presents = 0;
		Uint64 idle_start = 0;
		bool local_interaction = false;
		bool rendered = false;

		force_redraw = false;
		if (parsec_context.stats_enabled) {
			loop_sdl_events = parsec_context.stats_sdl_events;
			loop_parsec_events = parsec_context.stats_parsec_events;
			loop_frames = parsec_context.stats_frames;
			loop_presents = parsec_context.stats_presents;
			parsec_context.stats_loops++;
		}

		for (SDL_Event msg; SDL_PollEvent(&msg);) {
			if (parsec_context.stats_enabled) {
				parsec_context.stats_sdl_events++;
			}
			ParsecMessage pmsg = {0};
			local_interaction = true;

			/* Only the overlay needs a forced redraw for arbitrary SDL events; the
			 * connected video path should present on new frames instead of repainting
			 * the same texture for every keyboard/mouse event. */
			if (!parsec_context.connection) {
				force_redraw = true;
			}

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
							SDL_MouseButtonFlags mouse_buttons = SDL_GetMouseState(NULL, NULL);
							if ((mouse_buttons & SDL_BUTTON_LMASK) == 0 &&
							    (mouse_buttons & SDL_BUTTON_MMASK) == 0 &&
							    (mouse_buttons & SDL_BUTTON_RMASK) == 0) {

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
				case SDL_EVENT_MOUSE_MOTION: {
					bool relative_mouse = SDL_GetWindowRelativeMouseMode(parsec_context.window);

					/* check if we released relative mouse grab. */
					if (parsec_context.relative && !relative_mouse) {

						/* no mouse motion events should be forwarded. */
						break;
					}

					pmsg.type = MESSAGE_MOUSE_MOTION;
					pmsg.mouseMotion.relative = relative_mouse;
					pmsg.mouseMotion.x = relative_mouse ? (Sint32) msg.motion.xrel : (Sint32) msg.motion.x + 1;
					pmsg.mouseMotion.y = relative_mouse ? (Sint32) msg.motion.yrel : (Sint32) msg.motion.y + 1;
					break;
				}
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

		/* prioritize SDL responsiveness after local interaction without forcing
		 * redundant presents of the last video frame. */
		parsec_context.render_timeout = local_interaction ? 0 : 5;

		/* check parsec connection status. */
		e = ParsecClientGetStatus(parsec_context.parsec, &parsec_context.client_status);
		if (!parsec_context.decoder &&
		    parsec_context.client_status.decoder[DEFAULT_STREAM].width > 0 &&
		    parsec_context.client_status.decoder[DEFAULT_STREAM].height > 0) {
			vdi_stream_client__log_info("Use %s decoder\n", parsec_context.client_status.decoder[DEFAULT_STREAM].name[0] != '\0' ? parsec_context.client_status.decoder[DEFAULT_STREAM].name : "unknown");
			vdi_stream_client__log_info("Use %s codec\n", parsec_context.client_status.decoder[DEFAULT_STREAM].h265 ? "H.265 (HEVC)" : "H.264 (AVC)");
			vdi_stream_client__log_info("Use resolution %dx%d\n", parsec_context.client_status.decoder[DEFAULT_STREAM].width, parsec_context.client_status.decoder[DEFAULT_STREAM].height);
			parsec_context.decoder = true;
		}
		if (vdi_config->reconnect == 0 && e != PARSEC_CONNECTING && e != PARSEC_OK) {

			/* render shutdown text. */
			vdi_stream_client__render_text(&parsec_context, "Closing...");
			force_redraw = true;
			vdi_stream_client__log_error("Parsec disconnected\n");
			parsec_context.done = true;
		}
		if (vdi_config->reconnect == 1 && e != PARSEC_CONNECTING && e != PARSEC_OK &&
		    SDL_GetTicks() > last_time + vdi_config->timeout) {

			if (hevc_negotiation_enabled && !ffmpeg_hevc_retry_done && e == DECODE_ERR_DECODE) {
				vdi_stream_client__log_info("WARNING: FFmpeg H.265 (HEVC) decoder failed with %d (%s), switching reconnect to normal H.264 decoder index %u\n",
					e,
					vdi_stream_client__parsec_status_name(e),
					normal_decoder_index
				);
				ffmpeg_hevc_retry_done = true;
				hevc_negotiation_enabled = false;
				cfg.video[DEFAULT_STREAM].decoderH265 = 0;
				cfg.video[DEFAULT_STREAM].decoderCompatibility = 0;
				cfg.video[DEFAULT_STREAM].decoderIndex = normal_decoder_index;
			}

			/* render reconnect text. */
			vdi_stream_client__log_error("Parsec disconnected with status: %d (%s), reconnecting\n", e, vdi_stream_client__parsec_status_name(e));
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
			vdi_stream_client__log_error("Parsec network failure, reconnecting\n");
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
			if (parsec_context.stats_enabled) {
				parsec_context.stats_parsec_events++;
			}

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

		if (parsec_context.stats_enabled) {
			idle_start = SDL_GetTicks();
		}
		rendered = vdi_stream_client__video_render(&parsec_context, force_redraw);

		/* check if we need to resize window due to client resolution change. Prefer
		 * official decoder status when available, otherwise use the first decoded
		 * frame. The FFmpeg path may provide frame data before decoder dimensions
		 * appear in ParsecClientGetStatus(). */
		Sint32 resize_width = parsec_context.client_status.decoder[DEFAULT_STREAM].width > 0 ?
			parsec_context.client_status.decoder[DEFAULT_STREAM].width : parsec_context.frame_width;
		Sint32 resize_height = parsec_context.client_status.decoder[DEFAULT_STREAM].height > 0 ?
			parsec_context.client_status.decoder[DEFAULT_STREAM].height : parsec_context.frame_height;
		if ((parsec_context.window_width != resize_width || parsec_context.window_height != resize_height) &&
		    resize_width > 0 && resize_height > 0) {
			vdi_stream_client__log_info("Change resolution from %dx%d to %dx%d\n",
				parsec_context.window_width,
				parsec_context.window_height,
				resize_width,
				resize_height
			);
			SDL_SetWindowSize(parsec_context.window, resize_width, resize_height);
			SDL_SyncWindow(parsec_context.window);
			parsec_context.window_width = resize_width;
			parsec_context.window_height = resize_height;
		}

		/* Do not add a blanket SDL_Delay(1) to the streaming hot path. When
		 * connected, ParsecClientPollFrame(timeout) and SDL_RenderPresent(vsync)
		 * are the pacing points. Sleeping here adds latency and can cap/jitter
		 * frame delivery. When disconnected, no frame source is blocking this
		 * loop, so keep a small coarse idle sleep to avoid busy-spinning while
		 * reconnecting or showing the overlay. */
		if (!parsec_context.connection && !rendered && parsec_context.render_timeout > 0) {
			SDL_Delay(parsec_context.render_timeout);
		}

		if (parsec_context.stats_enabled &&
		    parsec_context.stats_sdl_events == loop_sdl_events &&
		    parsec_context.stats_parsec_events == loop_parsec_events &&
		    parsec_context.stats_frames == loop_frames &&
		    parsec_context.stats_presents == loop_presents &&
		    !rendered) {
			parsec_context.stats_idle_waits++;
			parsec_context.stats_idle_wait_ms += SDL_GetTicks() - idle_start;
		}

		vdi_stream_client__render_stats(&parsec_context);
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


	/* destroy video resources before releasing the parsec client. */
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

	/* destroy video resources before releasing the parsec client. */
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
