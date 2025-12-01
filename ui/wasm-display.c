/*
 * QEMU WASM Display Backend
 *
 * Exports framebuffer to JavaScript for Canvas/WebGL/WebGPU rendering.
 * Designed for Emscripten builds targeting browsers (Safari/Chrome/Firefox).
 *
 * Features:
 * - VirtIO-GPU integration with direct resource access
 * - WebGPU rendering support (experimental, QEMU 10.1+)
 * - iOS Safari WASM optimizations
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

#include "qemu/osdep.h"
#include "qemu/module.h"
#include "qemu/main-loop.h"
#include "qemu/timer.h"
#include "qapi/error.h"
#include "ui/console.h"
#include "ui/wasm-display.h"
#include "ui/input.h"
#include "ui/kbd-state.h"

#ifdef __EMSCRIPTEN__
#include <emscripten.h>
#include <emscripten/html5.h>
#endif

/* Maximum framebuffer size (4K resolution with RGBA) */
#define WASM_FB_MAX_WIDTH  3840
#define WASM_FB_MAX_HEIGHT 2160
#define WASM_FB_MAX_SIZE   (WASM_FB_MAX_WIDTH * WASM_FB_MAX_HEIGHT * 4)

/* Default framebuffer size */
#define WASM_FB_DEFAULT_WIDTH  1024
#define WASM_FB_DEFAULT_HEIGHT 768

/* Maximum tracked GPU resources */
#define WASM_MAX_GPU_RESOURCES 64

/* ------------------------------------------------------------------ */
/* Internal state structures                                          */
/* ------------------------------------------------------------------ */

typedef struct WasmDisplayState {
    DisplayChangeListener dcl;
    DisplaySurface *ds;
    QKbdState *kbd;

    /* Framebuffer for JavaScript access */
    uint8_t *fb_data;
    size_t fb_allocated_size;
    WasmFramebufferInfo fb_info;

    /* Rendering backend */
    WasmRenderBackend render_backend;

    /* VirtIO-GPU resources */
    WasmGpuResource gpu_resources[WASM_MAX_GPU_RESOURCES];
    int gpu_resource_count;
    uint32_t current_resource_id;

    /* WebGPU state */
    bool webgpu_initialized;
    WasmWebGPUTexture webgpu_texture;

    /* Mouse state */
    int mouse_x;
    int mouse_y;
    int mouse_buttons;
    bool mouse_grabbed;

    /* iOS Safari optimizations */
    bool ios_optimizations;
    int target_fps;
    bool low_power_mode;
    bool is_visible;

    /* Performance stats */
    WasmPerfStats perf_stats;
    bool profiling_enabled;
    int64_t last_frame_time;
    int64_t frame_time_accum;
    int frame_time_count;
} WasmDisplayState;

/* Global state for JavaScript access */
static WasmDisplayState *wasm_display_state = NULL;

/* Display capabilities (detected from browser) */
static WasmDisplayCaps wasm_caps = {0};

/* ------------------------------------------------------------------ */
/* Browser capability detection (called from JS)                      */
/* ------------------------------------------------------------------ */

#ifdef __EMSCRIPTEN__
EM_JS(void, wasm_detect_capabilities, (), {
    var caps = {};

    // WebGL detection
    try {
        var canvas = document.createElement('canvas');
        caps.webgl = !!(canvas.getContext('webgl') || canvas.getContext('webgl2'));
    } catch (e) {
        caps.webgl = false;
    }

    // WebGPU detection
    caps.webgpu = (typeof navigator !== 'undefined' && 'gpu' in navigator);

    // SharedArrayBuffer
    caps.sab = (typeof SharedArrayBuffer !== 'undefined');

    // OffscreenCanvas
    caps.offscreen = (typeof OffscreenCanvas !== 'undefined');

    // iOS Safari detection
    var ua = navigator.userAgent || '';
    caps.ios_safari = /iPad|iPhone|iPod/.test(ua) && /Safari/.test(ua) && !/Chrome/.test(ua);
    caps.mobile = /Android|webOS|iPhone|iPad|iPod|BlackBerry|IEMobile|Opera Mini/i.test(ua);

    // Max texture size
    caps.max_texture = 4096;
    if (caps.webgl) {
        try {
            var gl = canvas.getContext('webgl2') || canvas.getContext('webgl');
            if (gl) {
                caps.max_texture = gl.getParameter(gl.MAX_TEXTURE_SIZE);
            }
        } catch (e) {}
    }

    // Device pixel ratio
    caps.dpr = Math.round((window.devicePixelRatio || 1) * 100);

    // Store in Module for C access
    Module._wasm_caps = caps;

    // Call C function to update caps
    if (Module._wasm_update_caps) {
        Module._wasm_update_caps(
            caps.webgl ? 1 : 0,
            caps.webgpu ? 1 : 0,
            caps.sab ? 1 : 0,
            caps.offscreen ? 1 : 0,
            caps.ios_safari ? 1 : 0,
            caps.mobile ? 1 : 0,
            caps.max_texture,
            caps.dpr
        );
    }
});

