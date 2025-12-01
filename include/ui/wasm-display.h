/*
 * QEMU WASM Display Backend Header
 *
 * Exports framebuffer to JavaScript for Canvas/WebGL rendering.
 * Designed for Emscripten builds targeting browsers (Safari/Chrome/Firefox).
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

/**
 * Framebuffer information structure for JavaScript interop.
 * This structure is accessible from JavaScript via Emscripten.
 */
typedef struct WasmFramebufferInfo {
    uint8_t *data;      /* Pointer to RGBA pixel data */
    int32_t width;      /* Width in pixels */
    int32_t height;     /* Height in pixels */
    int32_t stride;     /* Bytes per row (pitch) */
    int32_t bpp;        /* Bits per pixel (typically 32 for RGBA) */
    uint32_t format;    /* Pixel format (pixman format code) */
    bool dirty;         /* True if framebuffer has been updated */
    uint64_t frame_count; /* Frame counter for sync */
} WasmFramebufferInfo;

/**
 * Get the framebuffer info structure.
 * Called from JavaScript to access framebuffer data.
 *
 * Returns: Pointer to WasmFramebufferInfo structure, or NULL if not initialized.
 */
WasmFramebufferInfo *wasm_get_framebuffer_info(void);

/**
 * Get raw framebuffer data pointer.
 * Called from JavaScript for direct memory access.
 *
 * Returns: Pointer to pixel data, or NULL if not initialized.
 */
uint8_t *wasm_get_framebuffer_data(void);

/**
 * Get framebuffer dimensions.
 * Called from JavaScript to get current resolution.
 *
 * @out_width: Output parameter for width
 * @out_height: Output parameter for height
 *
 * Returns: true if framebuffer is valid, false otherwise.
 */
bool wasm_get_framebuffer_size(int32_t *out_width, int32_t *out_height);

/**
 * Acknowledge framebuffer read.
 * Called from JavaScript after copying framebuffer to canvas.
 * Clears the dirty flag.
 */
void wasm_framebuffer_ack(void);

/**
 * Check if framebuffer has updates.
 *
 * Returns: true if framebuffer has been updated since last ack.
 */
bool wasm_framebuffer_is_dirty(void);

/**
 * Get current frame count.
 * Useful for JavaScript to detect frame updates.
 *
 * Returns: Current frame counter value.
 */
uint64_t wasm_get_frame_count(void);

/**
 * Send keyboard event from JavaScript.
 *
 * @keycode: QKeyCode value
 * @down: true for key press, false for key release
 */
void wasm_send_keyboard_event(int keycode, bool down);

/**
 * Send mouse motion event from JavaScript.
 *
 * @x: X position (relative to console)
 * @y: Y position (relative to console)
 */
void wasm_send_mouse_motion(int x, int y);

/**
 * Send mouse button event from JavaScript.
 *
 * @button: Button number (0=left, 1=middle, 2=right)
 * @down: true for button press, false for release
 */
void wasm_send_mouse_button(int button, bool down);

/**
 * Send mouse wheel event from JavaScript.
 *
 * @dx: Horizontal scroll delta
 * @dy: Vertical scroll delta
 */
void wasm_send_mouse_wheel(int dx, int dy);

#ifdef __cplusplus
}
#endif

#endif /* QEMU_UI_WASM_DISPLAY_H */
