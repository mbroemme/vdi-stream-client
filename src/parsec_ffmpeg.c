/*
 *  parsec_ffmpeg.c -- injected FFmpeg decoder for Parsec SDK
 *
 *  Copyright (c) 2020-2026 Maik Broemme <mbroemme@libmpq.org>
 *
 *  This program is free software: you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License as published by
 *  the Free Software Foundation, either version 3 of the License, or
 *  (at your option) any later version.
 *
 *  Additional permission under GNU GPL version 3 section 7 is described in
 *  COPYING.EXCEPTION, allowing this program to link with the Parsec SDK.
 */

#include "client.h"
#include "parsec_ffmpeg.h"

#include <limits.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>

#ifdef HAVE_FFMPEG_DECODER
#include <libavcodec/avcodec.h>
#include <libavutil/avutil.h>
#include <libavutil/frame.h>
#include <libavutil/hwcontext.h>
#include <libavutil/pixdesc.h>
#include <libavutil/pixfmt.h>
#include <libswscale/swscale.h>
#endif

#define VDI_STREAM_CLIENT_PARSEC_DECODER_COUNT              3u
#define VDI_STREAM_CLIENT_PARSEC_DECODER_ENTRY_SIZE         0x28u
#define VDI_STREAM_CLIENT_PARSEC_DECODER_INIT_OFFSET        0x00u
#define VDI_STREAM_CLIENT_PARSEC_DECODER_DECODE_OFFSET      0x08u
#define VDI_STREAM_CLIENT_PARSEC_DECODER_CLEANUP_OFFSET     0x10u
#define VDI_STREAM_CLIENT_PARSEC_DECODER_QUERY_OFFSET       0x18u
#define VDI_STREAM_CLIENT_PARSEC_DECODER_HIDDEN_OFFSET      0x20u
#define VDI_STREAM_CLIENT_PARSEC_FFMPEG_DECODER_INDEX       2u
#define VDI_STREAM_CLIENT_PARSEC_MAX_FRAME_BUFFER           0x1fa4000u

typedef Sint32 (*vdi_stream_client__parsec_decoder_init_fn)(void *decoder, void *stream, Uint32 stream_id, void *codec_selector, void *flags);
typedef Sint32 (*vdi_stream_client__parsec_decoder_decode_fn)(void *decoder, const void *packet, Uint32 packet_size, void *frame, Uint32 *frame_size);
typedef void (*vdi_stream_client__parsec_decoder_cleanup_fn)(void *decoder);
typedef void (*vdi_stream_client__parsec_decoder_query_fn)(void *h264, void *h265);

#ifdef HAVE_FFMPEG_DECODER
struct vdi_stream_client__parsec_ffmpeg_packet_s {
	Uint8 *data;
	Uint32 size;
	struct vdi_stream_client__parsec_ffmpeg_packet_s *next;
};

enum vdi_stream_client__parsec_ffmpeg_444_output_e {
	VDI_STREAM_CLIENT_FFMPEG_444_OUTPUT_NATIVE = 0,
	VDI_STREAM_CLIENT_FFMPEG_444_OUTPUT_I444,
	VDI_STREAM_CLIENT_FFMPEG_444_OUTPUT_RGBA,
};

struct vdi_stream_client__parsec_ffmpeg_decoder_s {
	AVCodecContext *codec;
	AVFrame *frame;
	AVFrame *sw_frame;
	AVFrame *convert_frame;
	AVPacket *packet;
	AVBufferRef *hw_device_ctx;
	struct SwsContext *sws;
	enum AVCodecID codec_id;
	enum AVPixelFormat hw_pix_fmt;
	enum AVPixelFormat sws_src_fmt;
	enum AVPixelFormat sws_dst_fmt;
	Sint32 sws_width;
	Sint32 sws_height;
	bool hwaccel;
	enum vdi_stream_client__parsec_ffmpeg_444_output_e output_444;
	bool logged;
	bool async;
	bool async_stop;
	bool async_output_ready;
	bool async_log_queue_drop;
	Sint32 async_status;
	SDL_Thread *async_thread;
	SDL_Mutex *async_mutex;
	struct vdi_stream_client__parsec_ffmpeg_packet_s *async_head;
	struct vdi_stream_client__parsec_ffmpeg_packet_s *async_tail;
	Uint32 async_packets;
	Uint32 async_bytes;
	Uint32 async_max_packets;
	Uint32 async_max_bytes;
	Uint8 *async_output;
	Uint32 async_output_size;
	Uint8 *async_worker_output;
	Uint32 async_worker_output_capacity;
};

static bool vdi_stream_client__parsec_ffmpeg_request_color444 = false;
#endif

static bool vdi_stream_client__parsec_make_writable(void *address, size_t len, int prot) {
	long page_size;
	uintptr_t start;
	uintptr_t end;

	page_size = sysconf(_SC_PAGESIZE);
	if (page_size <= 0) {
		return false;
	}

	start = (uintptr_t) address & ~((uintptr_t) page_size - 1u);
	end = ((uintptr_t) address + len + (uintptr_t) page_size - 1u) & ~((uintptr_t) page_size - 1u);
	return mprotect((void *) start, (size_t) (end - start), prot) == 0;
}

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

static bool vdi_stream_client__parsec_decoder_lookup(struct parsec_context_s *parsec_context, const char *name, bool h265, Uint32 *decoder_index) {
	ParsecDecoder decoders[8] = {0};
	Uint32 count;
	Uint32 i;

#ifdef HAVE_LIBPARSEC
	(void) parsec_context;
	count = ParsecGetDecoders(decoders, sizeof(decoders) / sizeof(decoders[0]));
#else
	count = ParsecGetDecoders(parsec_context->parsec, decoders, sizeof(decoders) / sizeof(decoders[0]));
#endif

	for (i = 0; i < count; i++) {
		if (name != NULL && strncmp(decoders[i].name, name, sizeof(decoders[i].name)) != 0) {
			continue;
		}
		if (h265 && !decoders[i].h265) {
			continue;
		}
		if (decoder_index != NULL) {
			*decoder_index = decoders[i].index;
		}
		return true;
	}

	return false;
}

static void vdi_stream_client__parsec_ffmpeg_query(void *h264, void *h265) {
	if (h264 != NULL) {
		memset(h264, 0, 12);
		((Uint8 *) h264)[0] = 1;
		/* Advertise 4:4:4 capability to the client-connect capability blob. */
		((Uint8 *) h264)[1] = 1;
	}
	if (h265 != NULL) {
		memset(h265, 0, 12);
		((Uint8 *) h265)[0] = 1;
		/* Advertise 4:4:4 capability to the client-connect capability blob. */
		((Uint8 *) h265)[1] = 1;
	}
}


static bool vdi_stream_client__parsec_ffmpeg_patch_decoder444_gate(struct parsec_context_s *parsec_context) {
#if defined(__linux__) && defined(__x86_64__)
	/*
	 * The public Linux SDK has a special decoder444 path in its internal
	 * capability-selection routine. Before it advertises 4:4:4 to the host it
	 * calls its own FFmpeg dependency checker for the hidden SDK decoder. That is
	 * correct for the bundled hidden decoder, but wrong after we replace the
	 * decoder callbacks with vdi-stream-client's FFmpeg implementation: the SDK
	 * dependency checker can fail even though our decoder can decode 4:4:4.
	 *
	 * Patch only the result branch of that checker:
	 *
	 *   test al, al
	 *   pop  r8
	 *   mov  r11, [rsp+0x18]
	 *   jne  enable_decoder444
	 *
	 * into an unconditional jump to enable_decoder444. The surrounding code still
	 * runs only when ParsecClientConfig.video[].decoder444 is true and still sets
	 * the SDK's own decoder index to the hidden FFmpeg entry.
	 */
	static bool patched = false;
	const Uint8 pattern[] = {
		0x84, 0xc0,                         /* test al, al */
		0x41, 0x58,                         /* pop r8 */
		0x4c, 0x8b, 0x5c, 0x24, 0x18,       /* mov r11, [rsp+0x18] */
		0x0f, 0x85                          /* jne rel32 */
	};
	Uint8 *func;
	Uint8 *scan;
	Uint8 *branch;
	Uint8 replacement[6];
	Uint32 i;
	int32_t old_rel;
	int32_t new_rel;
	uintptr_t target;

	if (patched) {
		return true;
	}

#ifdef HAVE_LIBPARSEC
	(void) parsec_context;
	func = (Uint8 *) (void *) ParsecClientConnect;
#else
	if (parsec_context->parsec == NULL || parsec_context->parsec->api.ParsecClientConnect == NULL) {
		return false;
	}
	func = (Uint8 *) (void *) parsec_context->parsec->api.ParsecClientConnect;
#endif

	/* On the SDK used here the helper is about 0xc600 bytes before
	 * ParsecClientConnect(). Keep the window intentionally narrow to avoid
	 * touching unrelated code in future SDKs. */
	scan = func - 0x10000u;
	for (i = 0; i + sizeof(pattern) + sizeof(old_rel) <= 0x14000u; i++) {
		if (memcmp(scan + i, pattern, sizeof(pattern)) != 0) {
			continue;
		}

		branch = scan + i + 9u;
		memcpy(&old_rel, branch + 2u, sizeof(old_rel));
		target = (uintptr_t) (branch + 6u + old_rel);
		new_rel = (int32_t) (target - (uintptr_t) (branch + 5u));

		replacement[0] = 0xe9; /* jmp rel32 */
		memcpy(replacement + 1u, &new_rel, sizeof(new_rel));
		replacement[5] = 0x90; /* preserve original 6-byte instruction length */

		if (!vdi_stream_client__parsec_make_writable(branch, sizeof(replacement), PROT_READ | PROT_WRITE | PROT_EXEC)) {
			vdi_stream_client__log_info("WARNING: Unable to make Parsec decoder444 negotiation branch writable\n");
			return false;
		}
		memcpy(branch, replacement, sizeof(replacement));
		__builtin___clear_cache((char *) branch, (char *) branch + sizeof(replacement));
		(void) vdi_stream_client__parsec_make_writable(branch, sizeof(replacement), PROT_READ | PROT_EXEC);

		patched = true;
		vdi_stream_client__log_info("Patch Parsec SDK 4:4:4 negotiation gate for injected FFmpeg decoder\n");
		return true;
	}

	vdi_stream_client__log_info("WARNING: Unable to locate Parsec decoder444 negotiation branch; 4:4:4 may fall back to 4:2:0\n");
	return false;
#else
	(void) parsec_context;
	return false;
#endif
}

