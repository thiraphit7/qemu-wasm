/*
 * QEMU WASM Audio Backend Implementation
 *
 * Provides audio output/input via Web Audio API for browser builds.
 * Uses AudioWorklet for low-latency audio when available.
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
#include "qemu/host-utils.h"
#include "qemu/module.h"
#include "audio.h"
#include "qemu/timer.h"
#include "ui/wasm-audio.h"

#ifdef __EMSCRIPTEN__
#include <emscripten.h>
#include <emscripten/html5.h>
#endif

#define AUDIO_CAP "wasmaudio"
#include "audio_int.h"

/* Ring buffer size (must be power of 2) */
#define WASM_AUDIO_RING_SIZE     16384
#define WASM_AUDIO_RING_MASK     (WASM_AUDIO_RING_SIZE - 1)

/* Default configuration */
#define WASM_AUDIO_DEFAULT_RATE    48000
#define WASM_AUDIO_DEFAULT_CHANNELS 2
#define WASM_AUDIO_DEFAULT_SAMPLES  1024

/* ------------------------------------------------------------------ */
/* Audio State                                                         */
/* ------------------------------------------------------------------ */

typedef struct WasmAudioRingBuffer {
    float *buffer;
    volatile uint32_t read_pos;
    volatile uint32_t write_pos;
    size_t size;
} WasmAudioRingBuffer;

typedef struct WasmAudioGlobalState {
    bool initialized;
    WasmAudioInfo info;
    WasmAudioConfig config;

    /* Output ring buffer */
    WasmAudioRingBuffer output_ring;

    /* Input ring buffer */
    WasmAudioRingBuffer input_ring;

    /* Volume control */
    float volume_left;
    float volume_right;
    bool muted;

    /* Input gain */
    float input_gain;

    /* iOS interruption state */
    bool interrupted;
    bool autoplay_blocked;

} WasmAudioGlobalState;

static WasmAudioGlobalState *wasm_audio_state = NULL;

/* ------------------------------------------------------------------ */
/* QEMU Audio Driver Structures                                        */
/* ------------------------------------------------------------------ */

typedef struct WasmVoiceOut {
    HWVoiceOut hw;
    RateCtl rate;
} WasmVoiceOut;

typedef struct WasmVoiceIn {
    HWVoiceIn hw;
    RateCtl rate;
} WasmVoiceIn;

/* ------------------------------------------------------------------ */
/* Ring Buffer Operations                                              */
/* ------------------------------------------------------------------ */

static inline size_t ring_available_write(WasmAudioRingBuffer *ring)
{
    uint32_t r = ring->read_pos;
    uint32_t w = ring->write_pos;
    return (r - w - 1) & WASM_AUDIO_RING_MASK;
}

static inline size_t ring_available_read(WasmAudioRingBuffer *ring)
{
    uint32_t r = ring->read_pos;
    uint32_t w = ring->write_pos;
    return (w - r) & WASM_AUDIO_RING_MASK;
}

static size_t ring_write(WasmAudioRingBuffer *ring, const float *data, size_t count)
{
    size_t available = ring_available_write(ring);
    if (count > available) {
        count = available;
    }

    uint32_t w = ring->write_pos;
    size_t to_end = WASM_AUDIO_RING_SIZE - w;

    if (count <= to_end) {
        memcpy(ring->buffer + w, data, count * sizeof(float));
    } else {
        memcpy(ring->buffer + w, data, to_end * sizeof(float));
        memcpy(ring->buffer, data + to_end, (count - to_end) * sizeof(float));
    }

    ring->write_pos = (w + count) & WASM_AUDIO_RING_MASK;
    return count;
}

static size_t ring_read(WasmAudioRingBuffer *ring, float *data, size_t count)
{
    size_t available = ring_available_read(ring);
    if (count > available) {
        count = available;
    }

    uint32_t r = ring->read_pos;
    size_t to_end = WASM_AUDIO_RING_SIZE - r;

    if (count <= to_end) {
        memcpy(data, ring->buffer + r, count * sizeof(float));
    } else {
        memcpy(data, ring->buffer + r, to_end * sizeof(float));
        memcpy(data + to_end, ring->buffer, (count - to_end) * sizeof(float));
    }

    ring->read_pos = (r + count) & WASM_AUDIO_RING_MASK;
    return count;
}

