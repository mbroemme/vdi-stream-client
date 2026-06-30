/*
 *  placebo.c -- VA-API zero-copy rendering via libplacebo
 *
 *  Copyright (c) 2026 Maik Broemme <mbroemme@libmpq.org>
 *
 *  This program is free software: you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License as published by
 *  the Free Software Foundation, either version 3 of the License, or
 *  (at your option) any later version.
 *
 *  Additional permission under GNU GPL version 3 section 7 is described in
 *  COPYING.EXCEPTION, allowing this program to link with the Parsec SDK.
 */

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include "ffmpeg.h"
#include "placebo.h"

#include <SDL3/SDL_vulkan.h>
#include <errno.h>
#include <libavutil/common.h>
#include <libavutil/error.h>
#include <libavutil/frame.h>
#include <libavutil/hwcontext.h>
#include <libavutil/hwcontext_drm.h>
#include <libavutil/hwcontext_vaapi.h>
#include <libavutil/pixdesc.h>
#include <libavutil/pixfmt.h>
#include <libdrm/drm_fourcc.h>
#include <libplacebo/log.h>
#include <libplacebo/renderer.h>
#include <libplacebo/utils/libav.h>
#include <libplacebo/utils/upload.h>
#include <libplacebo/vulkan.h>
#include <unistd.h>
#include <va/va.h>
#include <vulkan/vulkan.h>

#define VDI_STREAM_CLIENT_PLACEBO_VAAPI_SYNC_RETRIES 5u

struct vdi_stream_client__placebo_s
{
    pl_log log;
    pl_vk_inst instance;
    VkSurfaceKHR surface;
    pl_vulkan vulkan;
    pl_renderer renderer;
    pl_tex target;
    SDL_Texture *texture;
    VkSemaphore ready;
    Uint64 ready_value;
    Sint32 width;
    Sint32 height;
    char import_failure[256];
    bool target_held;
    bool linear_import;
    bool direct_disabled;
    bool direct_logged;
    bool upload_logged;
};

struct vdi_stream_client__placebo_source_s
{
    struct pl_frame frame;
    pl_tex textures[4];
    pl_tex planar_texture;
    VkImage images[4];
    VkDeviceMemory memories[4];
    AVFrame *drm_frame;
    bool mapped_avframe;
};

/* Forward libplacebo warnings and errors into SDL logging while suppressing
 * lower-priority chatter from the rendering library. */
static void
vdi_stream_client__placebo_log(void *opaque, enum pl_log_level level, const char *message)
{
    (void)opaque;

    if (level <= PL_LOG_ERR) {
        SDL_LogError(SDL_LOG_CATEGORY_APPLICATION, "libplacebo: %s\n", message);
    } else if (level == PL_LOG_WARN) {
        SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION, "libplacebo: %s\n", message);
    }
}

/* Wait for the timeline semaphore value signaled when libplacebo finishes using
 * the shared target texture before SDL samples from it. */
static bool
vdi_stream_client__placebo_wait_ready(struct vdi_stream_client__placebo_s *placebo)
{
    VkSemaphoreWaitInfo wait_info = {
        .sType = VK_STRUCTURE_TYPE_SEMAPHORE_WAIT_INFO,
        .semaphoreCount = 1,
        .pSemaphores = &placebo->ready,
        .pValues = &placebo->ready_value,
    };

    return vkWaitSemaphores(placebo->vulkan->device, &wait_info, UINT64_MAX) == VK_SUCCESS;
}

/* Hold the libplacebo render target for SDL sampling. This transitions ownership
 * with libplacebo and waits until the target is ready to be wrapped by SDL. */
static bool
vdi_stream_client__placebo_hold_target(struct vdi_stream_client__placebo_s *placebo)
{
    if (placebo->target_held) {
        return true;
    }
    if (placebo->vulkan == NULL || placebo->target == NULL || placebo->ready == VK_NULL_HANDLE) {
        return false;
    }

    placebo->ready_value++;
    if (!pl_vulkan_hold_ex(
            placebo->vulkan->gpu,
            pl_vulkan_hold_params(
                    .tex = placebo->target, .layout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                    .qf = VK_QUEUE_FAMILY_IGNORED,
                    .semaphore = { .sem = placebo->ready, .value = placebo->ready_value }
            )
        ) ||
        !vdi_stream_client__placebo_wait_ready(placebo)) {
        return false;
    }

    placebo->target_held = true;
    return true;
}

/* Release the shared target back to libplacebo before the next render pass. SDL
 * and libplacebo share the main-thread Vulkan queue, so ordering is preserved. */
static void
vdi_stream_client__placebo_release_target(struct vdi_stream_client__placebo_s *placebo)
{
    if (!placebo->target_held) {
        return;
    }

    if (placebo->vulkan != NULL && placebo->target != NULL) {

        /* SDL_RenderPresent submitted the previous sample on this same
         * main-thread graphics queue, so the next libplacebo submission is
         * ordered after it. The hold path uses a timeline semaphore before
         * handing the image back to SDL. */
        pl_vulkan_release_ex(
            placebo->vulkan->gpu,
            pl_vulkan_release_params(
                    .tex = placebo->target, .layout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                    .qf = VK_QUEUE_FAMILY_IGNORED
            )
        );
    }
    placebo->target_held = false;
}

/* Destroy the SDL-wrapped libplacebo target texture and clear any active video
 * frame reference to it before dimensions or render paths change. */
static void
vdi_stream_client__placebo_target_destroy(
    struct parsec_context_s *parsec_context, struct vdi_stream_client__placebo_s *placebo
)
{
    if (parsec_context->frame_video_texture == placebo->texture) {
        parsec_context->frame_video_texture = NULL;
    }

    vdi_stream_client__placebo_release_target(placebo);
    if (placebo->vulkan != NULL) {

        /* SDL and libplacebo submit to the same Vulkan graphics queue on the
         * main thread. This waits for prior SDL sampling before destruction. */
        pl_gpu_finish(placebo->vulkan->gpu);
    }
    SDL_DestroyTexture(placebo->texture);
    placebo->texture = NULL;
    if (placebo->vulkan != NULL) {
        pl_tex_destroy(placebo->vulkan->gpu, &placebo->target);
    }
    placebo->width = 0;
    placebo->height = 0;
}

