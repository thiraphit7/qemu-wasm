/*
 * QEMU WASM VirtIO-GPU WebGPU Hooks Implementation
 *
 * Provides WebGPU/WebGL acceleration for VirtIO-GPU devices.
 * Experimental support for QEMU 10.1+ WebGPU over WebGL fallback.
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
#include "qapi/error.h"
#include "ui/wasm-virtio-gpu.h"

#ifdef __EMSCRIPTEN__
#include <emscripten.h>
#include <emscripten/html5.h>
#include <emscripten/html5_webgpu.h>
#endif

/* Maximum resources */
#define WASM_GPU_MAX_RESOURCES 4096
#define WASM_GPU_MAX_CONTEXTS  256
#define WASM_GPU_MAX_SCANOUTS  16
#define WASM_GPU_MAX_FENCES    1024

/* ------------------------------------------------------------------ */
/* Global State                                                        */
/* ------------------------------------------------------------------ */

typedef struct WasmGpuState {
    bool initialized;
    WasmGpuCapabilities caps;
    WasmGpuStats stats;

    /* Resource tracking */
    WasmGpuResource *resources[WASM_GPU_MAX_RESOURCES];
    uint32_t resource_count;

    /* 3D context tracking */
    WasmGpu3DContext *contexts[WASM_GPU_MAX_CONTEXTS];
    uint32_t context_count;

    /* Scanout tracking */
    WasmGpuScanout scanouts[WASM_GPU_MAX_SCANOUTS];

    /* Fence tracking */
    struct {
        uint64_t fence_id;
        uint32_t ctx_id;
        bool signaled;
    } fences[WASM_GPU_MAX_FENCES];
    uint32_t fence_count;

} WasmGpuState;

static WasmGpuState *wasm_gpu_state = NULL;

/* ------------------------------------------------------------------ */
/* JavaScript Interop via Emscripten                                   */
/* ------------------------------------------------------------------ */

#ifdef __EMSCRIPTEN__

/* Detect WebGPU/WebGL capabilities */
EM_JS(int, js_detect_gpu_backend, (), {
    if (typeof navigator !== 'undefined' && navigator.gpu) {
        return 4; /* WASM_GPU_BACKEND_WEBGPU */
    }
    var canvas = document.createElement('canvas');
    var gl2 = canvas.getContext('webgl2');
    if (gl2) {
        return 3; /* WASM_GPU_BACKEND_WEBGL2 */
    }
    var gl = canvas.getContext('webgl') || canvas.getContext('experimental-webgl');
    if (gl) {
        return 2; /* WASM_GPU_BACKEND_WEBGL */
    }
    return 1; /* WASM_GPU_BACKEND_CANVAS2D */
});

/* Get GPU features */
EM_JS(int, js_get_gpu_features, (), {
    var features = 0;
    var canvas = document.createElement('canvas');
    var gl = canvas.getContext('webgl2');
    if (gl) {
        features |= 1;  /* TEXTURE_3D */
        if (gl.getExtension('EXT_color_buffer_float')) {
            features |= 8; /* FLOAT32 */
        }
    }
    if (typeof navigator !== 'undefined' && navigator.gpu) {
        features |= 2;  /* COMPUTE */
        features |= 4;  /* STORAGE_BUFFER */
        features |= 32; /* INDIRECT_DRAW */
    }
    return features;
});

/* Get max texture size */
EM_JS(int, js_get_max_texture_size, (), {
    var canvas = document.createElement('canvas');
    var gl = canvas.getContext('webgl2') || canvas.getContext('webgl');
    if (gl) {
        return gl.getParameter(gl.MAX_TEXTURE_SIZE);
    }
    return 4096; /* Safe default */
});

/* Get renderer string */
EM_JS(void, js_get_renderer_info, (char *vendor, int vendor_len,
                                    char *renderer, int renderer_len), {
    var canvas = document.createElement('canvas');
    var gl = canvas.getContext('webgl2') || canvas.getContext('webgl');
    if (gl) {
        var dbg = gl.getExtension('WEBGL_debug_renderer_info');
        var v = dbg ? gl.getParameter(dbg.UNMASKED_VENDOR_WEBGL) : 'Unknown';
        var r = dbg ? gl.getParameter(dbg.UNMASKED_RENDERER_WEBGL) : 'Unknown';
        stringToUTF8(v.substring(0, vendor_len - 1), vendor, vendor_len);
        stringToUTF8(r.substring(0, renderer_len - 1), renderer, renderer_len);
    } else {
        stringToUTF8('Unknown', vendor, vendor_len);
        stringToUTF8('Canvas2D', renderer, renderer_len);
    }
});

