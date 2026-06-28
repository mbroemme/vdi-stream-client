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
#include <libavutil/hwcontext_vaapi.h>
#include <libavutil/pixdesc.h>
#include <libavutil/pixfmt.h>
#include <limits.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <sys/mman.h>
#include <unistd.h>

/* va-api includes. */
#include <va/va.h>
#include <va/va_str.h>

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
#define VDI_STREAM_CLIENT_PARSEC_SOFTWARE_DECODER_INDEX 0u
#define VDI_STREAM_CLIENT_PARSEC_HARDWARE_DECODER_INDEX 1u
#define VDI_STREAM_CLIENT_PARSEC_FFMPEG_DECODER_INDEX 2u
#define VDI_STREAM_CLIENT_PARSEC_MAX_FRAME_BUFFER 0x1fa4000u
#define VDI_STREAM_CLIENT_PARSEC_FFMPEG_FRAME_MAGIC 0x56444646u
#define VDI_STREAM_CLIENT_PARSEC_FFMPEG_FRAME_VERSION 1u
#define VDI_STREAM_CLIENT_PARSEC_FFMPEG_FRAME_SLOTS 16u

/* The public Parsec frame callback only carries a raw image pointer. For FFmpeg
 * frames, carry a small descriptor through that buffer so the renderer can
 * upload directly from retained AVFrame planes instead of copying them into a
 * second contiguous ParsecFrame image first. */
struct vdi_stream_client__parsec_ffmpeg_frame_slot_s
{
    SDL_Mutex *lock;
    AVFrame *frame;
    Uint64 generation;
};

struct vdi_stream_client__parsec_ffmpeg_frame_descriptor_s
{
    Uint32 magic;
    Uint32 version;
    uintptr_t slot;
    Uint64 generation;
};

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
    bool mode_published;
    SDL_Mutex *frame_lock;
    struct vdi_stream_client__parsec_ffmpeg_frame_slot_s
        frame_slots[VDI_STREAM_CLIENT_PARSEC_FFMPEG_FRAME_SLOTS];
    Uint64 frame_generation;
    Uint32 frame_slot;
};

static atomic_bool vdi_stream_client__parsec_ffmpeg_stats_enabled;
static atomic_uint_fast64_t vdi_stream_client__parsec_ffmpeg_video_packet_bytes;
static atomic_uint_fast64_t vdi_stream_client__parsec_ffmpeg_send_packet_calls;
static atomic_uint_fast64_t vdi_stream_client__parsec_ffmpeg_send_packet_ns;
static atomic_uint_fast64_t vdi_stream_client__parsec_ffmpeg_receive_frame_calls;
static atomic_uint_fast64_t vdi_stream_client__parsec_ffmpeg_receive_frame_ns;
static atomic_uint_fast64_t vdi_stream_client__parsec_ffmpeg_hwframe_transfer_calls;
static atomic_uint_fast64_t vdi_stream_client__parsec_ffmpeg_hwframe_transfer_ns;
static atomic_uint_fast64_t vdi_stream_client__parsec_ffmpeg_descriptor_fallback_calls;
static atomic_uint_fast64_t vdi_stream_client__parsec_ffmpeg_descriptor_fallback_ns;
static atomic_bool vdi_stream_client__parsec_ffmpeg_hardware_active;
static atomic_bool vdi_stream_client__parsec_ffmpeg_h264_acceleration;
static atomic_bool vdi_stream_client__parsec_ffmpeg_hevc_acceleration;
static atomic_bool vdi_stream_client__parsec_ffmpeg_color444;

/* The injected decoder is created once and reused for the lifetime of the
 * process. The Parsec SDK tears the decoder down and recreates it on every
 * reconnect, but building a fresh VA-API/UVD decode context on the long-lived
 * shared amdgpu device makes the kernel reject that context's first decode
 * submission with -ENOMEM and aborts (the context created on the first connect,
 * while the device was fresh, keeps working). Keeping one decode context across
 * reconnects avoids creating a new one. */
static SDL_Mutex *vdi_stream_client__parsec_ffmpeg_decoder_lock;
static struct vdi_stream_client__parsec_ffmpeg_decoder_s
    *vdi_stream_client__parsec_ffmpeg_decoder_cache;

static const char *vdi_stream_client__parsec_ffmpeg_error(Sint32 errnum, char *buffer, size_t len);
static void
vdi_stream_client__parsec_ffmpeg_free(struct vdi_stream_client__parsec_ffmpeg_decoder_s *ffmpeg);

/* Release every retained frame slot so a reused decoder does not keep the
 * previous stream's hardware surfaces referenced after a reconnect. */
static void
vdi_stream_client__parsec_ffmpeg_reset_frames(
    struct vdi_stream_client__parsec_ffmpeg_decoder_s *ffmpeg
)
{
    if (ffmpeg == NULL || ffmpeg->frame_lock == NULL) {
        return;
    }
    SDL_LockMutex(ffmpeg->frame_lock);
    for (Uint32 i = 0; i < VDI_STREAM_CLIENT_PARSEC_FFMPEG_FRAME_SLOTS; i++) {
        av_frame_free(&ffmpeg->frame_slots[i].frame);
        ffmpeg->frame_slots[i].generation = 0;
    }
    ffmpeg->frame_generation = 0;
    ffmpeg->frame_slot = 0;
    SDL_UnlockMutex(ffmpeg->frame_lock);
}

/* Return the CPU-visible pixel format for an AVFrame. VA-API frames expose this
 * through their hardware frames context, while software frames store it directly
 * in AVFrame::format. */
static enum AVPixelFormat
vdi_stream_client__parsec_ffmpeg_frame_software_format(const AVFrame *frame)
{
    AVHWFramesContext *frames_context;

    if (frame == NULL) {
        return AV_PIX_FMT_NONE;
    }
    if (frame->format != AV_PIX_FMT_VAAPI || frame->hw_frames_ctx == NULL) {
        return frame->format;
    }

    frames_context = (AVHWFramesContext *)frame->hw_frames_ctx->data;
    return frames_context != NULL ? frames_context->sw_format : AV_PIX_FMT_NONE;
}

/* Transfer a hardware AVFrame into a software AVFrame and record timing
 * counters used by --stats. */
Sint32
vdi_stream_client__parsec_ffmpeg_hwframe_transfer(AVFrame *destination, const AVFrame *source)
{
    bool stats_enabled =
        atomic_load_explicit(&vdi_stream_client__parsec_ffmpeg_stats_enabled, memory_order_relaxed);
    Uint64 stage_start_ns = stats_enabled ? SDL_GetTicksNS() : 0;
    Sint32 err = av_hwframe_transfer_data(destination, source, 0);

    if (stats_enabled) {
        atomic_fetch_add_explicit(
            &vdi_stream_client__parsec_ffmpeg_hwframe_transfer_calls, (uint_fast64_t)1,
            memory_order_relaxed
        );
        atomic_fetch_add_explicit(
            &vdi_stream_client__parsec_ffmpeg_hwframe_transfer_ns,
            (uint_fast64_t)(SDL_GetTicksNS() - stage_start_ns), memory_order_relaxed
        );
    }
    return err;
}