#ifdef HAVE_FFMPEG_DECODER
static const char *vdi_stream_client__parsec_ffmpeg_error(Sint32 errnum, char *buffer, size_t len) {
	if (buffer == NULL || len == 0) {
		return "unknown";
	}
	if (av_strerror(errnum, buffer, len) < 0) {
		snprintf(buffer, len, "ffmpeg error %d", errnum);
	}
	return buffer;
}

static bool vdi_stream_client__parsec_ffmpeg_env_enabled(const char *name, bool default_value) {
	const char *value = getenv(name);

	if (value == NULL || value[0] == '\0') {
		return default_value;
	}
	if (strcmp(value, "0") == 0 || strcmp(value, "false") == 0 || strcmp(value, "FALSE") == 0 || strcmp(value, "no") == 0 || strcmp(value, "NO") == 0) {
		return false;
	}
	return true;
}

static int vdi_stream_client__parsec_ffmpeg_env_int(const char *name, int default_value, int min_value, int max_value) {
	const char *value = getenv(name);
	char *endptr = NULL;
	long parsed;

	if (value == NULL || value[0] == '\0') {
		return default_value;
	}
	parsed = strtol(value, &endptr, 10);
	if (endptr == value || *endptr != '\0') {
		return default_value;
	}
	if (parsed < min_value) {
		return min_value;
	}
	if (parsed > max_value) {
		return max_value;
	}
	return (int) parsed;
}


static bool vdi_stream_client__parsec_ffmpeg_env_is(const char *name, const char *expected) {
	const char *value = getenv(name);

	if (value == NULL || expected == NULL) {
		return false;
	}
	return strcmp(value, expected) == 0;
}

static enum vdi_stream_client__parsec_ffmpeg_444_output_e vdi_stream_client__parsec_ffmpeg_444_output_mode(void) {
	const char *mode = getenv("VDI_STREAM_CLIENT_FFMPEG_444_OUTPUT");
	const char *renderer = getenv("VDI_STREAM_CLIENT_VIDEO_RENDERER");

	if (mode == NULL || mode[0] == '\0') {
		/* OpenGL can render native 4:4:4 YUV/VUYX directly in the shader.  That
		 * avoids CPU-side swscale conversion and keeps the copied frame size small.
		 * When the user explicitly selects the SDL renderer, keep the previous RGBA
		 * default because SDL has no native I444/VUYX streaming texture. */
		if (renderer != NULL && (strcmp(renderer, "sdl") == 0 || strcmp(renderer, "SDL") == 0)) {
			return VDI_STREAM_CLIENT_FFMPEG_444_OUTPUT_RGBA;
		}
		return VDI_STREAM_CLIENT_FFMPEG_444_OUTPUT_NATIVE;
	}
	if (strcmp(mode, "rgba") == 0 || strcmp(mode, "RGBA") == 0) {
		return VDI_STREAM_CLIENT_FFMPEG_444_OUTPUT_RGBA;
	}
	if (strcmp(mode, "i444") == 0 || strcmp(mode, "I444") == 0) {
		return VDI_STREAM_CLIENT_FFMPEG_444_OUTPUT_I444;
	}
	if (strcmp(mode, "native") != 0 && strcmp(mode, "NATIVE") != 0) {
		vdi_stream_client__log_info("WARNING: Unsupported VDI_STREAM_CLIENT_FFMPEG_444_OUTPUT=%s; use native, i444, or rgba\n", mode);
	}
	return VDI_STREAM_CLIENT_FFMPEG_444_OUTPUT_NATIVE;
}

static const char *vdi_stream_client__parsec_ffmpeg_444_output_name(enum vdi_stream_client__parsec_ffmpeg_444_output_e mode) {
	switch (mode) {
		case VDI_STREAM_CLIENT_FFMPEG_444_OUTPUT_RGBA:
			return "RGBA";
		case VDI_STREAM_CLIENT_FFMPEG_444_OUTPUT_I444:
			return "I444";
		case VDI_STREAM_CLIENT_FFMPEG_444_OUTPUT_NATIVE:
		default:
			return "native";
	}
}

static const char *vdi_stream_client__parsec_ffmpeg_hwaccel_mode(void) {
	const char *mode = getenv("VDI_STREAM_CLIENT_FFMPEG_HWACCEL");

	if (mode == NULL || mode[0] == '\0') {
		return "vaapi";
	}
	return mode;
}

static bool vdi_stream_client__parsec_ffmpeg_hwaccel_requested(void) {
	const char *mode = vdi_stream_client__parsec_ffmpeg_hwaccel_mode();

	return strcmp(mode, "0") != 0 && strcmp(mode, "false") != 0 && strcmp(mode, "FALSE") != 0 &&
	       strcmp(mode, "no") != 0 && strcmp(mode, "NO") != 0 && strcmp(mode, "none") != 0 &&
	       strcmp(mode, "software") != 0;
}

static enum AVPixelFormat vdi_stream_client__parsec_ffmpeg_get_hw_format(AVCodecContext *codec, const enum AVPixelFormat *formats) {
	struct vdi_stream_client__parsec_ffmpeg_decoder_s *ffmpeg = codec != NULL ? codec->opaque : NULL;
	const enum AVPixelFormat *format;

	if (ffmpeg != NULL) {
		for (format = formats; format != NULL && *format != AV_PIX_FMT_NONE; format++) {
			if (*format == ffmpeg->hw_pix_fmt) {
				return *format;
			}
		}
		vdi_stream_client__log_info("WARNING: FFmpeg VAAPI pixel format was not offered by decoder; falling back to software frames\n");
	}

	return formats != NULL ? formats[0] : AV_PIX_FMT_NONE;
}

