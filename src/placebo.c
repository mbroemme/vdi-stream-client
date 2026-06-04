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
#include <libavutil/frame.h>
#include <libavutil/pixfmt.h>
#include <libplacebo/log.h>
#include <libplacebo/renderer.h>
#include <libplacebo/utils/libav.h>
#include <libplacebo/vulkan.h>
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
    struct pl_frame source = { 0 };
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
    if (!pl_map_avframe_ex(placebo->vulkan->gpu, &source, pl_avframe_params(.frame = av_frame))) {
        vdi_stream_client__placebo_disable(
            parsec_context, placebo, "DMA-BUF import failed, possibly due to a hybrid-GPU mismatch"
        );
        goto done;
    }

    target.planes[0].texture = placebo->target;
    rendered = pl_render_image(placebo->renderer, &source, &target, &pl_render_fast_params);
    pl_unmap_avframe(placebo->vulkan->gpu, &source);
    if (!rendered || !vdi_stream_client__placebo_hold_target(placebo)) {
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