/* Validate and return the lightweight descriptor stored behind a ParsecFrame
 * image pointer when the FFmpeg decoder passes retained AVFrames downstream. */
static const struct vdi_stream_client__parsec_ffmpeg_frame_descriptor_s *
vdi_stream_client__parsec_ffmpeg_frame_descriptor(const ParsecFrame *frame, const void *image)
{
    const struct vdi_stream_client__parsec_ffmpeg_frame_descriptor_s *descriptor = image;

    if (frame == NULL || image == NULL || frame->size != sizeof(*descriptor) ||
        descriptor->magic != VDI_STREAM_CLIENT_PARSEC_FFMPEG_FRAME_MAGIC ||
        descriptor->version != VDI_STREAM_CLIENT_PARSEC_FFMPEG_FRAME_VERSION) {
        return NULL;
    }
    return descriptor;
}

/* Map supported FFmpeg pixel formats to Parsec and SDL formats. Planar 4:4:4 is
 * accepted only for descriptor-based paths because SDL has no direct texture
 * upload format for it here. */
static bool
vdi_stream_client__parsec_ffmpeg_frame_pixel_format(
    enum AVPixelFormat format, ParsecColorFormat *parsec_format, SDL_PixelFormat *sdl_format
)
{
    const AVPixFmtDescriptor *descriptor;

    switch (format) {
    case AV_PIX_FMT_YUV420P:
#if LIBAVUTIL_VERSION_MAJOR < 59
    case AV_PIX_FMT_YUVJ420P:
#endif
        if (parsec_format != NULL) {
            *parsec_format = FORMAT_I420;
        }
        if (sdl_format != NULL) {
            *sdl_format = SDL_PIXELFORMAT_IYUV;
        }
        return true;
    case AV_PIX_FMT_NV12:
        if (parsec_format != NULL) {
            *parsec_format = FORMAT_NV12;
        }
        if (sdl_format != NULL) {
            *sdl_format = SDL_PIXELFORMAT_NV12;
        }
        return true;
    default:
        break;
    }

    descriptor = av_pix_fmt_desc_get(format);
    if (descriptor == NULL || (descriptor->flags & AV_PIX_FMT_FLAG_RGB) != 0 ||
        descriptor->nb_components < 3 || descriptor->log2_chroma_w != 0 ||
        descriptor->log2_chroma_h != 0) {
        return false;
    }
    if (parsec_format != NULL) {
        *parsec_format = FORMAT_I444;
    }
    return sdl_format == NULL;
}

/* Resolve a descriptor to its retained AVFrame slot and hold the shared slot
 * mutex while the caller inspects or clones the frame. */
static AVFrame *
vdi_stream_client__parsec_ffmpeg_frame_lock(
    const ParsecFrame *frame, const void *image,
    struct vdi_stream_client__parsec_ffmpeg_frame_slot_s **slot_out
)
{
    const struct vdi_stream_client__parsec_ffmpeg_frame_descriptor_s *descriptor;
    struct vdi_stream_client__parsec_ffmpeg_frame_slot_s *slot;

    if (slot_out != NULL) {
        *slot_out = NULL;
    }
    descriptor = vdi_stream_client__parsec_ffmpeg_frame_descriptor(frame, image);
    if (descriptor == NULL || descriptor->slot == 0) {
        return NULL;
    }

    slot = (struct vdi_stream_client__parsec_ffmpeg_frame_slot_s *)(uintptr_t)descriptor->slot;
    if (slot->lock != NULL) {
        SDL_LockMutex(slot->lock);
    }
    if (slot->generation != descriptor->generation || slot->frame == NULL) {
        if (slot->lock != NULL) {
            SDL_UnlockMutex(slot->lock);
        }
        return NULL;
    }

    if (slot_out != NULL) {
        *slot_out = slot;
    }
    return slot->frame;
}

/* Release a frame slot lock acquired by vdi_stream_client__parsec_ffmpeg_frame_lock(). */
static void
vdi_stream_client__parsec_ffmpeg_frame_unlock(
    struct vdi_stream_client__parsec_ffmpeg_frame_slot_s *slot
)
{
    if (slot != NULL && slot->lock != NULL) {
        SDL_UnlockMutex(slot->lock);
    }
}

/* Report whether the frame/image pair carries an FFmpeg descriptor instead of a
 * raw Parsec image buffer. */
bool
vdi_stream_client__parsec_ffmpeg_frame_is_descriptor(const ParsecFrame *frame, const void *image)
{
    return vdi_stream_client__parsec_ffmpeg_frame_descriptor(frame, image) != NULL;
}

/* Report whether a descriptor currently references a VA-API hardware frame,
 * which lets the video path choose libplacebo zero-copy rendering. */
bool
vdi_stream_client__parsec_ffmpeg_frame_is_hardware(const ParsecFrame *frame, const void *image)
{
    struct vdi_stream_client__parsec_ffmpeg_frame_slot_s *slot;
    AVFrame *av_frame;
    bool hardware;

    av_frame = vdi_stream_client__parsec_ffmpeg_frame_lock(frame, image, &slot);
    if (av_frame == NULL) {
        return false;
    }
    hardware = av_frame->format == AV_PIX_FMT_VAAPI && av_frame->hw_frames_ctx != NULL;
    vdi_stream_client__parsec_ffmpeg_frame_unlock(slot);
    return hardware;
}

/* Clone the retained AVFrame referenced by a descriptor. Callers own the
 * returned reference and can safely use it after the descriptor slot unlocks. */
AVFrame *
vdi_stream_client__parsec_ffmpeg_frame_ref(const ParsecFrame *frame, const void *image)
{
    struct vdi_stream_client__parsec_ffmpeg_frame_slot_s *slot;
    AVFrame *av_frame;
    AVFrame *reference = NULL;

    av_frame = vdi_stream_client__parsec_ffmpeg_frame_lock(frame, image, &slot);
    if (av_frame != NULL) {
        reference = av_frame_clone(av_frame);
        vdi_stream_client__parsec_ffmpeg_frame_unlock(slot);
    }
    return reference;
}

/* Query the SDL texture format required to upload a descriptor-backed FFmpeg
 * frame through the software renderer fallback path. */
bool
vdi_stream_client__parsec_ffmpeg_frame_texture_format(
    const ParsecFrame *frame, const void *image, SDL_PixelFormat *pixel_format
)
{
    struct vdi_stream_client__parsec_ffmpeg_frame_slot_s *slot;
    AVFrame *av_frame;
    bool supported;

    av_frame = vdi_stream_client__parsec_ffmpeg_frame_lock(frame, image, &slot);
    if (av_frame == NULL) {
        return false;
    }

    supported = vdi_stream_client__parsec_ffmpeg_frame_pixel_format(
        vdi_stream_client__parsec_ffmpeg_frame_software_format(av_frame), NULL, pixel_format
    );
    vdi_stream_client__parsec_ffmpeg_frame_unlock(slot);
    return supported;
}

