/*
 * QEMU WASM VirtIO-GPU WebGPU Hooks Header
 *
 * Provides WebGPU acceleration hooks for VirtIO-GPU devices in WASM builds.
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

#ifndef QEMU_UI_WASM_VIRTIO_GPU_H
#define QEMU_UI_WASM_VIRTIO_GPU_H

#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ------------------------------------------------------------------ */
/* WebGPU Backend Types                                               */
/* ------------------------------------------------------------------ */

typedef enum WasmGpuBackendType {
    WASM_GPU_BACKEND_NONE = 0,
    WASM_GPU_BACKEND_CANVAS2D,      /* Software rendering via Canvas 2D */
    WASM_GPU_BACKEND_WEBGL,         /* WebGL 1.0/2.0 */
    WASM_GPU_BACKEND_WEBGL2,        /* WebGL 2.0 only */
    WASM_GPU_BACKEND_WEBGPU,        /* Native WebGPU */
    WASM_GPU_BACKEND_WEBGPU_COMPAT, /* WebGPU compatibility (WebGL fallback) */
} WasmGpuBackendType;

typedef enum WasmGpuFeature {
    WASM_GPU_FEATURE_NONE           = 0,
    WASM_GPU_FEATURE_TEXTURE_3D     = (1 << 0),
    WASM_GPU_FEATURE_COMPUTE        = (1 << 1),
    WASM_GPU_FEATURE_STORAGE_BUFFER = (1 << 2),
    WASM_GPU_FEATURE_FLOAT32        = (1 << 3),
    WASM_GPU_FEATURE_TIMESTAMP      = (1 << 4),
    WASM_GPU_FEATURE_INDIRECT_DRAW  = (1 << 5),
    WASM_GPU_FEATURE_DEPTH_CLIP     = (1 << 6),
    WASM_GPU_FEATURE_MULTISAMPLING  = (1 << 7),
} WasmGpuFeature;

typedef struct WasmGpuCapabilities {
    WasmGpuBackendType backend;
    uint32_t features;              /* Bitmask of WasmGpuFeature */
    uint32_t max_texture_size;
    uint32_t max_texture_layers;
    uint32_t max_buffer_size;
    uint32_t max_uniform_buffer_size;
    uint32_t max_compute_workgroup_size[3];
    uint32_t max_compute_workgroups[3];
    bool supports_virgl;            /* VirGL 3D acceleration support */
    bool supports_blob;             /* Blob resources support */
    char vendor[64];
    char renderer[128];
} WasmGpuCapabilities;

/* ------------------------------------------------------------------ */
/* VirtIO-GPU Resource Management                                      */
/* ------------------------------------------------------------------ */

typedef struct WasmGpuResource {
    uint32_t resource_id;
    uint32_t width;
    uint32_t height;
    uint32_t format;                /* VirtIO GPU format */
    uint32_t bind;                  /* Bind flags */
    uint32_t target;                /* Target type (2D, 3D, etc.) */
    uint64_t size;
    void *host_ptr;                 /* Host memory pointer if mapped */
    int js_texture_id;              /* JavaScript texture handle */
    bool is_blob;
    bool is_dirty;
} WasmGpuResource;

typedef struct WasmGpuScanout {
    uint32_t scanout_id;
    uint32_t resource_id;
    uint32_t x, y;
    uint32_t width, height;
    bool enabled;
    bool needs_flush;
} WasmGpuScanout;

/* ------------------------------------------------------------------ */
/* VirtIO-GPU WebGPU Hook Interface                                    */
/* ------------------------------------------------------------------ */

/**
 * Initialize the WebGPU/WebGL backend.
 * Returns capabilities structure with detected features.
 */
WasmGpuCapabilities *wasm_gpu_init(WasmGpuBackendType preferred);

/**
 * Shutdown and cleanup GPU resources.
 */
void wasm_gpu_shutdown(void);

/**
 * Get current GPU capabilities.
 */
WasmGpuCapabilities *wasm_gpu_get_capabilities(void);

/**
 * Create a GPU resource (texture/buffer).
 */
int wasm_gpu_resource_create(WasmGpuResource *res);

/**
 * Destroy a GPU resource.
 */
void wasm_gpu_resource_destroy(uint32_t resource_id);

/**
 * Attach backing storage to a resource.
 */
int wasm_gpu_resource_attach_backing(uint32_t resource_id,
                                     void *data, size_t size);

/**
 * Detach backing storage from a resource.
 */
void wasm_gpu_resource_detach_backing(uint32_t resource_id);

/**
 * Transfer data to a resource (host -> guest display memory).
 */
int wasm_gpu_transfer_to_host(uint32_t resource_id,
                              uint32_t x, uint32_t y,
                              uint32_t width, uint32_t height,
                              uint64_t offset);