static bool vdi_stream_client__parsec_ffmpeg_setup_vaapi(struct vdi_stream_client__parsec_ffmpeg_decoder_s *ffmpeg, const AVCodec *codec) {
	const AVCodecHWConfig *config;
	const char *device;
	Sint32 err;
	int i;
	char errbuf[AV_ERROR_MAX_STRING_SIZE];

	if (ffmpeg == NULL || codec == NULL || !vdi_stream_client__parsec_ffmpeg_hwaccel_requested()) {
		return false;
	}

	if (!vdi_stream_client__parsec_ffmpeg_env_is("VDI_STREAM_CLIENT_FFMPEG_HWACCEL", "vaapi") &&
	    !vdi_stream_client__parsec_ffmpeg_env_is("VDI_STREAM_CLIENT_FFMPEG_HWACCEL", "VAAPI") &&
	    !vdi_stream_client__parsec_ffmpeg_env_is("VDI_STREAM_CLIENT_FFMPEG_HWACCEL", "auto") &&
	    !vdi_stream_client__parsec_ffmpeg_env_is("VDI_STREAM_CLIENT_FFMPEG_HWACCEL", "AUTO") &&
	    getenv("VDI_STREAM_CLIENT_FFMPEG_HWACCEL") != NULL && getenv("VDI_STREAM_CLIENT_FFMPEG_HWACCEL")[0] != '\0') {
		vdi_stream_client__log_info("WARNING: Unsupported FFmpeg hardware decoder '%s'; use 'vaapi' or 'none'\n", getenv("VDI_STREAM_CLIENT_FFMPEG_HWACCEL"));
		return false;
	}

	ffmpeg->hw_pix_fmt = AV_PIX_FMT_NONE;
	for (i = 0;; i++) {
		config = avcodec_get_hw_config(codec, i);
		if (config == NULL) {
			break;
		}
		if ((config->methods & AV_CODEC_HW_CONFIG_METHOD_HW_DEVICE_CTX) != 0 &&
		    config->device_type == AV_HWDEVICE_TYPE_VAAPI) {
			ffmpeg->hw_pix_fmt = config->pix_fmt;
			break;
		}
	}

	if (ffmpeg->hw_pix_fmt == AV_PIX_FMT_NONE) {
		vdi_stream_client__log_info("WARNING: FFmpeg %s decoder does not advertise VAAPI support; use software decoder\n",
			ffmpeg->codec_id == AV_CODEC_ID_HEVC ? "H.265 (HEVC)" : "H.264 (AVC)");
		return false;
	}

	device = getenv("VDI_STREAM_CLIENT_VAAPI_DEVICE");
	if (device != NULL && device[0] == '\0') {
		device = NULL;
	}

	/* Passing NULL lets FFmpeg/libva choose the default render node/display. */
	err = av_hwdevice_ctx_create(&ffmpeg->hw_device_ctx, AV_HWDEVICE_TYPE_VAAPI, device, NULL, 0);
	if (err < 0) {
		vdi_stream_client__log_info("WARNING: FFmpeg VAAPI device creation failed%s%s: %s; use software decoder\n",
			device != NULL ? " for " : "",
			device != NULL ? device : "",
			vdi_stream_client__parsec_ffmpeg_error(err, errbuf, sizeof(errbuf)));
		return false;
	}

	ffmpeg->codec->hw_device_ctx = av_buffer_ref(ffmpeg->hw_device_ctx);
	if (ffmpeg->codec->hw_device_ctx == NULL) {
		vdi_stream_client__log_info("WARNING: FFmpeg VAAPI device reference failed; use software decoder\n");
		av_buffer_unref(&ffmpeg->hw_device_ctx);
		return false;
	}
	ffmpeg->codec->opaque = ffmpeg;
	ffmpeg->codec->get_format = vdi_stream_client__parsec_ffmpeg_get_hw_format;
	ffmpeg->hwaccel = true;
	return true;
}


static void vdi_stream_client__parsec_ffmpeg_configure_context(AVCodecContext *codec) {
	if (codec == NULL) {
		return;
	}

	codec->thread_count = vdi_stream_client__parsec_ffmpeg_env_int("VDI_STREAM_CLIENT_FFMPEG_THREADS", 0, 0, 64);
	codec->thread_type = FF_THREAD_SLICE;
	if (vdi_stream_client__parsec_ffmpeg_env_enabled("VDI_STREAM_CLIENT_FFMPEG_FRAME_THREADS", false)) {
		codec->thread_type |= FF_THREAD_FRAME;
	}
	if (vdi_stream_client__parsec_ffmpeg_env_enabled("VDI_STREAM_CLIENT_FFMPEG_LOW_DELAY", true)) {
		codec->flags |= AV_CODEC_FLAG_LOW_DELAY;
	}
	if (vdi_stream_client__parsec_ffmpeg_env_enabled("VDI_STREAM_CLIENT_FFMPEG_DROP_NONREF", false)) {
		codec->skip_frame = AVDISCARD_NONREF;
	}
	codec->flags2 |= AV_CODEC_FLAG2_FAST;
}


static void vdi_stream_client__parsec_ffmpeg_packet_free(struct vdi_stream_client__parsec_ffmpeg_packet_s *packet) {
	if (packet == NULL) {
		return;
	}
	free(packet->data);
	free(packet);
}

static void vdi_stream_client__parsec_ffmpeg_queue_clear_locked(struct vdi_stream_client__parsec_ffmpeg_decoder_s *ffmpeg) {
	struct vdi_stream_client__parsec_ffmpeg_packet_s *packet;

	if (ffmpeg == NULL) {
		return;
	}

	packet = ffmpeg->async_head;
	while (packet != NULL) {
		struct vdi_stream_client__parsec_ffmpeg_packet_s *next = packet->next;
		vdi_stream_client__parsec_ffmpeg_packet_free(packet);
		packet = next;
	}
	ffmpeg->async_head = NULL;
	ffmpeg->async_tail = NULL;
	ffmpeg->async_packets = 0;
	ffmpeg->async_bytes = 0;
}

static bool vdi_stream_client__parsec_ffmpeg_async_enqueue(struct vdi_stream_client__parsec_ffmpeg_decoder_s *ffmpeg, const void *packet_data, Uint32 packet_size) {
	struct vdi_stream_client__parsec_ffmpeg_packet_s *packet;

	if (ffmpeg == NULL || ffmpeg->async_mutex == NULL || packet_data == NULL || packet_size == 0) {
		return true;
	}

	packet = calloc(1, sizeof(*packet));
	if (packet == NULL) {
		return false;
	}
	packet->data = malloc(packet_size);
	if (packet->data == NULL) {
		free(packet);
		return false;
	}
	memcpy(packet->data, packet_data, packet_size);
	packet->size = packet_size;

	SDL_LockMutex(ffmpeg->async_mutex);
	if (!ffmpeg->async_stop &&
	    ((ffmpeg->async_max_packets > 0 && ffmpeg->async_packets >= ffmpeg->async_max_packets) ||
	     (ffmpeg->async_max_bytes > 0 && ffmpeg->async_bytes + packet_size > ffmpeg->async_max_bytes))) {
		/* Do not let decode lag grow without bound. Dropping compressed packets can
		 * cause short visual artifacts until the next clean reference point, but it is
		 * better than showing seconds-old desktop frames under heavy 4:4:4 load. */
		if (!ffmpeg->async_log_queue_drop) {
			vdi_stream_client__log_info("WARNING: FFmpeg async decoder queue is full; drop compressed packets to avoid video latency buildup\n");
			ffmpeg->async_log_queue_drop = true;
		}
		SDL_UnlockMutex(ffmpeg->async_mutex);
		vdi_stream_client__parsec_ffmpeg_packet_free(packet);
		return true;
	}
	if (ffmpeg->async_stop) {
		SDL_UnlockMutex(ffmpeg->async_mutex);
		vdi_stream_client__parsec_ffmpeg_packet_free(packet);
		return true;
	}
	if (ffmpeg->async_tail != NULL) {
		ffmpeg->async_tail->next = packet;
	} else {
		ffmpeg->async_head = packet;
	}
	ffmpeg->async_tail = packet;
	ffmpeg->async_packets++;
	ffmpeg->async_bytes += packet_size;
	SDL_UnlockMutex(ffmpeg->async_mutex);
	return true;
}

static struct vdi_stream_client__parsec_ffmpeg_packet_s *vdi_stream_client__parsec_ffmpeg_async_pop(struct vdi_stream_client__parsec_ffmpeg_decoder_s *ffmpeg) {
	struct vdi_stream_client__parsec_ffmpeg_packet_s *packet;

	if (ffmpeg == NULL || ffmpeg->async_mutex == NULL) {
		return NULL;
	}

	SDL_LockMutex(ffmpeg->async_mutex);
	packet = ffmpeg->async_head;
	if (packet != NULL) {
		ffmpeg->async_head = packet->next;
		if (ffmpeg->async_head == NULL) {
			ffmpeg->async_tail = NULL;
		}
		ffmpeg->async_packets--;
		ffmpeg->async_bytes -= packet->size;
		packet->next = NULL;
	}
	SDL_UnlockMutex(ffmpeg->async_mutex);
	return packet;
}

static bool vdi_stream_client__parsec_ffmpeg_async_take_output(struct vdi_stream_client__parsec_ffmpeg_decoder_s *ffmpeg, void *frame_data, Uint32 *frame_size) {
	bool ready = false;

	if (ffmpeg == NULL || ffmpeg->async_mutex == NULL || frame_data == NULL) {
		return false;
	}

	SDL_LockMutex(ffmpeg->async_mutex);
	if (ffmpeg->async_output_ready && ffmpeg->async_output != NULL && ffmpeg->async_output_size > 0) {
		memcpy(frame_data, ffmpeg->async_output, ffmpeg->async_output_size);
		if (frame_size != NULL) {
			*frame_size = ffmpeg->async_output_size;
		}
		ffmpeg->async_output_ready = false;
		ready = true;
	}
	SDL_UnlockMutex(ffmpeg->async_mutex);
	return ready;
}