/* Upload a descriptor-backed FFmpeg frame into an SDL texture. Hardware frames
 * are transferred to a software AVFrame before SDL receives the planes. */
bool
vdi_stream_client__parsec_ffmpeg_frame_update(
    SDL_Texture *texture, const ParsecFrame *frame, const void *image, Uint64 *upload_ns
)
{
    AVFrame *av_frame;
    AVFrame *sw_frame = NULL;
    Uint64 upload_start_ns;
    Sint32 err;
    char errbuf[AV_ERROR_MAX_STRING_SIZE];
    bool ok = false;

    av_frame = vdi_stream_client__parsec_ffmpeg_frame_ref(frame, image);
    if (av_frame == NULL) {
        SDL_LogError(SDL_LOG_CATEGORY_APPLICATION, "FFmpeg frame descriptor is no longer valid\n");
        return false;
    }

    if (av_frame->format == AV_PIX_FMT_VAAPI) {
        sw_frame = av_frame_alloc();
        if (sw_frame == NULL) {
            goto done;
        }
        err = vdi_stream_client__parsec_ffmpeg_hwframe_transfer(sw_frame, av_frame);
        if (err < 0) {
            SDL_LogWarn(
                SDL_LOG_CATEGORY_APPLICATION, "FFmpeg hardware frame transfer failed: %s\n",
                vdi_stream_client__parsec_ffmpeg_error(err, errbuf, sizeof(errbuf))
            );
            goto done;
        }
        av_frame_free(&av_frame);
        av_frame = sw_frame;
        sw_frame = NULL;
    }

    upload_start_ns = upload_ns != NULL ? SDL_GetTicksNS() : 0;
    switch (av_frame->format) {
    case AV_PIX_FMT_YUV420P:
#if LIBAVUTIL_VERSION_MAJOR < 59
    case AV_PIX_FMT_YUVJ420P:
#endif
        if (av_frame->data[0] != NULL && av_frame->data[1] != NULL && av_frame->data[2] != NULL) {
            ok = SDL_UpdateYUVTexture(
                texture, NULL, av_frame->data[0], av_frame->linesize[0], av_frame->data[1],
                av_frame->linesize[1], av_frame->data[2], av_frame->linesize[2]
            );
        }
        break;
    case AV_PIX_FMT_NV12:
        if (av_frame->data[0] != NULL && av_frame->data[1] != NULL) {
            ok = SDL_UpdateNVTexture(
                texture, NULL, av_frame->data[0], av_frame->linesize[0], av_frame->data[1],
                av_frame->linesize[1]
            );
        }
        break;
    default:
        break;
    }
    if (upload_ns != NULL) {
        *upload_ns += SDL_GetTicksNS() - upload_start_ns;
    }

done:
    av_frame_free(&sw_frame);
    av_frame_free(&av_frame);
    if (!ok) {
        SDL_LogError(
            SDL_LOG_CATEGORY_APPLICATION, "Video texture update failed: %s\n", SDL_GetError()
        );
    }
    return ok;
}

/* Release the retained AVFrame slot after the renderer has consumed a
 * descriptor-backed frame. Raw Parsec image buffers are ignored. */
void
vdi_stream_client__parsec_ffmpeg_frame_release(const ParsecFrame *frame, const void *image)
{
    struct vdi_stream_client__parsec_ffmpeg_frame_slot_s *slot;
    AVFrame *av_frame;

    av_frame = vdi_stream_client__parsec_ffmpeg_frame_lock(frame, image, &slot);
    if (av_frame == NULL) {
        return;
    }

    (void)av_frame;
    av_frame_free(&slot->frame);
    slot->generation = 0;
    vdi_stream_client__parsec_ffmpeg_frame_unlock(slot);
}

/* Atomically drain FFmpeg decoder counters into the caller's stats structure and
 * reset them for the next render statistics interval. */
void
vdi_stream_client__parsec_ffmpeg_drain_stats(struct vdi_stream_client__parsec_ffmpeg_stats_s *stats)
{
    if (stats == NULL) {
        return;
    }

    stats->video_packet_bytes = (Uint64)atomic_exchange_explicit(
        &vdi_stream_client__parsec_ffmpeg_video_packet_bytes, (uint_fast64_t)0, memory_order_relaxed
    );
    stats->send_packet_calls = (Uint64)atomic_exchange_explicit(
        &vdi_stream_client__parsec_ffmpeg_send_packet_calls, (uint_fast64_t)0, memory_order_relaxed
    );
    stats->send_packet_ns = (Uint64)atomic_exchange_explicit(
        &vdi_stream_client__parsec_ffmpeg_send_packet_ns, (uint_fast64_t)0, memory_order_relaxed
    );
    stats->receive_frame_calls = (Uint64)atomic_exchange_explicit(
        &vdi_stream_client__parsec_ffmpeg_receive_frame_calls, (uint_fast64_t)0,
        memory_order_relaxed
    );
    stats->receive_frame_ns = (Uint64)atomic_exchange_explicit(
        &vdi_stream_client__parsec_ffmpeg_receive_frame_ns, (uint_fast64_t)0, memory_order_relaxed
    );
    stats->hwframe_transfer_calls = (Uint64)atomic_exchange_explicit(
        &vdi_stream_client__parsec_ffmpeg_hwframe_transfer_calls, (uint_fast64_t)0,
        memory_order_relaxed
    );
    stats->hwframe_transfer_ns = (Uint64)atomic_exchange_explicit(
        &vdi_stream_client__parsec_ffmpeg_hwframe_transfer_ns, (uint_fast64_t)0,
        memory_order_relaxed
    );
    stats->descriptor_fallback_calls = (Uint64)atomic_exchange_explicit(
        &vdi_stream_client__parsec_ffmpeg_descriptor_fallback_calls, (uint_fast64_t)0,
        memory_order_relaxed
    );
    stats->descriptor_fallback_ns = (Uint64)atomic_exchange_explicit(
        &vdi_stream_client__parsec_ffmpeg_descriptor_fallback_ns, (uint_fast64_t)0,
        memory_order_relaxed
    );
}

/* Return whether the currently published FFmpeg decoder instance is using
 * hardware acceleration. The video setup uses this to decide on Vulkan support. */
bool
vdi_stream_client__parsec_ffmpeg_decoder_is_hardware(void)
{
    return atomic_load_explicit(
        &vdi_stream_client__parsec_ffmpeg_hardware_active, memory_order_acquire
    );
}

/* Check whether a VA-API profile exposes the VLD decode entrypoint needed for
 * video decoding. */