/* ------------------------------------------------------------------ */
/* JavaScript Interop                                                  */
/* ------------------------------------------------------------------ */

#ifdef __EMSCRIPTEN__

/* Initialize Web Audio context */
EM_JS(int, js_audio_init, (int sample_rate, int channels, int buffer_size,
                            int use_worklet), {
    if (window._wasmAudio) {
        return 0; /* Already initialized */
    }

    try {
        var AudioContext = window.AudioContext || window.webkitAudioContext;
        if (!AudioContext) {
            console.error('Web Audio API not supported');
            return -1;
        }

        /* Create audio context with iOS-friendly options */
        var contextOptions = {
            sampleRate: sample_rate,
            latencyHint: 'interactive'
        };

        var ctx = new AudioContext(contextOptions);

        window._wasmAudio = {
            context: ctx,
            sampleRate: ctx.sampleRate,
            channels: channels,
            bufferSize: buffer_size,
            useWorklet: use_worklet && typeof AudioWorkletNode !== 'undefined',
            processor: null,
            gainNode: null,
            inputNode: null,
            inputStream: null,
            started: false,
            suspended: ctx.state === 'suspended'
        };

        /* Create gain node for volume control */
        window._wasmAudio.gainNode = ctx.createGain();
        window._wasmAudio.gainNode.connect(ctx.destination);

        /* Use ScriptProcessorNode (deprecated but widely supported) */
        if (!window._wasmAudio.useWorklet) {
            var processor = ctx.createScriptProcessor(buffer_size, 0, channels);
            processor.onaudioprocess = function(e) {
                var output = e.outputBuffer;
                var samples = output.length * channels;

                /* Get samples from WASM ring buffer */
                if (typeof Module !== 'undefined' && Module._wasm_audio_fill_buffer) {
                    Module._wasm_audio_fill_buffer(output.length);
                }

                /* Read interleaved data from ring buffer */
                var ringPtr = Module._wasm_audio_get_output_buffer();
                if (ringPtr) {
                    var ringData = new Float32Array(HEAPF32.buffer, ringPtr, samples);

                    for (var ch = 0; ch < channels; ch++) {
                        var channelData = output.getChannelData(ch);
                        for (var i = 0; i < output.length; i++) {
                            channelData[i] = ringData[i * channels + ch];
                        }
                    }
                } else {
                    /* Fill with silence */
                    for (var ch = 0; ch < channels; ch++) {
                        var channelData = output.getChannelData(ch);
                        for (var i = 0; i < output.length; i++) {
                            channelData[i] = 0;
                        }
                    }
                }
            };
            processor.connect(window._wasmAudio.gainNode);
            window._wasmAudio.processor = processor;
        }

        /* Handle iOS audio interruptions */
        document.addEventListener('visibilitychange', function() {
            if (document.hidden) {
                if (window._wasmAudio && window._wasmAudio.context.state === 'running') {
                    window._wasmAudio.context.suspend();
                }
            } else {
                if (window._wasmAudio && window._wasmAudio.context.state === 'suspended') {
                    window._wasmAudio.context.resume();
                }
            }
        });

        /* iOS Safari specific: handle audio session interruption */
        if (/iPhone|iPad|iPod/.test(navigator.userAgent)) {
            document.addEventListener('pause', function() {
                if (Module._wasm_audio_handle_interruption) {
                    Module._wasm_audio_handle_interruption(1);
                }
            });
            document.addEventListener('resume', function() {
                if (Module._wasm_audio_handle_interruption) {
                    Module._wasm_audio_handle_interruption(0);
                }
            });
        }

        console.log('WASM Audio: initialized at ' + ctx.sampleRate + 'Hz');
        return 0;

    } catch (e) {
        console.error('WASM Audio init error:', e);
        return -1;
    }
});