/* Tear down all temporary textures, imported Vulkan images, memory objects, and
 * AVFrame references created while mapping one VA-API frame. */
static void
vdi_stream_client__placebo_source_unmap(
    struct vdi_stream_client__placebo_s *placebo, struct vdi_stream_client__placebo_source_s *source
)
{
    if (source->mapped_avframe) {
        pl_unmap_avframe(placebo->vulkan->gpu, &source->frame);
    }
    if (source->planar_texture != NULL) {
        pl_tex_destroy(placebo->vulkan->gpu, &source->planar_texture);
        SDL_memset(source->textures, 0, sizeof(source->textures));
    }
    for (size_t i = 0; i < 4; i++) {
        pl_tex_destroy(placebo->vulkan->gpu, &source->textures[i]);
        if (source->images[i] != VK_NULL_HANDLE) {
            vkDestroyImage(placebo->vulkan->device, source->images[i], NULL);
        }
        if (source->memories[i] != VK_NULL_HANDLE) {
            vkFreeMemory(placebo->vulkan->device, source->memories[i], NULL);
        }
    }
    av_frame_free(&source->drm_frame);
    SDL_memset(source, 0, sizeof(*source));
}

/* Detect Mesa RADV on AMD hardware so the renderer can enable the legacy linear
 * external-memory path used by older VA-API exports without DRM modifiers. */
static bool
vdi_stream_client__placebo_is_radv(struct vdi_stream_client__placebo_s *placebo)
{
    VkPhysicalDeviceDriverProperties driver_properties = {
        .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DRIVER_PROPERTIES,
    };
    VkPhysicalDeviceProperties2 properties = {
        .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2,
        .pNext = &driver_properties,
    };

    vkGetPhysicalDeviceProperties2(placebo->vulkan->phys_device, &properties);
    return properties.properties.vendorID == 0x1002 &&
           driver_properties.driverID == VK_DRIVER_ID_MESA_RADV;
}

/* Validate a VA-API frame and, on Intel iHD, pre-sync the retained surface
 * before FFmpeg exports it as DRM PRIME. Other drivers continue through the
 * normal FFmpeg mapping path, while transient Intel iHD sync failures are
 * retried here before falling back to upload. */
static bool
vdi_stream_client__placebo_vaapi_sync(const AVFrame *av_frame)
{
    const AVHWFramesContext *frames_context;
    const AVVAAPIDeviceContext *device_context;
    const char *vendor;
    VASurfaceID surface;
    VAStatus status;

    if (av_frame == NULL || av_frame->format != AV_PIX_FMT_VAAPI ||
        av_frame->hw_frames_ctx == NULL) {
        return false;
    }
    frames_context = (const AVHWFramesContext *)av_frame->hw_frames_ctx->data;
    if (frames_context == NULL || frames_context->device_ctx == NULL ||
        frames_context->device_ctx->type != AV_HWDEVICE_TYPE_VAAPI) {
        return false;
    }
    device_context = (const AVVAAPIDeviceContext *)frames_context->device_ctx->hwctx;
    if (device_context == NULL || device_context->display == NULL) {
        return false;
    }
    vendor = vaQueryVendorString(device_context->display);
    if (vendor == NULL || SDL_strstr(vendor, "Intel iHD driver") == NULL) {
        return true;
    }

    surface = (VASurfaceID)(uintptr_t)av_frame->data[3];
    if (surface == VA_INVALID_SURFACE) {
        return false;
    }

    /* Intel iHD status reports can briefly lag the surface fence after a
     * Wayland output power cycle. Retry only that specific synchronization
     * failure before asking FFmpeg to export the same surface again. */
    for (Uint32 attempt = 0; attempt < VDI_STREAM_CLIENT_PLACEBO_VAAPI_SYNC_RETRIES; attempt++) {
        status = vaSyncSurface(device_context->display, surface);
        if (status == VA_STATUS_SUCCESS) {
            return true;
        }
        if (status != VA_STATUS_ERROR_OPERATION_FAILED) {
            return false;
        }
        if (attempt + 1 < VDI_STREAM_CLIENT_PLACEBO_VAAPI_SYNC_RETRIES) {
            SDL_Delay(1);
        }
    }
    return false;
}

/* Pick a Vulkan memory type compatible with an imported external image,
 * preferring device-local memory and falling back to the first allowed type. */
static bool
vdi_stream_client__placebo_memory_type(
    struct vdi_stream_client__placebo_s *placebo, Uint32 memory_type_bits, Uint32 *memory_type_index
)
{
    VkPhysicalDeviceMemoryProperties properties;
    Sint32 fallback = -1;

    vkGetPhysicalDeviceMemoryProperties(placebo->vulkan->phys_device, &properties);
    for (Uint32 i = 0; i < properties.memoryTypeCount; i++) {
        if ((memory_type_bits & (1U << i)) == 0) {
            continue;
        }
        if ((properties.memoryTypes[i].propertyFlags & VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT) != 0) {
            *memory_type_index = i;
            return true;
        }
        if (fallback < 0) {
            fallback = (Sint32)i;
        }
    }
    if (fallback < 0) {
        return false;
    }
    *memory_type_index = (Uint32)fallback;
    return true;
}

/* Import a contiguous linear NV12 DMA-BUF as a single two-plane Vulkan image for
 * RADV systems that lack DRM modifier import but accept opaque FD memory. */