/* Create texture in JavaScript */
EM_JS(int, js_create_texture, (int resource_id, int width, int height, int format), {
    if (!window._wasmGpuTextures) {
        window._wasmGpuTextures = {};
    }
    window._wasmGpuTextures[resource_id] = {
        width: width,
        height: height,
        format: format,
        data: null,
        dirty: false
    };
    if (window.onWasmGpuResourceCreate) {
        window.onWasmGpuResourceCreate(resource_id, width, height, format);
    }
    return 0;
});

/* Destroy texture in JavaScript */
EM_JS(void, js_destroy_texture, (int resource_id), {
    if (window._wasmGpuTextures && window._wasmGpuTextures[resource_id]) {
        delete window._wasmGpuTextures[resource_id];
    }
    if (window.onWasmGpuResourceDestroy) {
        window.onWasmGpuResourceDestroy(resource_id);
    }
});

/* Upload texture data */
EM_JS(int, js_upload_texture, (int resource_id, int x, int y,
                                int width, int height, void *data, int size), {
    if (!window._wasmGpuTextures || !window._wasmGpuTextures[resource_id]) {
        return -1;
    }
    var tex = window._wasmGpuTextures[resource_id];
    tex.data = new Uint8Array(HEAPU8.buffer, data, size).slice();
    tex.dirty = true;
    tex.updateRegion = { x: x, y: y, width: width, height: height };

    if (window.onWasmGpuTextureUpload) {
        window.onWasmGpuTextureUpload(resource_id, x, y, width, height, tex.data);
    }
    return 0;
});

/* Flush to display */
EM_JS(void, js_flush_scanout, (int scanout_id, int resource_id), {
    if (window.onWasmGpuFlush) {
        window.onWasmGpuFlush(scanout_id, resource_id);
    }
});

/* Update cursor */
EM_JS(void, js_update_cursor, (int resource_id, int hot_x, int hot_y,
                                void *data, int width, int height), {
    if (window.onWasmGpuCursorUpdate) {
        var cursorData = null;
        if (data && width > 0 && height > 0) {
            cursorData = new Uint8Array(HEAPU8.buffer, data, width * height * 4).slice();
        }
        window.onWasmGpuCursorUpdate(resource_id, hot_x, hot_y, cursorData, width, height);
    }
});

/* Notify GPU init */
EM_JS(void, js_notify_gpu_init, (int backend, int features), {
    console.log('WASM GPU: Initialized with backend=' + backend + ', features=0x' + features.toString(16));
    if (window.onWasmGpuInit) {
        window.onWasmGpuInit(backend, features);
    }
});

/* Check if WebGPU compat mode (WebGL fallback) is available */
EM_JS(int, js_check_webgpu_compat, (), {
    /* Check for WebGPU with WebGL fallback (experimental in Chrome) */
    if (typeof navigator !== 'undefined' && navigator.gpu) {
        /* Check if adapter supports compatibility mode */
        return 1;
    }
    return 0;
});

#endif /* __EMSCRIPTEN__ */

/* ------------------------------------------------------------------ */
/* Initialization and Shutdown                                         */
/* ------------------------------------------------------------------ */