/* Resume audio context (required after user gesture) */
EM_JS(int, js_audio_resume, (), {
    if (!window._wasmAudio || !window._wasmAudio.context) {
        return -1;
    }

    var ctx = window._wasmAudio.context;
    if (ctx.state === 'suspended') {
        ctx.resume().then(function() {
            console.log('WASM Audio: resumed');
            window._wasmAudio.suspended = false;
        }).catch(function(e) {
            console.error('WASM Audio resume error:', e);
        });
    }
    return 0;
});

/* Suspend audio context */
EM_JS(int, js_audio_suspend, (), {
    if (!window._wasmAudio || !window._wasmAudio.context) {
        return -1;
    }

    window._wasmAudio.context.suspend();
    window._wasmAudio.suspended = true;
    return 0;
});

/* Get audio context state */
EM_JS(int, js_audio_get_state, (), {
    if (!window._wasmAudio || !window._wasmAudio.context) {
        return 0; /* CLOSED */
    }
    switch (window._wasmAudio.context.state) {
        case 'running': return 2;
        case 'suspended': return 1;
        case 'closed': return 0;
        default: return 0;
    }
});

/* Set output volume */
EM_JS(void, js_audio_set_volume, (float left, float right), {
    if (window._wasmAudio && window._wasmAudio.gainNode) {
        /* Use average for mono gain node */
        window._wasmAudio.gainNode.gain.value = (left + right) / 2.0;
    }
});

/* Request microphone access */
EM_JS(int, js_audio_request_input, (), {
    if (!window._wasmAudio) {
        return -1;
    }

    navigator.mediaDevices.getUserMedia({ audio: true })
        .then(function(stream) {
            window._wasmAudio.inputStream = stream;
            var ctx = window._wasmAudio.context;
            var source = ctx.createMediaStreamSource(stream);

            /* Create analyzer/processor for input */
            var processor = ctx.createScriptProcessor(1024, 1, 1);
            processor.onaudioprocess = function(e) {
                var input = e.inputBuffer.getChannelData(0);
                if (Module._wasm_audio_push_input) {
                    /* Copy to WASM input buffer */
                    var ptr = Module._wasm_audio_get_input_buffer();
                    if (ptr) {
                        var heapData = new Float32Array(HEAPF32.buffer, ptr, input.length);
                        heapData.set(input);
                        Module._wasm_audio_push_input(input.length);
                    }
                }
            };

            source.connect(processor);
            processor.connect(ctx.destination);
            window._wasmAudio.inputNode = processor;

            console.log('WASM Audio: microphone enabled');
        })
        .catch(function(e) {
            console.error('WASM Audio: microphone access denied', e);
        });

    return 0;
});

/* Check if autoplay is allowed */
EM_JS(int, js_audio_autoplay_allowed, (), {
    /* Check for user gesture requirement */
    if (!window._wasmAudio || !window._wasmAudio.context) {
        return 0;
    }
    return window._wasmAudio.context.state === 'running' ? 1 : 0;
});

/* Shutdown audio */
EM_JS(void, js_audio_shutdown, (), {
    if (window._wasmAudio) {
        if (window._wasmAudio.inputStream) {
            window._wasmAudio.inputStream.getTracks().forEach(function(track) {
                track.stop();
            });
        }
        if (window._wasmAudio.context) {
            window._wasmAudio.context.close();
        }
        window._wasmAudio = null;
        console.log('WASM Audio: shutdown');
    }
});

/* Get actual sample rate */
EM_JS(int, js_audio_get_sample_rate, (), {
    if (window._wasmAudio && window._wasmAudio.context) {
        return window._wasmAudio.context.sampleRate;
    }
    return 48000;
});

/* Get output latency */
EM_JS(float, js_audio_get_output_latency, (), {
    if (window._wasmAudio && window._wasmAudio.context) {
        /* baseLatency + outputLatency (if available) */
        var ctx = window._wasmAudio.context;
        var latency = ctx.baseLatency || 0;
        if (ctx.outputLatency) {
            latency += ctx.outputLatency;
        }
        return latency;
    }
    return 0.02; /* 20ms default */
});