EMSCRIPTEN_KEEPALIVE
void wasm_update_caps(int webgl, int webgpu, int sab, int offscreen,
                      int ios_safari, int mobile, int max_tex, int dpr)
{
    wasm_caps.webgl_available = webgl;
    wasm_caps.webgpu_available = webgpu;
    wasm_caps.shared_array_buffer = sab;
    wasm_caps.offscreen_canvas = offscreen;
    wasm_caps.is_ios_safari = ios_safari;
    wasm_caps.is_mobile = mobile;
    wasm_caps.max_texture_size = max_tex;
    wasm_caps.device_pixel_ratio = dpr;
}
#endif

/* ------------------------------------------------------------------ */
/* Exported functions for JavaScript access                           */
/* ------------------------------------------------------------------ */

#ifdef __EMSCRIPTEN__
EMSCRIPTEN_KEEPALIVE
#endif
WasmDisplayCaps *wasm_get_display_caps(void)
{
    return &wasm_caps;
}

#ifdef __EMSCRIPTEN__
EMSCRIPTEN_KEEPALIVE
#endif
void wasm_set_render_backend(WasmRenderBackend backend)
{
    if (wasm_display_state) {
        wasm_display_state->render_backend = backend;
#ifdef __EMSCRIPTEN__
        EM_ASM({
            if (window.onWasmRenderBackendChange) {
                window.onWasmRenderBackendChange($0);
            }
        }, backend);
#endif
    }
}

#ifdef __EMSCRIPTEN__
EMSCRIPTEN_KEEPALIVE
#endif
WasmRenderBackend wasm_get_render_backend(void)
{
    return wasm_display_state ? wasm_display_state->render_backend : WASM_RENDER_CANVAS2D;
}

#ifdef __EMSCRIPTEN__
EMSCRIPTEN_KEEPALIVE
#endif
WasmFramebufferInfo *wasm_get_framebuffer_info(void)
{
    if (!wasm_display_state) {
        return NULL;
    }
    return &wasm_display_state->fb_info;
}

#ifdef __EMSCRIPTEN__
EMSCRIPTEN_KEEPALIVE
#endif
uint8_t *wasm_get_framebuffer_data(void)
{
    if (!wasm_display_state || !wasm_display_state->fb_data) {
        return NULL;
    }
    return wasm_display_state->fb_data;
}

#ifdef __EMSCRIPTEN__
EMSCRIPTEN_KEEPALIVE
#endif
bool wasm_get_framebuffer_size(int32_t *out_width, int32_t *out_height)
{
    if (!wasm_display_state) {
        if (out_width) *out_width = 0;
        if (out_height) *out_height = 0;
        return false;
    }
    if (out_width) *out_width = wasm_display_state->fb_info.width;
    if (out_height) *out_height = wasm_display_state->fb_info.height;
    return true;
}

#ifdef __EMSCRIPTEN__
EMSCRIPTEN_KEEPALIVE
#endif
void wasm_framebuffer_ack(void)
{
    if (wasm_display_state) {
        wasm_display_state->fb_info.dirty = false;
        wasm_display_state->fb_info.dirty_x = 0;
        wasm_display_state->fb_info.dirty_y = 0;
        wasm_display_state->fb_info.dirty_width = 0;
        wasm_display_state->fb_info.dirty_height = 0;
    }
}

#ifdef __EMSCRIPTEN__
EMSCRIPTEN_KEEPALIVE
#endif
bool wasm_framebuffer_is_dirty(void)
{
    return wasm_display_state ? wasm_display_state->fb_info.dirty : false;
}

#ifdef __EMSCRIPTEN__
EMSCRIPTEN_KEEPALIVE
#endif
uint64_t wasm_get_frame_count(void)
{
    return wasm_display_state ? wasm_display_state->fb_info.frame_count : 0;
}

#ifdef __EMSCRIPTEN__
EMSCRIPTEN_KEEPALIVE
#endif
void wasm_get_dirty_region(int32_t *x, int32_t *y, int32_t *w, int32_t *h)
{
    if (!wasm_display_state) {
        if (x) *x = 0;
        if (y) *y = 0;
        if (w) *w = 0;
        if (h) *h = 0;
        return;
    }
    if (x) *x = wasm_display_state->fb_info.dirty_x;
    if (y) *y = wasm_display_state->fb_info.dirty_y;
    if (w) *w = wasm_display_state->fb_info.dirty_width;
    if (h) *h = wasm_display_state->fb_info.dirty_height;
}

