/*
 * QEMU WASM Audio Backend Header
 *
 * Provides audio output/input via Web Audio API for browser builds.
 * Supports AudioWorklet for low-latency audio processing.
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

#ifndef QEMU_UI_WASM_AUDIO_H
#define QEMU_UI_WASM_AUDIO_H

#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ------------------------------------------------------------------ */
/* Audio Configuration                                                 */
/* ------------------------------------------------------------------ */

typedef enum WasmAudioState {
    WASM_AUDIO_STATE_CLOSED = 0,
    WASM_AUDIO_STATE_SUSPENDED,
    WASM_AUDIO_STATE_RUNNING,
    WASM_AUDIO_STATE_INTERRUPTED,  /* iOS Safari specific */
} WasmAudioState;

typedef enum WasmAudioBackendType {
    WASM_AUDIO_BACKEND_NONE = 0,
    WASM_AUDIO_BACKEND_SCRIPT_PROCESSOR, /* Deprecated but widely supported */
    WASM_AUDIO_BACKEND_AUDIO_WORKLET,    /* Modern low-latency */
} WasmAudioBackendType;

typedef struct WasmAudioConfig {
    int32_t sample_rate;        /* Sample rate in Hz (e.g., 44100, 48000) */
    int32_t channels;           /* Number of channels (1=mono, 2=stereo) */
    int32_t buffer_size;        /* Buffer size in samples */
    int32_t latency_hint;       /* Latency hint in ms (0=interactive, 1=balanced, 2=playback) */
    WasmAudioBackendType backend;
    bool enable_input;          /* Enable microphone input */
} WasmAudioConfig;

typedef struct WasmAudioInfo {
    WasmAudioState state;
    WasmAudioBackendType backend;
    int32_t actual_sample_rate;
    int32_t actual_buffer_size;
    float output_latency_sec;
    float input_latency_sec;
    uint64_t samples_played;
    uint64_t samples_captured;
    uint64_t underruns;
    uint64_t overruns;
} WasmAudioInfo;

/* ------------------------------------------------------------------ */
/* Audio Initialization                                                */
/* ------------------------------------------------------------------ */

/**
 * Initialize Web Audio API backend.
 *
 * @config: Audio configuration parameters
 *
 * Returns: 0 on success, negative on error
 */
int wasm_audio_init(const WasmAudioConfig *config);

/**
 * Shutdown audio backend and free resources.
 */
void wasm_audio_shutdown(void);

/**
 * Get current audio info/state.
 */
WasmAudioInfo *wasm_audio_get_info(void);

/**
 * Resume audio context (required after user gesture on iOS/Safari).
 * Should be called from a user event handler.
 */
int wasm_audio_resume(void);

/**
 * Suspend audio context.
 */
int wasm_audio_suspend(void);

/* ------------------------------------------------------------------ */
/* Audio Output                                                        */
/* ------------------------------------------------------------------ */

/**
 * Write audio samples to output buffer.
 *
 * @data: Pointer to audio samples (interleaved if stereo)
 * @samples: Number of samples to write
 *
 * Returns: Number of samples actually written
 */
size_t wasm_audio_write(const void *data, size_t samples);

/**
 * Get available space in output buffer.
 *
 * Returns: Number of samples that can be written
 */
size_t wasm_audio_get_free(void);

/**
 * Set output volume.
 *
 * @left: Left channel volume (0.0 - 1.0)
 * @right: Right channel volume (0.0 - 1.0)
 */
void wasm_audio_set_volume(float left, float right);

/**
 * Mute/unmute output.
 */
void wasm_audio_set_mute(bool mute);

/* ------------------------------------------------------------------ */
/* Audio Input (Microphone)                                            */
/* ------------------------------------------------------------------ */

/**
 * Request microphone access.
 * Requires user gesture and permission grant.
 *
 * Returns: 0 on success, negative on error
 */
int wasm_audio_request_input(void);

/**
 * Read audio samples from input buffer.
 *
 * @data: Buffer to store captured samples
 * @samples: Maximum number of samples to read
 *
 * Returns: Number of samples actually read
 */
size_t wasm_audio_read(void *data, size_t samples);

/**
 * Get available samples in input buffer.
 */
size_t wasm_audio_get_available(void);

/**
 * Set input volume/gain.
 */
void wasm_audio_set_input_gain(float gain);

/* ------------------------------------------------------------------ */
/* Audio Format Conversion                                             */
/* ------------------------------------------------------------------ */

/**
 * Convert audio format.
 *
 * @src: Source buffer
 * @src_fmt: Source format (bits per sample, signed flag)
 * @dst: Destination buffer
 * @dst_fmt: Destination format
 * @samples: Number of samples to convert
 */
void wasm_audio_convert(const void *src, int src_bits, bool src_signed,
                        void *dst, int dst_bits, bool dst_signed,
                        size_t samples);

/* ------------------------------------------------------------------ */
/* iOS Safari Specific                                                 */
/* ------------------------------------------------------------------ */

/**
 * Check if audio is in interrupted state (iOS Safari).
 */
bool wasm_audio_is_interrupted(void);

/**
 * Handle iOS audio session interruption.
 * Called automatically from JavaScript when interruption occurs.
 */
void wasm_audio_handle_interruption(bool began);

/**
 * Check if audio autoplay is allowed.
 * Returns false if user gesture is required first.
 */
bool wasm_audio_autoplay_allowed(void);

#ifdef __cplusplus
}
#endif

#endif /* QEMU_UI_WASM_AUDIO_H */