/**
 * Transfer data from a resource (guest display -> host).
 */
int wasm_gpu_transfer_from_host(uint32_t resource_id,
                                uint32_t x, uint32_t y,
                                uint32_t width, uint32_t height,
                                uint64_t offset);

/**
 * Flush a resource to the display.
 */
int wasm_gpu_resource_flush(uint32_t resource_id,
                            uint32_t x, uint32_t y,
                            uint32_t width, uint32_t height);

/**
 * Set scanout (map resource to display output).
 */
int wasm_gpu_set_scanout(WasmGpuScanout *scanout);

/**
 * Update cursor resource.
 */
int wasm_gpu_cursor_update(uint32_t resource_id,
                           uint32_t hot_x, uint32_t hot_y);

/**
 * Move cursor position.
 */
void wasm_gpu_cursor_move(uint32_t scanout_id, uint32_t x, uint32_t y);

/* ------------------------------------------------------------------ */
/* 3D Context Hooks (VirGL-like support)                               */
/* ------------------------------------------------------------------ */

typedef struct WasmGpu3DContext {
    uint32_t ctx_id;
    uint32_t capset_id;
    bool active;
} WasmGpu3DContext;

/**
 * Create a 3D rendering context.
 */
int wasm_gpu_ctx_create(uint32_t ctx_id, uint32_t capset_id,
                        const char *debug_name);

/**
 * Destroy a 3D rendering context.
 */
void wasm_gpu_ctx_destroy(uint32_t ctx_id);

/**
 * Attach resource to 3D context.
 */
int wasm_gpu_ctx_attach_resource(uint32_t ctx_id, uint32_t resource_id);

/**
 * Detach resource from 3D context.
 */
void wasm_gpu_ctx_detach_resource(uint32_t ctx_id, uint32_t resource_id);

/**
 * Submit a 3D command buffer.
 */
int wasm_gpu_submit_3d(uint32_t ctx_id, void *cmd_buf, size_t cmd_size);

/**
 * Create a fence for synchronization.
 */
int wasm_gpu_create_fence(uint32_t ctx_id, uint64_t fence_id);

/**
 * Poll fence completion status.
 */
bool wasm_gpu_fence_is_signaled(uint64_t fence_id);

/* ------------------------------------------------------------------ */
/* Blob Resource Support (experimental)                                */
/* ------------------------------------------------------------------ */

typedef struct WasmGpuBlobResource {
    uint32_t resource_id;
    uint32_t blob_mem;              /* Memory type */
    uint32_t blob_flags;            /* Blob flags */
    uint64_t blob_id;               /* Unique blob ID */
    uint64_t size;
    void *mapped_ptr;
} WasmGpuBlobResource;

/**
 * Create a blob resource.
 */
int wasm_gpu_blob_create(WasmGpuBlobResource *blob);

/**
 * Map blob resource memory.
 */
void *wasm_gpu_blob_map(uint32_t resource_id);

/**
 * Unmap blob resource memory.
 */
void wasm_gpu_blob_unmap(uint32_t resource_id);

/* ------------------------------------------------------------------ */
/* Performance Statistics                                              */
/* ------------------------------------------------------------------ */

typedef struct WasmGpuStats {
    uint64_t frames_rendered;
    uint64_t bytes_uploaded;
    uint64_t bytes_downloaded;
    uint64_t commands_submitted;
    uint64_t resources_allocated;
    uint64_t texture_memory;
    float avg_frame_time_ms;
    float avg_upload_time_ms;
    uint32_t pending_fences;
} WasmGpuStats;

/**
 * Get GPU statistics.
 */
WasmGpuStats *wasm_gpu_get_stats(void);

/**
 * Reset GPU statistics.
 */
void wasm_gpu_reset_stats(void);

/* ------------------------------------------------------------------ */
/* JavaScript Callbacks (set by JavaScript side)                       */
/* ------------------------------------------------------------------ */

/**
 * Notify JavaScript that GPU is initialized.
 */
void wasm_gpu_notify_init(WasmGpuBackendType backend, uint32_t features);

/**
 * Notify JavaScript of a new frame.
 */
void wasm_gpu_notify_frame(uint32_t scanout_id);

/**
 * Notify JavaScript of resource creation.
 */
void wasm_gpu_notify_resource_create(uint32_t resource_id,
                                     uint32_t width, uint32_t height,
                                     uint32_t format);

/**
 * Notify JavaScript of resource destruction.
 */
void wasm_gpu_notify_resource_destroy(uint32_t resource_id);

#ifdef __cplusplus
}
#endif

#endif /* QEMU_UI_WASM_VIRTIO_GPU_H */
