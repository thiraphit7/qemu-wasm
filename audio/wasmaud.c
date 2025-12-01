/*
 * QEMU WASM Audio Backend
 *
 * Audio output via Web Audio API for Emscripten builds.
 * Supports both AudioWorklet (modern) and ScriptProcessorNode (legacy).
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
#include "qemu/host-utils.h"
#include "audio.h"
#include "qemu/timer.h"

#ifdef __EMSCRIPTEN__
#include <emscripten.h>
#include <emscripten/html5.h>
#endif

#define AUDIO_CAP "wasmaud"
#include "audio_int.h"

/* Audio buffer configuration */
#define WASM_AUDIO_SAMPLE_RATE  48000
#define WASM_AUDIO_CHANNELS     2
#define WASM_AUDIO_BUFFER_SIZE  4096
#define WASM_AUDIO_RING_SIZE    (WASM_AUDIO_BUFFER_SIZE * 4)

/* ------------------------------------------------------------------ */
/* Internal structures                                                */
/* ------------------------------------------------------------------ */

typedef struct WasmAudioState {
    bool initialized;
    bool worklet_available;
    int sample_rate;
    float volume;
    bool muted;
} WasmAudioState;

typedef struct WasmVoiceOut {
    HWVoiceOut hw;
    RateCtl rate;
    WasmAudioState *state;

    /* Ring buffer for audio samples */
    uint8_t *ring_buffer;
    size_t ring_size;
    size_t ring_read_pos;
    size_t ring_write_pos;
    size_t ring_used;

    bool enabled;
} WasmVoiceOut;

typedef struct WasmVoiceIn {
    HWVoiceIn hw;
    RateCtl rate;
    WasmAudioState *state;
    bool enabled;
} WasmVoiceIn;

/* Global audio state */
static WasmAudioState wasm_audio_state = {0};

/* ------------------------------------------------------------------ */
/* JavaScript Audio API integration                                   */
/* ------------------------------------------------------------------ */

#ifdef __EMSCRIPTEN__

/* Initialize Web Audio context */
EM_JS(int, wasm_audio_js_init, (int sample_rate), {
    try {
        if (Module._audioContext) {
            return 1;  // Already initialized
        }

        // Create AudioContext with specified sample rate
        var AudioContextClass = window.AudioContext || window.webkitAudioContext;
        Module._audioContext = new AudioContextClass({
            sampleRate: sample_rate,
            latencyHint: 'interactive'
        });

        // Check if AudioWorklet is available
        Module._audioWorkletAvailable = !!(Module._audioContext.audioWorklet);

        // Create gain node for volume control
        Module._audioGain = Module._audioContext.createGain();
        Module._audioGain.connect(Module._audioContext.destination);

        // Ring buffer for audio samples (shared with C)
        Module._audioRingBuffer = null;
        Module._audioRingRead = 0;
        Module._audioRingWrite = 0;

        console.log('WASM Audio: Initialized with sample rate', sample_rate);
        return 1;
    } catch (e) {
        console.error('WASM Audio: Init failed:', e);
        return 0;
    }
});