static bool
vdi_stream_client__placebo_source_import_linear(
    struct vdi_stream_client__placebo_s *placebo,
    struct vdi_stream_client__placebo_source_s *source, const AVFrame *av_frame,
    const AVDRMPlaneDescriptor *const drm_planes[2], const AVDRMObjectDescriptor *drm_object,
    size_t object_size
)
{
    const VkFormat vulkan_format = VK_FORMAT_G8_B8R8_2PLANE_420_UNORM;
    const Uint32 allocation_width = (Uint32)FFALIGN(av_frame->width, 16);
    const Uint32 allocation_height = (Uint32)FFALIGN(av_frame->height, 16);

    /* RADV advertises linear external images through opaque FD handles. */
    const VkExternalMemoryHandleTypeFlagBits handle_type =
        VK_EXTERNAL_MEMORY_HANDLE_TYPE_OPAQUE_FD_BIT;
    const VkImageCreateFlags image_flags =
        VK_IMAGE_CREATE_MUTABLE_FORMAT_BIT | VK_IMAGE_CREATE_EXTENDED_USAGE_BIT;
    VkPhysicalDeviceExternalImageFormatInfo external_info = {
        .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_EXTERNAL_IMAGE_FORMAT_INFO,
        .handleType = handle_type,
    };
    VkPhysicalDeviceImageFormatInfo2 format_info = {
        .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_IMAGE_FORMAT_INFO_2,
        .pNext = &external_info,
        .format = vulkan_format,
        .type = VK_IMAGE_TYPE_2D,
        .tiling = VK_IMAGE_TILING_LINEAR,
        .usage = VK_IMAGE_USAGE_SAMPLED_BIT,
        .flags = image_flags,
    };
    VkExternalImageFormatProperties external_properties = {
        .sType = VK_STRUCTURE_TYPE_EXTERNAL_IMAGE_FORMAT_PROPERTIES,
    };
    VkImageFormatProperties2 format_properties = {
        .sType = VK_STRUCTURE_TYPE_IMAGE_FORMAT_PROPERTIES_2,
        .pNext = &external_properties,
    };
    VkExternalMemoryImageCreateInfo external_create_info = {
        .sType = VK_STRUCTURE_TYPE_EXTERNAL_MEMORY_IMAGE_CREATE_INFO,
        .handleTypes = handle_type,
    };
    VkImageCreateInfo image_info = {
        .sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO,
        .pNext = &external_create_info,
        .imageType = VK_IMAGE_TYPE_2D,
        .format = vulkan_format,
        .extent = { .width = allocation_width, .height = allocation_height, .depth = 1 },
        .mipLevels = 1,
        .arrayLayers = 1,
        .samples = VK_SAMPLE_COUNT_1_BIT,
        .tiling = VK_IMAGE_TILING_LINEAR,
        .usage = VK_IMAGE_USAGE_SAMPLED_BIT,
        .flags = image_flags,
        .sharingMode = VK_SHARING_MODE_EXCLUSIVE,
        .initialLayout = VK_IMAGE_LAYOUT_PREINITIALIZED,
    };
    VkMemoryDedicatedAllocateInfo dedicated_info = {
        .sType = VK_STRUCTURE_TYPE_MEMORY_DEDICATED_ALLOCATE_INFO,
    };
    VkImportMemoryFdInfoKHR import_info = {
        .sType = VK_STRUCTURE_TYPE_IMPORT_MEMORY_FD_INFO_KHR,
        .pNext = &dedicated_info,
        .handleType = handle_type,
        .fd = -1,
    };
    VkMemoryAllocateInfo memory_info = {
        .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
        .pNext = &import_info,
    };
    VkMemoryRequirements requirements;
    Uint32 memory_type_index;
    int imported_fd;
    VkResult result;

    if (object_size == 0) {
        SDL_snprintf(
            placebo->import_failure, sizeof(placebo->import_failure), "NV12 DMA-BUF is empty"
        );
        return false;
    }
    result = vkGetPhysicalDeviceImageFormatProperties2(
        placebo->vulkan->phys_device, &format_info, &format_properties
    );
    if (result != VK_SUCCESS ||
        (external_properties.externalMemoryProperties.externalMemoryFeatures &
         VK_EXTERNAL_MEMORY_FEATURE_IMPORTABLE_BIT) == 0) {
        SDL_snprintf(
            placebo->import_failure, sizeof(placebo->import_failure),
            "NV12 linear opaque-FD image support failed (%d, features 0x%x)", result,
            external_properties.externalMemoryProperties.externalMemoryFeatures
        );
        return false;
    }

    result = vkCreateImage(placebo->vulkan->device, &image_info, NULL, &source->images[0]);
    if (result != VK_SUCCESS) {
        SDL_snprintf(
            placebo->import_failure, sizeof(placebo->import_failure),
            "NV12 Vulkan image creation failed (%d)", result
        );
        return false;
    }
    vkGetImageMemoryRequirements(placebo->vulkan->device, source->images[0], &requirements);
    if (!vdi_stream_client__placebo_memory_type(
            placebo, requirements.memoryTypeBits, &memory_type_index
        )) {
        SDL_snprintf(
            placebo->import_failure, sizeof(placebo->import_failure),
            "NV12 image has no compatible Vulkan memory type"
        );
        return false;
    }
    if (requirements.size > object_size) {
        SDL_snprintf(
            placebo->import_failure, sizeof(placebo->import_failure),
            "NV12 Vulkan image size %llu exceeds DMA-BUF size %llu",
            (unsigned long long)requirements.size, (unsigned long long)object_size
        );
        return false;
    }
    for (int i = 0; i < 2; i++) {
        VkImageSubresource subresource = {
            .aspectMask = VK_IMAGE_ASPECT_PLANE_0_BIT << i,
        };
        VkSubresourceLayout layout;
        VkDeviceSize plane_end;

        vkGetImageSubresourceLayout(
            placebo->vulkan->device, source->images[0], &subresource, &layout
        );
        if (layout.rowPitch != (VkDeviceSize)drm_planes[i]->pitch) {
            SDL_snprintf(
                placebo->import_failure, sizeof(placebo->import_failure),
                "plane %d row pitch differs (VA-API %llu, RADV %llu)", i,
                (unsigned long long)drm_planes[i]->pitch, (unsigned long long)layout.rowPitch
            );
            return false;
        }
        if (layout.offset != (VkDeviceSize)drm_planes[i]->offset) {
            SDL_snprintf(
                placebo->import_failure, sizeof(placebo->import_failure),
                "plane %d offset differs (VA-API %llu, RADV %llu)", i,
                (unsigned long long)drm_planes[i]->offset, (unsigned long long)layout.offset
            );
            return false;
        }
        plane_end = layout.offset + layout.size;
        if (plane_end < layout.offset || plane_end > object_size) {
            SDL_snprintf(
                placebo->import_failure, sizeof(placebo->import_failure),
                "plane %d range %llu+%llu exceeds DMA-BUF size %llu", i,
                (unsigned long long)layout.offset, (unsigned long long)layout.size,
                (unsigned long long)object_size
            );
            return false;
        }
    }

    imported_fd = dup(drm_object->fd);
    if (imported_fd < 0) {
        SDL_snprintf(
            placebo->import_failure, sizeof(placebo->import_failure),
            "NV12 DMA-BUF duplication failed"
        );
        return false;
    }

    /* A two-plane image deliberately bypasses RADV's single-plane RadeonSI
     * metadata compatibility hook. Matching Mesa's 16-pixel video allocation
     * dimensions makes RADV calculate the same contiguous legacy NV12 layout
     * without applying whole-BO metadata to each plane independently. */
    dedicated_info.image = source->images[0];
    import_info.fd = imported_fd;
    memory_info.allocationSize = object_size;
    memory_info.memoryTypeIndex = memory_type_index;
    result = vkAllocateMemory(placebo->vulkan->device, &memory_info, NULL, &source->memories[0]);
    if (result != VK_SUCCESS) {
        close(imported_fd);
        SDL_snprintf(
            placebo->import_failure, sizeof(placebo->import_failure),
            "NV12 RadeonSI memory import failed (%d)", result
        );
        return false;
    }

    if (vkBindImageMemory(placebo->vulkan->device, source->images[0], source->memories[0], 0) !=
        VK_SUCCESS) {
        SDL_snprintf(
            placebo->import_failure, sizeof(placebo->import_failure),
            "NV12 Vulkan image binding failed"
        );
        return false;
    }

    source->planar_texture = pl_vulkan_wrap(
        placebo->vulkan->gpu, pl_vulkan_wrap_params(
                                      .image = source->images[0], .width = (int)allocation_width,
                                      .height = (int)allocation_height, .format = vulkan_format,
                                      .usage = VK_IMAGE_USAGE_SAMPLED_BIT
                              )
    );
    if (source->planar_texture == NULL || source->planar_texture->planes[0] == NULL ||
        source->planar_texture->planes[1] == NULL) {
        SDL_snprintf(
            placebo->import_failure, sizeof(placebo->import_failure),
            "NV12 libplacebo image wrapping failed"
        );
        return false;
    }
    source->textures[0] = source->planar_texture->planes[0];
    source->textures[1] = source->planar_texture->planes[1];
    return true;
}