/* ------------------------------------------------------------------ */
/* VirtIO-GPU integration                                             */
/* ------------------------------------------------------------------ */

#ifdef __EMSCRIPTEN__
EMSCRIPTEN_KEEPALIVE
#endif
WasmGpuResource *wasm_gpu_get_current_resource(void)
{
    if (!wasm_display_state) {
        return NULL;
    }
    return wasm_gpu_get_resource(wasm_display_state->current_resource_id);
}

#ifdef __EMSCRIPTEN__
EMSCRIPTEN_KEEPALIVE
#endif
WasmGpuResource *wasm_gpu_get_resource(uint32_t resource_id)
{
    if (!wasm_display_state || resource_id == 0) {
        return NULL;
    }
    for (int i = 0; i < wasm_display_state->gpu_resource_count; i++) {
        if (wasm_display_state->gpu_resources[i].resource_id == resource_id) {
            return &wasm_display_state->gpu_resources[i];
        }
    }
    return NULL;
}

#ifdef __EMSCRIPTEN__
EMSCRIPTEN_KEEPALIVE
#endif
void wasm_gpu_resource_created(uint32_t resource_id, uint32_t width,
                               uint32_t height, uint32_t format)
{
    if (!wasm_display_state) {
        return;
    }
    if (wasm_display_state->gpu_resource_count >= WASM_MAX_GPU_RESOURCES) {
        fprintf(stderr, "wasm-display: max GPU resources reached\n");
        return;
    }

    WasmGpuResource *res = &wasm_display_state->gpu_resources[
        wasm_display_state->gpu_resource_count++
    ];
    res->resource_id = resource_id;
    res->width = width;
    res->height = height;
    res->format = format;
    res->data = NULL;
    res->size = 0;
    res->is_blob = false;

#ifdef __EMSCRIPTEN__
    EM_ASM({
        if (window.onWasmGpuResourceCreated) {
            window.onWasmGpuResourceCreated($0, $1, $2, $3);
        }
    }, resource_id, width, height, format);
#endif
}

#ifdef __EMSCRIPTEN__
EMSCRIPTEN_KEEPALIVE
#endif
void wasm_gpu_resource_destroyed(uint32_t resource_id)
{
    if (!wasm_display_state) {
        return;
    }
    for (int i = 0; i < wasm_display_state->gpu_resource_count; i++) {
        if (wasm_display_state->gpu_resources[i].resource_id == resource_id) {
            /* Shift remaining resources */
            memmove(&wasm_display_state->gpu_resources[i],
                    &wasm_display_state->gpu_resources[i + 1],
                    (wasm_display_state->gpu_resource_count - i - 1) *
                    sizeof(WasmGpuResource));
            wasm_display_state->gpu_resource_count--;
            break;
        }
    }

#ifdef __EMSCRIPTEN__
    EM_ASM({
        if (window.onWasmGpuResourceDestroyed) {
            window.onWasmGpuResourceDestroyed($0);
        }
    }, resource_id);
#endif
}

#ifdef __EMSCRIPTEN__
EMSCRIPTEN_KEEPALIVE
#endif
void wasm_gpu_scanout_set(uint32_t scanout_id, uint32_t resource_id,
                          uint32_t width, uint32_t height)
{
    if (!wasm_display_state) {
        return;
    }
    wasm_display_state->current_resource_id = resource_id;
    wasm_display_state->fb_info.resource_id = resource_id;
    wasm_display_state->fb_info.scanout_id = scanout_id;

#ifdef __EMSCRIPTEN__
    EM_ASM({
        if (window.onWasmGpuScanoutSet) {
            window.onWasmGpuScanoutSet($0, $1, $2, $3);
        }
    }, scanout_id, resource_id, width, height);
#endif
}

#ifdef __EMSCRIPTEN__
EMSCRIPTEN_KEEPALIVE
#endif
void wasm_gpu_resource_flush(uint32_t resource_id,
                             int32_t x, int32_t y,
                             int32_t width, int32_t height)
{
    if (!wasm_display_state) {
        return;
    }

    /* Update dirty region */
    WasmFramebufferInfo *fb = &wasm_display_state->fb_info;
    if (!fb->dirty) {
        fb->dirty_x = x;
        fb->dirty_y = y;
        fb->dirty_width = width;
        fb->dirty_height = height;
    } else {
        /* Expand dirty region */
        int32_t x2 = MAX(fb->dirty_x + fb->dirty_width, x + width);
        int32_t y2 = MAX(fb->dirty_y + fb->dirty_height, y + height);
        fb->dirty_x = MIN(fb->dirty_x, x);
        fb->dirty_y = MIN(fb->dirty_y, y);
        fb->dirty_width = x2 - fb->dirty_x;
        fb->dirty_height = y2 - fb->dirty_y;
    }
    fb->dirty = true;

#ifdef __EMSCRIPTEN__
    EM_ASM({
        if (window.onWasmGpuResourceFlush) {
            window.onWasmGpuResourceFlush($0, $1, $2, $3, $4);
        }
    }, resource_id, x, y, width, height);
#endif
}

