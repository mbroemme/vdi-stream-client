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
#include <libavutil/common.h>
#include <libavutil/frame.h>
#include <libavutil/hwcontext.h>
#include <libavutil/hwcontext_drm.h>
#include <libavutil/pixdesc.h>
#include <libavutil/pixfmt.h>
#include <libplacebo/log.h>
#include <libplacebo/renderer.h>
#include <libplacebo/utils/libav.h>
#include <libplacebo/utils/upload.h>
#include <libplacebo/vulkan.h>
#include <unistd.h>
#include <vulkan/vulkan.h>

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
    bool target_held;
    bool direct_disabled;
    bool direct_logged;
};

struct vdi_stream_client__placebo_source_s
{
    struct pl_frame frame;
    pl_tex textures[4];
    AVFrame *drm_frame;
};

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

static void
vdi_stream_client__placebo_source_unmap(
    struct vdi_stream_client__placebo_s *placebo, struct vdi_stream_client__placebo_source_s *source
)
{
    for (size_t i = 0; i < 4; i++) {
        pl_tex_destroy(placebo->vulkan->gpu, &source->textures[i]);
    }
    av_frame_free(&source->drm_frame);
    SDL_memset(&source->frame, 0, sizeof(source->frame));
}

static bool
vdi_stream_client__placebo_source_map(
    struct vdi_stream_client__placebo_s *placebo, const AVFrame *av_frame,
    struct vdi_stream_client__placebo_source_s *source
)
{
    const AVHWFramesContext *frames_context;
    const AVPixFmtDescriptor *pixel_descriptor;
    const AVDRMFrameDescriptor *drm_descriptor;
    struct pl_plane_data plane_data[4] = { 0 };
    int plane_count;
    int descriptor_plane_count = 0;
    int layer = 0;
    int layer_plane = 0;
    int err;

    if (av_frame == NULL || av_frame->format != AV_PIX_FMT_VAAPI ||
        av_frame->hw_frames_ctx == NULL) {
        return false;
    }

    frames_context = (const AVHWFramesContext *)av_frame->hw_frames_ctx->data;
    if (frames_context == NULL) {
        return false;
    }
    pixel_descriptor = av_pix_fmt_desc_get(frames_context->sw_format);
    if (pixel_descriptor == NULL) {
        return false;
    }

    source->drm_frame = av_frame_alloc();
    if (source->drm_frame == NULL) {
        return false;
    }
    source->drm_frame->format = AV_PIX_FMT_DRM_PRIME;
    source->drm_frame->width = av_frame->width;
    source->drm_frame->height = av_frame->height;
    source->drm_frame->hw_frames_ctx = av_buffer_ref(av_frame->hw_frames_ctx);
    if (source->drm_frame->hw_frames_ctx == NULL) {
        goto error;
    }

    err = av_hwframe_map(source->drm_frame, av_frame, AV_HWFRAME_MAP_READ | AV_HWFRAME_MAP_DIRECT);
    if (err < 0) {
        goto error;
    }
    drm_descriptor = (const AVDRMFrameDescriptor *)source->drm_frame->data[0];
    if (drm_descriptor == NULL || drm_descriptor->nb_layers <= 0 ||
        drm_descriptor->nb_objects <= 0) {
        goto error;
    }

    pl_frame_from_avframe(&source->frame, av_frame);
    plane_count =
        pl_plane_data_from_pixfmt(plane_data, &source->frame.repr.bits, frames_context->sw_format);
    if (plane_count <= 0 || plane_count != source->frame.num_planes) {
        goto error;
    }

    for (int i = 0; i < drm_descriptor->nb_layers; i++) {
        descriptor_plane_count += drm_descriptor->layers[i].nb_planes;
    }
    if (descriptor_plane_count != plane_count) {
        goto error;
    }

    for (int i = 0; i < plane_count; i++) {
        const AVDRMLayerDescriptor *drm_layer;
        const AVDRMPlaneDescriptor *drm_plane;
        const AVDRMObjectDescriptor *drm_object;
        struct pl_plane *plane = &source->frame.planes[i];
        struct pl_tex_params texture_params;
        pl_fmt format;
        size_t object_size;
        int component_mapping[4];
        bool is_chroma = i == 1 || i == 2;

        while (layer < drm_descriptor->nb_layers &&
               layer_plane >= drm_descriptor->layers[layer].nb_planes) {
            layer++;
            layer_plane = 0;
        }
        if (layer >= drm_descriptor->nb_layers) {
            goto error;
        }

        drm_layer = &drm_descriptor->layers[layer];
        drm_plane = &drm_layer->planes[layer_plane++];
        if (drm_plane->object_index < 0 || drm_plane->object_index >= drm_descriptor->nb_objects ||
            drm_plane->pitch == 0) {
            goto error;
        }
        drm_object = &drm_descriptor->objects[drm_plane->object_index];
        if (drm_object->fd < 0) {
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
        format = pl_plane_find_fmt(placebo->vulkan->gpu, component_mapping, &plane_data[i]);
        if (format == NULL) {
            goto error;
        }

        object_size = drm_object->size;
        if (object_size == 0) {
            off_t end = lseek(drm_object->fd, 0, SEEK_END);

            if (end <= 0 || lseek(drm_object->fd, 0, SEEK_SET) < 0) {
                goto error;
            }
            object_size = (size_t)end;
        }

        texture_params = (struct pl_tex_params) {
            .w = plane_data[i].width,
            .h = plane_data[i].height,
            .format = format,
            .sampleable = true,
            .blit_src = (format->caps & PL_FMT_CAP_BLITTABLE) != 0,
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
        if (source->textures[i] == NULL) {
            goto error;
        }

        plane->texture = source->textures[i];
        plane->components = 0;
        for (size_t component = 0;
             component < sizeof(component_mapping) / sizeof(component_mapping[0]); component++) {
            plane->component_mapping[component] = component_mapping[component];
            if (component_mapping[component] >= 0) {
                plane->components = (int)component + 1;
            }
        }
    }

    return true;

error:
    vdi_stream_client__placebo_source_unmap(placebo, source);
    return false;
}

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

static void
vdi_stream_client__placebo_disable(
    struct parsec_context_s *parsec_context, struct vdi_stream_client__placebo_s *placebo,
    const char *reason
)
{
    if (!placebo->direct_disabled) {
        SDL_LogWarn(
            SDL_LOG_CATEGORY_APPLICATION,
            "Disable VA-API DRM PRIME zero-copy rendering: %s; use SDL upload fallback\n", reason
        );
    }
    placebo->direct_disabled = true;
    if (parsec_context->stats_enabled) {
        parsec_context->stats_zero_copy_fallbacks++;
    }
}

bool
vdi_stream_client__placebo_init(struct parsec_context_s *parsec_context)
{
    struct vdi_stream_client__placebo_s *placebo;
    SDL_PropertiesID props = 0;
    const char *const *extensions;
    Uint32 extension_count;
    VkPhysicalDeviceProperties device_properties;
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
    extensions = SDL_Vulkan_GetInstanceExtensions(&extension_count);
    if (extensions == NULL) {
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
    if (placebo->vulkan == NULL ||
        (placebo->vulkan->gpu->import_caps.tex & PL_HANDLE_DMA_BUF) == 0) {
        goto error;
    }

    placebo->renderer = pl_renderer_create(placebo->log, placebo->vulkan->gpu);
    placebo->ready = pl_vulkan_sem_create(
        placebo->vulkan->gpu,
        pl_vulkan_sem_params(.type = VK_SEMAPHORE_TYPE_TIMELINE, .initial_value = 0)
    );
    if (placebo->renderer == NULL || placebo->ready == VK_NULL_HANDLE) {
        goto error;
    }

    props = SDL_CreateProperties();
    if (props == 0) {
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
        goto error;
    }

    vkGetPhysicalDeviceProperties(placebo->vulkan->phys_device, &device_properties);
    SDL_LogInfo(
        SDL_LOG_CATEGORY_APPLICATION, "Use %s Vulkan device for VA-API DRM PRIME zero-copy\n",
        device_properties.deviceName
    );
    return true;

error:
    SDL_DestroyProperties(props);
    vdi_stream_client__placebo_destroy(parsec_context);
    return false;
}

bool
vdi_stream_client__placebo_render(
    struct parsec_context_s *parsec_context, const ParsecFrame *frame, const void *image
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

    if (placebo == NULL || !vdi_stream_client__parsec_ffmpeg_frame_is_hardware(frame, image)) {
        return false;
    }
    if (placebo->direct_disabled) {
        if (parsec_context->stats_enabled) {
            parsec_context->stats_zero_copy_fallbacks++;
        }
        return false;
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
    imported = vdi_stream_client__placebo_source_map(placebo, av_frame, &imported_source);
    if (!imported) {
        if (!vdi_stream_client__placebo_hold_target(placebo)) {
            vdi_stream_client__placebo_target_destroy(parsec_context, placebo);
        }
        vdi_stream_client__placebo_disable(
            parsec_context, placebo, "DMA-BUF import failed, possibly due to a hybrid-GPU mismatch"
        );
        goto done;
    }

    target.planes[0].texture = placebo->target;
    rendered =
        pl_render_image(placebo->renderer, &imported_source.frame, &target, &pl_render_fast_params);
    vdi_stream_client__placebo_source_unmap(placebo, &imported_source);
    if (!vdi_stream_client__placebo_hold_target(placebo)) {
        vdi_stream_client__placebo_target_destroy(parsec_context, placebo);
        rendered = false;
    }
    if (!rendered) {
        rendered = false;
        vdi_stream_client__placebo_disable(parsec_context, placebo, "Vulkan rendering failed");
        goto done;
    }

    parsec_context->frame_video_texture = placebo->texture;
    if (!placebo->direct_logged) {
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