/* Map a VA-API AVFrame to DRM PRIME, import each DMA-BUF plane into Vulkan, and
 * populate a libplacebo frame that can be rendered without copying through CPU
 * memory. */
static bool
vdi_stream_client__placebo_source_map(
    struct vdi_stream_client__placebo_s *placebo, const AVFrame *av_frame,
    struct vdi_stream_client__placebo_source_s *source
)
{
    const AVHWFramesContext *frames_context;
    const AVPixFmtDescriptor *pixel_descriptor;
    const AVDRMFrameDescriptor *drm_descriptor;
    const AVDRMPlaneDescriptor *drm_planes[4] = { NULL };
    const AVDRMObjectDescriptor *drm_objects[4] = { NULL };
    struct pl_plane_data plane_data[4] = { 0 };
    pl_fmt formats[4] = { NULL };
    size_t object_sizes[4] = { 0 };
    int component_mappings[4][4] = { 0 };
    int plane_count;
    int descriptor_plane_count = 0;
    int layer = 0;
    int layer_plane = 0;
    int err;

    SDL_strlcpy(
        placebo->import_failure, "invalid VA-API DRM PRIME frame", sizeof(placebo->import_failure)
    );
    if (av_frame == NULL || av_frame->format != AV_PIX_FMT_VAAPI ||
        av_frame->hw_frames_ctx == NULL) {
        SDL_strlcpy(
            placebo->import_failure, "missing VA-API frame context", sizeof(placebo->import_failure)
        );
        return false;
    }

    frames_context = (const AVHWFramesContext *)av_frame->hw_frames_ctx->data;
    if (frames_context == NULL) {
        SDL_strlcpy(
            placebo->import_failure, "missing VA-API hardware frames context",
            sizeof(placebo->import_failure)
        );
        return false;
    }
    pixel_descriptor = av_pix_fmt_desc_get(frames_context->sw_format);
    if (pixel_descriptor == NULL) {
        SDL_snprintf(
            placebo->import_failure, sizeof(placebo->import_failure),
            "unsupported VA-API software format %d", frames_context->sw_format
        );
        return false;
    }

    source->drm_frame = av_frame_alloc();
    if (source->drm_frame == NULL) {
        SDL_strlcpy(
            placebo->import_failure, "DRM PRIME frame allocation failed",
            sizeof(placebo->import_failure)
        );
        return false;
    }
    source->drm_frame->format = AV_PIX_FMT_DRM_PRIME;
    source->drm_frame->width = av_frame->width;
    source->drm_frame->height = av_frame->height;
    source->drm_frame->hw_frames_ctx = av_buffer_ref(av_frame->hw_frames_ctx);
    if (source->drm_frame->hw_frames_ctx == NULL) {
        SDL_strlcpy(
            placebo->import_failure, "VA-API frame context reference failed",
            sizeof(placebo->import_failure)
        );
        goto error;
    }

    if (vdi_stream_client__placebo_vaapi_sync(av_frame)) {
        err = av_hwframe_map(
            source->drm_frame, av_frame, AV_HWFRAME_MAP_READ | AV_HWFRAME_MAP_DIRECT
        );
    } else {
        err = AVERROR(EIO);
    }
    if (err < 0) {
        SDL_snprintf(
            placebo->import_failure, sizeof(placebo->import_failure),
            "VA-API to DRM PRIME mapping failed (%d)", err
        );
        goto error;
    }
    drm_descriptor = (const AVDRMFrameDescriptor *)source->drm_frame->data[0];
    if (drm_descriptor == NULL || drm_descriptor->nb_layers <= 0 ||
        drm_descriptor->nb_objects <= 0) {
        SDL_strlcpy(
            placebo->import_failure, "VA-API returned an empty DRM PRIME descriptor",
            sizeof(placebo->import_failure)
        );
        goto error;
    }

    pl_frame_from_avframe(&source->frame, av_frame);
    plane_count =
        pl_plane_data_from_pixfmt(plane_data, &source->frame.repr.bits, frames_context->sw_format);
    if (plane_count <= 0 || plane_count != source->frame.num_planes) {
        SDL_snprintf(
            placebo->import_failure, sizeof(placebo->import_failure),
            "software plane count differs (format %d, libplacebo %d)", plane_count,
            source->frame.num_planes
        );
        goto error;
    }

    for (int i = 0; i < drm_descriptor->nb_layers; i++) {
        descriptor_plane_count += drm_descriptor->layers[i].nb_planes;
    }
    if (descriptor_plane_count != plane_count) {
        SDL_snprintf(
            placebo->import_failure, sizeof(placebo->import_failure),
            "DRM PRIME plane count differs (VA-API %d, format %d)", descriptor_plane_count,
            plane_count
        );
        goto error;
    }

    for (int i = 0; i < plane_count; i++) {
        const AVDRMLayerDescriptor *drm_layer;
        const AVDRMPlaneDescriptor *drm_plane;
        const AVDRMObjectDescriptor *drm_object;
        struct pl_tex_params texture_params;
        size_t object_size;
        bool is_chroma = i == 1 || i == 2;

        while (layer < drm_descriptor->nb_layers &&
               layer_plane >= drm_descriptor->layers[layer].nb_planes) {
            layer++;
            layer_plane = 0;
        }
        if (layer >= drm_descriptor->nb_layers) {
            SDL_snprintf(
                placebo->import_failure, sizeof(placebo->import_failure),
                "DRM PRIME plane %d has no matching layer", i
            );
            goto error;
        }

        drm_layer = &drm_descriptor->layers[layer];
        drm_plane = &drm_layer->planes[layer_plane++];
        drm_planes[i] = drm_plane;
        if (drm_plane->object_index < 0 || drm_plane->object_index >= drm_descriptor->nb_objects ||
            drm_plane->pitch <= 0 || drm_plane->offset < 0) {
            SDL_snprintf(
                placebo->import_failure, sizeof(placebo->import_failure),
                "DRM PRIME plane %d has invalid object, pitch, or offset", i
            );
            goto error;
        }
        drm_object = &drm_descriptor->objects[drm_plane->object_index];
        drm_objects[i] = drm_object;
        if (drm_object->fd < 0) {
            SDL_snprintf(
                placebo->import_failure, sizeof(placebo->import_failure),
                "DRM PRIME plane %d has no DMA-BUF file descriptor", i
            );
            goto error;
        }

        plane_data[i].width =
            AV_CEIL_RSHIFT(av_frame->width, is_chroma ? pixel_descriptor->log2_chroma_w : 0);
        plane_data[i].height =
            AV_CEIL_RSHIFT(av_frame->height, is_chroma ? pixel_descriptor->log2_chroma_h : 0);
        plane_data[i].row_stride = drm_plane->pitch;

        /* NVIDIA exports the NV12 chroma plane as DRM_FORMAT_RG88. Derive the
         * texture format from the decoded software layout so its component
         * order and modifier capabilities match Vulkan, as mpv's interop does. */
        formats[i] = pl_plane_find_fmt(placebo->vulkan->gpu, component_mappings[i], &plane_data[i]);
        if (formats[i] == NULL) {
            SDL_snprintf(
                placebo->import_failure, sizeof(placebo->import_failure),
                "DRM PRIME plane %d has no compatible Vulkan format", i
            );
            goto error;
        }

        object_size = drm_object->size;
        if (object_size == 0) {
            off_t end = lseek(drm_object->fd, 0, SEEK_END);

            if (end <= 0 || lseek(drm_object->fd, 0, SEEK_SET) < 0) {
                SDL_snprintf(
                    placebo->import_failure, sizeof(placebo->import_failure),
                    "DRM PRIME object %d size query failed", drm_plane->object_index
                );
                goto error;
            }
            object_size = (size_t)end;
        }
        object_sizes[i] = object_size;

        if (placebo->linear_import) {
            if (drm_object->format_modifier != DRM_FORMAT_MOD_LINEAR &&
                drm_object->format_modifier != DRM_FORMAT_MOD_INVALID) {
                SDL_snprintf(
                    placebo->import_failure, sizeof(placebo->import_failure),
                    "DRM PRIME plane %d has unsupported modifier 0x%016llx", i,
                    (unsigned long long)drm_object->format_modifier
                );
                goto error;
            }
        } else {
            texture_params = (struct pl_tex_params) {
                .w = plane_data[i].width,
                .h = plane_data[i].height,
                .format = formats[i],
                .sampleable = true,
                .blit_src = (formats[i]->caps & PL_FMT_CAP_BLITTABLE) != 0,
                .import_handle = PL_HANDLE_DMA_BUF,
                .shared_mem = {
                    .handle.fd = drm_object->fd,
                    .size = object_size,
                    .offset = drm_plane->offset,
                    .drm_format_mod = drm_object->format_modifier,
                    .stride_w = drm_plane->pitch,
                },
            };
            source->textures[i] = pl_tex_create(placebo->vulkan->gpu, &texture_params);
        }
        if (!placebo->linear_import && source->textures[i] == NULL) {
            goto error;
        }
    }

    if (placebo->linear_import) {
        if (frames_context->sw_format != AV_PIX_FMT_NV12 || plane_count != 2 ||
            drm_planes[0]->object_index != drm_planes[1]->object_index ||
            drm_objects[0] != drm_objects[1] || object_sizes[0] != object_sizes[1]) {
            SDL_strlcpy(
                placebo->import_failure, "RADV linear import requires one contiguous NV12 DMA-BUF",
                sizeof(placebo->import_failure)
            );
            goto error;
        }
        if (!vdi_stream_client__placebo_source_import_linear(
                placebo, source, av_frame, drm_planes, drm_objects[0], object_sizes[0]
            )) {
            goto error;
        }
    }

    for (int i = 0; i < plane_count; i++) {
        struct pl_plane *plane = &source->frame.planes[i];

        plane->texture = source->textures[i];
        plane->components = 0;
        for (size_t component = 0; component < 4; component++) {
            plane->component_mapping[component] = component_mappings[i][component];
            if (component_mappings[i][component] >= 0) {
                plane->components = (int)component + 1;
            }
        }
    }

    if (placebo->linear_import) {
        pl_vulkan_release_ex(
            placebo->vulkan->gpu,
            pl_vulkan_release_params(
                    .tex = source->planar_texture, .layout = VK_IMAGE_LAYOUT_PREINITIALIZED,
                    .qf = VK_QUEUE_FAMILY_IGNORED
            )
        );
    }
    return true;

error:
    vdi_stream_client__placebo_source_unmap(placebo, source);
    return false;
}