/* ------------------------------------------------------------------ */
/* WebGPU support (experimental)                                      */
/* ------------------------------------------------------------------ */

#ifdef __EMSCRIPTEN__
EMSCRIPTEN_KEEPALIVE
#endif
bool wasm_webgpu_available(void)
{
    return wasm_caps.webgpu_available;
}

#ifdef __EMSCRIPTEN__
EMSCRIPTEN_KEEPALIVE
#endif
bool wasm_webgpu_init(void)
{
    if (!wasm_display_state || !wasm_caps.webgpu_available) {
        return false;
    }

#ifdef __EMSCRIPTEN__
    int result = EM_ASM_INT({
        if (!navigator.gpu) return 0;

        // Store promise for async init
        Module._webgpu_init_promise = (async function() {
            try {
                const adapter = await navigator.gpu.requestAdapter({
                    powerPreference: $0 ? 'low-power' : 'high-performance'
                });
                if (!adapter) return false;

                const device = await adapter.requestDevice();
                Module._webgpu_device = device;
                Module._webgpu_adapter = adapter;

                // Create initial texture
                Module._webgpu_texture = device.createTexture({
                    size: [$1, $2],
                    format: 'rgba8unorm',
                    usage: GPUTextureUsage.TEXTURE_BINDING |
                           GPUTextureUsage.COPY_DST |
                           GPUTextureUsage.RENDER_ATTACHMENT
                });

                return true;
            } catch (e) {
                console.error('WebGPU init failed:', e);
                return false;
            }
        })();

        return 1;  // Async init started
    }, wasm_display_state->low_power_mode,
       wasm_display_state->fb_info.width,
       wasm_display_state->fb_info.height);

    if (result) {
        wasm_display_state->webgpu_initialized = true;
        wasm_display_state->render_backend = WASM_RENDER_WEBGPU;
    }
    return result != 0;
#else
    return false;
#endif
}

#ifdef __EMSCRIPTEN__
EMSCRIPTEN_KEEPALIVE
#endif
WasmWebGPUTexture *wasm_webgpu_get_texture(void)
{
    if (!wasm_display_state || !wasm_display_state->webgpu_initialized) {
        return NULL;
    }
    return &wasm_display_state->webgpu_texture;
}

#ifdef __EMSCRIPTEN__
EMSCRIPTEN_KEEPALIVE
#endif
void wasm_webgpu_upload_texture(void)
{
    if (!wasm_display_state || !wasm_display_state->webgpu_initialized) {
        return;
    }

#ifdef __EMSCRIPTEN__
    EM_ASM({
        if (!Module._webgpu_device || !Module._webgpu_texture) return;

        const device = Module._webgpu_device;
        const texture = Module._webgpu_texture;
        const width = $0;
        const height = $1;
        const dataPtr = $2;
        const stride = $3;

        // Get pixel data from WASM memory
        const data = new Uint8Array(HEAPU8.buffer, dataPtr, height * stride);

        // Upload to GPU texture
        device.queue.writeTexture(
            { texture: texture },
            data,
            { bytesPerRow: stride, rowsPerImage: height },
            { width: width, height: height }
        );
    }, wasm_display_state->fb_info.width,
       wasm_display_state->fb_info.height,
       wasm_display_state->fb_data,
       wasm_display_state->fb_info.stride);
#endif

    wasm_display_state->webgpu_texture.needs_upload = false;
}

#ifdef __EMSCRIPTEN__
EMSCRIPTEN_KEEPALIVE
#endif
void wasm_webgpu_present(void)
{
    if (!wasm_display_state) {
        return;
    }
    wasm_display_state->perf_stats.frames_rendered++;
}

/* ------------------------------------------------------------------ */
/* Input handling                                                     */
/* ------------------------------------------------------------------ */

#ifdef __EMSCRIPTEN__
EMSCRIPTEN_KEEPALIVE
#endif
void wasm_send_keyboard_event(int keycode, bool down)
{
    if (!wasm_display_state) {
        return;
    }
    qkbd_state_key_event(wasm_display_state->kbd, (QKeyCode)keycode, down);
}