static bool
vdi_stream_client__parsec_ffmpeg_vaapi_profile_decode(
    VADisplay display, VAProfile profile, VAEntrypoint *entrypoints, Sint32 max_entrypoints
)
{
    Sint32 entrypoint_count = max_entrypoints;

    if (vaQueryConfigEntrypoints(display, profile, entrypoints, &entrypoint_count) !=
        VA_STATUS_SUCCESS) {
        return false;
    }
    for (Sint32 i = 0; i < entrypoint_count; i++) {
        if (entrypoints[i] == VAEntrypointVLD) {
            return true;
        }
    }
    return false;
}

/* Identify HEVC VA-API profiles that represent 4:4:4 chroma modes. */
static bool
vdi_stream_client__parsec_ffmpeg_vaapi_profile_hevc444(VAProfile profile)
{
    switch (profile) {
    case VAProfileHEVCMain444:
    case VAProfileHEVCMain444_10:
    case VAProfileHEVCMain444_12:
    case VAProfileHEVCSccMain444:
    case VAProfileHEVCSccMain444_10:
        return true;
    default:
        return false;
    }
}

/* Verify that a 4:4:4 HEVC profile can actually output a YUV444 render target,
 * not just appear in the driver's profile list. */
static bool
vdi_stream_client__parsec_ffmpeg_vaapi_profile_yuv444(VADisplay display, VAProfile profile)
{
    VAConfigAttrib attribute = {
        .type = VAConfigAttribRTFormat,
    };
    const Uint32 formats = VA_RT_FORMAT_YUV444 | VA_RT_FORMAT_YUV444_10 | VA_RT_FORMAT_YUV444_12;

    return vaGetConfigAttributes(display, profile, VAEntrypointVLD, &attribute, 1) ==
               VA_STATUS_SUCCESS &&
           attribute.value != VA_ATTRIB_NOT_SUPPORTED && (attribute.value & formats) != 0;
}

/* Probe VA-API decode capabilities used by the decoder policy. The probe only
 * queries profile metadata and does not allocate decode surfaces. */
bool
vdi_stream_client__parsec_ffmpeg_vaapi_codecs(bool *h264, bool *hevc, bool *hevc444)
{
    AVBufferRef *device_ref = NULL;
    AVHWDeviceContext *device_context;
    AVVAAPIDeviceContext *vaapi_context;
    VAEntrypoint *entrypoints = NULL;
    VAProfile *profiles = NULL;
    VADisplay display;
    Sint32 max_entrypoints;
    Sint32 max_profiles;
    Sint32 profile_count;
    bool available = false;

    if (h264 == NULL || hevc == NULL || hevc444 == NULL) {
        return false;
    }
    *h264 = false;
    *hevc = false;
    *hevc444 = false;

    /* Query profile metadata only. No VA config, context, surface or frame is created. */
    if (av_hwdevice_ctx_create(&device_ref, AV_HWDEVICE_TYPE_VAAPI, NULL, NULL, 0) < 0 ||
        device_ref == NULL) {
        goto cleanup;
    }
    device_context = (AVHWDeviceContext *)device_ref->data;
    vaapi_context = device_context != NULL ? device_context->hwctx : NULL;
    display = vaapi_context != NULL ? vaapi_context->display : NULL;
    if (display == NULL) {
        goto cleanup;
    }

    max_profiles = vaMaxNumProfiles(display);
    max_entrypoints = vaMaxNumEntrypoints(display);
    if (max_profiles <= 0 || max_entrypoints <= 0) {
        goto cleanup;
    }
    profiles = SDL_malloc((size_t)max_profiles * sizeof(*profiles));
    entrypoints = SDL_malloc((size_t)max_entrypoints * sizeof(*entrypoints));
    if (profiles == NULL || entrypoints == NULL) {
        goto cleanup;
    }

    profile_count = max_profiles;
    if (vaQueryConfigProfiles(display, profiles, &profile_count) != VA_STATUS_SUCCESS) {
        goto cleanup;
    }
    available = true;
    for (Sint32 i = 0; i < profile_count; i++) {
        const char *name;

        if (!vdi_stream_client__parsec_ffmpeg_vaapi_profile_decode(
                display, profiles[i], entrypoints, max_entrypoints
            )) {
            continue;
        }
        name = vaProfileStr(profiles[i]);
        if (name == NULL) {
            continue;
        }
        if (SDL_strncmp(name, "VAProfileH264", sizeof("VAProfileH264") - 1) == 0) {
            *h264 = true;
        } else if (SDL_strncmp(name, "VAProfileHEVC", sizeof("VAProfileHEVC") - 1) == 0) {
            *hevc = true;
            if (vdi_stream_client__parsec_ffmpeg_vaapi_profile_hevc444(profiles[i]) &&
                vdi_stream_client__parsec_ffmpeg_vaapi_profile_yuv444(display, profiles[i])) {
                *hevc444 = true;
            }
        }
    }

cleanup:
    SDL_free(entrypoints);
    SDL_free(profiles);
    av_buffer_unref(&device_ref);
    return available;
}

/* Temporarily change page protections so the client can update Parsec SDK
 * decoder table entries and narrow negotiation patches at runtime. */
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

/* Patch the Parsec SDK branch that rejects 4:4:4 negotiation based on its
 * bundled decoder. The injected FFmpeg decoder owns that capability instead. */
static bool
vdi_stream_client__parsec_ffmpeg_patch_decoder444_gate(struct parsec_context_s *parsec_context)
{
    static bool patched;
    const Uint8 pattern[] = {
        0x84, 0xc0,                   /* test al, al */
        0x41, 0x58,                   /* pop r8 */
        0x4c, 0x8b, 0x5c, 0x24, 0x18, /* mov r11, [rsp+0x18] */
        0x0f, 0x85,                   /* jne rel32 */
    };
    Uint8 replacement[6];
    Uint8 *branch;
    Uint8 *func;
    Uint8 *scan;
    int32_t old_relative;
    int32_t new_relative;
    uintptr_t target;
    Uint32 offset;

    if (patched) {
        return true;
    }

#ifdef HAVE_LIBPARSEC
    (void)parsec_context;
    func = (Uint8 *)(void *)ParsecClientConnect;
#else
    if (parsec_context->parsec == NULL || parsec_context->parsec->api.ParsecClientConnect == NULL) {
        return false;
    }
    func = (Uint8 *)(void *)parsec_context->parsec->api.ParsecClientConnect;
#endif

    /* Parsec SDK 6.0 checks its bundled FFmpeg dependency before enabling
     * decoder444. The injected decoder replaces that implementation, so bypass
     * only the result branch of that dependency check. */
    scan = (Uint8 *)((uintptr_t)func - 0x10000u);
    for (offset = 0; offset + sizeof(pattern) + sizeof(old_relative) <= 0x14000u; offset++) {
        if (SDL_memcmp(scan + offset, pattern, sizeof(pattern)) != 0) {
            continue;
        }

        branch = scan + offset + 9u;
        SDL_memcpy(&old_relative, branch + 2u, sizeof(old_relative));
        target = (uintptr_t)(branch + 6u + old_relative);
        new_relative = (int32_t)(target - (uintptr_t)(branch + 5u));
        replacement[0] = 0xe9;
        SDL_memcpy(replacement + 1u, &new_relative, sizeof(new_relative));
        replacement[5] = 0x90;

        if (!vdi_stream_client__parsec_make_writable(
                branch, sizeof(replacement), PROT_READ | PROT_WRITE
            )) {
            return false;
        }
        SDL_memcpy(branch, replacement, sizeof(replacement));
        __builtin___clear_cache((char *)branch, (char *)branch + sizeof(replacement));
        if (!vdi_stream_client__parsec_make_writable(
                branch, sizeof(replacement), PROT_READ | PROT_EXEC
            )) {
            return false;
        }
        patched = true;
        return true;
    }

    return false;
}