static bool vdi_stream_client__parsec_ffmpeg_async_start(struct vdi_stream_client__parsec_ffmpeg_decoder_s *ffmpeg);
static void vdi_stream_client__parsec_ffmpeg_async_stop(struct vdi_stream_client__parsec_ffmpeg_decoder_s *ffmpeg);

static void vdi_stream_client__parsec_ffmpeg_free(struct vdi_stream_client__parsec_ffmpeg_decoder_s *ffmpeg) {
	if (ffmpeg == NULL) {
		return;
	}
	vdi_stream_client__parsec_ffmpeg_async_stop(ffmpeg);
	av_packet_free(&ffmpeg->packet);
	av_frame_free(&ffmpeg->convert_frame);
	av_frame_free(&ffmpeg->sw_frame);
	av_frame_free(&ffmpeg->frame);
	sws_freeContext(ffmpeg->sws);
	avcodec_free_context(&ffmpeg->codec);
	av_buffer_unref(&ffmpeg->hw_device_ctx);
	free(ffmpeg);
}

static Sint32 vdi_stream_client__parsec_ffmpeg_init(void *decoder, void *stream, Uint32 stream_id, void *codec_selector, void *flags) {
	struct vdi_stream_client__parsec_ffmpeg_decoder_s *ffmpeg;
	const AVCodec *codec;
	Uint8 selector;
	Sint32 err;
	char errbuf[AV_ERROR_MAX_STRING_SIZE];

	(void) stream;
	(void) stream_id;
	(void) flags;

	if (decoder == NULL) {
		return DECODE_ERR_INIT;
	}

	ffmpeg = calloc(1, sizeof(*ffmpeg));
	if (ffmpeg == NULL) {
		return DECODE_ERR_BUFFER;
	}
	*((void **) decoder) = ffmpeg;

	selector = codec_selector != NULL ? ((const Uint8 *) codec_selector)[0] : 2;
	ffmpeg->codec_id = selector == 2 ? AV_CODEC_ID_HEVC : AV_CODEC_ID_H264;
	ffmpeg->output_444 = vdi_stream_client__parsec_ffmpeg_444_output_mode();
	ffmpeg->async = vdi_stream_client__parsec_ffmpeg_env_enabled("VDI_STREAM_CLIENT_FFMPEG_ASYNC", vdi_stream_client__parsec_ffmpeg_request_color444);
	ffmpeg->async_max_packets = (Uint32) vdi_stream_client__parsec_ffmpeg_env_int("VDI_STREAM_CLIENT_FFMPEG_ASYNC_MAX_PACKETS", 512, 0, 65535);
	ffmpeg->async_max_bytes = (Uint32) vdi_stream_client__parsec_ffmpeg_env_int("VDI_STREAM_CLIENT_FFMPEG_ASYNC_MAX_BYTES", 64 * 1024 * 1024, 0, 512 * 1024 * 1024);

	codec = avcodec_find_decoder(ffmpeg->codec_id);
	if (codec == NULL) {
		vdi_stream_client__log_info("WARNING: FFmpeg decoder for %s not found\n", ffmpeg->codec_id == AV_CODEC_ID_HEVC ? "H.265 (HEVC)" : "H.264 (AVC)");
		vdi_stream_client__parsec_ffmpeg_free(ffmpeg);
		*((void **) decoder) = NULL;
		return DECODE_ERR_NO_SUPPORT;
	}

	ffmpeg->codec = avcodec_alloc_context3(codec);
	if (ffmpeg->codec == NULL) {
		vdi_stream_client__parsec_ffmpeg_free(ffmpeg);
		*((void **) decoder) = NULL;
		return DECODE_ERR_BUFFER;
	}

	/*
	 * Do not enable FF_THREAD_FRAME by default. Frame threading increases
	 * throughput, but it also buffers several decoded pictures internally. For
	 * interactive streaming that looks exactly like delayed keyboard/mouse input
	 * because the host already reacted, but the displayed frame arrives late.
	 *
	 * Keep slice threading enabled by default because it does not add the same
	 * frame queue. Users can still opt back into frame threading when they prefer
	 * FPS over latency:
	 *
	 *   VDI_STREAM_CLIENT_FFMPEG_FRAME_THREADS=1
	 *   VDI_STREAM_CLIENT_FFMPEG_THREADS=0     # auto, or a positive number
	 */
	vdi_stream_client__parsec_ffmpeg_configure_context(ffmpeg->codec);
	(void) vdi_stream_client__parsec_ffmpeg_setup_vaapi(ffmpeg, codec);

	err = avcodec_open2(ffmpeg->codec, codec, NULL);
	if (err < 0 && ffmpeg->hwaccel) {
		vdi_stream_client__log_info("WARNING: FFmpeg VAAPI avcodec_open2 failed: %s; retry software decoder\n", vdi_stream_client__parsec_ffmpeg_error(err, errbuf, sizeof(errbuf)));
		avcodec_free_context(&ffmpeg->codec);
		av_buffer_unref(&ffmpeg->hw_device_ctx);
		ffmpeg->hwaccel = false;
		ffmpeg->hw_pix_fmt = AV_PIX_FMT_NONE;

		ffmpeg->codec = avcodec_alloc_context3(codec);
		if (ffmpeg->codec == NULL) {
			vdi_stream_client__parsec_ffmpeg_free(ffmpeg);
			*((void **) decoder) = NULL;
			return DECODE_ERR_BUFFER;
		}
		vdi_stream_client__parsec_ffmpeg_configure_context(ffmpeg->codec);
		err = avcodec_open2(ffmpeg->codec, codec, NULL);
	}
	if (err < 0) {
		vdi_stream_client__log_info("WARNING: FFmpeg avcodec_open2 failed: %s\n", vdi_stream_client__parsec_ffmpeg_error(err, errbuf, sizeof(errbuf)));
		vdi_stream_client__parsec_ffmpeg_free(ffmpeg);
		*((void **) decoder) = NULL;
		return DECODE_ERR_INIT;
	}

	ffmpeg->frame = av_frame_alloc();
	ffmpeg->sw_frame = av_frame_alloc();
	ffmpeg->convert_frame = av_frame_alloc();
	ffmpeg->packet = av_packet_alloc();
	if (ffmpeg->frame == NULL || ffmpeg->sw_frame == NULL || ffmpeg->convert_frame == NULL || ffmpeg->packet == NULL) {
		vdi_stream_client__parsec_ffmpeg_free(ffmpeg);
		*((void **) decoder) = NULL;
		return DECODE_ERR_BUFFER;
	}

	vdi_stream_client__log_info("Use vdi-stream-client FFmpeg %s decoder for %s\n",
		ffmpeg->hwaccel ? "VAAPI hardware" : "software",
		ffmpeg->codec_id == AV_CODEC_ID_HEVC ? "H.265 (HEVC)" : "H.264 (AVC)");
	if (ffmpeg->hwaccel) {
		vdi_stream_client__log_info("Use FFmpeg VAAPI device%s%s; output is transferred to system memory for renderer upload\n",
			getenv("VDI_STREAM_CLIENT_VAAPI_DEVICE") != NULL && getenv("VDI_STREAM_CLIENT_VAAPI_DEVICE")[0] != '\0' ? " " : "",
			getenv("VDI_STREAM_CLIENT_VAAPI_DEVICE") != NULL && getenv("VDI_STREAM_CLIENT_VAAPI_DEVICE")[0] != '\0' ? getenv("VDI_STREAM_CLIENT_VAAPI_DEVICE") : "default");
	}
	vdi_stream_client__log_info("Use FFmpeg low-latency settings: threads=%d, frame_threads=%s, low_delay=%s, drop_nonref=%s\n",
		ffmpeg->codec->thread_count,
		(ffmpeg->codec->thread_type & FF_THREAD_FRAME) != 0 ? "yes" : "no",
		(ffmpeg->codec->flags & AV_CODEC_FLAG_LOW_DELAY) != 0 ? "yes" : "no",
		ffmpeg->codec->skip_frame == AVDISCARD_NONREF ? "yes" : "no"
	);
	vdi_stream_client__log_info("Use FFmpeg 4:4:4 output path: %s\n", vdi_stream_client__parsec_ffmpeg_444_output_name(ffmpeg->output_444));
	if (!vdi_stream_client__parsec_ffmpeg_async_start(ffmpeg)) {
		vdi_stream_client__parsec_ffmpeg_free(ffmpeg);
		*((void **) decoder) = NULL;
		return DECODE_ERR_INIT;
	}
	return PARSEC_OK;
}