#ifdef __EMSCRIPTEN__
EMSCRIPTEN_KEEPALIVE
#endif
void wasm_send_mouse_motion(int x, int y)
{
    if (!wasm_display_state || !wasm_display_state->dcl.con) {
        return;
    }

    wasm_display_state->mouse_x = x;
    wasm_display_state->mouse_y = y;

    qemu_input_queue_abs(wasm_display_state->dcl.con,
                         INPUT_AXIS_X, x,
                         0, wasm_display_state->fb_info.width);
    qemu_input_queue_abs(wasm_display_state->dcl.con,
                         INPUT_AXIS_Y, y,
                         0, wasm_display_state->fb_info.height);
    qemu_input_event_sync();
}

#ifdef __EMSCRIPTEN__
EMSCRIPTEN_KEEPALIVE
#endif
void wasm_send_mouse_motion_relative(int dx, int dy)
{
    if (!wasm_display_state || !wasm_display_state->dcl.con) {
        return;
    }

    qemu_input_queue_rel(wasm_display_state->dcl.con, INPUT_AXIS_X, dx);
    qemu_input_queue_rel(wasm_display_state->dcl.con, INPUT_AXIS_Y, dy);
    qemu_input_event_sync();
}

#ifdef __EMSCRIPTEN__
EMSCRIPTEN_KEEPALIVE
#endif
void wasm_send_mouse_button(int button, bool down)
{
    if (!wasm_display_state || !wasm_display_state->dcl.con) {
        return;
    }

    InputButton btn;
    switch (button) {
    case 0:
        btn = INPUT_BUTTON_LEFT;
        break;
    case 1:
        btn = INPUT_BUTTON_MIDDLE;
        break;
    case 2:
        btn = INPUT_BUTTON_RIGHT;
        break;
    default:
        return;
    }

    qemu_input_queue_btn(wasm_display_state->dcl.con, btn, down);
    qemu_input_event_sync();
}

#ifdef __EMSCRIPTEN__
EMSCRIPTEN_KEEPALIVE
#endif
void wasm_send_mouse_wheel(int dx, int dy)
{
    if (!wasm_display_state || !wasm_display_state->dcl.con) {
        return;
    }

    if (dy != 0) {
        InputButton btn = dy > 0 ? INPUT_BUTTON_WHEEL_UP : INPUT_BUTTON_WHEEL_DOWN;
        qemu_input_queue_btn(wasm_display_state->dcl.con, btn, true);
        qemu_input_event_sync();
        qemu_input_queue_btn(wasm_display_state->dcl.con, btn, false);
        qemu_input_event_sync();
    }
}

#ifdef __EMSCRIPTEN__
EMSCRIPTEN_KEEPALIVE
#endif
void wasm_send_touch_event(int touch_id, int x, int y, int type)
{
    if (!wasm_display_state || !wasm_display_state->dcl.con) {
        return;
    }

    /* Type: 0=start, 1=move, 2=end */
    if (type == 0 || type == 1) {
        qemu_input_queue_abs(wasm_display_state->dcl.con,
                             INPUT_AXIS_X, x,
                             0, wasm_display_state->fb_info.width);
        qemu_input_queue_abs(wasm_display_state->dcl.con,
                             INPUT_AXIS_Y, y,
                             0, wasm_display_state->fb_info.height);
    }

    if (type == 0) {
        qemu_input_queue_btn(wasm_display_state->dcl.con, INPUT_BUTTON_LEFT, true);
    } else if (type == 2) {
        qemu_input_queue_btn(wasm_display_state->dcl.con, INPUT_BUTTON_LEFT, false);
    }

    qemu_input_event_sync();
}

/* ------------------------------------------------------------------ */
/* iOS Safari optimizations                                           */
/* ------------------------------------------------------------------ */

#ifdef __EMSCRIPTEN__
EMSCRIPTEN_KEEPALIVE
#endif
void wasm_ios_safari_optimize(bool enable)
{
    if (!wasm_display_state) {
        return;
    }
    wasm_display_state->ios_optimizations = enable;

    if (enable) {
        /* Apply iOS-specific optimizations */
        wasm_display_state->target_fps = 60;  /* Default to 60fps */

#ifdef __EMSCRIPTEN__
        EM_ASM({
            // Disable Safari's heavy memory pressure warnings
            if (window.webkit && window.webkit.messageHandlers) {
                // Running in WKWebView
            }

            // Request high-priority rendering
            if (document.body) {
                document.body.style.webkitTransform = 'translateZ(0)';
            }

            // Setup visibility change handler
            document.addEventListener('visibilitychange', function() {
                if (Module._wasm_handle_visibility_change) {
                    Module._wasm_handle_visibility_change(
                        document.visibilityState === 'visible' ? 1 : 0
                    );
                }
            });

            // ProMotion detection (120Hz)
            if (window.screen && window.screen.refreshRate === 120) {
                if (Module._wasm_ios_set_target_fps) {
                    Module._wasm_ios_set_target_fps(120);
                }
            }
        });
#endif
    }
}