#ifdef __EMSCRIPTEN__
EMSCRIPTEN_KEEPALIVE
#endif
WasmGpuCapabilities *wasm_gpu_init(WasmGpuBackendType preferred)
{
    if (wasm_gpu_state && wasm_gpu_state->initialized) {
        return &wasm_gpu_state->caps;
    }

    wasm_gpu_state = g_new0(WasmGpuState, 1);

#ifdef __EMSCRIPTEN__
    /* Detect available backend */
    int detected = js_detect_gpu_backend();

    /* Use preferred if available, otherwise fall back */
    if (preferred == WASM_GPU_BACKEND_WEBGPU && detected < WASM_GPU_BACKEND_WEBGPU) {
        /* Check for WebGPU compatibility mode */
        if (js_check_webgpu_compat()) {
            wasm_gpu_state->caps.backend = WASM_GPU_BACKEND_WEBGPU_COMPAT;
        } else {
            wasm_gpu_state->caps.backend = (WasmGpuBackendType)detected;
        }
    } else if (preferred != WASM_GPU_BACKEND_NONE && preferred <= detected) {
        wasm_gpu_state->caps.backend = preferred;
    } else {
        wasm_gpu_state->caps.backend = (WasmGpuBackendType)detected;
    }

    /* Get features */
    wasm_gpu_state->caps.features = js_get_gpu_features();
    wasm_gpu_state->caps.max_texture_size = js_get_max_texture_size();
    wasm_gpu_state->caps.max_texture_layers = 256;
    wasm_gpu_state->caps.max_buffer_size = 256 * 1024 * 1024; /* 256MB */
    wasm_gpu_state->caps.max_uniform_buffer_size = 64 * 1024;

    /* Get renderer info */
    js_get_renderer_info(wasm_gpu_state->caps.vendor,
                         sizeof(wasm_gpu_state->caps.vendor),
                         wasm_gpu_state->caps.renderer,
                         sizeof(wasm_gpu_state->caps.renderer));

    /* Set feature flags based on backend */
    if (wasm_gpu_state->caps.backend >= WASM_GPU_BACKEND_WEBGPU) {
        wasm_gpu_state->caps.supports_virgl = true;
        wasm_gpu_state->caps.supports_blob = true;
        wasm_gpu_state->caps.max_compute_workgroup_size[0] = 256;
        wasm_gpu_state->caps.max_compute_workgroup_size[1] = 256;
        wasm_gpu_state->caps.max_compute_workgroup_size[2] = 64;
        wasm_gpu_state->caps.max_compute_workgroups[0] = 65535;
        wasm_gpu_state->caps.max_compute_workgroups[1] = 65535;
        wasm_gpu_state->caps.max_compute_workgroups[2] = 65535;
    }

    /* Notify JavaScript */
    js_notify_gpu_init(wasm_gpu_state->caps.backend, wasm_gpu_state->caps.features);

#else
    /* Non-Emscripten build - use Canvas2D fallback */
    wasm_gpu_state->caps.backend = WASM_GPU_BACKEND_CANVAS2D;
    wasm_gpu_state->caps.max_texture_size = 4096;
    g_strlcpy(wasm_gpu_state->caps.vendor, "Software", sizeof(wasm_gpu_state->caps.vendor));
    g_strlcpy(wasm_gpu_state->caps.renderer, "Canvas2D", sizeof(wasm_gpu_state->caps.renderer));
#endif

    wasm_gpu_state->initialized = true;

    fprintf(stderr, "wasm-virtio-gpu: initialized backend=%d (%s)\n",
            wasm_gpu_state->caps.backend, wasm_gpu_state->caps.renderer);

    return &wasm_gpu_state->caps;
}

#ifdef __EMSCRIPTEN__
EMSCRIPTEN_KEEPALIVE
#endif
void wasm_gpu_shutdown(void)
{
    if (!wasm_gpu_state) {
        return;
    }

    /* Cleanup resources */
    for (uint32_t i = 0; i < WASM_GPU_MAX_RESOURCES; i++) {
        if (wasm_gpu_state->resources[i]) {
            wasm_gpu_resource_destroy(i);
        }
    }

    /* Cleanup contexts */
    for (uint32_t i = 0; i < WASM_GPU_MAX_CONTEXTS; i++) {
        if (wasm_gpu_state->contexts[i]) {
            wasm_gpu_ctx_destroy(i);
        }
    }

    g_free(wasm_gpu_state);
    wasm_gpu_state = NULL;

    fprintf(stderr, "wasm-virtio-gpu: shutdown complete\n");
}

#ifdef __EMSCRIPTEN__
EMSCRIPTEN_KEEPALIVE
#endif
WasmGpuCapabilities *wasm_gpu_get_capabilities(void)
{
    if (!wasm_gpu_state) {
        return NULL;
    }
    return &wasm_gpu_state->caps;
}

/* ------------------------------------------------------------------ */
/* Resource Management                                                 */
/* ------------------------------------------------------------------ */