/* Fallback path for VA-API frames that cannot be imported directly. It transfers
 * the hardware frame to a software AVFrame and lets libplacebo upload it to
 * temporary GPU textures. */
static bool
vdi_stream_client__placebo_source_upload(
    struct vdi_stream_client__placebo_s *placebo, const AVFrame *av_frame,
    struct vdi_stream_client__placebo_source_s *source
)
{
    AVFrame *sw_frame = av_frame_alloc();
    Sint32 err;

    if (sw_frame == NULL) {
        SDL_strlcpy(
            placebo->import_failure, "software AVFrame allocation failed",
            sizeof(placebo->import_failure)
        );
        return false;
    }
    err = vdi_stream_client__parsec_ffmpeg_hwframe_transfer(sw_frame, av_frame);
    if (err < 0) {
        SDL_snprintf(
            placebo->import_failure, sizeof(placebo->import_failure),
            "FFmpeg hardware frame transfer failed (%d)", err
        );
        av_frame_free(&sw_frame);
        return false;
    }
    if (!pl_map_avframe_ex(
            placebo->vulkan->gpu, &source->frame,
            pl_avframe_params(.frame = sw_frame, .tex = source->textures)
        )) {
        SDL_strlcpy(
            placebo->import_failure, "libplacebo AVFrame upload failed",
            sizeof(placebo->import_failure)
        );
        av_frame_free(&sw_frame);
        for (size_t i = 0; i < 4; i++) {
            pl_tex_destroy(placebo->vulkan->gpu, &source->textures[i]);
        }
        SDL_memset(source, 0, sizeof(*source));
        return false;
    }
    source->mapped_avframe = true;
    av_frame_free(&sw_frame);
    return true;
}