#ifdef __EMSCRIPTEN__
EMSCRIPTEN_KEEPALIVE
#endif
void wasm_ios_set_target_fps(int fps)
{
    if (wasm_display_state) {
        wasm_display_state->target_fps = fps;
    }
}

#ifdef __EMSCRIPTEN__
EMSCRIPTEN_KEEPALIVE
#endif
void wasm_ios_low_power_mode(bool enable)
{
    if (!wasm_display_state) {
        return;
    }
    wasm_display_state->low_power_mode = enable;

#ifdef __EMSCRIPTEN__
    EM_ASM({
        if (window.onWasmLowPowerModeChange) {
            window.onWasmLowPowerModeChange($0);
        }
    }, enable ? 1 : 0);
#endif
}

#ifdef __EMSCRIPTEN__
EMSCRIPTEN_KEEPALIVE
#endif
void wasm_handle_visibility_change(bool visible)
{
    if (!wasm_display_state) {
        return;
    }
    wasm_display_state->is_visible = visible;

#ifdef __EMSCRIPTEN__
    EM_ASM({
        if (window.onWasmVisibilityChange) {
            window.onWasmVisibilityChange($0);
        }
    }, visible ? 1 : 0);
#endif
}

/* ------------------------------------------------------------------ */
/* Performance & debugging                                            */
/* ------------------------------------------------------------------ */

#ifdef __EMSCRIPTEN__
EMSCRIPTEN_KEEPALIVE
#endif
WasmPerfStats *wasm_get_perf_stats(void)
{
    return wasm_display_state ? &wasm_display_state->perf_stats : NULL;
}

#ifdef __EMSCRIPTEN__
EMSCRIPTEN_KEEPALIVE
#endif
void wasm_reset_perf_stats(void)
{
    if (wasm_display_state) {
        memset(&wasm_display_state->perf_stats, 0, sizeof(WasmPerfStats));
        wasm_display_state->frame_time_accum = 0;
        wasm_display_state->frame_time_count = 0;
    }
}

#ifdef __EMSCRIPTEN__
EMSCRIPTEN_KEEPALIVE
#endif
void wasm_enable_profiling(bool enable)
{
    if (wasm_display_state) {
        wasm_display_state->profiling_enabled = enable;
    }
}

/* ------------------------------------------------------------------ */
/* DisplayChangeListener callbacks                                    */
/* ------------------------------------------------------------------ */

static void wasm_refresh(DisplayChangeListener *dcl)
{
    graphic_hw_update(dcl->con);
}