#endif /* __EMSCRIPTEN__ */

/* ------------------------------------------------------------------ */
/* Global Audio API Implementation                                     */
/* ------------------------------------------------------------------ */

/* Temporary buffer for audio processing */
static float *wasm_audio_temp_buffer = NULL;
static size_t wasm_audio_temp_buffer_size = 0;

#ifdef __EMSCRIPTEN__
EMSCRIPTEN_KEEPALIVE
#endif
float *wasm_audio_get_output_buffer(void)
{
    if (!wasm_audio_state) {
        return NULL;
    }
    return wasm_audio_state->output_ring.buffer;
}

#ifdef __EMSCRIPTEN__
EMSCRIPTEN_KEEPALIVE
#endif
float *wasm_audio_get_input_buffer(void)
{
    if (!wasm_audio_temp_buffer) {
        wasm_audio_temp_buffer_size = 4096;
        wasm_audio_temp_buffer = g_malloc(wasm_audio_temp_buffer_size * sizeof(float));
    }
    return wasm_audio_temp_buffer;
}

#ifdef __EMSCRIPTEN__
EMSCRIPTEN_KEEPALIVE
#endif
void wasm_audio_fill_buffer(int samples)
{
    /* Called from JavaScript to request samples */
    if (!wasm_audio_state) {
        return;
    }

    /* Check for underrun */
    size_t available = ring_available_read(&wasm_audio_state->output_ring);
    int channels = wasm_audio_state->config.channels;
    size_t needed = samples * channels;

    if (available < needed) {
        wasm_audio_state->info.underruns++;
    }
}

#ifdef __EMSCRIPTEN__
EMSCRIPTEN_KEEPALIVE
#endif
void wasm_audio_push_input(int samples)
{
    /* Called from JavaScript when input samples are available */
    if (!wasm_audio_state || !wasm_audio_temp_buffer) {
        return;
    }

    ring_write(&wasm_audio_state->input_ring, wasm_audio_temp_buffer, samples);
    wasm_audio_state->info.samples_captured += samples;
}

#ifdef __EMSCRIPTEN__
EMSCRIPTEN_KEEPALIVE
#endif
int wasm_audio_init(const WasmAudioConfig *config)
{
    if (wasm_audio_state && wasm_audio_state->initialized) {
        return 0;
    }

    wasm_audio_state = g_new0(WasmAudioGlobalState, 1);

    /* Set default config if not provided */
    if (config) {
        memcpy(&wasm_audio_state->config, config, sizeof(WasmAudioConfig));
    } else {
        wasm_audio_state->config.sample_rate = WASM_AUDIO_DEFAULT_RATE;
        wasm_audio_state->config.channels = WASM_AUDIO_DEFAULT_CHANNELS;
        wasm_audio_state->config.buffer_size = WASM_AUDIO_DEFAULT_SAMPLES;
        wasm_audio_state->config.backend = WASM_AUDIO_BACKEND_SCRIPT_PROCESSOR;
    }

    /* Allocate ring buffers */
    wasm_audio_state->output_ring.buffer = g_malloc0(WASM_AUDIO_RING_SIZE * sizeof(float));
    wasm_audio_state->output_ring.size = WASM_AUDIO_RING_SIZE;

    wasm_audio_state->input_ring.buffer = g_malloc0(WASM_AUDIO_RING_SIZE * sizeof(float));
    wasm_audio_state->input_ring.size = WASM_AUDIO_RING_SIZE;

    /* Default volume */
    wasm_audio_state->volume_left = 1.0f;
    wasm_audio_state->volume_right = 1.0f;
    wasm_audio_state->input_gain = 1.0f;

#ifdef __EMSCRIPTEN__
    int use_worklet = (wasm_audio_state->config.backend == WASM_AUDIO_BACKEND_AUDIO_WORKLET);
    int ret = js_audio_init(wasm_audio_state->config.sample_rate,
                            wasm_audio_state->config.channels,
                            wasm_audio_state->config.buffer_size,
                            use_worklet);
    if (ret < 0) {
        g_free(wasm_audio_state->output_ring.buffer);
        g_free(wasm_audio_state->input_ring.buffer);
        g_free(wasm_audio_state);
        wasm_audio_state = NULL;
        return -1;
    }

    /* Update actual sample rate */
    wasm_audio_state->info.actual_sample_rate = js_audio_get_sample_rate();
    wasm_audio_state->info.output_latency_sec = js_audio_get_output_latency();
#endif

    wasm_audio_state->info.state = WASM_AUDIO_STATE_SUSPENDED;
    wasm_audio_state->initialized = true;

    fprintf(stderr, "wasm-audio: initialized at %dHz, %d channels\n",
            wasm_audio_state->info.actual_sample_rate,
            wasm_audio_state->config.channels);

    return 0;
}