static void vdi_stream_client__parsec_ffmpeg_cleanup(void *decoder) {
	struct vdi_stream_client__parsec_ffmpeg_decoder_s *ffmpeg;

	if (decoder == NULL) {
		return;
	}

	ffmpeg = *((struct vdi_stream_client__parsec_ffmpeg_decoder_s **) decoder);
	if (ffmpeg == NULL) {
		return;
	}

	vdi_stream_client__parsec_ffmpeg_free(ffmpeg);
	*((void **) decoder) = NULL;
}

static bool vdi_stream_client__parsec_ffmpeg_copy_plane(Uint8 *dst, const Uint8 *src, Sint32 dst_pitch, Sint32 src_pitch, Sint32 width, Sint32 height) {
	Sint32 row;

	if (dst == NULL || src == NULL || width <= 0 || height <= 0) {
		return false;
	}

	for (row = 0; row < height; row++) {
		memcpy(dst + (size_t) row * (size_t) dst_pitch, src + (size_t) row * (size_t) src_pitch, (size_t) width);
	}
	return true;
}

static Sint32 vdi_stream_client__parsec_ffmpeg_write_i420(const AVFrame *source, ParsecFrame *frame, Uint32 *frame_size) {
	Uint32 width = (Uint32) source->width;
	Uint32 height = (Uint32) source->height;
	Uint32 chroma_width = width / 2;
	Uint32 chroma_height = height / 2;
	Uint32 y_size = width * height;
	Uint32 u_size = chroma_width * chroma_height;
	Uint32 required = (Uint32) sizeof(*frame) + y_size + u_size * 2;
	Uint8 *dst = (Uint8 *) frame + sizeof(*frame);

	if (width == 0 || height == 0 || (width & 1u) != 0 || (height & 1u) != 0) {
		return DECODE_ERR_RESOLUTION;
	}
	if (required > VDI_STREAM_CLIENT_PARSEC_MAX_FRAME_BUFFER) {
		if (frame_size != NULL) {
			*frame_size = required;
		}
		return DECODE_ERR_BUFFER;
	}

	frame->format = FORMAT_I420;
	frame->rotation = ROTATION_NONE;
	frame->size = required - (Uint32) sizeof(*frame);
	frame->width = width;
	frame->height = height;
	frame->fullWidth = width;
	frame->fullHeight = height;
	if (frame_size != NULL) {
		*frame_size = required;
	}

	if (!vdi_stream_client__parsec_ffmpeg_copy_plane(dst, source->data[0], (Sint32) width, source->linesize[0], (Sint32) width, (Sint32) height)) {
		return DECODE_ERR_BUFFER;
	}
	dst += y_size;
	if (!vdi_stream_client__parsec_ffmpeg_copy_plane(dst, source->data[1], (Sint32) chroma_width, source->linesize[1], (Sint32) chroma_width, (Sint32) chroma_height)) {
		return DECODE_ERR_BUFFER;
	}
	dst += u_size;
	if (!vdi_stream_client__parsec_ffmpeg_copy_plane(dst, source->data[2], (Sint32) chroma_width, source->linesize[2], (Sint32) chroma_width, (Sint32) chroma_height)) {
		return DECODE_ERR_BUFFER;
	}

	return PARSEC_OK;
}


static Sint32 vdi_stream_client__parsec_ffmpeg_write_i444(const AVFrame *source, ParsecFrame *frame, Uint32 *frame_size) {
	Uint32 width = (Uint32) source->width;
	Uint32 height = (Uint32) source->height;
	Uint32 plane_size;
	Uint32 required;
	Uint8 *dst = (Uint8 *) frame + sizeof(*frame);

	if (width == 0 || height == 0) {
		return DECODE_ERR_RESOLUTION;
	}
	if (width > UINT32_MAX / height) {
		return DECODE_ERR_BUFFER;
	}
	plane_size = width * height;
	if (plane_size > (UINT32_MAX - (Uint32) sizeof(*frame)) / 3u) {
		return DECODE_ERR_BUFFER;
	}
	required = (Uint32) sizeof(*frame) + plane_size * 3u;
	if (required > VDI_STREAM_CLIENT_PARSEC_MAX_FRAME_BUFFER) {
		if (frame_size != NULL) {
			*frame_size = required;
		}
		return DECODE_ERR_BUFFER;
	}

	frame->format = FORMAT_I444;
	frame->rotation = ROTATION_NONE;
	frame->size = required - (Uint32) sizeof(*frame);
	frame->width = width;
	frame->height = height;
	frame->fullWidth = width;
	frame->fullHeight = height;
	if (frame_size != NULL) {
		*frame_size = required;
	}

	if (!vdi_stream_client__parsec_ffmpeg_copy_plane(dst, source->data[0], (Sint32) width, source->linesize[0], (Sint32) width, (Sint32) height)) {
		return DECODE_ERR_BUFFER;
	}
	dst += plane_size;
	if (!vdi_stream_client__parsec_ffmpeg_copy_plane(dst, source->data[1], (Sint32) width, source->linesize[1], (Sint32) width, (Sint32) height)) {
		return DECODE_ERR_BUFFER;
	}
	dst += plane_size;
	if (!vdi_stream_client__parsec_ffmpeg_copy_plane(dst, source->data[2], (Sint32) width, source->linesize[2], (Sint32) width, (Sint32) height)) {
		return DECODE_ERR_BUFFER;
	}

	return PARSEC_OK;
}

static Sint32 vdi_stream_client__parsec_ffmpeg_write_rgba(const AVFrame *source, ParsecFrame *frame, Uint32 *frame_size) {
	Uint32 width = (Uint32) source->width;
	Uint32 height = (Uint32) source->height;
	Uint32 row_size;
	Uint32 image_size;
	Uint32 required;
	Uint8 *dst = (Uint8 *) frame + sizeof(*frame);

	if (width == 0 || height == 0) {
		return DECODE_ERR_RESOLUTION;
	}
	if (width > UINT32_MAX / 4u || width * 4u > UINT32_MAX / height) {
		return DECODE_ERR_BUFFER;
	}
	row_size = width * 4u;
	image_size = row_size * height;
	if (image_size > UINT32_MAX - (Uint32) sizeof(*frame)) {
		return DECODE_ERR_BUFFER;
	}
	required = (Uint32) sizeof(*frame) + image_size;
	if (required > VDI_STREAM_CLIENT_PARSEC_MAX_FRAME_BUFFER) {
		if (frame_size != NULL) {
			*frame_size = required;
		}
		return DECODE_ERR_BUFFER;
	}

	frame->format = FORMAT_RGBA;
	frame->rotation = ROTATION_NONE;
	frame->size = required - (Uint32) sizeof(*frame);
	frame->width = width;
	frame->height = height;
	frame->fullWidth = width;
	frame->fullHeight = height;
	if (frame_size != NULL) {
		*frame_size = required;
	}

	if (!vdi_stream_client__parsec_ffmpeg_copy_plane(dst, source->data[0], (Sint32) row_size, source->linesize[0], (Sint32) row_size, (Sint32) height)) {
		return DECODE_ERR_BUFFER;
	}

	return PARSEC_OK;
}

static Sint32 vdi_stream_client__parsec_ffmpeg_write_vuyx(const AVFrame *source, ParsecFrame *frame, Uint32 *frame_size) {
	Uint32 width = (Uint32) source->width;
	Uint32 height = (Uint32) source->height;
	Uint32 row_size;
	Uint32 image_size;
	Uint32 required;
	Uint8 *dst = (Uint8 *) frame + sizeof(*frame);

	if (width == 0 || height == 0) {
		return DECODE_ERR_RESOLUTION;
	}
	if (width > UINT32_MAX / 4u || width * 4u > UINT32_MAX / height) {
		return DECODE_ERR_BUFFER;
	}
	row_size = width * 4u;
	image_size = row_size * height;
	if (image_size > UINT32_MAX - (Uint32) sizeof(*frame)) {
		return DECODE_ERR_BUFFER;
	}
	required = (Uint32) sizeof(*frame) + image_size;
	if (required > VDI_STREAM_CLIENT_PARSEC_MAX_FRAME_BUFFER) {
		if (frame_size != NULL) {
			*frame_size = required;
		}
		return DECODE_ERR_BUFFER;
	}

	frame->format = VDI_STREAM_CLIENT_FORMAT_VUYX;
	frame->rotation = ROTATION_NONE;
	frame->size = required - (Uint32) sizeof(*frame);
	frame->width = width;
	frame->height = height;
	frame->fullWidth = width;
	frame->fullHeight = height;
	if (frame_size != NULL) {
		*frame_size = required;
	}

	if (!vdi_stream_client__parsec_ffmpeg_copy_plane(dst, source->data[0], (Sint32) row_size, source->linesize[0], (Sint32) row_size, (Sint32) height)) {
		return DECODE_ERR_BUFFER;
	}

	return PARSEC_OK;
}