/* Set the hidden flag on a raw Parsec decoder table entry so only the injected
 * FFmpeg decoder is visible for client negotiation. */
static bool
vdi_stream_client__parsec_decoder_set_hidden(Uint8 *entry, bool hidden)
{
    Uint8 *value;

    if (entry == NULL) {
        return false;
    }

    value = entry + VDI_STREAM_CLIENT_PARSEC_DECODER_HIDDEN_OFFSET;
    if (*value == (Uint8)hidden) {
        return true;
    }
    if (!vdi_stream_client__parsec_make_writable(value, sizeof(*value), PROT_READ | PROT_WRITE)) {
        return false;
    }
    *value = (Uint8)hidden;
    (void)vdi_stream_client__parsec_make_writable(value, sizeof(*value), PROT_READ);
    return true;
}

/* Locate the in-process Parsec decoder table by scanning ParsecGetDecoders for
 * its RIP-relative table load. */
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
        if (SDL_memcmp(func + offset, pattern, sizeof(pattern)) != 0) {
            continue;
        }

        displacement_raw = (Uint32)func[offset + 3] | ((Uint32)func[offset + 4] << 8) |
                           ((Uint32)func[offset + 5] << 16) | ((Uint32)func[offset + 6] << 24);
        return (Uint8 *)(func + offset + sizeof(pattern) + sizeof(displacement_raw) +
                         (int32_t)displacement_raw);
    }
    return NULL;
}

/* Query Parsec's public decoder list and return the index for the requested
 * decoder name after the table has been patched. */
static bool
vdi_stream_client__parsec_decoder_lookup(
    struct parsec_context_s *parsec_context, const char *name, Uint32 *decoder_index
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
        if (name != NULL && SDL_strncmp(decoders[i].name, name, sizeof(decoders[i].name)) != 0) {
            continue;
        }
        if (decoder_index != NULL) {
            *decoder_index = decoders[i].index;
        }
        return true;
    }

    return false;
}

/* Populate one Parsec decoder query capability blob. H.265 can advertise 4:4:4
 * support, while H.264 remains 4:2:0 only. */
static void
vdi_stream_client__parsec_ffmpeg_query_enable(void *query, bool color444)
{
    Uint8 *bytes = query;

    if (bytes == NULL) {
        return;
    }

    for (size_t i = 0; i < 12; i++) {
        bytes[i] = 0;
    }
    bytes[0] = 1;
    bytes[1] = color444;
}

/* Decoder query callback installed into the Parsec decoder table. It reports
 * the H.264 and H.265 capabilities selected during startup. */
static void
vdi_stream_client__parsec_ffmpeg_query(void *h264, void *h265)
{
    bool color444 =
        atomic_load_explicit(&vdi_stream_client__parsec_ffmpeg_color444, memory_order_relaxed);

    vdi_stream_client__parsec_ffmpeg_query_enable(h264, false);
    vdi_stream_client__parsec_ffmpeg_query_enable(h265, color444);
}

/* Convert an FFmpeg error code into caller-provided storage, falling back to a
 * numeric message if libavutil cannot format it. */
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

/* FFmpeg get_format callback that selects the VA-API hardware pixel format
 * discovered during decoder initialization, falling back to FFmpeg's first
 * offered format if the expected one is absent. */
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

/* Attach a VA-API device to the codec context when the selected codec and
 * runtime policy both support hardware acceleration. */
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

/* Apply low-latency FFmpeg codec settings shared by hardware and software
 * decoder contexts. */
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

/* Log each codec/acceleration mode once so reconnects or multiple decoder
 * instances do not spam identical mode messages. */
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

/* Release all resources owned by one injected FFmpeg decoder instance,
 * including retained frame slots that may still be referenced by descriptors. */
static void
vdi_stream_client__parsec_ffmpeg_free(struct vdi_stream_client__parsec_ffmpeg_decoder_s *ffmpeg)
{
    if (ffmpeg == NULL) {
        return;
    }

    if (ffmpeg->mode_published) {
        atomic_store_explicit(
            &vdi_stream_client__parsec_ffmpeg_hardware_active, false, memory_order_release
        );
    }

    if (ffmpeg->frame_lock != NULL) {
        SDL_LockMutex(ffmpeg->frame_lock);
    }
    for (Uint32 i = 0; i < VDI_STREAM_CLIENT_PARSEC_FFMPEG_FRAME_SLOTS; i++) {
        av_frame_free(&ffmpeg->frame_slots[i].frame);
        ffmpeg->frame_slots[i].generation = 0;
    }
    if (ffmpeg->frame_lock != NULL) {
        SDL_UnlockMutex(ffmpeg->frame_lock);
        SDL_DestroyMutex(ffmpeg->frame_lock);
    }

    av_packet_free(&ffmpeg->packet);
    av_frame_free(&ffmpeg->sw_frame);
    av_frame_free(&ffmpeg->frame);
    avcodec_free_context(&ffmpeg->codec);
    av_buffer_unref(&ffmpeg->hw_device_ctx);
    SDL_free(ffmpeg);
}

/* Common Parsec decoder init callback. It allocates FFmpeg state, selects H.264
 * or H.265 from Parsec's selector byte, tries VA-API first when allowed, and
 * falls back to software decoding if opening the hardware context fails. */