#ifdef __EMSCRIPTEN__
EMSCRIPTEN_KEEPALIVE
#endif
void wasm_audio_shutdown(void)
{
    if (!wasm_audio_state) {
        return;
    }

#ifdef __EMSCRIPTEN__
    js_audio_shutdown();
#endif

    g_free(wasm_audio_state->output_ring.buffer);
    g_free(wasm_audio_state->input_ring.buffer);
    g_free(wasm_audio_state);
    wasm_audio_state = NULL;

    if (wasm_audio_temp_buffer) {
        g_free(wasm_audio_temp_buffer);
        wasm_audio_temp_buffer = NULL;
    }

    fprintf(stderr, "wasm-audio: shutdown complete\n");
}

#ifdef __EMSCRIPTEN__
EMSCRIPTEN_KEEPALIVE
#endif
WasmAudioInfo *wasm_audio_get_info(void)
{
    if (!wasm_audio_state) {
        return NULL;
    }

#ifdef __EMSCRIPTEN__
    wasm_audio_state->info.state = (WasmAudioState)js_audio_get_state();
#endif

    return &wasm_audio_state->info;
}

#ifdef __EMSCRIPTEN__
EMSCRIPTEN_KEEPALIVE
#endif
int wasm_audio_resume(void)
{
#ifdef __EMSCRIPTEN__
    return js_audio_resume();
#else
    return 0;
#endif
}

#ifdef __EMSCRIPTEN__
EMSCRIPTEN_KEEPALIVE
#endif
int wasm_audio_suspend(void)
{
#ifdef __EMSCRIPTEN__
    return js_audio_suspend();
#else
    return 0;
#endif
}

#ifdef __EMSCRIPTEN__
EMSCRIPTEN_KEEPALIVE
#endif
size_t wasm_audio_write(const void *data, size_t samples)
{
    if (!wasm_audio_state || !data) {
        return 0;
    }

    const float *fdata = (const float *)data;
    size_t written = ring_write(&wasm_audio_state->output_ring, fdata,
                                samples * wasm_audio_state->config.channels);

    wasm_audio_state->info.samples_played += written / wasm_audio_state->config.channels;
    return written / wasm_audio_state->config.channels;
}

#ifdef __EMSCRIPTEN__
EMSCRIPTEN_KEEPALIVE
#endif
size_t wasm_audio_get_free(void)
{
    if (!wasm_audio_state) {
        return 0;
    }

    return ring_available_write(&wasm_audio_state->output_ring) /
           wasm_audio_state->config.channels;
}

#ifdef __EMSCRIPTEN__
EMSCRIPTEN_KEEPALIVE
#endif
void wasm_audio_set_volume(float left, float right)
{
    if (!wasm_audio_state) {
        return;
    }

    wasm_audio_state->volume_left = left;
    wasm_audio_state->volume_right = right;

#ifdef __EMSCRIPTEN__
    if (!wasm_audio_state->muted) {
        js_audio_set_volume(left, right);
    }
#endif
}