static Sint32 vdi_stream_client__parsec_ffmpeg_write_nv12(const AVFrame *source, ParsecFrame *frame, Uint32 *frame_size) {
	Uint32 width = (Uint32) source->width;
	Uint32 height = (Uint32) source->height;
	Uint32 y_size = width * height;
	Uint32 uv_size = width * (height / 2);
	Uint32 required = (Uint32) sizeof(*frame) + y_size + uv_size;
	Uint8 *dst = (Uint8 *) frame + sizeof(*frame);

	if (width == 0 || height == 0 || (width & 1u) != 0 || (height & 1u) != 0) {
		return DECODE_ERR_RESOLUTION;
	}
	if (required > VDI_STREAM_CLIENT_PARSEC_MAX_FRAME_BUFFER) {
		if (frame_size != NULL) {
			*frame_size = required;
		}
		return DECODE_ERR_BUFFER;
	}

	frame->format = FORMAT_NV12;
	frame->rotation = ROTATION_NONE;
	frame->size = required - (Uint32) sizeof(*frame);
	frame->width = width;
	frame->height = height;
	frame->fullWidth = width;
	frame->fullHeight = height;
	if (frame_size != NULL) {
		*frame_size = required;
	}

	if (!vdi_stream_client__parsec_ffmpeg_copy_plane(dst, source->data[0], (Sint32) width, source->linesize[0], (Sint32) width, (Sint32) height)) {
		return DECODE_ERR_BUFFER;
	}
	dst += y_size;
	if (!vdi_stream_client__parsec_ffmpeg_copy_plane(dst, source->data[1], (Sint32) width, source->linesize[1], (Sint32) width, (Sint32) (height / 2))) {
		return DECODE_ERR_BUFFER;
	}

	return PARSEC_OK;
}


static const char *vdi_stream_client__parsec_ffmpeg_pix_fmt_name(enum AVPixelFormat format) {
	const char *name = av_get_pix_fmt_name(format);

	return name != NULL ? name : "unknown";
}

static bool vdi_stream_client__parsec_ffmpeg_pixel_format_is_444(enum AVPixelFormat format) {
	const AVPixFmtDescriptor *desc = av_pix_fmt_desc_get(format);

	return desc != NULL && desc->nb_components >= 3 && desc->log2_chroma_w == 0 && desc->log2_chroma_h == 0;
}

static bool vdi_stream_client__parsec_ffmpeg_pixel_format_is_vuyx(enum AVPixelFormat format) {
	enum AVPixelFormat vuyx = av_get_pix_fmt("vuyx");

	return vuyx != AV_PIX_FMT_NONE && format == vuyx;
}

static bool vdi_stream_client__parsec_ffmpeg_rgba_fits(Sint32 width, Sint32 height) {
	Uint32 row_size;
	Uint32 image_size;

	if (width <= 0 || height <= 0 || (Uint32) width > UINT32_MAX / 4u) {
		return false;
	}
	row_size = (Uint32) width * 4u;
	if (row_size > UINT32_MAX / (Uint32) height) {
		return false;
	}
	image_size = row_size * (Uint32) height;
	return image_size <= VDI_STREAM_CLIENT_PARSEC_MAX_FRAME_BUFFER - (Uint32) sizeof(ParsecFrame);
}

static Sint32 vdi_stream_client__parsec_ffmpeg_convert_frame(struct vdi_stream_client__parsec_ffmpeg_decoder_s *ffmpeg, const AVFrame *source, enum AVPixelFormat target_format, AVFrame **converted) {
	Sint32 err;
	int scaled;
	char errbuf[AV_ERROR_MAX_STRING_SIZE];

	if (ffmpeg == NULL || source == NULL || ffmpeg->convert_frame == NULL || converted == NULL || source->width <= 0 || source->height <= 0) {
		return DECODE_ERR_BUFFER;
	}

	ffmpeg->sws = sws_getCachedContext(ffmpeg->sws,
		source->width, source->height, (enum AVPixelFormat) source->format,
		source->width, source->height, target_format,
		SWS_POINT, NULL, NULL, NULL);
	if (ffmpeg->sws == NULL) {
		vdi_stream_client__log_info("WARNING: FFmpeg swscale cannot convert pixel format %d (%s) to %d (%s)\n",
			source->format, vdi_stream_client__parsec_ffmpeg_pix_fmt_name((enum AVPixelFormat) source->format),
			target_format, vdi_stream_client__parsec_ffmpeg_pix_fmt_name(target_format));
		return DECODE_ERR_PIXEL_FORMAT;
	}

	av_frame_unref(ffmpeg->convert_frame);
	ffmpeg->convert_frame->format = target_format;
	ffmpeg->convert_frame->width = source->width;
	ffmpeg->convert_frame->height = source->height;
	err = av_frame_get_buffer(ffmpeg->convert_frame, 32);
	if (err < 0) {
		vdi_stream_client__log_info("WARNING: FFmpeg conversion frame allocation failed: %s\n", vdi_stream_client__parsec_ffmpeg_error(err, errbuf, sizeof(errbuf)));
		return DECODE_ERR_BUFFER;
	}

	scaled = sws_scale(ffmpeg->sws, (const uint8_t * const *) source->data, source->linesize, 0, source->height, ffmpeg->convert_frame->data, ffmpeg->convert_frame->linesize);
	if (scaled != source->height) {
		vdi_stream_client__log_info("WARNING: FFmpeg swscale converted only %d of %d rows from pixel format %d (%s)\n",
			scaled, source->height, source->format, vdi_stream_client__parsec_ffmpeg_pix_fmt_name((enum AVPixelFormat) source->format));
		return DECODE_ERR_PIXEL_FORMAT;
	}

	if (ffmpeg->sws_src_fmt != (enum AVPixelFormat) source->format || ffmpeg->sws_dst_fmt != target_format ||
	    ffmpeg->sws_width != source->width || ffmpeg->sws_height != source->height) {
		vdi_stream_client__log_info("Convert FFmpeg pixel format %d (%s) to %d (%s) for Parsec frame output\n",
			source->format, vdi_stream_client__parsec_ffmpeg_pix_fmt_name((enum AVPixelFormat) source->format),
			target_format, vdi_stream_client__parsec_ffmpeg_pix_fmt_name(target_format));
		ffmpeg->sws_src_fmt = (enum AVPixelFormat) source->format;
		ffmpeg->sws_dst_fmt = target_format;
		ffmpeg->sws_width = source->width;
		ffmpeg->sws_height = source->height;
	}

	*converted = ffmpeg->convert_frame;
	return PARSEC_OK;
}

static Sint32 vdi_stream_client__parsec_ffmpeg_convert_and_write_frame(struct vdi_stream_client__parsec_ffmpeg_decoder_s *ffmpeg, const AVFrame *source, ParsecFrame *frame, Uint32 *frame_size) {
	AVFrame *converted = NULL;
	Sint32 status;
	enum AVPixelFormat target_format;

	if (source == NULL) {
		return DECODE_ERR_BUFFER;
	}

	/*
	 * Prefer native 4:4:4 output when the renderer can handle it.  This keeps
	 * software YUV444P as I444 and VAAPI-transferred VUYX as packed VUYX, avoiding
	 * the expensive swscale-to-RGBA path.  Use swscale only for formats that the
	 * renderer cannot consume directly, or when the user explicitly asks for RGBA.
	 */
	if (vdi_stream_client__parsec_ffmpeg_pixel_format_is_444((enum AVPixelFormat) source->format)) {
		if (ffmpeg != NULL && ffmpeg->output_444 == VDI_STREAM_CLIENT_FFMPEG_444_OUTPUT_RGBA &&
		    vdi_stream_client__parsec_ffmpeg_rgba_fits(source->width, source->height)) {
			target_format = AV_PIX_FMT_RGBA;
		} else {
			target_format = AV_PIX_FMT_YUV444P;
		}
	} else {
		target_format = AV_PIX_FMT_YUV420P;
	}
	status = vdi_stream_client__parsec_ffmpeg_convert_frame(ffmpeg, source, target_format, &converted);
	if (status != PARSEC_OK) {
		return status;
	}

	if (target_format == AV_PIX_FMT_RGBA) {
		return vdi_stream_client__parsec_ffmpeg_write_rgba(converted, frame, frame_size);
	}
	if (target_format == AV_PIX_FMT_YUV444P) {
		return vdi_stream_client__parsec_ffmpeg_write_i444(converted, frame, frame_size);
	}
	return vdi_stream_client__parsec_ffmpeg_write_i420(converted, frame, frame_size);
}