static Sint32
vdi_stream_client__parsec_ffmpeg_init_common(
    void *decoder, void *stream, Uint32 stream_id, void *codec_selector, void *flags
)
{
    struct vdi_stream_client__parsec_ffmpeg_decoder_s *ffmpeg;
    const AVCodec *codec;
    Uint8 selector;
    enum AVCodecID requested_codec_id;
    bool acceleration;
    Sint32 err;
    char errbuf[AV_ERROR_MAX_STRING_SIZE];

    (void)stream;
    (void)stream_id;
    (void)flags;

    if (decoder == NULL) {
        return DECODE_ERR_INIT;
    }

    selector = codec_selector != NULL ? ((const Uint8 *)codec_selector)[0] : 2;
    requested_codec_id = selector == 2 ? AV_CODEC_ID_HEVC : AV_CODEC_ID_H264;

    /* Reuse the decode context built on a previous connect when its codec still
     * matches. This keeps the proven VA-API/UVD context alive across reconnects
     * instead of creating a new one on the aged shared amdgpu device. */
    if (vdi_stream_client__parsec_ffmpeg_decoder_lock != NULL) {
        SDL_LockMutex(vdi_stream_client__parsec_ffmpeg_decoder_lock);
        ffmpeg = vdi_stream_client__parsec_ffmpeg_decoder_cache;
        if (ffmpeg != NULL && ffmpeg->codec_id == requested_codec_id) {
            SDL_UnlockMutex(vdi_stream_client__parsec_ffmpeg_decoder_lock);
            vdi_stream_client__parsec_ffmpeg_reset_frames(ffmpeg);
            if (ffmpeg->codec != NULL) {
                avcodec_flush_buffers(ffmpeg->codec);
            }
            *((void **)decoder) = ffmpeg;
            atomic_store_explicit(
                &vdi_stream_client__parsec_ffmpeg_hardware_active, ffmpeg->hwaccel,
                memory_order_release
            );
            return PARSEC_OK;
        }

        /* A cached decoder for a different codec cannot be reused. */
        if (ffmpeg != NULL) {
            vdi_stream_client__parsec_ffmpeg_free(ffmpeg);
            vdi_stream_client__parsec_ffmpeg_decoder_cache = NULL;
        }
        SDL_UnlockMutex(vdi_stream_client__parsec_ffmpeg_decoder_lock);
    }

    ffmpeg = SDL_calloc(1, sizeof(*ffmpeg));
    if (ffmpeg == NULL) {
        return DECODE_ERR_BUFFER;
    }
    *((void **)decoder) = ffmpeg;

    ffmpeg->frame_lock = SDL_CreateMutex();
    if (ffmpeg->frame_lock == NULL) {
        vdi_stream_client__parsec_ffmpeg_free(ffmpeg);
        *((void **)decoder) = NULL;
        return DECODE_ERR_BUFFER;
    }
    for (Uint32 i = 0; i < VDI_STREAM_CLIENT_PARSEC_FFMPEG_FRAME_SLOTS; i++) {
        ffmpeg->frame_slots[i].lock = ffmpeg->frame_lock;
    }

    ffmpeg->codec_id = requested_codec_id;
    acceleration = atomic_load_explicit(
        ffmpeg->codec_id == AV_CODEC_ID_HEVC ? &vdi_stream_client__parsec_ffmpeg_hevc_acceleration
                                             : &vdi_stream_client__parsec_ffmpeg_h264_acceleration,
        memory_order_relaxed
    );

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
    atomic_store_explicit(
        &vdi_stream_client__parsec_ffmpeg_hardware_active, ffmpeg->hwaccel, memory_order_release
    );
    ffmpeg->mode_published = true;

    /* Retain the freshly built decoder so later reconnects reuse it. */
    if (vdi_stream_client__parsec_ffmpeg_decoder_lock != NULL) {
        SDL_LockMutex(vdi_stream_client__parsec_ffmpeg_decoder_lock);
        vdi_stream_client__parsec_ffmpeg_decoder_cache = ffmpeg;
        SDL_UnlockMutex(vdi_stream_client__parsec_ffmpeg_decoder_lock);
    }
    return PARSEC_OK;
}

/* Parsec decoder init callback installed into the SDK decoder table. It exists
 * as a stable callback symbol and delegates to the shared initializer. */
static Sint32
vdi_stream_client__parsec_ffmpeg_init(
    void *decoder, void *stream, Uint32 stream_id, void *codec_selector, void *flags
)
{
    return vdi_stream_client__parsec_ffmpeg_init_common(
        decoder, stream, stream_id, codec_selector, flags
    );
}

/* Parsec decoder cleanup callback that releases the FFmpeg instance and clears
 * Parsec's decoder-private pointer. */
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

    /* Keep the cached decoder alive so the next reconnect reuses its decode
     * context; only detach it from the SDK slot. Non-cached instances (for
     * example when the cache lock is unavailable) are freed normally. */
    if (vdi_stream_client__parsec_ffmpeg_decoder_lock != NULL) {
        SDL_LockMutex(vdi_stream_client__parsec_ffmpeg_decoder_lock);
        if (ffmpeg == vdi_stream_client__parsec_ffmpeg_decoder_cache) {
            SDL_UnlockMutex(vdi_stream_client__parsec_ffmpeg_decoder_lock);
            *((void **)decoder) = NULL;
            return;
        }
        SDL_UnlockMutex(vdi_stream_client__parsec_ffmpeg_decoder_lock);
    }

    vdi_stream_client__parsec_ffmpeg_free(ffmpeg);
    *((void **)decoder) = NULL;
}

/* Free the reused decode context and its cache lock once the Parsec client and
 * all decoder slots have been torn down. Safe to call when nothing was cached. */
void
vdi_stream_client__parsec_ffmpeg_decoder_destroy(void)
{
    if (vdi_stream_client__parsec_ffmpeg_decoder_lock != NULL) {
        SDL_LockMutex(vdi_stream_client__parsec_ffmpeg_decoder_lock);
        if (vdi_stream_client__parsec_ffmpeg_decoder_cache != NULL) {
            vdi_stream_client__parsec_ffmpeg_free(vdi_stream_client__parsec_ffmpeg_decoder_cache);
            vdi_stream_client__parsec_ffmpeg_decoder_cache = NULL;
        }
        SDL_UnlockMutex(vdi_stream_client__parsec_ffmpeg_decoder_lock);
        SDL_DestroyMutex(vdi_stream_client__parsec_ffmpeg_decoder_lock);
        vdi_stream_client__parsec_ffmpeg_decoder_lock = NULL;
    }
}

/* Copy one AVFrame plane into a tightly packed destination plane, honoring the
 * source and destination pitches for every row. */
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

        SDL_memcpy(dst_row, src_row, (size_t)width);
    }
    return true;
}

/* Write a ParsecFrame header that points at a retained AVFrame descriptor
 * instead of copying pixel planes. The renderer later clones and releases the
 * retained frame through the descriptor slot. */