static void wasm_gfx_update(DisplayChangeListener *dcl,
                            int x, int y, int w, int h)
{
    WasmDisplayState *wds = container_of(dcl, WasmDisplayState, dcl);
    DisplaySurface *surface = wds->ds;
    int64_t start_time = 0;

    if (!surface || !wds->fb_data) {
        return;
    }

    /* Skip rendering if not visible (iOS optimization) */
    if (wds->ios_optimizations && !wds->is_visible) {
        return;
    }

    if (wds->profiling_enabled) {
        start_time = qemu_clock_get_us(QEMU_CLOCK_REALTIME);
    }

    int src_stride = surface_stride(surface);
    int dst_stride = wds->fb_info.stride;
    uint8_t *src = (uint8_t *)surface_data(surface);
    uint8_t *dst = wds->fb_data;
    int bpp = surface_bytes_per_pixel(surface);

    /* Clip update region */
    int max_x = MIN(x + w, surface_width(surface));
    int max_y = MIN(y + h, surface_height(surface));
    x = MAX(0, x);
    y = MAX(0, y);

    /* iOS Safari optimization: use memcpy for full-width updates */
    bool use_fast_path = wds->ios_optimizations &&
                         x == 0 && max_x == surface_width(surface) && bpp == 4;

    if (use_fast_path) {
        /* Fast path: bulk copy for full-width rows */
        for (int row = y; row < max_y; row++) {
            uint8_t *src_line = src + row * src_stride;
            uint8_t *dst_line = dst + row * dst_stride;

            /* SIMD-friendly conversion: BGRX to RGBA */
            for (int col = 0; col < max_x; col += 4) {
                /* Process 4 pixels at a time when possible */
                for (int p = 0; p < 4 && col + p < max_x; p++) {
                    uint32_t pixel = ((uint32_t *)src_line)[col + p];
                    uint8_t *d = dst_line + (col + p) * 4;
                    d[0] = (pixel >> 16) & 0xFF;
                    d[1] = (pixel >> 8) & 0xFF;
                    d[2] = pixel & 0xFF;
                    d[3] = 0xFF;
                }
            }
        }
    } else {
        /* Standard path: row-by-row with format conversion */
        for (int row = y; row < max_y; row++) {
            uint8_t *src_line = src + row * src_stride + x * bpp;
            uint8_t *dst_line = dst + row * dst_stride + x * 4;

            if (bpp == 4) {
                for (int col = x; col < max_x; col++) {
                    uint32_t pixel = *(uint32_t *)src_line;
                    dst_line[0] = (pixel >> 16) & 0xFF;
                    dst_line[1] = (pixel >> 8) & 0xFF;
                    dst_line[2] = pixel & 0xFF;
                    dst_line[3] = 0xFF;
                    src_line += 4;
                    dst_line += 4;
                }
            } else {
                memcpy(dst_line, src_line, (max_x - x) * bpp);
            }
        }
    }

    /* Update dirty region tracking */
    WasmFramebufferInfo *fb = &wds->fb_info;
    if (!fb->dirty) {
        fb->dirty_x = x;
        fb->dirty_y = y;
        fb->dirty_width = max_x - x;
        fb->dirty_height = max_y - y;
    } else {
        int32_t x2 = MAX(fb->dirty_x + fb->dirty_width, max_x);
        int32_t y2 = MAX(fb->dirty_y + fb->dirty_height, max_y);
        fb->dirty_x = MIN(fb->dirty_x, x);
        fb->dirty_y = MIN(fb->dirty_y, y);
        fb->dirty_width = x2 - fb->dirty_x;
        fb->dirty_height = y2 - fb->dirty_y;
    }

    fb->dirty = true;
    fb->frame_count++;

    /* Update performance stats */
    if (wds->profiling_enabled) {
        int64_t end_time = qemu_clock_get_us(QEMU_CLOCK_REALTIME);
        int64_t copy_time = end_time - start_time;
        wds->frame_time_accum += copy_time;
        wds->frame_time_count++;
        if (wds->frame_time_count >= 60) {
            wds->perf_stats.avg_copy_time_ms =
                (double)wds->frame_time_accum / wds->frame_time_count / 1000.0;
            wds->frame_time_accum = 0;
            wds->frame_time_count = 0;
        }
        wds->perf_stats.bytes_transferred += (max_y - y) * dst_stride;
    }

#ifdef __EMSCRIPTEN__
    /* Notify JavaScript that framebuffer was updated */
    EM_ASM({
        if (typeof window !== 'undefined' && window.onWasmFramebufferUpdate) {
            window.onWasmFramebufferUpdate($0, $1, $2, $3);
        }
    }, x, y, max_x - x, max_y - y);
#endif
}

static void wasm_gfx_switch(DisplayChangeListener *dcl,
                            struct DisplaySurface *new_surface)
{
    WasmDisplayState *wds = container_of(dcl, WasmDisplayState, dcl);

    wds->ds = new_surface;

    if (!new_surface) {
        return;
    }

    int width = surface_width(new_surface);
    int height = surface_height(new_surface);
    int stride = width * 4;
    size_t size = stride * height;

    if (width > WASM_FB_MAX_WIDTH || height > WASM_FB_MAX_HEIGHT) {
        fprintf(stderr, "wasm-display: resolution %dx%d exceeds maximum %dx%d\n",
                width, height, WASM_FB_MAX_WIDTH, WASM_FB_MAX_HEIGHT);
        return;
    }

    /* Reallocate buffer if needed */
    if (size > wds->fb_allocated_size) {
        g_free(wds->fb_data);
        wds->fb_data = g_malloc0(size);
        wds->fb_allocated_size = size;
    }

    /* Update framebuffer info */
    wds->fb_info.data = wds->fb_data;
    wds->fb_info.width = width;
    wds->fb_info.height = height;
    wds->fb_info.stride = stride;
    wds->fb_info.bpp = 32;
    wds->fb_info.format = surface_format(new_surface);
    wds->fb_info.dirty = true;
    wds->fb_info.dirty_x = 0;
    wds->fb_info.dirty_y = 0;
    wds->fb_info.dirty_width = width;
    wds->fb_info.dirty_height = height;
    wds->fb_info.frame_count++;

    /* Update WebGPU texture if needed */
    if (wds->webgpu_initialized) {
        wds->webgpu_texture.width = width;
        wds->webgpu_texture.height = height;
        wds->webgpu_texture.needs_upload = true;

#ifdef __EMSCRIPTEN__
        EM_ASM({
            if (Module._webgpu_device && Module._webgpu_texture) {
                Module._webgpu_texture.destroy();
                Module._webgpu_texture = Module._webgpu_device.createTexture({
                    size: [$0, $1],
                    format: 'rgba8unorm',
                    usage: GPUTextureUsage.TEXTURE_BINDING |
                           GPUTextureUsage.COPY_DST |
                           GPUTextureUsage.RENDER_ATTACHMENT
                });
            }
        }, width, height);
#endif
    }

#ifdef __EMSCRIPTEN__
    EM_ASM({
        if (typeof window !== 'undefined' && window.onWasmFramebufferResize) {
            window.onWasmFramebufferResize($0, $1);
        }
    }, width, height);
#endif