static Sint32 vdi_stream_client__parsec_ffmpeg_write_frame(struct vdi_stream_client__parsec_ffmpeg_decoder_s *ffmpeg, void *frame_data, Uint32 *frame_size) {
	AVFrame *source;
	Sint32 err;
	char errbuf[AV_ERROR_MAX_STRING_SIZE];

	if (ffmpeg == NULL || ffmpeg->frame == NULL || ffmpeg->sw_frame == NULL || frame_data == NULL) {
		return DECODE_ERR_BUFFER;
	}

	source = ffmpeg->frame;
	if (ffmpeg->frame->format == ffmpeg->hw_pix_fmt && ffmpeg->hwaccel) {
		av_frame_unref(ffmpeg->sw_frame);
		err = av_hwframe_transfer_data(ffmpeg->sw_frame, ffmpeg->frame, 0);
		if (err < 0) {
			av_frame_unref(ffmpeg->sw_frame);
			err = av_hwframe_transfer_data(ffmpeg->sw_frame, ffmpeg->frame, 0);
		}
		if (err < 0) {
			vdi_stream_client__log_info("WARNING: FFmpeg VAAPI frame transfer failed: %s\n", vdi_stream_client__parsec_ffmpeg_error(err, errbuf, sizeof(errbuf)));
			return DECODE_ERR_DECODE;
		}
		source = ffmpeg->sw_frame;
	}

	if (!ffmpeg->logged) {
		if (source != ffmpeg->frame) {
			vdi_stream_client__log_info("FFmpeg decoded pixel format %d (%s) via VAAPI hardware format %d (%s)\n",
				source->format, vdi_stream_client__parsec_ffmpeg_pix_fmt_name((enum AVPixelFormat) source->format),
				ffmpeg->frame->format, vdi_stream_client__parsec_ffmpeg_pix_fmt_name((enum AVPixelFormat) ffmpeg->frame->format));
		} else {
			vdi_stream_client__log_info("FFmpeg decoded pixel format %d (%s)\n",
				source->format, vdi_stream_client__parsec_ffmpeg_pix_fmt_name((enum AVPixelFormat) source->format));
		}
		ffmpeg->logged = true;
	}

	if (ffmpeg->output_444 == VDI_STREAM_CLIENT_FFMPEG_444_OUTPUT_NATIVE &&
	    vdi_stream_client__parsec_ffmpeg_pixel_format_is_vuyx((enum AVPixelFormat) source->format)) {
		return vdi_stream_client__parsec_ffmpeg_write_vuyx(source, (ParsecFrame *) frame_data, frame_size);
	}

	switch (source->format) {
		case AV_PIX_FMT_YUV420P:
#if LIBAVUTIL_VERSION_MAJOR < 59
		case AV_PIX_FMT_YUVJ420P:
#endif
			return vdi_stream_client__parsec_ffmpeg_write_i420(source, (ParsecFrame *) frame_data, frame_size);
		case AV_PIX_FMT_NV12:
			return vdi_stream_client__parsec_ffmpeg_write_nv12(source, (ParsecFrame *) frame_data, frame_size);
		case AV_PIX_FMT_YUV444P:
#if LIBAVUTIL_VERSION_MAJOR < 59
		case AV_PIX_FMT_YUVJ444P:
#endif
			if (ffmpeg->output_444 == VDI_STREAM_CLIENT_FFMPEG_444_OUTPUT_RGBA) {
				return vdi_stream_client__parsec_ffmpeg_convert_and_write_frame(ffmpeg, source, (ParsecFrame *) frame_data, frame_size);
			}
			return vdi_stream_client__parsec_ffmpeg_write_i444(source, (ParsecFrame *) frame_data, frame_size);
		default:
			return vdi_stream_client__parsec_ffmpeg_convert_and_write_frame(ffmpeg, source, (ParsecFrame *) frame_data, frame_size);
	}
}

static Sint32 vdi_stream_client__parsec_ffmpeg_decode_sync(struct vdi_stream_client__parsec_ffmpeg_decoder_s *ffmpeg, const void *packet_data, Uint32 packet_size, void *frame_data, Uint32 *frame_size) {
	Sint32 err;
	char errbuf[AV_ERROR_MAX_STRING_SIZE];

	if (ffmpeg == NULL || ffmpeg->codec == NULL || ffmpeg->frame == NULL || ffmpeg->packet == NULL) {
		return DECODE_ERR_INIT;
	}
	if (packet_data == NULL || packet_size == 0) {
		return DECODE_WRN_ACCEPTED;
	}

	av_packet_unref(ffmpeg->packet);
	ffmpeg->packet->data = (Uint8 *) packet_data;
	ffmpeg->packet->size = (int) packet_size;

	err = avcodec_send_packet(ffmpeg->codec, ffmpeg->packet);
	if (err == AVERROR(EAGAIN)) {
		/* Drain one frame first, then the SDK will call us again with more data. */
		err = avcodec_receive_frame(ffmpeg->codec, ffmpeg->frame);
		if (err == 0) {
			if (frame_data == NULL) {
				return DECODE_WRN_ACCEPTED;
			}
			return vdi_stream_client__parsec_ffmpeg_write_frame(ffmpeg, frame_data, frame_size);
		}
		return DECODE_WRN_ACCEPTED;
	}
	if (err < 0) {
		vdi_stream_client__log_info("WARNING: FFmpeg avcodec_send_packet failed: %s\n", vdi_stream_client__parsec_ffmpeg_error(err, errbuf, sizeof(errbuf)));
		return DECODE_ERR_DECODE;
	}

	err = avcodec_receive_frame(ffmpeg->codec, ffmpeg->frame);
	if (err == AVERROR(EAGAIN) || err == AVERROR_EOF) {
		return DECODE_WRN_ACCEPTED;
	}
	if (err < 0) {
		vdi_stream_client__log_info("WARNING: FFmpeg avcodec_receive_frame failed: %s\n", vdi_stream_client__parsec_ffmpeg_error(err, errbuf, sizeof(errbuf)));
		return DECODE_ERR_DECODE;
	}

	if (frame_data == NULL) {
		return DECODE_WRN_ACCEPTED;
	}

	return vdi_stream_client__parsec_ffmpeg_write_frame(ffmpeg, frame_data, frame_size);
}

static Sint32 vdi_stream_client__parsec_ffmpeg_async_worker(void *opaque) {
	struct vdi_stream_client__parsec_ffmpeg_decoder_s *ffmpeg = (struct vdi_stream_client__parsec_ffmpeg_decoder_s *) opaque;
	struct vdi_stream_client__parsec_ffmpeg_packet_s *packet;

	while (ffmpeg != NULL) {
		if (ffmpeg->async_mutex != NULL) {
			SDL_LockMutex(ffmpeg->async_mutex);
			if (ffmpeg->async_stop) {
				SDL_UnlockMutex(ffmpeg->async_mutex);
				break;
			}
			SDL_UnlockMutex(ffmpeg->async_mutex);
		}

		packet = vdi_stream_client__parsec_ffmpeg_async_pop(ffmpeg);
		if (packet == NULL) {
			SDL_Delay(1);
			continue;
		}

		if (ffmpeg->async_worker_output != NULL) {
			Uint32 output_size = ffmpeg->async_worker_output_capacity;
			Sint32 status = vdi_stream_client__parsec_ffmpeg_decode_sync(ffmpeg, packet->data, packet->size, ffmpeg->async_worker_output, &output_size);
			if (status == PARSEC_OK) {
				SDL_LockMutex(ffmpeg->async_mutex);
				if (!ffmpeg->async_stop) {
					Uint8 *tmp = ffmpeg->async_output;
					ffmpeg->async_output = ffmpeg->async_worker_output;
					ffmpeg->async_output_size = output_size;
					ffmpeg->async_output_ready = true;
					ffmpeg->async_worker_output = tmp;
					/* Keep only the newest completed frame. This intentionally drops older
					 * decoded frames when the renderer falls behind. */
				}
				SDL_UnlockMutex(ffmpeg->async_mutex);
			} else if (status < 0 && status != DECODE_ERR_DECODE) {
				SDL_LockMutex(ffmpeg->async_mutex);
				ffmpeg->async_status = status;
				SDL_UnlockMutex(ffmpeg->async_mutex);
			} else if (status == DECODE_ERR_DECODE) {
				/* Packet drops or late joins can cause transient decode errors. Keep the
				 * decoder alive and let FFmpeg resynchronize at the next reference point. */
			}
		}

		vdi_stream_client__parsec_ffmpeg_packet_free(packet);
	}

	return VDI_STREAM_CLIENT_SUCCESS;
}