/* Ensure the libplacebo render target and SDL texture wrapper exist for the
 * current frame size. The created texture is later sampled by SDL's renderer. */
static bool
vdi_stream_client__placebo_target_create(
    struct parsec_context_s *parsec_context, struct vdi_stream_client__placebo_s *placebo,
    Sint32 width, Sint32 height
)
{
    SDL_PropertiesID props;
    VkImage image;
    VkFormat format;
    VkImageUsageFlags usage;
    pl_fmt rgba;
    bool configured;

    if (placebo->target != NULL && placebo->texture != NULL && placebo->width == width &&
        placebo->height == height) {
        return true;
    }

    vdi_stream_client__placebo_target_destroy(parsec_context, placebo);
    rgba = pl_find_named_fmt(placebo->vulkan->gpu, "rgba8");
    if (rgba == NULL || (rgba->caps & PL_FMT_CAP_RENDERABLE) == 0 ||
        (rgba->caps & PL_FMT_CAP_SAMPLEABLE) == 0) {
        return false;
    }

    placebo->target = pl_tex_create(
        placebo->vulkan->gpu,
        pl_tex_params(
                .w = width, .h = height, .format = rgba, .sampleable = true, .renderable = true
        )
    );
    if (placebo->target == NULL) {
        return false;
    }

    image = pl_vulkan_unwrap(placebo->vulkan->gpu, placebo->target, &format, &usage);
    if (image == VK_NULL_HANDLE || format != VK_FORMAT_R8G8B8A8_UNORM ||
        (usage & VK_IMAGE_USAGE_SAMPLED_BIT) == 0 ||
        !vdi_stream_client__placebo_hold_target(placebo)) {
        vdi_stream_client__placebo_target_destroy(parsec_context, placebo);
        return false;
    }

    props = SDL_CreateProperties();
    if (props == 0) {
        vdi_stream_client__placebo_target_destroy(parsec_context, placebo);
        return false;
    }
    configured = SDL_SetNumberProperty(
                     props, SDL_PROP_TEXTURE_CREATE_FORMAT_NUMBER, SDL_PIXELFORMAT_RGBA32
                 ) &&
                 SDL_SetNumberProperty(
                     props, SDL_PROP_TEXTURE_CREATE_ACCESS_NUMBER, SDL_TEXTUREACCESS_STATIC
                 ) &&
                 SDL_SetNumberProperty(props, SDL_PROP_TEXTURE_CREATE_WIDTH_NUMBER, width) &&
                 SDL_SetNumberProperty(props, SDL_PROP_TEXTURE_CREATE_HEIGHT_NUMBER, height) &&
                 SDL_SetNumberProperty(
                     props, SDL_PROP_TEXTURE_CREATE_VULKAN_TEXTURE_NUMBER, (Sint64)(uintptr_t)image
                 );
    if (configured) {
        placebo->texture = SDL_CreateTextureWithProperties(parsec_context->renderer, props);
    }
    SDL_DestroyProperties(props);
    if (placebo->texture == NULL) {
        vdi_stream_client__placebo_target_destroy(parsec_context, placebo);
        return false;
    }

    placebo->width = width;
    placebo->height = height;
    return true;
}

/* Permanently disable direct DMA-BUF zero-copy for this renderer instance and
 * record a stats fallback. Later frames use the Vulkan upload fallback. */
static void
vdi_stream_client__placebo_disable(
    struct parsec_context_s *parsec_context, struct vdi_stream_client__placebo_s *placebo,
    const char *reason
)
{
    if (!placebo->direct_disabled) {
        SDL_LogWarn(
            SDL_LOG_CATEGORY_APPLICATION,
            "Disable VA-API DRM PRIME zero-copy rendering: %s; use Vulkan upload fallback\n", reason
        );
    }
    placebo->direct_disabled = true;
    if (parsec_context->stats_enabled) {
        parsec_context->stats_zero_copy_fallbacks++;
    }
}

/* Initialize the libplacebo Vulkan bridge for an SDL Vulkan window. This creates
 * the shared Vulkan renderer, SDL renderer wrapper, timeline semaphore, and
 * capability flags needed for VA-API DRM PRIME rendering. */
