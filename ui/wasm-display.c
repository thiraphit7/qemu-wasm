/*
 * QEMU WASM Display Backend
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

#include "qemu/osdep.h"
#include "qemu/module.h"
#include "qemu/main-loop.h"
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

typedef struct WasmDisplayState {
    DisplayChangeListener dcl;
    DisplaySurface *ds;
    QKbdState *kbd;

    /* Framebuffer for JavaScript access */
    uint8_t *fb_data;
    size_t fb_allocated_size;
    WasmFramebufferInfo fb_info;

    /* Mouse state */
    int mouse_x;
    int mouse_y;
    int mouse_buttons;
    bool mouse_grabbed;
} WasmDisplayState;

/* Global state for JavaScript access */
static WasmDisplayState *wasm_display_state = NULL;

/* ------------------------------------------------------------------ */
/* Exported functions for JavaScript access                           */
/* ------------------------------------------------------------------ */

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
    }
}

#ifdef __EMSCRIPTEN__
EMSCRIPTEN_KEEPALIVE
#endif
bool wasm_framebuffer_is_dirty(void)
{
    if (!wasm_display_state) {
        return false;
    }
    return wasm_display_state->fb_info.dirty;
}

#ifdef __EMSCRIPTEN__
EMSCRIPTEN_KEEPALIVE
#endif
uint64_t wasm_get_frame_count(void)
{
    if (!wasm_display_state) {
        return 0;
    }
    return wasm_display_state->fb_info.frame_count;
}

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

    if (!surface || !wds->fb_data) {
        return;
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

    /* Copy updated region */
    for (int row = y; row < max_y; row++) {
        uint8_t *src_line = src + row * src_stride + x * bpp;
        uint8_t *dst_line = dst + row * dst_stride + x * 4;

        if (bpp == 4) {
            /* Direct copy for 32-bit surfaces (XRGB/ARGB) */
            /* Convert BGRX to RGBA for Canvas compatibility */
            for (int col = x; col < max_x; col++) {
                uint32_t pixel = *(uint32_t *)src_line;
                /* Extract BGRX and convert to RGBA */
                dst_line[0] = (pixel >> 16) & 0xFF; /* R */
                dst_line[1] = (pixel >> 8) & 0xFF;  /* G */
                dst_line[2] = pixel & 0xFF;         /* B */
                dst_line[3] = 0xFF;                 /* A */
                src_line += 4;
                dst_line += 4;
            }
        } else {
            /* For other formats, copy as-is (may need conversion) */
            memcpy(dst_line, src_line, (max_x - x) * bpp);
        }
    }

    wds->fb_info.dirty = true;
    wds->fb_info.frame_count++;

#ifdef __EMSCRIPTEN__
    /* Notify JavaScript that framebuffer was updated */
    EM_ASM({
        if (typeof window !== 'undefined' && window.onWasmFramebufferUpdate) {
            window.onWasmFramebufferUpdate();
        }
    });
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
    int stride = width * 4;  /* Always RGBA in output */
    size_t size = stride * height;

    /* Clamp to maximum size */
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
    wds->fb_info.frame_count++;

#ifdef __EMSCRIPTEN__
    /* Notify JavaScript about resolution change */
    EM_ASM({
        if (typeof window !== 'undefined' && window.onWasmFramebufferResize) {
            window.onWasmFramebufferResize($0, $1);
        }
    }, width, height);
#endif

    /* Do initial full update */
    wasm_gfx_update(dcl, 0, 0, width, height);
}

static void wasm_mouse_set(DisplayChangeListener *dcl,
                           int x, int y, int on)
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

static void wasm_cursor_define(DisplayChangeListener *dcl,
                               QEMUCursor *cursor)
{
#ifdef __EMSCRIPTEN__
    if (cursor) {
        /* Export cursor data to JavaScript for custom cursor rendering */
        EM_ASM({
            if (typeof window !== 'undefined' && window.onWasmCursorDefine) {
                window.onWasmCursorDefine($0, $1, $2, $3);
            }
        }, cursor->width, cursor->height, cursor->hot_x, cursor->hot_y);
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
    /* Notify JavaScript that display is ready */
    EM_ASM({
        if (typeof window !== 'undefined' && window.onWasmDisplayReady) {
            window.onWasmDisplayReady();
        }
    });
#endif

    fprintf(stderr, "wasm-display: initialized with %dx%d framebuffer\n",
            wds->fb_info.width, wds->fb_info.height);
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