static bool vdi_stream_client__parsec_ffmpeg_async_start(struct vdi_stream_client__parsec_ffmpeg_decoder_s *ffmpeg) {
	if (ffmpeg == NULL || !ffmpeg->async) {
		return true;
	}

	ffmpeg->async_mutex = SDL_CreateMutex();
	ffmpeg->async_output = malloc(VDI_STREAM_CLIENT_PARSEC_MAX_FRAME_BUFFER);
	ffmpeg->async_worker_output = malloc(VDI_STREAM_CLIENT_PARSEC_MAX_FRAME_BUFFER);
	ffmpeg->async_worker_output_capacity = VDI_STREAM_CLIENT_PARSEC_MAX_FRAME_BUFFER;
	ffmpeg->async_status = PARSEC_OK;
	if (ffmpeg->async_mutex == NULL || ffmpeg->async_output == NULL || ffmpeg->async_worker_output == NULL) {
		vdi_stream_client__log_info("WARNING: Unable to allocate FFmpeg async decoder resources; use synchronous decode path\n");
		if (ffmpeg->async_mutex != NULL) {
			SDL_DestroyMutex(ffmpeg->async_mutex);
			ffmpeg->async_mutex = NULL;
		}
		free(ffmpeg->async_output);
		free(ffmpeg->async_worker_output);
		ffmpeg->async_output = NULL;
		ffmpeg->async_worker_output = NULL;
		ffmpeg->async = false;
		return true;
	}

	ffmpeg->async_thread = SDL_CreateThread(vdi_stream_client__parsec_ffmpeg_async_worker, "vdi_stream_client__ffmpeg_decoder", ffmpeg);
	if (ffmpeg->async_thread == NULL) {
		vdi_stream_client__log_info("WARNING: Unable to start FFmpeg async decoder thread: %s; use synchronous decode path\n", SDL_GetError());
		if (ffmpeg->async_mutex != NULL) {
			SDL_DestroyMutex(ffmpeg->async_mutex);
			ffmpeg->async_mutex = NULL;
		}
		free(ffmpeg->async_output);
		free(ffmpeg->async_worker_output);
		ffmpeg->async_output = NULL;
		ffmpeg->async_worker_output = NULL;
		ffmpeg->async = false;
		return true;
	}

	vdi_stream_client__log_info("Use FFmpeg async decoder thread: queue_packets=%u, queue_bytes=%u\n",
		ffmpeg->async_max_packets,
		ffmpeg->async_max_bytes);
	return true;
}

static void vdi_stream_client__parsec_ffmpeg_async_stop(struct vdi_stream_client__parsec_ffmpeg_decoder_s *ffmpeg) {
	if (ffmpeg == NULL || !ffmpeg->async) {
		return;
	}

	if (ffmpeg->async_mutex != NULL) {
		SDL_LockMutex(ffmpeg->async_mutex);
		ffmpeg->async_stop = true;
		SDL_UnlockMutex(ffmpeg->async_mutex);
	}
	if (ffmpeg->async_thread != NULL) {
		SDL_WaitThread(ffmpeg->async_thread, NULL);
		ffmpeg->async_thread = NULL;
	}
	if (ffmpeg->async_mutex != NULL) {
		SDL_LockMutex(ffmpeg->async_mutex);
		vdi_stream_client__parsec_ffmpeg_queue_clear_locked(ffmpeg);
		SDL_UnlockMutex(ffmpeg->async_mutex);
		if (ffmpeg->async_mutex != NULL) {
			SDL_DestroyMutex(ffmpeg->async_mutex);
			ffmpeg->async_mutex = NULL;
		}
	}
	free(ffmpeg->async_output);
	ffmpeg->async_output = NULL;
	free(ffmpeg->async_worker_output);
	ffmpeg->async_worker_output = NULL;
}

static Sint32 vdi_stream_client__parsec_ffmpeg_decode(void *decoder, const void *packet_data, Uint32 packet_size, void *frame_data, Uint32 *frame_size) {
	struct vdi_stream_client__parsec_ffmpeg_decoder_s *ffmpeg = decoder;
	Sint32 async_status = PARSEC_OK;

	if (ffmpeg == NULL) {
		return DECODE_ERR_INIT;
	}
	if (!ffmpeg->async) {
		return vdi_stream_client__parsec_ffmpeg_decode_sync(ffmpeg, packet_data, packet_size, frame_data, frame_size);
	}

	if (!vdi_stream_client__parsec_ffmpeg_async_enqueue(ffmpeg, packet_data, packet_size)) {
		return DECODE_ERR_BUFFER;
	}
	if (vdi_stream_client__parsec_ffmpeg_async_take_output(ffmpeg, frame_data, frame_size)) {
		return PARSEC_OK;
	}

	if (ffmpeg->async_mutex != NULL) {
		SDL_LockMutex(ffmpeg->async_mutex);
		async_status = ffmpeg->async_status;
		SDL_UnlockMutex(ffmpeg->async_mutex);
	}
	if (async_status < 0) {
		return async_status;
	}
	return DECODE_WRN_ACCEPTED;
}

#endif

bool vdi_stream_client__parsec_ffmpeg_decoder_enable(struct parsec_context_s *parsec_context, Uint32 *decoder_index, bool request_color444) {
#if defined(__linux__) && defined(__x86_64__)
#ifndef HAVE_FFMPEG_DECODER
	(void) parsec_context;
	(void) decoder_index;
	(void) request_color444;
	vdi_stream_client__log_info("WARNING: vdi-stream-client was built without FFmpeg decoder support\n");
	return false;
#else
	Uint8 *table;
	Uint8 *entry;
	Uint8 *hidden;

	vdi_stream_client__parsec_ffmpeg_request_color444 = request_color444;

	table = vdi_stream_client__parsec_decoder_table(parsec_context);
	if (table == NULL) {
		vdi_stream_client__log_info("WARNING: Unable to locate Parsec decoder table\n");
		return false;
	}

	entry = table + VDI_STREAM_CLIENT_PARSEC_FFMPEG_DECODER_INDEX * VDI_STREAM_CLIENT_PARSEC_DECODER_ENTRY_SIZE;
	hidden = entry + VDI_STREAM_CLIENT_PARSEC_DECODER_HIDDEN_OFFSET;

	if (*hidden != 0) {
		if (*hidden != 1) {
			vdi_stream_client__log_info("WARNING: Unexpected Parsec FFmpeg decoder hidden marker %u\n", (unsigned int) *hidden);
			return false;
		}
		if (!vdi_stream_client__parsec_make_writable(hidden, sizeof(*hidden), PROT_READ | PROT_WRITE)) {
			vdi_stream_client__log_info("WARNING: Unable to make Parsec decoder hidden marker writable\n");
			return false;
		}
		*hidden = 0;
		(void) vdi_stream_client__parsec_make_writable(hidden, sizeof(*hidden), PROT_READ);
	}

	if (!vdi_stream_client__parsec_make_writable(entry, VDI_STREAM_CLIENT_PARSEC_DECODER_ENTRY_SIZE, PROT_READ | PROT_WRITE)) {
		vdi_stream_client__log_info("WARNING: Unable to make Parsec decoder table writable\n");
		return false;
	}

	((uintptr_t *) (void *) (entry + VDI_STREAM_CLIENT_PARSEC_DECODER_INIT_OFFSET))[0] = (uintptr_t) (void *) vdi_stream_client__parsec_ffmpeg_init;
	((uintptr_t *) (void *) (entry + VDI_STREAM_CLIENT_PARSEC_DECODER_DECODE_OFFSET))[0] = (uintptr_t) (void *) vdi_stream_client__parsec_ffmpeg_decode;
	((uintptr_t *) (void *) (entry + VDI_STREAM_CLIENT_PARSEC_DECODER_CLEANUP_OFFSET))[0] = (uintptr_t) (void *) vdi_stream_client__parsec_ffmpeg_cleanup;
	((uintptr_t *) (void *) (entry + VDI_STREAM_CLIENT_PARSEC_DECODER_QUERY_OFFSET))[0] = (uintptr_t) (void *) vdi_stream_client__parsec_ffmpeg_query;
	(void) vdi_stream_client__parsec_make_writable(entry, VDI_STREAM_CLIENT_PARSEC_DECODER_ENTRY_SIZE, PROT_READ);

	if (request_color444) {
		if (!vdi_stream_client__parsec_ffmpeg_patch_decoder444_gate(parsec_context)) {
			vdi_stream_client__log_info("WARNING: Requested 4:4:4 color, but SDK negotiation patch failed; host may still send 4:2:0\n");
		}
	}

	if (!vdi_stream_client__parsec_decoder_lookup(parsec_context, "FFMPEG", true, decoder_index)) {
		vdi_stream_client__log_info("WARNING: Patched FFmpeg decoder is not visible through ParsecGetDecoders()\n");
		return false;
	}

	return true;
#endif
#else
	(void) parsec_context;
	(void) decoder_index;
	(void) request_color444;
	vdi_stream_client__log_info("WARNING: injected FFmpeg decoder is only implemented for Linux x86_64\n");
	return false;
#endif
}