#ifdef __EMSCRIPTEN__
EMSCRIPTEN_KEEPALIVE
#endif
int wasm_gpu_resource_create(WasmGpuResource *res)
{
    if (!wasm_gpu_state || !res || res->resource_id >= WASM_GPU_MAX_RESOURCES) {
        return -1;
    }

    if (wasm_gpu_state->resources[res->resource_id]) {
        /* Resource already exists */
        return -1;
    }

    WasmGpuResource *new_res = g_new0(WasmGpuResource, 1);
    memcpy(new_res, res, sizeof(WasmGpuResource));

    wasm_gpu_state->resources[res->resource_id] = new_res;
    wasm_gpu_state->resource_count++;
    wasm_gpu_state->stats.resources_allocated++;

#ifdef __EMSCRIPTEN__
    js_create_texture(res->resource_id, res->width, res->height, res->format);
#endif

    return 0;
}

#ifdef __EMSCRIPTEN__
EMSCRIPTEN_KEEPALIVE
#endif
void wasm_gpu_resource_destroy(uint32_t resource_id)
{
    if (!wasm_gpu_state || resource_id >= WASM_GPU_MAX_RESOURCES) {
        return;
    }

    WasmGpuResource *res = wasm_gpu_state->resources[resource_id];
    if (!res) {
        return;
    }

#ifdef __EMSCRIPTEN__
    js_destroy_texture(resource_id);
#endif

    g_free(res->host_ptr);
    g_free(res);
    wasm_gpu_state->resources[resource_id] = NULL;
    wasm_gpu_state->resource_count--;
}

#ifdef __EMSCRIPTEN__
EMSCRIPTEN_KEEPALIVE
#endif
int wasm_gpu_resource_attach_backing(uint32_t resource_id,
                                     void *data, size_t size)
{
    if (!wasm_gpu_state || resource_id >= WASM_GPU_MAX_RESOURCES) {
        return -1;
    }

    WasmGpuResource *res = wasm_gpu_state->resources[resource_id];
    if (!res) {
        return -1;
    }

    res->host_ptr = data;
    res->size = size;
    res->is_dirty = true;

    wasm_gpu_state->stats.texture_memory += size;

    return 0;
}

#ifdef __EMSCRIPTEN__
EMSCRIPTEN_KEEPALIVE
#endif
void wasm_gpu_resource_detach_backing(uint32_t resource_id)
{
    if (!wasm_gpu_state || resource_id >= WASM_GPU_MAX_RESOURCES) {
        return;
    }

    WasmGpuResource *res = wasm_gpu_state->resources[resource_id];
    if (!res) {
        return;
    }

    if (res->size > 0) {
        wasm_gpu_state->stats.texture_memory -= res->size;
    }
    res->host_ptr = NULL;
    res->size = 0;
}

#ifdef __EMSCRIPTEN__
EMSCRIPTEN_KEEPALIVE
#endif
int wasm_gpu_transfer_to_host(uint32_t resource_id,
                              uint32_t x, uint32_t y,
                              uint32_t width, uint32_t height,
                              uint64_t offset)
{
    if (!wasm_gpu_state || resource_id >= WASM_GPU_MAX_RESOURCES) {
        return -1;
    }

    WasmGpuResource *res = wasm_gpu_state->resources[resource_id];
    if (!res || !res->host_ptr) {
        return -1;
    }

#ifdef __EMSCRIPTEN__
    uint8_t *data = (uint8_t *)res->host_ptr + offset;
    size_t size = width * height * 4; /* Assume RGBA */

    js_upload_texture(resource_id, x, y, width, height, data, size);

    wasm_gpu_state->stats.bytes_uploaded += size;
#endif

    res->is_dirty = false;
    return 0;
}

#ifdef __EMSCRIPTEN__
EMSCRIPTEN_KEEPALIVE
#endif
int wasm_gpu_transfer_from_host(uint32_t resource_id,
                                uint32_t x, uint32_t y,
                                uint32_t width, uint32_t height,
                                uint64_t offset)
{
    if (!wasm_gpu_state || resource_id >= WASM_GPU_MAX_RESOURCES) {
        return -1;
    }

    WasmGpuResource *res = wasm_gpu_state->resources[resource_id];
    if (!res || !res->host_ptr) {
        return -1;
    }

    /* Download is handled through JavaScript callbacks */
    wasm_gpu_state->stats.bytes_downloaded += width * height * 4;
    return 0;
}

