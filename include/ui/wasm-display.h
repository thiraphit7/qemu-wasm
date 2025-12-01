/*
 * QEMU WASM Display Backend Header
 *
 * Exports framebuffer to JavaScript for Canvas/WebGL/WebGPU rendering.
 * Designed for Emscripten builds targeting browsers (Safari/Chrome/Firefox).
 *
 * Features:
 * - VirtIO-GPU integration with direct resource access
 * - WebGPU rendering support (experimental, QEMU 10.1+)
 * - iOS Safari WASM optimizations
 * - Web Audio API integration
 *
 * Copyright (c) 2025 QEMU-WASM Contributors
 *
 * Permission is hereby granted, free of charge, to any person obtaining a copy
 * of this software and associated documentation files (the "Software"), to deal
 * in the Software without restriction, including without limitation the rights
 * to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
 * copies of the Software, and to permit persons to whom the Software is
 * furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in
 * all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.
 */

#ifndef QEMU_UI_WASM_DISPLAY_H
#define QEMU_UI_WASM_DISPLAY_H

#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ------------------------------------------------------------------ */
/* Rendering backend selection                                        */
/* ------------------------------------------------------------------ */

typedef enum WasmRenderBackend {
    WASM_RENDER_CANVAS2D = 0,   /* Default: Canvas 2D putImageData */
    WASM_RENDER_WEBGL,          /* WebGL texture upload */
    WASM_RENDER_WEBGPU,         /* WebGPU (experimental) */
} WasmRenderBackend;

/* ------------------------------------------------------------------ */
/* Framebuffer structures                                             */
/* ------------------------------------------------------------------ */

/**
 * Framebuffer information structure for JavaScript interop.
 * This structure is accessible from JavaScript via Emscripten.
 */
typedef struct WasmFramebufferInfo {
    uint8_t *data;          /* Pointer to RGBA pixel data */
    int32_t width;          /* Width in pixels */
    int32_t height;         /* Height in pixels */
    int32_t stride;         /* Bytes per row (pitch) */
    int32_t bpp;            /* Bits per pixel (typically 32 for RGBA) */
    uint32_t format;        /* Pixel format (pixman format code) */
    bool dirty;             /* True if framebuffer has been updated */
    uint64_t frame_count;   /* Frame counter for sync */

    /* Dirty region tracking for partial updates */
    int32_t dirty_x;
    int32_t dirty_y;
    int32_t dirty_width;
    int32_t dirty_height;

    /* VirtIO-GPU resource info */
    uint32_t resource_id;   /* Current VirtIO-GPU resource ID */
    uint32_t scanout_id;    /* Current scanout index */
} WasmFramebufferInfo;

/**
 * VirtIO-GPU resource information for direct access.
 */
typedef struct WasmGpuResource {
    uint32_t resource_id;
    uint32_t width;
    uint32_t height;
    uint32_t format;        /* DRM fourcc format */
    uint8_t *data;          /* Pixel data pointer */
    size_t size;
    bool is_blob;           /* True if blob resource */
} WasmGpuResource;

/**
 * WebGPU texture descriptor (experimental).
 */
typedef struct WasmWebGPUTexture {
    uint32_t texture_id;    /* JS-side texture handle */
    uint32_t width;
    uint32_t height;
    uint32_t format;        /* WebGPU texture format */
    bool needs_upload;      /* True if data changed */
} WasmWebGPUTexture;

/* ------------------------------------------------------------------ */
/* Display capabilities & configuration                               */
/* ------------------------------------------------------------------ */

/**
 * Display capabilities structure.
 */
typedef struct WasmDisplayCaps {
    bool webgl_available;
    bool webgpu_available;
    bool shared_array_buffer;   /* SharedArrayBuffer support */
    bool offscreen_canvas;      /* OffscreenCanvas support */
    bool is_ios_safari;         /* iOS Safari detected */
    bool is_mobile;             /* Mobile device detected */
    int32_t max_texture_size;   /* Maximum texture dimension */
    int32_t device_pixel_ratio; /* Device pixel ratio * 100 */
} WasmDisplayCaps;

/**
 * Get display capabilities detected from browser.
 */
WasmDisplayCaps *wasm_get_display_caps(void);

/**
 * Set rendering backend.
 */
void wasm_set_render_backend(WasmRenderBackend backend);

/**
 * Get current rendering backend.
 */
WasmRenderBackend wasm_get_render_backend(void);

/* ------------------------------------------------------------------ */
/* Framebuffer access functions                                       */
/* ------------------------------------------------------------------ */

/**
 * Get the framebuffer info structure.
 */
WasmFramebufferInfo *wasm_get_framebuffer_info(void);

/**
 * Get raw framebuffer data pointer.
 */
uint8_t *wasm_get_framebuffer_data(void);

/**
 * Get framebuffer dimensions.
 */