static Sint32
vdi_stream_client__parsec_ffmpeg_write_frame_descriptor(
    struct vdi_stream_client__parsec_ffmpeg_decoder_s *ffmpeg, const AVFrame *source,
    ParsecFrame *frame, Uint32 *frame_size
)
{
    struct vdi_stream_client__parsec_ffmpeg_frame_descriptor_s *descriptor;
    struct vdi_stream_client__parsec_ffmpeg_frame_slot_s *slot;
    AVFrame *retained;
    ParsecColorFormat parsec_format;
    Uint32 width;
    Uint32 height;
    Uint32 required = (Uint32)sizeof(*frame) + (Uint32)sizeof(*descriptor);
    Uint64 generation;

    if (ffmpeg == NULL || source == NULL || frame == NULL || ffmpeg->frame_lock == NULL) {
        return DECODE_ERR_BUFFER;
    }

    width = (Uint32)source->width;
    height = (Uint32)source->height;
    if (width == 0 || height == 0 || (width & 1u) != 0 || (height & 1u) != 0) {
        return DECODE_ERR_RESOLUTION;
    }
    if (!vdi_stream_client__parsec_ffmpeg_frame_pixel_format(
            vdi_stream_client__parsec_ffmpeg_frame_software_format(source), &parsec_format, NULL
        )) {
        return DECODE_ERR_PIXEL_FORMAT;
    }

    retained = av_frame_clone(source);
    if (retained == NULL) {
        return DECODE_ERR_BUFFER;
    }

    SDL_LockMutex(ffmpeg->frame_lock);
    slot = &ffmpeg->frame_slots[ffmpeg->frame_slot % VDI_STREAM_CLIENT_PARSEC_FFMPEG_FRAME_SLOTS];
    ffmpeg->frame_slot++;
    av_frame_free(&slot->frame);
    slot->frame = retained;
    ffmpeg->frame_generation++;
    if (ffmpeg->frame_generation == 0) {
        ffmpeg->frame_generation++;
    }
    slot->generation = ffmpeg->frame_generation;
    generation = slot->generation;
    SDL_UnlockMutex(ffmpeg->frame_lock);

    frame->format = parsec_format;
    frame->rotation = ROTATION_NONE;
    frame->size = sizeof(*descriptor);
    frame->width = width;
    frame->height = height;
    frame->fullWidth = width;
    frame->fullHeight = height;
    if (frame_size != NULL) {
        *frame_size = required;
    }

    descriptor = (struct vdi_stream_client__parsec_ffmpeg_frame_descriptor_s *)((Uint8 *)frame +
                                                                                sizeof(*frame));
    descriptor->magic = VDI_STREAM_CLIENT_PARSEC_FFMPEG_FRAME_MAGIC;
    descriptor->version = VDI_STREAM_CLIENT_PARSEC_FFMPEG_FRAME_VERSION;
    descriptor->slot = (uintptr_t)slot;
    descriptor->generation = generation;
    return PARSEC_OK;
}

/* Serialize a YUV420P AVFrame into Parsec's contiguous I420 frame buffer when
 * descriptor passing is unavailable or unsuitable. */
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

/* Serialize an NV12 AVFrame into Parsec's contiguous NV12 frame buffer when the
 * renderer cannot consume a retained FFmpeg descriptor directly. */
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

/* Convert the most recently decoded FFmpeg frame into Parsec decoder output.
 * Hardware frames prefer descriptor-based zero-copy handoff, then transfer and
 * fall back to software descriptors or packed buffers as needed. */
static Sint32
vdi_stream_client__parsec_ffmpeg_write_frame(
    struct vdi_stream_client__parsec_ffmpeg_decoder_s *ffmpeg, void *frame_data, Uint32 *frame_size
)
{
    AVFrame *source;
    Sint32 err;
    Uint64 stage_start_ns;
    bool stats_enabled;
    char errbuf[AV_ERROR_MAX_STRING_SIZE];

    if (ffmpeg == NULL || ffmpeg->frame == NULL || ffmpeg->sw_frame == NULL || frame_data == NULL) {
        return DECODE_ERR_BUFFER;
    }

    source = ffmpeg->frame;
    stats_enabled =
        atomic_load_explicit(&vdi_stream_client__parsec_ffmpeg_stats_enabled, memory_order_relaxed);
    if (ffmpeg->hwaccel && ffmpeg->frame->format == ffmpeg->hw_pix_fmt) {
        err = vdi_stream_client__parsec_ffmpeg_write_frame_descriptor(
            ffmpeg, source, (ParsecFrame *)frame_data, frame_size
        );
        if (err == PARSEC_OK) {
            return PARSEC_OK;
        }

        av_frame_unref(ffmpeg->sw_frame);
        err = vdi_stream_client__parsec_ffmpeg_hwframe_transfer(ffmpeg->sw_frame, ffmpeg->frame);
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
        err = vdi_stream_client__parsec_ffmpeg_write_frame_descriptor(
            ffmpeg, source, (ParsecFrame *)frame_data, frame_size
        );
        if (err == PARSEC_OK) {
            return PARSEC_OK;
        }
        stage_start_ns = stats_enabled ? SDL_GetTicksNS() : 0;
        err = vdi_stream_client__parsec_ffmpeg_write_i420(
            source, (ParsecFrame *)frame_data, frame_size
        );
        break;
    case AV_PIX_FMT_NV12:
        err = vdi_stream_client__parsec_ffmpeg_write_frame_descriptor(
            ffmpeg, source, (ParsecFrame *)frame_data, frame_size
        );
        if (err == PARSEC_OK) {
            return PARSEC_OK;
        }
        stage_start_ns = stats_enabled ? SDL_GetTicksNS() : 0;
        err = vdi_stream_client__parsec_ffmpeg_write_nv12(
            source, (ParsecFrame *)frame_data, frame_size
        );
        break;
    default:
        if (vdi_stream_client__parsec_ffmpeg_frame_pixel_format(
                (enum AVPixelFormat)source->format, NULL, NULL
            )) {
            err = vdi_stream_client__parsec_ffmpeg_write_frame_descriptor(
                ffmpeg, source, (ParsecFrame *)frame_data, frame_size
            );
            if (err == PARSEC_OK) {
                return PARSEC_OK;
            }
        }
        SDL_LogWarn(
            SDL_LOG_CATEGORY_APPLICATION, "Unsupported FFmpeg pixel format %d\n", source->format
        );
        return DECODE_ERR_PIXEL_FORMAT;
    }

    if (stats_enabled) {
        atomic_fetch_add_explicit(
            &vdi_stream_client__parsec_ffmpeg_descriptor_fallback_calls, (uint_fast64_t)1,
            memory_order_relaxed
        );
        atomic_fetch_add_explicit(
            &vdi_stream_client__parsec_ffmpeg_descriptor_fallback_ns,
            (uint_fast64_t)(SDL_GetTicksNS() - stage_start_ns), memory_order_relaxed
        );
    }
    return err;
}

/* Wrap avcodec_send_packet with optional timing counters for render statistics. */
static Sint32
vdi_stream_client__parsec_ffmpeg_send_packet(AVCodecContext *codec, const AVPacket *packet)
{
    bool stats_enabled =
        atomic_load_explicit(&vdi_stream_client__parsec_ffmpeg_stats_enabled, memory_order_relaxed);
    Uint64 stage_start_ns = stats_enabled ? SDL_GetTicksNS() : 0;
    Sint32 err = avcodec_send_packet(codec, packet);

    if (stats_enabled) {
        atomic_fetch_add_explicit(
            &vdi_stream_client__parsec_ffmpeg_send_packet_calls, (uint_fast64_t)1,
            memory_order_relaxed
        );
        atomic_fetch_add_explicit(
            &vdi_stream_client__parsec_ffmpeg_send_packet_ns,
            (uint_fast64_t)(SDL_GetTicksNS() - stage_start_ns), memory_order_relaxed
        );
    }
    return err;
}