/* Start audio playback with ScriptProcessorNode */
EM_JS(int, wasm_audio_js_start, (int buffer_ptr, int buffer_size, int sample_rate), {
    try {
        if (!Module._audioContext) return 0;

        // Resume context if suspended (required for autoplay policy)
        if (Module._audioContext.state === 'suspended') {
            Module._audioContext.resume();
        }

        // Store ring buffer reference
        Module._audioRingBuffer = buffer_ptr;
        Module._audioRingSize = buffer_size;

        // Create ScriptProcessorNode (fallback for broader compatibility)
        // Buffer size: 2048 samples, stereo in/out
        var bufferSize = 2048;
        Module._audioProcessor = Module._audioContext.createScriptProcessor(
            bufferSize, 2, 2
        );

        Module._audioProcessor.onaudioprocess = function(e) {
            var leftOut = e.outputBuffer.getChannelData(0);
            var rightOut = e.outputBuffer.getChannelData(1);
            var samples = e.outputBuffer.length;

            // Read from ring buffer
            var ringPtr = Module._audioRingBuffer;
            var ringSize = Module._audioRingSize;
            var readPos = Module._audioRingRead;
            var writePos = Module._audioRingWrite;

            // Calculate available data
            var available = (writePos - readPos + ringSize) % ringSize;
            var bytesNeeded = samples * 4;  // 2 channels * 2 bytes per sample (int16)

            if (available < bytesNeeded) {
                // Underrun - fill with silence
                for (var i = 0; i < samples; i++) {
                    leftOut[i] = 0;
                    rightOut[i] = 0;
                }
                return;
            }

            // Read interleaved int16 samples and convert to float32
            for (var i = 0; i < samples; i++) {
                var pos = (readPos + i * 4) % ringSize;
                var left = Module.HEAP16[(ringPtr + pos) >> 1];
                var right = Module.HEAP16[(ringPtr + pos + 2) >> 1];

                // Convert int16 to float32 [-1, 1]
                leftOut[i] = left / 32768.0;
                rightOut[i] = right / 32768.0;
            }

            // Update read position
            Module._audioRingRead = (readPos + bytesNeeded) % ringSize;
        };

        // Connect to gain node
        Module._audioProcessor.connect(Module._audioGain);

        console.log('WASM Audio: Started playback');
        return 1;
    } catch (e) {
        console.error('WASM Audio: Start failed:', e);
        return 0;
    }
});

/* Stop audio playback */
EM_JS(void, wasm_audio_js_stop, (), {
    try {
        if (Module._audioProcessor) {
            Module._audioProcessor.disconnect();
            Module._audioProcessor = null;
        }
        console.log('WASM Audio: Stopped playback');
    } catch (e) {
        console.error('WASM Audio: Stop failed:', e);
    }
});

/* Set volume (0.0 to 1.0) */
EM_JS(void, wasm_audio_js_set_volume, (float volume), {
    try {
        if (Module._audioGain) {
            Module._audioGain.gain.value = volume;
        }
    } catch (e) {}
});

/* Get current ring buffer write position */
EM_JS(int, wasm_audio_js_get_free, (), {
    if (!Module._audioRingBuffer) return 0;

    var ringSize = Module._audioRingSize;
    var readPos = Module._audioRingRead;
    var writePos = Module._audioRingWrite;

    // Calculate free space
    var used = (writePos - readPos + ringSize) % ringSize;
    return ringSize - used - 4;  // Leave some margin
});

/* Update write position after writing samples */
EM_JS(void, wasm_audio_js_advance_write, (int bytes), {
    if (!Module._audioRingBuffer) return;
    Module._audioRingWrite = (Module._audioRingWrite + bytes) % Module._audioRingSize;
});

/* Check if AudioWorklet is available */
EM_JS(int, wasm_audio_js_has_worklet, (), {
    return Module._audioWorkletAvailable ? 1 : 0;
});

/* Resume audio context (for autoplay policy) */
EM_JS(void, wasm_audio_js_resume, (), {
    if (Module._audioContext && Module._audioContext.state === 'suspended') {
        Module._audioContext.resume();
    }
});

#endif /* __EMSCRIPTEN__ */

/* ------------------------------------------------------------------ */
/* Exported functions for JavaScript                                  */
/* ------------------------------------------------------------------ */

#ifdef __EMSCRIPTEN__
EMSCRIPTEN_KEEPALIVE
#endif
void wasm_audio_resume(void)
{
#ifdef __EMSCRIPTEN__
    wasm_audio_js_resume();
#endif
}

#ifdef __EMSCRIPTEN__
EMSCRIPTEN_KEEPALIVE
#endif
void wasm_audio_set_volume(float volume)
{
    wasm_audio_state.volume = volume;
#ifdef __EMSCRIPTEN__
    wasm_audio_js_set_volume(volume);
#endif
}