bool wasm_get_framebuffer_size(int32_t *out_width, int32_t *out_height);

/**
 * Acknowledge framebuffer read (clears dirty flag).
 */
void wasm_framebuffer_ack(void);

/**
 * Check if framebuffer has updates.
 */
bool wasm_framebuffer_is_dirty(void);

/**
 * Get current frame count.
 */
uint64_t wasm_get_frame_count(void);

/**
 * Get dirty region for partial updates.
 */
void wasm_get_dirty_region(int32_t *x, int32_t *y, int32_t *w, int32_t *h);

/* ------------------------------------------------------------------ */
/* VirtIO-GPU integration                                             */
/* ------------------------------------------------------------------ */

/**
 * Get current VirtIO-GPU resource info.
 */
WasmGpuResource *wasm_gpu_get_current_resource(void);

/**
 * Get resource by ID.
 */
WasmGpuResource *wasm_gpu_get_resource(uint32_t resource_id);

/**
 * Notify that a GPU resource was created.
 */
void wasm_gpu_resource_created(uint32_t resource_id, uint32_t width,
                               uint32_t height, uint32_t format);

/**
 * Notify that a GPU resource was destroyed.
 */
void wasm_gpu_resource_destroyed(uint32_t resource_id);

/**
 * Notify that a scanout was configured.
 */
void wasm_gpu_scanout_set(uint32_t scanout_id, uint32_t resource_id,
                          uint32_t width, uint32_t height);

/**
 * Notify GPU resource flush (partial update).
 */
void wasm_gpu_resource_flush(uint32_t resource_id,
                             int32_t x, int32_t y,
                             int32_t width, int32_t height);

/* ------------------------------------------------------------------ */
/* WebGPU support (experimental)                                      */
/* ------------------------------------------------------------------ */

/**
 * Check if WebGPU is available and initialized.
 */
bool wasm_webgpu_available(void);

/**
 * Initialize WebGPU context.
 * Returns true on success.
 */
bool wasm_webgpu_init(void);

/**
 * Get WebGPU texture for current framebuffer.
 */
WasmWebGPUTexture *wasm_webgpu_get_texture(void);

/**
 * Upload framebuffer to WebGPU texture.
 */
void wasm_webgpu_upload_texture(void);

/**
 * Signal that WebGPU render pass completed.
 */
void wasm_webgpu_present(void);

/* ------------------------------------------------------------------ */
/* Input handling                                                     */
/* ------------------------------------------------------------------ */

/**
 * Send keyboard event from JavaScript.
 */
void wasm_send_keyboard_event(int keycode, bool down);

/**
 * Send mouse motion event from JavaScript.
 */
void wasm_send_mouse_motion(int x, int y);

/**
 * Send relative mouse motion (for pointer lock).
 */
void wasm_send_mouse_motion_relative(int dx, int dy);

/**
 * Send mouse button event from JavaScript.
 */
void wasm_send_mouse_button(int button, bool down);

/**
 * Send mouse wheel event from JavaScript.
 */
void wasm_send_mouse_wheel(int dx, int dy);

/**
 * Send touch event from JavaScript.
 */
void wasm_send_touch_event(int touch_id, int x, int y, int type);

/* ------------------------------------------------------------------ */
/* iOS Safari optimizations                                           */
/* ------------------------------------------------------------------ */

/**
 * Enable iOS Safari specific optimizations.
 * - Reduced memory allocation frequency
 * - Frame pacing for 60Hz/120Hz ProMotion
 * - Power-efficient rendering hints
 */
void wasm_ios_safari_optimize(bool enable);

/**
 * Set target frame rate for iOS (60 or 120 for ProMotion).
 */
void wasm_ios_set_target_fps(int fps);

/**
 * Request low power mode (reduces GPU usage).
 */
void wasm_ios_low_power_mode(bool enable);

/**
 * Handle visibility change (pause rendering when hidden).
 */
void wasm_handle_visibility_change(bool visible);

/* ------------------------------------------------------------------ */
/* Performance & debugging                                            */
/* ------------------------------------------------------------------ */

/**
 * Performance statistics.
 */
typedef struct WasmPerfStats {
    uint64_t frames_rendered;
    uint64_t frames_dropped;
    uint64_t bytes_transferred;
    double avg_frame_time_ms;
    double avg_copy_time_ms;
    double avg_render_time_ms;
    int32_t current_fps;
} WasmPerfStats;

/**
 * Get performance statistics.
 */
WasmPerfStats *wasm_get_perf_stats(void);

/**
 * Reset performance statistics.
 */
void wasm_reset_perf_stats(void);

/**
 * Enable/disable performance profiling.
 */
void wasm_enable_profiling(bool enable);

#ifdef __cplusplus
}
#endif

#endif /* QEMU_UI_WASM_DISPLAY_H */