#ifdef __EMSCRIPTEN__
EMSCRIPTEN_KEEPALIVE
#endif
void wasm_audio_set_mute(bool mute)
{
    if (!wasm_audio_state) {
        return;
    }

    wasm_audio_state->muted = mute;

#ifdef __EMSCRIPTEN__
    if (mute) {
        js_audio_set_volume(0, 0);
    } else {
        js_audio_set_volume(wasm_audio_state->volume_left,
                            wasm_audio_state->volume_right);
    }
#endif
}

#ifdef __EMSCRIPTEN__
EMSCRIPTEN_KEEPALIVE
#endif
int wasm_audio_request_input(void)
{
#ifdef __EMSCRIPTEN__
    return js_audio_request_input();
#else
    return -1;
#endif
}

#ifdef __EMSCRIPTEN__
EMSCRIPTEN_KEEPALIVE
#endif
size_t wasm_audio_read(void *data, size_t samples)
{
    if (!wasm_audio_state || !data) {
        return 0;
    }

    float *fdata = (float *)data;
    return ring_read(&wasm_audio_state->input_ring, fdata, samples);
}

#ifdef __EMSCRIPTEN__
EMSCRIPTEN_KEEPALIVE
#endif
size_t wasm_audio_get_available(void)
{
    if (!wasm_audio_state) {
        return 0;
    }

    return ring_available_read(&wasm_audio_state->input_ring);
}

#ifdef __EMSCRIPTEN__
EMSCRIPTEN_KEEPALIVE
#endif
void wasm_audio_set_input_gain(float gain)
{
    if (wasm_audio_state) {
        wasm_audio_state->input_gain = gain;
    }
}

#ifdef __EMSCRIPTEN__
EMSCRIPTEN_KEEPALIVE
#endif
bool wasm_audio_is_interrupted(void)
{
    if (!wasm_audio_state) {
        return false;
    }
    return wasm_audio_state->interrupted;
}

#ifdef __EMSCRIPTEN__
EMSCRIPTEN_KEEPALIVE
#endif
void wasm_audio_handle_interruption(bool began)
{
    if (!wasm_audio_state) {
        return;
    }

    wasm_audio_state->interrupted = began;
    wasm_audio_state->info.state = began ? WASM_AUDIO_STATE_INTERRUPTED :
                                           WASM_AUDIO_STATE_SUSPENDED;
}

#ifdef __EMSCRIPTEN__
EMSCRIPTEN_KEEPALIVE
#endif
bool wasm_audio_autoplay_allowed(void)
{
#ifdef __EMSCRIPTEN__
    return js_audio_autoplay_allowed() != 0;
#else
    return true;
#endif
}

/* ------------------------------------------------------------------ */
/* QEMU Audio Driver Implementation                                    */
/* ------------------------------------------------------------------ */

static size_t wasm_write(HWVoiceOut *hw, void *buf, size_t len)
{
    WasmVoiceOut *wasm = (WasmVoiceOut *)hw;

    /* Convert to float samples */
    size_t samples = len / hw->info.bytes_per_frame;

    /* Simple conversion from int16 to float */
    if (hw->info.bits == 16 && hw->info.is_signed) {
        int16_t *src = (int16_t *)buf;
        size_t float_samples = samples * hw->info.nchannels;

        if (!wasm_audio_temp_buffer || wasm_audio_temp_buffer_size < float_samples) {
            g_free(wasm_audio_temp_buffer);
            wasm_audio_temp_buffer_size = float_samples;
            wasm_audio_temp_buffer = g_malloc(float_samples * sizeof(float));
        }

        for (size_t i = 0; i < float_samples; i++) {
            wasm_audio_temp_buffer[i] = src[i] / 32768.0f;
        }

        wasm_audio_write(wasm_audio_temp_buffer, samples);
    }

    return audio_rate_get_bytes(&wasm->rate, &hw->info, len);
}