#ifdef __EMSCRIPTEN__
EMSCRIPTEN_KEEPALIVE
#endif
void wasm_audio_set_muted(int muted)
{
    wasm_audio_state.muted = muted != 0;
#ifdef __EMSCRIPTEN__
    wasm_audio_js_set_volume(muted ? 0.0f : wasm_audio_state.volume);
#endif
}

/* ------------------------------------------------------------------ */
/* Audio driver callbacks                                             */
/* ------------------------------------------------------------------ */

static size_t wasm_write(HWVoiceOut *hw, void *buf, size_t len)
{
    WasmVoiceOut *wasm = (WasmVoiceOut *)hw;

    if (!wasm->enabled || !wasm->ring_buffer) {
        return audio_rate_get_bytes(&wasm->rate, &hw->info, len);
    }

#ifdef __EMSCRIPTEN__
    /* Check available space in ring buffer */
    int free_space = wasm_audio_js_get_free();
    if (free_space <= 0) {
        return 0;  // Buffer full
    }

    size_t to_write = MIN(len, (size_t)free_space);

    /* Write to ring buffer */
    size_t write_pos = wasm->ring_write_pos;
    size_t first_chunk = MIN(to_write, wasm->ring_size - write_pos);

    memcpy(wasm->ring_buffer + write_pos, buf, first_chunk);
    if (to_write > first_chunk) {
        memcpy(wasm->ring_buffer, (uint8_t *)buf + first_chunk, to_write - first_chunk);
    }

    wasm->ring_write_pos = (write_pos + to_write) % wasm->ring_size;
    wasm_audio_js_advance_write(to_write);

    return to_write;
#else
    return audio_rate_get_bytes(&wasm->rate, &hw->info, len);
#endif
}

static int wasm_init_out(HWVoiceOut *hw, struct audsettings *as, void *drv_opaque)
{
    WasmVoiceOut *wasm = (WasmVoiceOut *)hw;
    WasmAudioState *state = (WasmAudioState *)drv_opaque;

    wasm->state = state;

    /* Configure audio format */
    struct audsettings obt_as = {
        .freq = state->sample_rate,
        .nchannels = WASM_AUDIO_CHANNELS,
        .fmt = AUDIO_FORMAT_S16,
        .endianness = 0  /* Little endian */
    };

    audio_pcm_init_info(&hw->info, &obt_as);
    hw->samples = WASM_AUDIO_BUFFER_SIZE;

    /* Allocate ring buffer */
    wasm->ring_size = WASM_AUDIO_RING_SIZE;
    wasm->ring_buffer = g_malloc0(wasm->ring_size);
    wasm->ring_read_pos = 0;
    wasm->ring_write_pos = 0;
    wasm->ring_used = 0;

    audio_rate_start(&wasm->rate);

    return 0;
}

static void wasm_fini_out(HWVoiceOut *hw)
{
    WasmVoiceOut *wasm = (WasmVoiceOut *)hw;

#ifdef __EMSCRIPTEN__
    wasm_audio_js_stop();
#endif

    g_free(wasm->ring_buffer);
    wasm->ring_buffer = NULL;
}

static void wasm_enable_out(HWVoiceOut *hw, bool enable)
{
    WasmVoiceOut *wasm = (WasmVoiceOut *)hw;

    wasm->enabled = enable;

#ifdef __EMSCRIPTEN__
    if (enable) {
        audio_rate_start(&wasm->rate);
        wasm_audio_js_start((int)(uintptr_t)wasm->ring_buffer,
                           wasm->ring_size,
                           wasm->state->sample_rate);
    } else {
        wasm_audio_js_stop();
    }
#else
    if (enable) {
        audio_rate_start(&wasm->rate);
    }
#endif
}

static void wasm_volume_out(HWVoiceOut *hw, Volume *vol)
{
    WasmVoiceOut *wasm = (WasmVoiceOut *)hw;

    if (vol->mute) {
        wasm_audio_set_muted(1);
    } else {
        /* Average left/right channels, convert from 0-255 to 0.0-1.0 */
        float volume = (vol->vol[0] + vol->vol[1]) / 2.0f / 255.0f;
        wasm->state->volume = volume;
        wasm_audio_set_volume(volume);
    }
}