bool
vdi_stream_client__placebo_init(struct parsec_context_s *parsec_context)
{
    struct vdi_stream_client__placebo_s *placebo;
    SDL_PropertiesID props = 0;
    const char *const *extensions;
    Uint32 extension_count;
    VkPhysicalDeviceProperties device_properties;
    char failure[256] = "unknown failure";
    bool configured;

    if (parsec_context == NULL || parsec_context->window == NULL ||
        (SDL_GetWindowFlags(parsec_context->window) & SDL_WINDOW_VULKAN) == 0) {
        return false;
    }

    placebo = SDL_calloc(1, sizeof(*placebo));
    if (placebo == NULL) {
        return false;
    }
    parsec_context->placebo = placebo;

    placebo->log = pl_log_create(
        PL_API_VER,
        pl_log_params(.log_cb = vdi_stream_client__placebo_log, .log_level = PL_LOG_WARN)
    );
    if (placebo->log == NULL) {
        SDL_strlcpy(failure, "libplacebo log creation failed", sizeof(failure));
        goto error;
    }
    extensions = SDL_Vulkan_GetInstanceExtensions(&extension_count);
    if (extensions == NULL) {
        SDL_snprintf(
            failure, sizeof(failure), "Vulkan instance extension query failed: %s", SDL_GetError()
        );
        goto error;
    }
    placebo->instance = pl_vk_inst_create(
        placebo->log,
        pl_vk_inst_params(.extensions = extensions, .num_extensions = (int)extension_count)
    );
    if (placebo->instance == NULL ||
        !SDL_Vulkan_CreateSurface(
            parsec_context->window, placebo->instance->instance, NULL, &placebo->surface
        )) {
        SDL_strlcpy(failure, "Vulkan instance or Wayland surface creation failed", sizeof(failure));
        goto error;
    }

    placebo->vulkan = pl_vulkan_create(
        placebo->log,
        pl_vulkan_params(
                .instance = placebo->instance->instance,
                .get_proc_addr = placebo->instance->get_proc_addr, .surface = placebo->surface,
                .async_transfer = false, .async_compute = false, .queue_count = 1
        )
    );
    if (placebo->vulkan == NULL) {
        SDL_strlcpy(failure, "libplacebo Vulkan device creation failed", sizeof(failure));
        goto error;
    }
    if ((placebo->vulkan->gpu->import_caps.tex & PL_HANDLE_DMA_BUF) == 0) {

        /* Pre-GFX9 RADV exports linear RadeonSI video surfaces but does not
         * expose VK_EXT_image_drm_format_modifier. RADV accepts those BOs
         * through its opaque-FD external-memory compatibility path. */
        placebo->linear_import = (placebo->vulkan->gpu->import_caps.tex & PL_HANDLE_FD) != 0 &&
                                 vdi_stream_client__placebo_is_radv(placebo);
    }
    if ((placebo->vulkan->gpu->import_caps.tex & PL_HANDLE_DMA_BUF) == 0 &&
        !placebo->linear_import) {
        SDL_strlcpy(
            failure, "Vulkan device lacks compatible DMA-BUF import support", sizeof(failure)
        );
        goto error;
    }

    placebo->renderer = pl_renderer_create(placebo->log, placebo->vulkan->gpu);
    placebo->ready = pl_vulkan_sem_create(
        placebo->vulkan->gpu,
        pl_vulkan_sem_params(.type = VK_SEMAPHORE_TYPE_TIMELINE, .initial_value = 0)
    );
    if (placebo->renderer == NULL || placebo->ready == VK_NULL_HANDLE) {
        SDL_strlcpy(
            failure, "libplacebo renderer or timeline semaphore creation failed", sizeof(failure)
        );
        goto error;
    }

    props = SDL_CreateProperties();
    if (props == 0) {
        SDL_snprintf(
            failure, sizeof(failure), "SDL Vulkan renderer properties failed: %s", SDL_GetError()
        );
        goto error;
    }
    configured = SDL_SetStringProperty(props, SDL_PROP_RENDERER_CREATE_NAME_STRING, "vulkan") &&
                 SDL_SetPointerProperty(
                     props, SDL_PROP_RENDERER_CREATE_WINDOW_POINTER, parsec_context->window
                 ) &&
                 SDL_SetPointerProperty(
                     props, SDL_PROP_RENDERER_CREATE_VULKAN_INSTANCE_POINTER,
                     (void *)(uintptr_t)placebo->vulkan->instance
                 ) &&
                 SDL_SetNumberProperty(
                     props, SDL_PROP_RENDERER_CREATE_VULKAN_SURFACE_NUMBER,
                     (Sint64)(uintptr_t)placebo->surface
                 ) &&
                 SDL_SetPointerProperty(
                     props, SDL_PROP_RENDERER_CREATE_VULKAN_PHYSICAL_DEVICE_POINTER,
                     (void *)(uintptr_t)placebo->vulkan->phys_device
                 ) &&
                 SDL_SetPointerProperty(
                     props, SDL_PROP_RENDERER_CREATE_VULKAN_DEVICE_POINTER,
                     (void *)(uintptr_t)placebo->vulkan->device
                 ) &&
                 SDL_SetNumberProperty(
                     props, SDL_PROP_RENDERER_CREATE_VULKAN_GRAPHICS_QUEUE_FAMILY_INDEX_NUMBER,
                     placebo->vulkan->queue_graphics.index
                 ) &&
                 SDL_SetNumberProperty(
                     props, SDL_PROP_RENDERER_CREATE_VULKAN_PRESENT_QUEUE_FAMILY_INDEX_NUMBER,
                     placebo->vulkan->queue_graphics.index
                 );
    if (configured) {
        parsec_context->renderer = SDL_CreateRendererWithProperties(props);
    }
    SDL_DestroyProperties(props);
    props = 0;
    if (parsec_context->renderer == NULL) {
        SDL_snprintf(
            failure, sizeof(failure), "SDL Vulkan renderer creation failed: %s", SDL_GetError()
        );
        goto error;
    }

    /* A resolution-change reset rebuilds the renderer; keep its re-initialization
     * silent by skipping the one-time mode banners and marking the per-frame
     * messages as already logged. */
    if (parsec_context->silent_reinit) {
        placebo->direct_logged = true;
        placebo->upload_logged = true;
    } else {
        vkGetPhysicalDeviceProperties(placebo->vulkan->phys_device, &device_properties);
        SDL_LogInfo(
            SDL_LOG_CATEGORY_APPLICATION, "Use %s Vulkan device for VA-API DRM PRIME zero-copy\n",
            device_properties.deviceName
        );
        if (placebo->linear_import) {
            SDL_LogInfo(
                SDL_LOG_CATEGORY_APPLICATION,
                "Use RADV linear external-memory import without DRM modifiers\n"
            );
        }
    }

    /* Only the RADV linear path shares and fragments the in-process amdgpu
     * device, so a mid-stream resolution change needs the full pipeline reset. */
    vdi_stream_client__parsec_ffmpeg_enable_resolution_reset(placebo->linear_import);
    return true;

error:
    SDL_DestroyProperties(props);
    vdi_stream_client__placebo_destroy(parsec_context);
    SDL_SetError("%s", failure);
    return false;
}