#ifdef __EMSCRIPTEN__
EMSCRIPTEN_KEEPALIVE
#endif
int wasm_gpu_resource_flush(uint32_t resource_id,
                            uint32_t x, uint32_t y,
                            uint32_t width, uint32_t height)
{
    if (!wasm_gpu_state || resource_id >= WASM_GPU_MAX_RESOURCES) {
        return -1;
    }

    WasmGpuResource *res = wasm_gpu_state->resources[resource_id];
    if (!res) {
        return -1;
    }

    /* Find scanout using this resource */
    for (int i = 0; i < WASM_GPU_MAX_SCANOUTS; i++) {
        if (wasm_gpu_state->scanouts[i].enabled &&
            wasm_gpu_state->scanouts[i].resource_id == resource_id) {

#ifdef __EMSCRIPTEN__
            js_flush_scanout(i, resource_id);
#endif
            wasm_gpu_state->stats.frames_rendered++;
        }
    }

    return 0;
}

#ifdef __EMSCRIPTEN__
EMSCRIPTEN_KEEPALIVE
#endif
int wasm_gpu_set_scanout(WasmGpuScanout *scanout)
{
    if (!wasm_gpu_state || !scanout ||
        scanout->scanout_id >= WASM_GPU_MAX_SCANOUTS) {
        return -1;
    }

    memcpy(&wasm_gpu_state->scanouts[scanout->scanout_id],
           scanout, sizeof(WasmGpuScanout));

    return 0;
}

#ifdef __EMSCRIPTEN__
EMSCRIPTEN_KEEPALIVE
#endif
int wasm_gpu_cursor_update(uint32_t resource_id,
                           uint32_t hot_x, uint32_t hot_y)
{
    if (!wasm_gpu_state || resource_id >= WASM_GPU_MAX_RESOURCES) {
        return -1;
    }

    WasmGpuResource *res = wasm_gpu_state->resources[resource_id];

#ifdef __EMSCRIPTEN__
    if (res && res->host_ptr) {
        js_update_cursor(resource_id, hot_x, hot_y,
                         res->host_ptr, res->width, res->height);
    } else {
        js_update_cursor(resource_id, hot_x, hot_y, NULL, 0, 0);
    }
#endif

    return 0;
}

#ifdef __EMSCRIPTEN__
EMSCRIPTEN_KEEPALIVE
#endif
void wasm_gpu_cursor_move(uint32_t scanout_id, uint32_t x, uint32_t y)
{
#ifdef __EMSCRIPTEN__
    EM_ASM({
        if (window.onWasmGpuCursorMove) {
            window.onWasmGpuCursorMove($0, $1, $2);
        }
    }, scanout_id, x, y);
#endif
}

/* ------------------------------------------------------------------ */
/* 3D Context Management                                               */
/* ------------------------------------------------------------------ */

#ifdef __EMSCRIPTEN__
EMSCRIPTEN_KEEPALIVE
#endif
int wasm_gpu_ctx_create(uint32_t ctx_id, uint32_t capset_id,
                        const char *debug_name)
{
    if (!wasm_gpu_state || ctx_id >= WASM_GPU_MAX_CONTEXTS) {
        return -1;
    }

    if (wasm_gpu_state->contexts[ctx_id]) {
        return -1; /* Context exists */
    }

    WasmGpu3DContext *ctx = g_new0(WasmGpu3DContext, 1);
    ctx->ctx_id = ctx_id;
    ctx->capset_id = capset_id;
    ctx->active = true;

    wasm_gpu_state->contexts[ctx_id] = ctx;
    wasm_gpu_state->context_count++;

#ifdef __EMSCRIPTEN__
    EM_ASM({
        if (window.onWasmGpu3DContextCreate) {
            window.onWasmGpu3DContextCreate($0, $1, UTF8ToString($2));
        }
    }, ctx_id, capset_id, debug_name ? debug_name : "");
#endif

    return 0;
}

#ifdef __EMSCRIPTEN__
EMSCRIPTEN_KEEPALIVE
#endif
void wasm_gpu_ctx_destroy(uint32_t ctx_id)
{
    if (!wasm_gpu_state || ctx_id >= WASM_GPU_MAX_CONTEXTS) {
        return;
    }

    WasmGpu3DContext *ctx = wasm_gpu_state->contexts[ctx_id];
    if (!ctx) {
        return;
    }

#ifdef __EMSCRIPTEN__
    EM_ASM({
        if (window.onWasmGpu3DContextDestroy) {
            window.onWasmGpu3DContextDestroy($0);
        }
    }, ctx_id);
#endif

    g_free(ctx);
    wasm_gpu_state->contexts[ctx_id] = NULL;
    wasm_gpu_state->context_count--;
}