static int wasm_init_out(HWVoiceOut *hw, struct audsettings *as, void *drv_opaque)
{
    WasmVoiceOut *wasm = (WasmVoiceOut *)hw;

    /* Initialize the global audio state if not done */
    WasmAudioConfig config = {
        .sample_rate = as->freq,
        .channels = as->nchannels,
        .buffer_size = 1024,
        .backend = WASM_AUDIO_BACKEND_SCRIPT_PROCESSOR,
    };
    wasm_audio_init(&config);

    audio_pcm_init_info(&hw->info, as);
    hw->samples = 1024;
    audio_rate_start(&wasm->rate);

    return 0;
}

static void wasm_fini_out(HWVoiceOut *hw)
{
    /* Global shutdown handled elsewhere */
}

static void wasm_enable_out(HWVoiceOut *hw, bool enable)
{
    WasmVoiceOut *wasm = (WasmVoiceOut *)hw;

    if (enable) {
        audio_rate_start(&wasm->rate);
        wasm_audio_resume();
    } else {
        wasm_audio_suspend();
    }
}

static int wasm_init_in(HWVoiceIn *hw, struct audsettings *as, void *drv_opaque)
{
    WasmVoiceIn *wasm = (WasmVoiceIn *)hw;

    audio_pcm_init_info(&hw->info, as);
    hw->samples = 1024;
    audio_rate_start(&wasm->rate);

    /* Request microphone access */
    wasm_audio_request_input();

    return 0;
}

static void wasm_fini_in(HWVoiceIn *hw)
{
    /* Cleanup handled by global shutdown */
}

static size_t wasm_read(HWVoiceIn *hw, void *buf, size_t size)
{
    WasmVoiceIn *wasm = (WasmVoiceIn *)hw;
    int64_t bytes = audio_rate_get_bytes(&wasm->rate, &hw->info, size);

    /* Read from input ring buffer and convert */
    size_t samples = bytes / hw->info.bytes_per_frame;
    size_t float_samples = samples * hw->info.nchannels;

    if (!wasm_audio_temp_buffer || wasm_audio_temp_buffer_size < float_samples) {
        g_free(wasm_audio_temp_buffer);
        wasm_audio_temp_buffer_size = float_samples;
        wasm_audio_temp_buffer = g_malloc(float_samples * sizeof(float));
    }

    size_t read_samples = wasm_audio_read(wasm_audio_temp_buffer, float_samples);

    /* Convert float to int16 */
    if (hw->info.bits == 16 && hw->info.is_signed) {
        int16_t *dst = (int16_t *)buf;
        for (size_t i = 0; i < read_samples; i++) {
            float sample = wasm_audio_temp_buffer[i] * wasm_audio_state->input_gain;
            if (sample > 1.0f) sample = 1.0f;
            if (sample < -1.0f) sample = -1.0f;
            dst[i] = (int16_t)(sample * 32767.0f);
        }
    } else {
        audio_pcm_info_clear_buf(&hw->info, buf, bytes / hw->info.bytes_per_frame);
    }

    return bytes;
}

static void wasm_enable_in(HWVoiceIn *hw, bool enable)
{
    WasmVoiceIn *wasm = (WasmVoiceIn *)hw;

    if (enable) {
        audio_rate_start(&wasm->rate);
    }
}

static void *wasm_audio_drv_init(Audiodev *dev, Error **errp)
{
    return &wasm_audio_drv_init;
}

static void wasm_audio_drv_fini(void *opaque)
{
    wasm_audio_shutdown();
}

static struct audio_pcm_ops wasm_pcm_ops = {
    .init_out = wasm_init_out,
    .fini_out = wasm_fini_out,
    .write    = wasm_write,
    .buffer_get_free = audio_generic_buffer_get_free,
    .run_buffer_out = audio_generic_run_buffer_out,
    .enable_out = wasm_enable_out,

    .init_in  = wasm_init_in,
    .fini_in  = wasm_fini_in,
    .read     = wasm_read,
    .run_buffer_in = audio_generic_run_buffer_in,
    .enable_in = wasm_enable_in,
};

static struct audio_driver wasm_audio_driver = {
    .name           = "wasm",
    .descr          = "Web Audio API audio output",
    .init           = wasm_audio_drv_init,
    .fini           = wasm_audio_drv_fini,
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