/* Input (microphone) - stub implementation */
static int wasm_init_in(HWVoiceIn *hw, struct audsettings *as, void *drv_opaque)
{
    WasmVoiceIn *wasm = (WasmVoiceIn *)hw;
    WasmAudioState *state = (WasmAudioState *)drv_opaque;

    wasm->state = state;

    audio_pcm_init_info(&hw->info, as);
    hw->samples = 1024;
    audio_rate_start(&wasm->rate);

    return 0;
}

static void wasm_fini_in(HWVoiceIn *hw)
{
    (void)hw;
}

static size_t wasm_read(HWVoiceIn *hw, void *buf, size_t size)
{
    WasmVoiceIn *wasm = (WasmVoiceIn *)hw;
    int64_t bytes = audio_rate_get_bytes(&wasm->rate, &hw->info, size);

    /* Fill with silence - microphone not implemented */
    audio_pcm_info_clear_buf(&hw->info, buf, bytes / hw->info.bytes_per_frame);
    return bytes;
}

static void wasm_enable_in(HWVoiceIn *hw, bool enable)
{
    WasmVoiceIn *wasm = (WasmVoiceIn *)hw;

    wasm->enabled = enable;
    if (enable) {
        audio_rate_start(&wasm->rate);
    }
}

/* ------------------------------------------------------------------ */
/* Driver initialization                                              */
/* ------------------------------------------------------------------ */

static void *wasm_audio_init(Audiodev *dev, Error **errp)
{
    WasmAudioState *state = &wasm_audio_state;

    state->sample_rate = WASM_AUDIO_SAMPLE_RATE;
    state->volume = 1.0f;
    state->muted = false;

#ifdef __EMSCRIPTEN__
    if (!wasm_audio_js_init(state->sample_rate)) {
        error_setg(errp, "Failed to initialize Web Audio API");
        return NULL;
    }

    state->worklet_available = wasm_audio_js_has_worklet();
    state->initialized = true;

    fprintf(stderr, "wasm-audio: initialized (sample rate: %d, worklet: %s)\n",
            state->sample_rate,
            state->worklet_available ? "yes" : "no");
#else
    state->initialized = true;
    fprintf(stderr, "wasm-audio: initialized (stub mode)\n");
#endif

    return state;
}

static void wasm_audio_fini(void *opaque)
{
    WasmAudioState *state = (WasmAudioState *)opaque;

#ifdef __EMSCRIPTEN__
    wasm_audio_js_stop();
#endif

    state->initialized = false;
}

/* PCM operations */
static struct audio_pcm_ops wasm_pcm_ops = {
    .init_out       = wasm_init_out,
    .fini_out       = wasm_fini_out,
    .write          = wasm_write,
    .buffer_get_free = audio_generic_buffer_get_free,
    .run_buffer_out = audio_generic_run_buffer_out,
    .enable_out     = wasm_enable_out,
    .volume_out     = wasm_volume_out,

    .init_in        = wasm_init_in,
    .fini_in        = wasm_fini_in,
    .read           = wasm_read,
    .run_buffer_in  = audio_generic_run_buffer_in,
    .enable_in      = wasm_enable_in,
};

/* Audio driver registration */
static struct audio_driver wasm_audio_driver = {
    .name           = "wasmaud",
    .descr          = "WASM Web Audio API output",
    .init           = wasm_audio_init,
    .fini           = wasm_audio_fini,
    .pcm_ops        = &wasm_pcm_ops,
    .max_voices_out = 1,
    .max_voices_in  = 1,
    .voice_size_out = sizeof(WasmVoiceOut),
    .voice_size_in  = sizeof(WasmVoiceIn),
};

static void register_audio_wasm(void)
{
    audio_driver_register(&wasm_audio_driver);
}

type_init(register_audio_wasm);