#ifdef __EMSCRIPTEN__
EMSCRIPTEN_KEEPALIVE
#endif
int wasm_gpu_ctx_attach_resource(uint32_t ctx_id, uint32_t resource_id)
{
    if (!wasm_gpu_state || ctx_id >= WASM_GPU_MAX_CONTEXTS ||
        resource_id >= WASM_GPU_MAX_RESOURCES) {
        return -1;
    }

#ifdef __EMSCRIPTEN__
    EM_ASM({
        if (window.onWasmGpu3DAttachResource) {
            window.onWasmGpu3DAttachResource($0, $1);
        }
    }, ctx_id, resource_id);
#endif

    return 0;
}

#ifdef __EMSCRIPTEN__
EMSCRIPTEN_KEEPALIVE
#endif
void wasm_gpu_ctx_detach_resource(uint32_t ctx_id, uint32_t resource_id)
{
    if (!wasm_gpu_state || ctx_id >= WASM_GPU_MAX_CONTEXTS ||
        resource_id >= WASM_GPU_MAX_RESOURCES) {
        return;
    }

#ifdef __EMSCRIPTEN__
    EM_ASM({
        if (window.onWasmGpu3DDetachResource) {
            window.onWasmGpu3DDetachResource($0, $1);
        }
    }, ctx_id, resource_id);
#endif
}

#ifdef __EMSCRIPTEN__
EMSCRIPTEN_KEEPALIVE
#endif
int wasm_gpu_submit_3d(uint32_t ctx_id, void *cmd_buf, size_t cmd_size)
{
    if (!wasm_gpu_state || ctx_id >= WASM_GPU_MAX_CONTEXTS || !cmd_buf) {
        return -1;
    }

    wasm_gpu_state->stats.commands_submitted++;

#ifdef __EMSCRIPTEN__
    EM_ASM({
        if (window.onWasmGpu3DSubmit) {
            var cmdData = new Uint8Array(HEAPU8.buffer, $1, $2).slice();
            window.onWasmGpu3DSubmit($0, cmdData);
        }
    }, ctx_id, cmd_buf, cmd_size);
#endif

    return 0;
}

#ifdef __EMSCRIPTEN__
EMSCRIPTEN_KEEPALIVE
#endif
int wasm_gpu_create_fence(uint32_t ctx_id, uint64_t fence_id)
{
    if (!wasm_gpu_state) {
        return -1;
    }

    /* Find free fence slot */
    for (uint32_t i = 0; i < WASM_GPU_MAX_FENCES; i++) {
        if (wasm_gpu_state->fences[i].fence_id == 0) {
            wasm_gpu_state->fences[i].fence_id = fence_id;
            wasm_gpu_state->fences[i].ctx_id = ctx_id;
            wasm_gpu_state->fences[i].signaled = false;
            wasm_gpu_state->fence_count++;
            wasm_gpu_state->stats.pending_fences++;
            return 0;
        }
    }

    return -1; /* No free slots */
}

#ifdef __EMSCRIPTEN__
EMSCRIPTEN_KEEPALIVE
#endif
bool wasm_gpu_fence_is_signaled(uint64_t fence_id)
{
    if (!wasm_gpu_state) {
        return true; /* Consider signaled if no state */
    }

    for (uint32_t i = 0; i < WASM_GPU_MAX_FENCES; i++) {
        if (wasm_gpu_state->fences[i].fence_id == fence_id) {
            if (wasm_gpu_state->fences[i].signaled) {
                /* Clear the fence */
                wasm_gpu_state->fences[i].fence_id = 0;
                wasm_gpu_state->fence_count--;
                wasm_gpu_state->stats.pending_fences--;
                return true;
            }
            return false;
        }
    }

    return true; /* Unknown fence considered signaled */
}

/* Signal fence from JavaScript */
#ifdef __EMSCRIPTEN__
EMSCRIPTEN_KEEPALIVE
#endif
void wasm_gpu_signal_fence(uint64_t fence_id)
{
    if (!wasm_gpu_state) {
        return;
    }

    for (uint32_t i = 0; i < WASM_GPU_MAX_FENCES; i++) {
        if (wasm_gpu_state->fences[i].fence_id == fence_id) {
            wasm_gpu_state->fences[i].signaled = true;
            return;
        }
    }
}

/* ------------------------------------------------------------------ */
/* Blob Resource Support                                               */
/* ------------------------------------------------------------------ */

