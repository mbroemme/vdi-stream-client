/*
 *  ffmpeg.c -- FFmpeg decoder integration for Parsec SDK
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

#include "ffmpeg.h"
#include "client.h"

#include <libavcodec/avcodec.h>
#include <libavutil/avutil.h>
#include <libavutil/frame.h>
#include <libavutil/hwcontext.h>
#include <libavutil/pixfmt.h>
#include <limits.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>

#if !defined(__linux__)
#error "The FFmpeg Parsec decoder integration currently supports Linux only."
#endif

#if !defined(__x86_64__)
#error "The FFmpeg Parsec decoder integration currently supports x86_64 only."
#endif

#define VDI_STREAM_CLIENT_PARSEC_DECODER_ENTRY_SIZE 0x28u
#define VDI_STREAM_CLIENT_PARSEC_DECODER_INIT_OFFSET 0x00u
#define VDI_STREAM_CLIENT_PARSEC_DECODER_DECODE_OFFSET 0x08u
#define VDI_STREAM_CLIENT_PARSEC_DECODER_CLEANUP_OFFSET 0x10u
#define VDI_STREAM_CLIENT_PARSEC_DECODER_QUERY_OFFSET 0x18u
#define VDI_STREAM_CLIENT_PARSEC_DECODER_HIDDEN_OFFSET 0x20u
#define VDI_STREAM_CLIENT_PARSEC_FFMPEG_DECODER_INDEX 2u
#define VDI_STREAM_CLIENT_PARSEC_MAX_FRAME_BUFFER 0x1fa4000u

struct vdi_stream_client__parsec_ffmpeg_decoder_s
{
    AVCodecContext *codec;
    AVFrame *frame;
    AVFrame *sw_frame;
    AVPacket *packet;
    AVBufferRef *hw_device_ctx;
    enum AVCodecID codec_id;
    enum AVPixelFormat hw_pix_fmt;
    bool hwaccel;
};

static bool
vdi_stream_client__parsec_make_writable(void *address, size_t len, int prot)
{
    long page_size;
    uintptr_t start;
    uintptr_t end;

    page_size = sysconf(_SC_PAGESIZE);
    if (page_size <= 0) {
        return false;
    }

    start = (uintptr_t)address & ~((uintptr_t)page_size - 1u);
    end = ((uintptr_t)address + len + (uintptr_t)page_size - 1u) & ~((uintptr_t)page_size - 1u);
    return mprotect((void *)start, (size_t)(end - start), prot) == 0;
}

static Uint8 *
vdi_stream_client__parsec_decoder_table(struct parsec_context_s *parsec_context)
{
    const Uint8 pattern[] = { 0x48, 0x8d, 0x2d };
    const Uint32 scan_len = 128;
    const Uint8 *func;
    Uint32 offset;
    Uint32 displacement_raw;

#ifdef HAVE_LIBPARSEC
    (void)parsec_context;
    func = (const Uint8 *)(const void *)ParsecGetDecoders;
#else
    if (parsec_context->parsec == NULL || parsec_context->parsec->api.ParsecGetDecoders == NULL) {
        return NULL;
    }
    func = (const Uint8 *)(const void *)parsec_context->parsec->api.ParsecGetDecoders;
#endif

    for (offset = 0; offset + sizeof(pattern) + sizeof(displacement_raw) < scan_len; offset++) {
        if (memcmp(func + offset, pattern, sizeof(pattern)) != 0) {
            continue;
        }

        displacement_raw = (Uint32)func[offset + 3] | ((Uint32)func[offset + 4] << 8) |
                           ((Uint32)func[offset + 5] << 16) | ((Uint32)func[offset + 6] << 24);
        return (Uint8 *)(func + offset + sizeof(pattern) + sizeof(displacement_raw) +
                         (int32_t)displacement_raw);
    }
    return NULL;
}

static bool
vdi_stream_client__parsec_decoder_lookup(
    struct parsec_context_s *parsec_context, const char *name, bool h265, Uint32 *decoder_index
)
{
    ParsecDecoder decoders[8] = { 0 };
    Uint32 count;
    Uint32 i;

#ifdef HAVE_LIBPARSEC
    (void)parsec_context;
    count = ParsecGetDecoders(decoders, sizeof(decoders) / sizeof(decoders[0]));
#else
    count =
        ParsecGetDecoders(parsec_context->parsec, decoders, sizeof(decoders) / sizeof(decoders[0]));
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

static void
vdi_stream_client__parsec_ffmpeg_query_enable(void *query)
{
    Uint8 *bytes = query;

    if (bytes == NULL) {
        return;
    }

    for (size_t i = 0; i < 12; i++) {
        bytes[i] = 0;
    }
    bytes[0] = 1;
}

static void
vdi_stream_client__parsec_ffmpeg_query(void *h264, void *h265)
{
    vdi_stream_client__parsec_ffmpeg_query_enable(h264);
    vdi_stream_client__parsec_ffmpeg_query_enable(h265);
}
static const char *
vdi_stream_client__parsec_ffmpeg_error(Sint32 errnum, char *buffer, size_t len)
{
    if (buffer == NULL || len == 0) {
        return "unknown";
    }
    if (av_strerror(errnum, buffer, len) < 0) {
        SDL_snprintf(buffer, len, "ffmpeg error %d", errnum);
    }
    return buffer;
}

static enum AVPixelFormat
vdi_stream_client__parsec_ffmpeg_get_hw_format(
    AVCodecContext *codec, const enum AVPixelFormat *formats
)
{
    struct vdi_stream_client__parsec_ffmpeg_decoder_s *ffmpeg =
        codec != NULL ? codec->opaque : NULL;
    const enum AVPixelFormat *format;

    if (ffmpeg != NULL) {
        for (format = formats; format != NULL && *format != AV_PIX_FMT_NONE; format++) {
            if (*format == ffmpeg->hw_pix_fmt) {
                return *format;
            }
        }
    }

    return formats != NULL ? formats[0] : AV_PIX_FMT_NONE;
}

static bool
vdi_stream_client__parsec_ffmpeg_setup_vaapi(
    struct vdi_stream_client__parsec_ffmpeg_decoder_s *ffmpeg, const AVCodec *codec,
    bool acceleration
)
{
    const AVCodecHWConfig *config;
    Sint32 err;
    int i;

    if (ffmpeg == NULL || codec == NULL || !acceleration) {
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
        return false;
    }

    err = av_hwdevice_ctx_create(&ffmpeg->hw_device_ctx, AV_HWDEVICE_TYPE_VAAPI, NULL, NULL, 0);
    if (err < 0) {
        return false;
    }

    ffmpeg->codec->hw_device_ctx = av_buffer_ref(ffmpeg->hw_device_ctx);
    if (ffmpeg->codec->hw_device_ctx == NULL) {
        av_buffer_unref(&ffmpeg->hw_device_ctx);
        return false;
    }
    ffmpeg->codec->opaque = ffmpeg;
    ffmpeg->codec->get_format = vdi_stream_client__parsec_ffmpeg_get_hw_format;
    ffmpeg->hwaccel = true;
    return true;
}

static void
vdi_stream_client__parsec_ffmpeg_configure_context(AVCodecContext *codec)
{
    if (codec == NULL) {
        return;
    }

    codec->thread_count = 0;
    codec->thread_type = FF_THREAD_SLICE;
    codec->flags |= AV_CODEC_FLAG_LOW_DELAY;
    codec->flags2 |= AV_CODEC_FLAG2_FAST;
}

static void
vdi_stream_client__parsec_ffmpeg_log_decoder_mode(
    const struct vdi_stream_client__parsec_ffmpeg_decoder_s *ffmpeg
)
{
    static atomic_int logged_modes;
    int mode;
    int mask;
    int previous;

    if (ffmpeg == NULL) {
        return;
    }

    mode = (ffmpeg->hwaccel ? 1 : 0) | (ffmpeg->codec_id == AV_CODEC_ID_HEVC ? 2 : 0);
    mask = 1 << mode;
    previous = atomic_fetch_or_explicit(&logged_modes, mask, memory_order_relaxed);
    if ((previous & mask) != 0) {
        return;
    }

    SDL_LogInfo(
        SDL_LOG_CATEGORY_APPLICATION, "Use FFmpeg %s decoder for %s\n",
        ffmpeg->hwaccel ? "VAAPI hardware" : "software",
        ffmpeg->codec_id == AV_CODEC_ID_HEVC ? "H.265 (HEVC)" : "H.264 (AVC)"
    );
}

static void
vdi_stream_client__parsec_ffmpeg_free(struct vdi_stream_client__parsec_ffmpeg_decoder_s *ffmpeg)
{
    if (ffmpeg == NULL) {
        return;
    }
    av_packet_free(&ffmpeg->packet);
    av_frame_free(&ffmpeg->sw_frame);
    av_frame_free(&ffmpeg->frame);
    avcodec_free_context(&ffmpeg->codec);
    av_buffer_unref(&ffmpeg->hw_device_ctx);
    free(ffmpeg);
}

static Sint32
vdi_stream_client__parsec_ffmpeg_init_common(
    void *decoder, void *stream, Uint32 stream_id, void *codec_selector, void *flags,
    bool acceleration
)
{
    struct vdi_stream_client__parsec_ffmpeg_decoder_s *ffmpeg;
    const AVCodec *codec;
    Uint8 selector;
    Sint32 err;
    char errbuf[AV_ERROR_MAX_STRING_SIZE];

    (void)stream;
    (void)stream_id;
    (void)flags;

    if (decoder == NULL) {
        return DECODE_ERR_INIT;
    }

    ffmpeg = calloc(1, sizeof(*ffmpeg));
    if (ffmpeg == NULL) {
        return DECODE_ERR_BUFFER;
    }
    *((void **)decoder) = ffmpeg;

    selector = codec_selector != NULL ? ((const Uint8 *)codec_selector)[0] : 2;
    ffmpeg->codec_id = selector == 2 ? AV_CODEC_ID_HEVC : AV_CODEC_ID_H264;

    codec = avcodec_find_decoder(ffmpeg->codec_id);
    if (codec == NULL) {
        vdi_stream_client__parsec_ffmpeg_free(ffmpeg);
        *((void **)decoder) = NULL;
        return DECODE_ERR_NO_SUPPORT;
    }

    ffmpeg->codec = avcodec_alloc_context3(codec);
    if (ffmpeg->codec == NULL) {
        vdi_stream_client__parsec_ffmpeg_free(ffmpeg);
        *((void **)decoder) = NULL;
        return DECODE_ERR_BUFFER;
    }

    vdi_stream_client__parsec_ffmpeg_configure_context(ffmpeg->codec);
    (void)vdi_stream_client__parsec_ffmpeg_setup_vaapi(ffmpeg, codec, acceleration);

    err = avcodec_open2(ffmpeg->codec, codec, NULL);
    if (err < 0 && ffmpeg->hwaccel) {
        avcodec_free_context(&ffmpeg->codec);
        av_buffer_unref(&ffmpeg->hw_device_ctx);
        ffmpeg->hwaccel = false;
        ffmpeg->hw_pix_fmt = AV_PIX_FMT_NONE;

        ffmpeg->codec = avcodec_alloc_context3(codec);
        if (ffmpeg->codec == NULL) {
            vdi_stream_client__parsec_ffmpeg_free(ffmpeg);
            *((void **)decoder) = NULL;
            return DECODE_ERR_BUFFER;
        }
        vdi_stream_client__parsec_ffmpeg_configure_context(ffmpeg->codec);
        err = avcodec_open2(ffmpeg->codec, codec, NULL);
    }
    if (err < 0) {
        SDL_LogWarn(
            SDL_LOG_CATEGORY_APPLICATION, "FFmpeg decoder failed to open: %s\n",
            vdi_stream_client__parsec_ffmpeg_error(err, errbuf, sizeof(errbuf))
        );
        vdi_stream_client__parsec_ffmpeg_free(ffmpeg);
        *((void **)decoder) = NULL;
        return DECODE_ERR_INIT;
    }

    ffmpeg->frame = av_frame_alloc();
    ffmpeg->sw_frame = av_frame_alloc();
    ffmpeg->packet = av_packet_alloc();
    if (ffmpeg->frame == NULL || ffmpeg->sw_frame == NULL || ffmpeg->packet == NULL) {
        vdi_stream_client__parsec_ffmpeg_free(ffmpeg);
        *((void **)decoder) = NULL;
        return DECODE_ERR_BUFFER;
    }

    vdi_stream_client__parsec_ffmpeg_log_decoder_mode(ffmpeg);
    return PARSEC_OK;
}

static Sint32
vdi_stream_client__parsec_ffmpeg_init(
    void *decoder, void *stream, Uint32 stream_id, void *codec_selector, void *flags
)
{
    return vdi_stream_client__parsec_ffmpeg_init_common(
        decoder, stream, stream_id, codec_selector, flags, true
    );
}

static Sint32
vdi_stream_client__parsec_ffmpeg_init_no_acceleration(
    void *decoder, void *stream, Uint32 stream_id, void *codec_selector, void *flags
)
{
    return vdi_stream_client__parsec_ffmpeg_init_common(
        decoder, stream, stream_id, codec_selector, flags, false
    );
}

static void
vdi_stream_client__parsec_ffmpeg_cleanup(void *decoder)
{
    struct vdi_stream_client__parsec_ffmpeg_decoder_s *ffmpeg;

    if (decoder == NULL) {
        return;
    }

    ffmpeg = *((struct vdi_stream_client__parsec_ffmpeg_decoder_s **)decoder);
    if (ffmpeg == NULL) {
        return;
    }

    vdi_stream_client__parsec_ffmpeg_free(ffmpeg);
    *((void **)decoder) = NULL;
}

static bool
vdi_stream_client__parsec_ffmpeg_copy_plane(
    Uint8 *dst, const Uint8 *src, Sint32 dst_pitch, Sint32 src_pitch, Sint32 width, Sint32 height
)
{
    Sint32 row;

    if (dst == NULL || src == NULL || width <= 0 || height <= 0) {
        return false;
    }

    for (row = 0; row < height; row++) {
        Uint8 *dst_row = dst + (size_t)row * (size_t)dst_pitch;
        const Uint8 *src_row = src + (size_t)row * (size_t)src_pitch;

        for (Sint32 column = 0; column < width; column++) {
            dst_row[column] = src_row[column];
        }
    }
    return true;
}

static Sint32
vdi_stream_client__parsec_ffmpeg_write_i420(
    const AVFrame *source, ParsecFrame *frame, Uint32 *frame_size
)
{
    Uint32 width = (Uint32)source->width;
    Uint32 height = (Uint32)source->height;
    Uint32 chroma_width = width / 2;
    Uint32 chroma_height = height / 2;
    Uint32 y_size = width * height;
    Uint32 u_size = chroma_width * chroma_height;
    Uint32 required = (Uint32)sizeof(*frame) + y_size + u_size * 2;
    Uint8 *dst = (Uint8 *)frame + sizeof(*frame);

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
    frame->size = required - (Uint32)sizeof(*frame);
    frame->width = width;
    frame->height = height;
    frame->fullWidth = width;
    frame->fullHeight = height;
    if (frame_size != NULL) {
        *frame_size = required;
    }

    if (!vdi_stream_client__parsec_ffmpeg_copy_plane(
            dst, source->data[0], (Sint32)width, source->linesize[0], (Sint32)width, (Sint32)height
        )) {
        return DECODE_ERR_BUFFER;
    }
    dst += y_size;
    if (!vdi_stream_client__parsec_ffmpeg_copy_plane(
            dst, source->data[1], (Sint32)chroma_width, source->linesize[1], (Sint32)chroma_width,
            (Sint32)chroma_height
        )) {
        return DECODE_ERR_BUFFER;
    }
    dst += u_size;
    if (!vdi_stream_client__parsec_ffmpeg_copy_plane(
            dst, source->data[2], (Sint32)chroma_width, source->linesize[2], (Sint32)chroma_width,
            (Sint32)chroma_height
        )) {
        return DECODE_ERR_BUFFER;
    }

    return PARSEC_OK;
}

static Sint32
vdi_stream_client__parsec_ffmpeg_write_nv12(
    const AVFrame *source, ParsecFrame *frame, Uint32 *frame_size
)
{
    Uint32 width = (Uint32)source->width;
    Uint32 height = (Uint32)source->height;
    Uint32 y_size = width * height;
    Uint32 uv_size = width * (height / 2);
    Uint32 required = (Uint32)sizeof(*frame) + y_size + uv_size;
    Uint8 *dst = (Uint8 *)frame + sizeof(*frame);

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
    frame->size = required - (Uint32)sizeof(*frame);
    frame->width = width;
    frame->height = height;
    frame->fullWidth = width;
    frame->fullHeight = height;
    if (frame_size != NULL) {
        *frame_size = required;
    }

    if (!vdi_stream_client__parsec_ffmpeg_copy_plane(
            dst, source->data[0], (Sint32)width, source->linesize[0], (Sint32)width, (Sint32)height
        )) {
        return DECODE_ERR_BUFFER;
    }
    dst += y_size;
    if (!vdi_stream_client__parsec_ffmpeg_copy_plane(
            dst, source->data[1], (Sint32)width, source->linesize[1], (Sint32)width,
            (Sint32)(height / 2)
        )) {
        return DECODE_ERR_BUFFER;
    }

    return PARSEC_OK;
}

static Sint32
vdi_stream_client__parsec_ffmpeg_write_frame(
    struct vdi_stream_client__parsec_ffmpeg_decoder_s *ffmpeg, void *frame_data, Uint32 *frame_size
)
{
    AVFrame *source;
    Sint32 err;
    char errbuf[AV_ERROR_MAX_STRING_SIZE];

    if (ffmpeg == NULL || ffmpeg->frame == NULL || ffmpeg->sw_frame == NULL || frame_data == NULL) {
        return DECODE_ERR_BUFFER;
    }

    source = ffmpeg->frame;
    if (ffmpeg->hwaccel && ffmpeg->frame->format == ffmpeg->hw_pix_fmt) {
        av_frame_unref(ffmpeg->sw_frame);
        err = av_hwframe_transfer_data(ffmpeg->sw_frame, ffmpeg->frame, 0);
        if (err < 0) {
            SDL_LogWarn(
                SDL_LOG_CATEGORY_APPLICATION, "FFmpeg hardware frame transfer failed: %s\n",
                vdi_stream_client__parsec_ffmpeg_error(err, errbuf, sizeof(errbuf))
            );
            return DECODE_ERR_DECODE;
        }
        source = ffmpeg->sw_frame;
    }

    switch (source->format) {
    case AV_PIX_FMT_YUV420P:
#if LIBAVUTIL_VERSION_MAJOR < 59
    case AV_PIX_FMT_YUVJ420P:
#endif
        return vdi_stream_client__parsec_ffmpeg_write_i420(
            source, (ParsecFrame *)frame_data, frame_size
        );
    case AV_PIX_FMT_NV12:
        return vdi_stream_client__parsec_ffmpeg_write_nv12(
            source, (ParsecFrame *)frame_data, frame_size
        );
    default:
        SDL_LogWarn(
            SDL_LOG_CATEGORY_APPLICATION, "Unsupported FFmpeg pixel format %d\n", source->format
        );
        return DECODE_ERR_PIXEL_FORMAT;
    }
}

static Sint32
vdi_stream_client__parsec_ffmpeg_decode(
    void *decoder, const void *packet_data, Uint32 packet_size, void *frame_data, Uint32 *frame_size
)
{
    struct vdi_stream_client__parsec_ffmpeg_decoder_s *ffmpeg = decoder;
    Sint32 err;
    char errbuf[AV_ERROR_MAX_STRING_SIZE];

    if (ffmpeg == NULL || ffmpeg->codec == NULL || ffmpeg->frame == NULL ||
        ffmpeg->packet == NULL) {
        return DECODE_ERR_INIT;
    }
    if (packet_data == NULL || packet_size == 0) {
        return DECODE_WRN_ACCEPTED;
    }

    av_packet_unref(ffmpeg->packet);
    ffmpeg->packet->data = (Uint8 *)packet_data;
    ffmpeg->packet->size = (int)packet_size;

    err = avcodec_send_packet(ffmpeg->codec, ffmpeg->packet);
    if (err == AVERROR(EAGAIN)) {
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
        SDL_LogWarn(
            SDL_LOG_CATEGORY_APPLICATION, "FFmpeg packet decode failed: %s\n",
            vdi_stream_client__parsec_ffmpeg_error(err, errbuf, sizeof(errbuf))
        );
        return DECODE_ERR_DECODE;
    }

    err = avcodec_receive_frame(ffmpeg->codec, ffmpeg->frame);
    if (err == AVERROR(EAGAIN) || err == AVERROR_EOF) {
        return DECODE_WRN_ACCEPTED;
    }
    if (err < 0) {
        SDL_LogWarn(
            SDL_LOG_CATEGORY_APPLICATION, "FFmpeg frame receive failed: %s\n",
            vdi_stream_client__parsec_ffmpeg_error(err, errbuf, sizeof(errbuf))
        );
        return DECODE_ERR_DECODE;
    }
    if (frame_data == NULL) {
        return DECODE_WRN_ACCEPTED;
    }

    return vdi_stream_client__parsec_ffmpeg_write_frame(ffmpeg, frame_data, frame_size);
}

bool
vdi_stream_client__parsec_ffmpeg_decoder_enable(
    struct parsec_context_s *parsec_context, Uint32 *decoder_index, bool acceleration
)
{
    Uint8 *table;
    Uint8 *entry;
    Uint8 *hidden;

    table = vdi_stream_client__parsec_decoder_table(parsec_context);
    if (table == NULL) {
        return false;
    }

    entry = table + VDI_STREAM_CLIENT_PARSEC_FFMPEG_DECODER_INDEX *
                        VDI_STREAM_CLIENT_PARSEC_DECODER_ENTRY_SIZE;
    hidden = entry + VDI_STREAM_CLIENT_PARSEC_DECODER_HIDDEN_OFFSET;

    if (*hidden != 0) {
        if (*hidden != 1) {
            return false;
        }
        if (!vdi_stream_client__parsec_make_writable(
                hidden, sizeof(*hidden), PROT_READ | PROT_WRITE
            )) {
            return false;
        }
        *hidden = 0;
        (void)vdi_stream_client__parsec_make_writable(hidden, sizeof(*hidden), PROT_READ);
    }

    if (!vdi_stream_client__parsec_make_writable(
            entry, VDI_STREAM_CLIENT_PARSEC_DECODER_ENTRY_SIZE, PROT_READ | PROT_WRITE
        )) {
        return false;
    }

    ((uintptr_t *)(void *)(entry + VDI_STREAM_CLIENT_PARSEC_DECODER_INIT_OFFSET))[0] =
        acceleration ? (uintptr_t)(void *)vdi_stream_client__parsec_ffmpeg_init
                     : (uintptr_t)(void *)vdi_stream_client__parsec_ffmpeg_init_no_acceleration;
    ((uintptr_t *)(void *)(entry + VDI_STREAM_CLIENT_PARSEC_DECODER_DECODE_OFFSET))[0] =
        (uintptr_t)(void *)vdi_stream_client__parsec_ffmpeg_decode;
    ((uintptr_t *)(void *)(entry + VDI_STREAM_CLIENT_PARSEC_DECODER_CLEANUP_OFFSET))[0] =
        (uintptr_t)(void *)vdi_stream_client__parsec_ffmpeg_cleanup;
    ((uintptr_t *)(void *)(entry + VDI_STREAM_CLIENT_PARSEC_DECODER_QUERY_OFFSET))[0] =
        (uintptr_t)(void *)vdi_stream_client__parsec_ffmpeg_query;
    (void)vdi_stream_client__parsec_make_writable(
        entry, VDI_STREAM_CLIENT_PARSEC_DECODER_ENTRY_SIZE, PROT_READ
    );

    return vdi_stream_client__parsec_decoder_lookup(parsec_context, "FFMPEG", true, decoder_index);
}