/* Render one VA-API hardware frame with libplacebo. The function first tries
 * direct DMA-BUF import, falls back to Vulkan upload when import fails, and
 * publishes the SDL texture that now contains the rendered RGB frame. */
bool
vdi_stream_client__placebo_render(
    struct parsec_context_s *parsec_context, const ParsecFrame *frame, const void *image,
    bool *handled
)
{
    struct vdi_stream_client__placebo_s *placebo = parsec_context->placebo;
    struct vdi_stream_client__placebo_source_s imported_source = { 0 };
    struct pl_frame target = {
        .num_planes = 1,
        .planes = { {
            .components = 4,
            .component_mapping = { 0, 1, 2, 3 },
        } },
        .repr = pl_color_repr_rgb,
        .color = pl_color_space_srgb,
    };
    AVFrame *av_frame = NULL;
    Uint64 stage_start_ns;
    bool imported = false;
    bool rendered = false;

    if (handled != NULL) {
        *handled = false;
    }
    if (placebo == NULL || !vdi_stream_client__parsec_ffmpeg_frame_is_hardware(frame, image)) {
        return false;
    }
    if (handled != NULL) {
        *handled = true;
    }

    stage_start_ns = parsec_context->stats_enabled ? SDL_GetTicksNS() : 0;
    av_frame = vdi_stream_client__parsec_ffmpeg_frame_ref(frame, image);
    if (av_frame == NULL || av_frame->format != AV_PIX_FMT_VAAPI) {
        vdi_stream_client__placebo_disable(parsec_context, placebo, "invalid hardware frame");
        goto done;
    }
    if (!vdi_stream_client__placebo_target_create(
            parsec_context, placebo, av_frame->width, av_frame->height
        )) {
        vdi_stream_client__placebo_disable(
            parsec_context, placebo, "Vulkan target creation failed"
        );
        goto done;
    }

    vdi_stream_client__placebo_release_target(placebo);
    if (!placebo->direct_disabled) {
        imported = vdi_stream_client__placebo_source_map(placebo, av_frame, &imported_source);
        if (!imported) {
            vdi_stream_client__placebo_disable(
                parsec_context, placebo,
                placebo->linear_import
                    ? placebo->import_failure
                    : "DMA-BUF import failed, possibly due to a hybrid-GPU mismatch"
            );
        }
    } else if (parsec_context->stats_enabled) {
        parsec_context->stats_zero_copy_fallbacks++;
    }
    if (!imported &&
        !vdi_stream_client__placebo_source_upload(placebo, av_frame, &imported_source)) {
        SDL_LogWarn(
            SDL_LOG_CATEGORY_APPLICATION, "VA-API Vulkan upload fallback failed: %s\n",
            placebo->import_failure
        );
        goto done;
    }

    target.planes[0].texture = placebo->target;
    rendered =
        pl_render_image(placebo->renderer, &imported_source.frame, &target, &pl_render_fast_params);
    if (!vdi_stream_client__placebo_hold_target(placebo)) {
        vdi_stream_client__placebo_target_destroy(parsec_context, placebo);
        rendered = false;
    }
    vdi_stream_client__placebo_source_unmap(placebo, &imported_source);
    if (!rendered) {
        rendered = false;
        vdi_stream_client__placebo_disable(parsec_context, placebo, "Vulkan rendering failed");
        goto done;
    }

    parsec_context->frame_video_texture = placebo->texture;
    if (placebo->direct_disabled && !placebo->upload_logged) {
        SDL_LogInfo(
            SDL_LOG_CATEGORY_APPLICATION,
            "Use AV_PIX_FMT_VAAPI with libplacebo Vulkan upload fallback\n"
        );
        placebo->upload_logged = true;
    } else if (!placebo->direct_disabled && !placebo->direct_logged) {
        SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION, "Use AV_PIX_FMT_VAAPI video pixel format\n");
        SDL_LogInfo(
            SDL_LOG_CATEGORY_APPLICATION,
            "Use VA-API DRM PRIME zero-copy video with libplacebo YUV shader\n"
        );
        placebo->direct_logged = true;
    }

done:
    av_frame_free(&av_frame);
    if (parsec_context->stats_enabled) {
        parsec_context->stats_zero_copy_calls++;
        parsec_context->stats_zero_copy_ns += SDL_GetTicksNS() - stage_start_ns;
    }
    return rendered;
}

/* Destroy the libplacebo bridge, SDL renderer wrapper, Vulkan surface, and all
 * target resources owned by parsec_context->placebo. */
void
vdi_stream_client__placebo_destroy(struct parsec_context_s *parsec_context)
{
    struct vdi_stream_client__placebo_s *placebo;

    if (parsec_context == NULL || parsec_context->placebo == NULL) {
        return;
    }
    placebo = parsec_context->placebo;

    vdi_stream_client__placebo_target_destroy(parsec_context, placebo);
    if (placebo->vulkan != NULL) {
        pl_vulkan_sem_destroy(placebo->vulkan->gpu, &placebo->ready);
    }
    pl_renderer_destroy(&placebo->renderer);

    SDL_DestroyRenderer(parsec_context->renderer);
    parsec_context->renderer = NULL;

    pl_vulkan_destroy(&placebo->vulkan);
    if (placebo->surface != VK_NULL_HANDLE && placebo->instance != NULL) {
        SDL_Vulkan_DestroySurface(placebo->instance->instance, placebo->surface, NULL);
    }
    pl_vk_inst_destroy(&placebo->instance);
    pl_log_destroy(&placebo->log);
    SDL_free(placebo);
    parsec_context->placebo = NULL;
}