/* Wrap avcodec_receive_frame with optional timing counters for render
 * statistics. */
static Sint32
vdi_stream_client__parsec_ffmpeg_receive_frame(AVCodecContext *codec, AVFrame *frame)
{
    bool stats_enabled =
        atomic_load_explicit(&vdi_stream_client__parsec_ffmpeg_stats_enabled, memory_order_relaxed);
    Uint64 stage_start_ns = stats_enabled ? SDL_GetTicksNS() : 0;
    Sint32 err = avcodec_receive_frame(codec, frame);

    if (stats_enabled) {
        atomic_fetch_add_explicit(
            &vdi_stream_client__parsec_ffmpeg_receive_frame_calls, (uint_fast64_t)1,
            memory_order_relaxed
        );
        atomic_fetch_add_explicit(
            &vdi_stream_client__parsec_ffmpeg_receive_frame_ns,
            (uint_fast64_t)(SDL_GetTicksNS() - stage_start_ns), memory_order_relaxed
        );
    }
    return err;
}

/* Parsec decoder decode callback. It feeds one compressed packet into FFmpeg,
 * handles EAGAIN/EOF as accepted input, and emits a ParsecFrame when FFmpeg has
 * a decoded frame ready. */
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
    if (atomic_load_explicit(
            &vdi_stream_client__parsec_ffmpeg_stats_enabled, memory_order_relaxed
        )) {
        atomic_fetch_add_explicit(
            &vdi_stream_client__parsec_ffmpeg_video_packet_bytes, (uint_fast64_t)packet_size,
            memory_order_relaxed
        );
    }

    av_packet_unref(ffmpeg->packet);
    ffmpeg->packet->data = (Uint8 *)packet_data;
    ffmpeg->packet->size = (int)packet_size;

    err = vdi_stream_client__parsec_ffmpeg_send_packet(ffmpeg->codec, ffmpeg->packet);
    if (err == AVERROR(EAGAIN)) {
        err = vdi_stream_client__parsec_ffmpeg_receive_frame(ffmpeg->codec, ffmpeg->frame);
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

    err = vdi_stream_client__parsec_ffmpeg_receive_frame(ffmpeg->codec, ffmpeg->frame);
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

/* Install the injected FFmpeg decoder into Parsec's decoder table, hide the SDK
 * software/hardware decoders, publish startup policy for callbacks, and return
 * the decoder index Parsec should request. */
bool
vdi_stream_client__parsec_ffmpeg_decoder_enable(
    struct parsec_context_s *parsec_context, Uint32 *decoder_index, bool h264_acceleration,
    bool hevc_acceleration, bool color444
)
{
    Uint8 *table;
    Uint8 *entry;

    atomic_store_explicit(
        &vdi_stream_client__parsec_ffmpeg_stats_enabled, parsec_context->stats_enabled != 0,
        memory_order_relaxed
    );
    atomic_store_explicit(
        &vdi_stream_client__parsec_ffmpeg_hardware_active, false, memory_order_release
    );
    atomic_store_explicit(
        &vdi_stream_client__parsec_ffmpeg_h264_acceleration, h264_acceleration, memory_order_relaxed
    );
    atomic_store_explicit(
        &vdi_stream_client__parsec_ffmpeg_hevc_acceleration, hevc_acceleration, memory_order_relaxed
    );
    atomic_store_explicit(
        &vdi_stream_client__parsec_ffmpeg_color444, color444, memory_order_relaxed
    );

    if (vdi_stream_client__parsec_ffmpeg_decoder_lock == NULL) {
        vdi_stream_client__parsec_ffmpeg_decoder_lock = SDL_CreateMutex();
    }

    table = vdi_stream_client__parsec_decoder_table(parsec_context);
    if (table == NULL) {
        return false;
    }

    entry = table + VDI_STREAM_CLIENT_PARSEC_FFMPEG_DECODER_INDEX *
                        VDI_STREAM_CLIENT_PARSEC_DECODER_ENTRY_SIZE;
    if (!vdi_stream_client__parsec_decoder_set_hidden(
            table + VDI_STREAM_CLIENT_PARSEC_SOFTWARE_DECODER_INDEX *
                        VDI_STREAM_CLIENT_PARSEC_DECODER_ENTRY_SIZE,
            true
        ) ||
        !vdi_stream_client__parsec_decoder_set_hidden(
            table + VDI_STREAM_CLIENT_PARSEC_HARDWARE_DECODER_INDEX *
                        VDI_STREAM_CLIENT_PARSEC_DECODER_ENTRY_SIZE,
            true
        ) ||
        !vdi_stream_client__parsec_decoder_set_hidden(entry, false)) {
        return false;
    }

    if (!vdi_stream_client__parsec_make_writable(
            entry, VDI_STREAM_CLIENT_PARSEC_DECODER_ENTRY_SIZE, PROT_READ | PROT_WRITE
        )) {
        return false;
    }

    ((uintptr_t *)(void *)(entry + VDI_STREAM_CLIENT_PARSEC_DECODER_INIT_OFFSET))[0] =
        (uintptr_t)(void *)vdi_stream_client__parsec_ffmpeg_init;
    ((uintptr_t *)(void *)(entry + VDI_STREAM_CLIENT_PARSEC_DECODER_DECODE_OFFSET))[0] =
        (uintptr_t)(void *)vdi_stream_client__parsec_ffmpeg_decode;
    ((uintptr_t *)(void *)(entry + VDI_STREAM_CLIENT_PARSEC_DECODER_CLEANUP_OFFSET))[0] =
        (uintptr_t)(void *)vdi_stream_client__parsec_ffmpeg_cleanup;
    ((uintptr_t *)(void *)(entry + VDI_STREAM_CLIENT_PARSEC_DECODER_QUERY_OFFSET))[0] =
        (uintptr_t)(void *)vdi_stream_client__parsec_ffmpeg_query;
    (void)vdi_stream_client__parsec_make_writable(
        entry, VDI_STREAM_CLIENT_PARSEC_DECODER_ENTRY_SIZE, PROT_READ
    );

    if (color444 && !vdi_stream_client__parsec_ffmpeg_patch_decoder444_gate(parsec_context)) {
        SDL_LogWarn(
            SDL_LOG_CATEGORY_APPLICATION,
            "Parsec SDK 4:4:4 negotiation gate patch failed; use 4:2:0 color\n"
        );
        atomic_store_explicit(
            &vdi_stream_client__parsec_ffmpeg_color444, false, memory_order_relaxed
        );
    }

    return vdi_stream_client__parsec_decoder_lookup(parsec_context, "FFMPEG", decoder_index);
}