#ifdef __EMSCRIPTEN__
EMSCRIPTEN_KEEPALIVE
#endif
int wasm_gpu_blob_create(WasmGpuBlobResource *blob)
{
    if (!wasm_gpu_state || !blob ||
        blob->resource_id >= WASM_GPU_MAX_RESOURCES) {
        return -1;
    }

    /* Create underlying resource */
    WasmGpuResource res = {
        .resource_id = blob->resource_id,
        .size = blob->size,
        .is_blob = true,
    };

    int ret = wasm_gpu_resource_create(&res);
    if (ret < 0) {
        return ret;
    }

    /* Allocate blob memory */
    WasmGpuResource *r = wasm_gpu_state->resources[blob->resource_id];
    if (r) {
        r->host_ptr = g_malloc0(blob->size);
        r->size = blob->size;
        blob->mapped_ptr = r->host_ptr;
    }

    return 0;
}

#ifdef __EMSCRIPTEN__
EMSCRIPTEN_KEEPALIVE
#endif
void *wasm_gpu_blob_map(uint32_t resource_id)
{
    if (!wasm_gpu_state || resource_id >= WASM_GPU_MAX_RESOURCES) {
        return NULL;
    }

    WasmGpuResource *res = wasm_gpu_state->resources[resource_id];
    if (!res || !res->is_blob) {
        return NULL;
    }

    return res->host_ptr;
}

#ifdef __EMSCRIPTEN__
EMSCRIPTEN_KEEPALIVE
#endif
void wasm_gpu_blob_unmap(uint32_t resource_id)
{
    /* Blob memory stays mapped until destroy */
}

/* ------------------------------------------------------------------ */
/* Statistics                                                          */
/* ------------------------------------------------------------------ */

#ifdef __EMSCRIPTEN__
EMSCRIPTEN_KEEPALIVE
#endif
WasmGpuStats *wasm_gpu_get_stats(void)
{
    if (!wasm_gpu_state) {
        return NULL;
    }
    return &wasm_gpu_state->stats;
}

#ifdef __EMSCRIPTEN__
EMSCRIPTEN_KEEPALIVE
#endif
void wasm_gpu_reset_stats(void)
{
    if (!wasm_gpu_state) {
        return;
    }

    /* Preserve resource/texture counts */
    uint64_t tex_mem = wasm_gpu_state->stats.texture_memory;
    uint64_t res_alloc = wasm_gpu_state->stats.resources_allocated;

    memset(&wasm_gpu_state->stats, 0, sizeof(WasmGpuStats));

    wasm_gpu_state->stats.texture_memory = tex_mem;
    wasm_gpu_state->stats.resources_allocated = res_alloc;
}

/* ------------------------------------------------------------------ */
/* JavaScript Notification Functions                                   */
/* ------------------------------------------------------------------ */

#ifdef __EMSCRIPTEN__
EMSCRIPTEN_KEEPALIVE
#endif
void wasm_gpu_notify_init(WasmGpuBackendType backend, uint32_t features)
{
#ifdef __EMSCRIPTEN__
    js_notify_gpu_init(backend, features);
#endif
}

#ifdef __EMSCRIPTEN__
EMSCRIPTEN_KEEPALIVE
#endif
void wasm_gpu_notify_frame(uint32_t scanout_id)
{
#ifdef __EMSCRIPTEN__
    EM_ASM({
        if (window.onWasmGpuFrame) {
            window.onWasmGpuFrame($0);
        }
    }, scanout_id);
#endif
}

#ifdef __EMSCRIPTEN__
EMSCRIPTEN_KEEPALIVE
#endif
void wasm_gpu_notify_resource_create(uint32_t resource_id,
                                     uint32_t width, uint32_t height,
                                     uint32_t format)
{
#ifdef __EMSCRIPTEN__
    EM_ASM({
        if (window.onWasmGpuResourceCreate) {
            window.onWasmGpuResourceCreate($0, $1, $2, $3);
        }
    }, resource_id, width, height, format);
#endif
}

#ifdef __EMSCRIPTEN__
EMSCRIPTEN_KEEPALIVE
#endif
void wasm_gpu_notify_resource_destroy(uint32_t resource_id)
{
#ifdef __EMSCRIPTEN__
    EM_ASM({
        if (window.onWasmGpuResourceDestroy) {
            window.onWasmGpuResourceDestroy($0);
        }
    }, resource_id);
#endif
}