    wasm_gfx_update(dcl, 0, 0, width, height);
}

static void wasm_mouse_set(DisplayChangeListener *dcl, int x, int y, int on)
{
    WasmDisplayState *wds = container_of(dcl, WasmDisplayState, dcl);
    wds->mouse_x = x;
    wds->mouse_y = y;

#ifdef __EMSCRIPTEN__
    EM_ASM({
        if (typeof window !== 'undefined' && window.onWasmMouseUpdate) {
            window.onWasmMouseUpdate($0, $1, $2);
        }
    }, x, y, on);
#endif
}

static void wasm_cursor_define(DisplayChangeListener *dcl, QEMUCursor *cursor)
{
#ifdef __EMSCRIPTEN__
    if (cursor) {
        EM_ASM({
            if (typeof window !== 'undefined' && window.onWasmCursorDefine) {
                window.onWasmCursorDefine($0, $1, $2, $3, $4);
            }
        }, cursor->width, cursor->height, cursor->hot_x, cursor->hot_y,
           (int)(uintptr_t)cursor->data);
    }
#endif
}

static const DisplayChangeListenerOps wasm_display_ops = {
    .dpy_name          = "wasm",
    .dpy_refresh       = wasm_refresh,
    .dpy_gfx_update    = wasm_gfx_update,
    .dpy_gfx_switch    = wasm_gfx_switch,
    .dpy_mouse_set     = wasm_mouse_set,
    .dpy_cursor_define = wasm_cursor_define,
};

/* ------------------------------------------------------------------ */
/* Display initialization                                             */
/* ------------------------------------------------------------------ */

static void wasm_display_init(DisplayState *ds, DisplayOptions *opts)
{
    QemuConsole *con;
    WasmDisplayState *wds;

    wds = g_new0(WasmDisplayState, 1);

    /* Allocate initial framebuffer */
    wds->fb_allocated_size = WASM_FB_DEFAULT_WIDTH * WASM_FB_DEFAULT_HEIGHT * 4;
    wds->fb_data = g_malloc0(wds->fb_allocated_size);

    /* Initialize framebuffer info */
    wds->fb_info.data = wds->fb_data;
    wds->fb_info.width = WASM_FB_DEFAULT_WIDTH;
    wds->fb_info.height = WASM_FB_DEFAULT_HEIGHT;
    wds->fb_info.stride = WASM_FB_DEFAULT_WIDTH * 4;
    wds->fb_info.bpp = 32;
    wds->fb_info.dirty = false;
    wds->fb_info.frame_count = 0;

    /* Default settings */
    wds->render_backend = WASM_RENDER_CANVAS2D;
    wds->is_visible = true;
    wds->target_fps = 60;

    /* Initialize keyboard state */
    wds->kbd = qkbd_state_init(NULL);

    /* Find first graphic console */
    con = qemu_console_lookup_by_index(0);
    if (!con || !qemu_console_is_graphic(con)) {
        fprintf(stderr, "wasm-display: no graphic console found\n");
        g_free(wds->fb_data);
        g_free(wds);
        return;
    }

    wds->dcl.con = con;
    wds->dcl.ops = &wasm_display_ops;

    register_displaychangelistener(&wds->dcl);

    /* Store global reference for JavaScript access */
    wasm_display_state = wds;

#ifdef __EMSCRIPTEN__
    /* Detect browser capabilities */
    wasm_detect_capabilities();

    /* Auto-enable iOS optimizations if detected */
    if (wasm_caps.is_ios_safari) {
        wasm_ios_safari_optimize(true);
    }

    /* Notify JavaScript that display is ready */
    EM_ASM({
        if (typeof window !== 'undefined' && window.onWasmDisplayReady) {
            window.onWasmDisplayReady();
        }
    });
#endif

    fprintf(stderr, "wasm-display: initialized with %dx%d framebuffer "
            "(WebGL: %s, WebGPU: %s, iOS: %s)\n",
            wds->fb_info.width, wds->fb_info.height,
            wasm_caps.webgl_available ? "yes" : "no",
            wasm_caps.webgpu_available ? "yes" : "no",
            wasm_caps.is_ios_safari ? "yes" : "no");
}

static QemuDisplay qemu_display_wasm = {
#ifdef CONFIG_WASM_DISPLAY
    .type       = DISPLAY_TYPE_WASM,
#else
    .type       = DISPLAY_TYPE_NONE,
#endif
    .init       = wasm_display_init,
};

static void register_wasm_display(void)
{
    qemu_display_register(&qemu_display_wasm);
}

type_init(register_wasm_display);
