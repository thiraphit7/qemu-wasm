/**
 * QEMU WASM Display - JavaScript Integration
 *
 * This module provides a JavaScript interface for rendering QEMU's
 * framebuffer output to HTML5 Canvas, WebGL, or WebGPU.
 *
 * Features:
 * - Canvas 2D rendering (default, widest compatibility)
 * - WebGL rendering (better performance)
 * - WebGPU rendering (experimental, best performance)
 * - VirtIO-GPU resource tracking
 * - iOS Safari optimizations
 * - Touch input support
 * - Web Audio integration
 *
 * Compatible with Safari (iOS/macOS), Chrome, Firefox, and Edge.
 *
 * Usage:
 *   import { WasmDisplay } from './wasm-display.js';
 *   const display = new WasmDisplay(canvas, Module, options);
 *   display.start();
 */

// Render backend constants (match C enum)
export const RenderBackend = {
    CANVAS2D: 0,
    WEBGL: 1,
    WEBGPU: 2
};

/**
 * QEMU WASM Display class
 */
export class WasmDisplay {
    /**
     * Create a new WASM display instance.
     *
     * @param {HTMLCanvasElement} canvas - Target canvas element
     * @param {Object} module - Emscripten Module object
     * @param {Object} options - Configuration options
     */
    constructor(canvas, module, options = {}) {
        this.canvas = canvas;
        this.module = module;

        // Options with defaults
        this.options = {
            renderBackend: options.renderBackend ?? RenderBackend.CANVAS2D,
            targetFps: options.targetFps ?? 60,
            enableInput: options.enableInput !== false,
            enableTouch: options.enableTouch !== false,
            scaleToFit: options.scaleToFit !== false,
            cursorStyle: options.cursorStyle ?? 'default',
            lowPowerMode: options.lowPowerMode ?? false,
            enableProfiling: options.enableProfiling ?? false,
            ...options
        };

        // State
        this.running = false;
        this.width = 0;
        this.height = 0;
        this.frameRequest = null;
        this.lastFrameTime = 0;
        this.frameInterval = 1000 / this.options.targetFps;

        // Rendering contexts
        this.ctx2d = null;
        this.glCtx = null;
        this.gpuDevice = null;
        this.imageData = null;
        this.glTexture = null;

        // iOS optimization state
        this.isVisible = true;
        this.iosOptimized = false;

        // Performance stats
        this.stats = {
            fps: 0,
            frameTime: 0,
            renderTime: 0,
            lastTime: performance.now(),
            frameCount: 0
        };

        // Bind methods
        this._render = this._render.bind(this);
        this._handleKeyDown = this._handleKeyDown.bind(this);
        this._handleKeyUp = this._handleKeyUp.bind(this);
        this._handleMouseMove = this._handleMouseMove.bind(this);
        this._handleMouseDown = this._handleMouseDown.bind(this);
        this._handleMouseUp = this._handleMouseUp.bind(this);
        this._handleWheel = this._handleWheel.bind(this);
        this._handleContextMenu = this._handleContextMenu.bind(this);
        this._handleTouchStart = this._handleTouchStart.bind(this);
        this._handleTouchMove = this._handleTouchMove.bind(this);
        this._handleTouchEnd = this._handleTouchEnd.bind(this);
        this._handleVisibilityChange = this._handleVisibilityChange.bind(this);

        // Setup callbacks
        this._setupCallbacks();
    }

    /**
     * Setup global callback functions for QEMU.
     */
    _setupCallbacks() {
        const self = this;

        window.onWasmDisplayReady = function() {
            console.log('WASM Display: Ready');
            self._initRenderingContext();
            if (self.options.onReady) {
                self.options.onReady();
            }
        };

        window.onWasmFramebufferUpdate = function(x, y, w, h) {
            // Partial update info available
            self._dirtyRegion = { x, y, w, h };
        };

        window.onWasmFramebufferResize = function(width, height) {
            console.log(`WASM Display: Resize to ${width}x${height}`);
            self._handleResize(width, height);
        };

        window.onWasmMouseUpdate = function(x, y, visible) {
            self.canvas.style.cursor = visible ? self.options.cursorStyle : 'none';
        };

        window.onWasmCursorDefine = function(width, height, hotX, hotY, dataPtr) {
            self._updateCustomCursor(width, height, hotX, hotY, dataPtr);
        };

        window.onWasmGpuResourceCreated = function(id, w, h, format) {
            console.log(`WASM Display: GPU resource ${id} created (${w}x${h})`);
        };

        window.onWasmGpuResourceDestroyed = function(id) {
            console.log(`WASM Display: GPU resource ${id} destroyed`);
        };

        window.onWasmGpuScanoutSet = function(scanoutId, resourceId, w, h) {
            console.log(`WASM Display: Scanout ${scanoutId} -> resource ${resourceId}`);
        };

        window.onWasmGpuResourceFlush = function(id, x, y, w, h) {
            // GPU resource flushed - can be used for partial updates
        };

        window.onWasmRenderBackendChange = function(backend) {
            console.log(`WASM Display: Backend changed to ${backend}`);
        };

        window.onWasmVisibilityChange = function(visible) {
            self.isVisible = visible;
        };

        window.onWasmLowPowerModeChange = function(enabled) {
            self.options.lowPowerMode = enabled;
        };
    }

    /**
     * Initialize rendering context based on selected backend.
     */
    async _initRenderingContext() {
        const backend = this.options.renderBackend;

        if (backend === RenderBackend.WEBGPU && navigator.gpu) {
            try {
                await this._initWebGPU();
                return;
            } catch (e) {
                console.warn('WebGPU init failed, falling back:', e);
            }
        }

        if (backend === RenderBackend.WEBGL || backend === RenderBackend.WEBGPU) {
            try {
                this._initWebGL();
                return;
            } catch (e) {
                console.warn('WebGL init failed, falling back:', e);
            }
        }

        // Default: Canvas 2D
        this._initCanvas2D();
    }

    /**
     * Initialize Canvas 2D context.
     */
    _initCanvas2D() {
        this.ctx2d = this.canvas.getContext('2d', {
            alpha: false,
            desynchronized: true  // Better performance
        });
        this.renderMode = 'canvas2d';
        console.log('WASM Display: Using Canvas 2D');
    }

    /**
     * Initialize WebGL context.
     */
    _initWebGL() {
        const gl = this.canvas.getContext('webgl2') ||
                   this.canvas.getContext('webgl');

        if (!gl) {
            throw new Error('WebGL not supported');
        }

        this.glCtx = gl;

        // Create texture
        this.glTexture = gl.createTexture();
        gl.bindTexture(gl.TEXTURE_2D, this.glTexture);
        gl.texParameteri(gl.TEXTURE_2D, gl.TEXTURE_WRAP_S, gl.CLAMP_TO_EDGE);
        gl.texParameteri(gl.TEXTURE_2D, gl.TEXTURE_WRAP_T, gl.CLAMP_TO_EDGE);
        gl.texParameteri(gl.TEXTURE_2D, gl.TEXTURE_MIN_FILTER, gl.LINEAR);
        gl.texParameteri(gl.TEXTURE_2D, gl.TEXTURE_MAG_FILTER, gl.LINEAR);

        // Create shader program
        this._createGLShaders();

        this.renderMode = 'webgl';
        console.log('WASM Display: Using WebGL');
    }

    /**
     * Create WebGL shaders.
     */
    _createGLShaders() {
        const gl = this.glCtx;

        const vsSource = `
            attribute vec4 aPosition;
            attribute vec2 aTexCoord;
            varying vec2 vTexCoord;
            void main() {
                gl_Position = aPosition;
                vTexCoord = aTexCoord;
            }
        `;

        const fsSource = `
            precision mediump float;
            varying vec2 vTexCoord;
            uniform sampler2D uSampler;
            void main() {
                gl_FragColor = texture2D(uSampler, vTexCoord);
            }
        `;

        const vs = gl.createShader(gl.VERTEX_SHADER);
        gl.shaderSource(vs, vsSource);
        gl.compileShader(vs);

        const fs = gl.createShader(gl.FRAGMENT_SHADER);
        gl.shaderSource(fs, fsSource);
        gl.compileShader(fs);

        this.glProgram = gl.createProgram();
        gl.attachShader(this.glProgram, vs);
        gl.attachShader(this.glProgram, fs);
        gl.linkProgram(this.glProgram);

        gl.useProgram(this.glProgram);

        // Setup vertex data
        const vertices = new Float32Array([
            -1, -1, 0, 1,
             1, -1, 1, 1,
            -1,  1, 0, 0,
             1,  1, 1, 0
        ]);

        const vbo = gl.createBuffer();
        gl.bindBuffer(gl.ARRAY_BUFFER, vbo);
        gl.bufferData(gl.ARRAY_BUFFER, vertices, gl.STATIC_DRAW);

        const aPosition = gl.getAttribLocation(this.glProgram, 'aPosition');
        gl.enableVertexAttribArray(aPosition);
        gl.vertexAttribPointer(aPosition, 2, gl.FLOAT, false, 16, 0);

        const aTexCoord = gl.getAttribLocation(this.glProgram, 'aTexCoord');
        gl.enableVertexAttribArray(aTexCoord);
        gl.vertexAttribPointer(aTexCoord, 2, gl.FLOAT, false, 16, 8);
    }

    /**
     * Initialize WebGPU context (experimental).
     */
    async _initWebGPU() {
        if (!navigator.gpu) {
            throw new Error('WebGPU not supported');
        }

        const adapter = await navigator.gpu.requestAdapter({
            powerPreference: this.options.lowPowerMode ? 'low-power' : 'high-performance'
        });

        if (!adapter) {
            throw new Error('No WebGPU adapter');
        }

        this.gpuDevice = await adapter.requestDevice();
        this.gpuContext = this.canvas.getContext('webgpu');

        this.gpuContext.configure({
            device: this.gpuDevice,
            format: navigator.gpu.getPreferredCanvasFormat(),
            alphaMode: 'opaque'
        });

        this.renderMode = 'webgpu';
        console.log('WASM Display: Using WebGPU');

        // Notify QEMU
        if (this.module._wasm_webgpu_init) {
            this.module._wasm_webgpu_init();
        }
    }

    /**
     * Start the display.
     */
    start() {
        if (this.running) return;

        this.running = true;
        this.isVisible = true;

        // Setup input handlers
        if (this.options.enableInput) {
            this._setupInputHandlers();
        }

        // Setup visibility handler
        document.addEventListener('visibilitychange', this._handleVisibilityChange);

        // Enable profiling if requested
        if (this.options.enableProfiling && this.module._wasm_enable_profiling) {
            this.module._wasm_enable_profiling(1);
        }

        // Start render loop
        this._render();

        console.log('WASM Display: Started');
    }

    /**
     * Stop the display.
     */
    stop() {
        this.running = false;

        if (this.frameRequest) {
            cancelAnimationFrame(this.frameRequest);
            this.frameRequest = null;
        }

        this._removeInputHandlers();
        document.removeEventListener('visibilitychange', this._handleVisibilityChange);

        console.log('WASM Display: Stopped');
    }

    /**
     * Main render loop.
     */
    _render() {
        if (!this.running) return;

        const now = performance.now();
        const elapsed = now - this.lastFrameTime;

        // Frame rate limiting (iOS optimization)
        if (elapsed < this.frameInterval) {
            this.frameRequest = requestAnimationFrame(this._render);
            return;
        }

        this.lastFrameTime = now - (elapsed % this.frameInterval);

        // Skip rendering if not visible
        if (!this.isVisible) {
            this.frameRequest = requestAnimationFrame(this._render);
            return;
        }

        try {
            const renderStart = performance.now();

            // Check if framebuffer needs update
            if (this._checkFramebufferDirty()) {
                this._updateDisplay();
                this._ackFramebuffer();

                // Update stats
                this.stats.frameCount++;
                this.stats.renderTime = performance.now() - renderStart;
            }

            // Calculate FPS every second
            if (now - this.stats.lastTime >= 1000) {
                this.stats.fps = this.stats.frameCount;
                this.stats.frameCount = 0;
                this.stats.lastTime = now;
            }

        } catch (e) {
            console.error('WASM Display: Render error', e);
        }

        this.frameRequest = requestAnimationFrame(this._render);
    }

    /**
     * Check if framebuffer has updates.
     */
    _checkFramebufferDirty() {
        if (this.module._wasm_framebuffer_is_dirty) {
            return this.module._wasm_framebuffer_is_dirty();
        }
        return false;
    }

    /**
     * Acknowledge framebuffer read.
     */
    _ackFramebuffer() {
        if (this.module._wasm_framebuffer_ack) {
            this.module._wasm_framebuffer_ack();
        }
    }

    /**
     * Update display from framebuffer.
     */
    _updateDisplay() {
        const fbInfo = this._getFramebufferInfo();
        if (!fbInfo) return;

        switch (this.renderMode) {
            case 'webgpu':
                this._renderWebGPU(fbInfo);
                break;
            case 'webgl':
                this._renderWebGL(fbInfo);
                break;
            default:
                this._renderCanvas2D(fbInfo);
                break;
        }
    }

    /**
     * Get framebuffer info from QEMU.
     */
    _getFramebufferInfo() {
        if (!this.module._wasm_get_framebuffer_info) return null;

        const infoPtr = this.module._wasm_get_framebuffer_info();
        if (!infoPtr) return null;

        const HEAP32 = this.module.HEAP32;
        const HEAPU8 = this.module.HEAPU8;

        const dataPtr = HEAP32[infoPtr >> 2];
        const width = HEAP32[(infoPtr + 4) >> 2];
        const height = HEAP32[(infoPtr + 8) >> 2];
        const stride = HEAP32[(infoPtr + 12) >> 2];
        const dirty = HEAPU8[infoPtr + 24] !== 0;

        if (width <= 0 || height <= 0 || !dataPtr) return null;

        return { dataPtr, width, height, stride, dirty };
    }

    /**
     * Render using Canvas 2D.
     */
    _renderCanvas2D(fbInfo) {
        const { dataPtr, width, height, stride } = fbInfo;

        if (this.width !== width || this.height !== height) {
            this._handleResize(width, height);
        }

        if (!this.imageData || this.imageData.width !== width || this.imageData.height !== height) {
            this.imageData = this.ctx2d.createImageData(width, height);
        }

        // Copy pixel data
        const HEAPU8 = this.module.HEAPU8;
        const dstData = this.imageData.data;

        for (let y = 0; y < height; y++) {
            const srcOffset = dataPtr + y * stride;
            const dstOffset = y * width * 4;
            for (let x = 0; x < width * 4; x++) {
                dstData[dstOffset + x] = HEAPU8[srcOffset + x];
            }
        }

        this.ctx2d.putImageData(this.imageData, 0, 0);
    }

    /**
     * Render using WebGL.
     */
    _renderWebGL(fbInfo) {
        const { dataPtr, width, height, stride } = fbInfo;
        const gl = this.glCtx;

        if (this.width !== width || this.height !== height) {
            this._handleResize(width, height);
        }

        // Upload texture
        const HEAPU8 = this.module.HEAPU8;
        const data = HEAPU8.subarray(dataPtr, dataPtr + height * stride);

        gl.bindTexture(gl.TEXTURE_2D, this.glTexture);
        gl.texImage2D(gl.TEXTURE_2D, 0, gl.RGBA, width, height, 0,
                      gl.RGBA, gl.UNSIGNED_BYTE, data);

        // Draw
        gl.viewport(0, 0, width, height);
        gl.drawArrays(gl.TRIANGLE_STRIP, 0, 4);
    }

    /**
     * Render using WebGPU.
     */
    _renderWebGPU(fbInfo) {
        if (!this.gpuDevice) return;

        const { width, height } = fbInfo;

        if (this.width !== width || this.height !== height) {
            this._handleResize(width, height);
        }

        // Upload texture via QEMU helper
        if (this.module._wasm_webgpu_upload_texture) {
            this.module._wasm_webgpu_upload_texture();
        }

        // Render pass would be done by custom shader setup
        // For now, fall back to Canvas 2D for display
        this._renderCanvas2D(fbInfo);
    }

    /**
     * Handle resolution change.
     */
    _handleResize(width, height) {
        this.width = width;
        this.height = height;

        this.canvas.width = width;
        this.canvas.height = height;

        if (this.options.scaleToFit) {
            const container = this.canvas.parentElement;
            if (container) {
                const cw = container.clientWidth;
                const ch = container.clientHeight;
                const scale = Math.min(cw / width, ch / height);
                this.canvas.style.width = `${width * scale}px`;
                this.canvas.style.height = `${height * scale}px`;
            }
        }

        this.imageData = null;

        if (this.options.onResize) {
            this.options.onResize(width, height);
        }
    }

    /**
     * Setup input event handlers.
     */
    _setupInputHandlers() {
        this.canvas.tabIndex = 0;
        this.canvas.style.cursor = this.options.cursorStyle;

        this.canvas.addEventListener('keydown', this._handleKeyDown);
        this.canvas.addEventListener('keyup', this._handleKeyUp);
        this.canvas.addEventListener('mousemove', this._handleMouseMove);
        this.canvas.addEventListener('mousedown', this._handleMouseDown);
        this.canvas.addEventListener('mouseup', this._handleMouseUp);
        this.canvas.addEventListener('wheel', this._handleWheel);
        this.canvas.addEventListener('contextmenu', this._handleContextMenu);

        if (this.options.enableTouch) {
            this.canvas.addEventListener('touchstart', this._handleTouchStart, { passive: false });
            this.canvas.addEventListener('touchmove', this._handleTouchMove, { passive: false });
            this.canvas.addEventListener('touchend', this._handleTouchEnd, { passive: false });
        }

        this.canvas.addEventListener('click', () => this.canvas.focus());
    }

    /**
     * Remove input event handlers.
     */
    _removeInputHandlers() {
        this.canvas.removeEventListener('keydown', this._handleKeyDown);
        this.canvas.removeEventListener('keyup', this._handleKeyUp);
        this.canvas.removeEventListener('mousemove', this._handleMouseMove);
        this.canvas.removeEventListener('mousedown', this._handleMouseDown);
        this.canvas.removeEventListener('mouseup', this._handleMouseUp);
        this.canvas.removeEventListener('wheel', this._handleWheel);
        this.canvas.removeEventListener('contextmenu', this._handleContextMenu);
        this.canvas.removeEventListener('touchstart', this._handleTouchStart);
        this.canvas.removeEventListener('touchmove', this._handleTouchMove);
        this.canvas.removeEventListener('touchend', this._handleTouchEnd);
    }

    /**
     * Handle visibility change.
     */
    _handleVisibilityChange() {
        this.isVisible = document.visibilityState === 'visible';
        if (this.module._wasm_handle_visibility_change) {
            this.module._wasm_handle_visibility_change(this.isVisible ? 1 : 0);
        }
    }

    /**
     * Get mouse position relative to canvas.
     */
    _getMousePosition(event) {
        const rect = this.canvas.getBoundingClientRect();
        const scaleX = this.width / rect.width;
        const scaleY = this.height / rect.height;
        return {
            x: Math.floor((event.clientX - rect.left) * scaleX),
            y: Math.floor((event.clientY - rect.top) * scaleY)
        };
    }

    /**
     * Convert browser keycode to QEMU QKeyCode.
     */
    _keyToQKeyCode(event) {
        const keyMap = {
            'Escape': 1, 'F1': 67, 'F2': 68, 'F3': 69, 'F4': 70,
            'F5': 71, 'F6': 72, 'F7': 73, 'F8': 74, 'F9': 75, 'F10': 76,
            'Backquote': 41, 'Digit1': 2, 'Digit2': 3, 'Digit3': 4,
            'Digit4': 5, 'Digit5': 6, 'Digit6': 7, 'Digit7': 8,
            'Digit8': 9, 'Digit9': 10, 'Digit0': 11, 'Minus': 12, 'Equal': 13,
            'Backspace': 14, 'Tab': 15, 'KeyQ': 16, 'KeyW': 17, 'KeyE': 18,
            'KeyR': 19, 'KeyT': 20, 'KeyY': 21, 'KeyU': 22, 'KeyI': 23, 'KeyO': 24,
            'KeyP': 25, 'BracketLeft': 26, 'BracketRight': 27, 'Enter': 28,
            'ControlLeft': 29, 'KeyA': 30, 'KeyS': 31, 'KeyD': 32, 'KeyF': 33,
            'KeyG': 34, 'KeyH': 35, 'KeyJ': 36, 'KeyK': 37, 'KeyL': 38,
            'Semicolon': 39, 'Quote': 40, 'ShiftLeft': 42, 'Backslash': 43,
            'KeyZ': 44, 'KeyX': 45, 'KeyC': 46, 'KeyV': 47, 'KeyB': 48,
            'KeyN': 49, 'KeyM': 50, 'Comma': 51, 'Period': 52, 'Slash': 53,
            'ShiftRight': 54, 'AltLeft': 56, 'Space': 57, 'CapsLock': 58,
            'ArrowUp': 103, 'ArrowLeft': 105, 'ArrowRight': 106, 'ArrowDown': 108,
            'Delete': 111, 'Home': 102, 'End': 107, 'PageUp': 104, 'PageDown': 109,
            'Insert': 110, 'ControlRight': 97, 'AltRight': 100
        };
        return keyMap[event.code] || 0;
    }

    _handleKeyDown(event) {
        event.preventDefault();
        const keycode = this._keyToQKeyCode(event);
        if (keycode && this.module._wasm_send_keyboard_event) {
            this.module._wasm_send_keyboard_event(keycode, 1);
        }
    }

    _handleKeyUp(event) {
        event.preventDefault();
        const keycode = this._keyToQKeyCode(event);
        if (keycode && this.module._wasm_send_keyboard_event) {
            this.module._wasm_send_keyboard_event(keycode, 0);
        }
    }

    _handleMouseMove(event) {
        const pos = this._getMousePosition(event);
        if (this.module._wasm_send_mouse_motion) {
            this.module._wasm_send_mouse_motion(pos.x, pos.y);
        }
    }

    _handleMouseDown(event) {
        event.preventDefault();
        if (this.module._wasm_send_mouse_button) {
            this.module._wasm_send_mouse_button(event.button, 1);
        }
    }

    _handleMouseUp(event) {
        event.preventDefault();
        if (this.module._wasm_send_mouse_button) {
            this.module._wasm_send_mouse_button(event.button, 0);
        }
    }

    _handleWheel(event) {
        event.preventDefault();
        if (this.module._wasm_send_mouse_wheel) {
            const dx = Math.sign(event.deltaX);
            const dy = Math.sign(event.deltaY);
            this.module._wasm_send_mouse_wheel(dx, -dy);
        }
    }

    _handleContextMenu(event) {
        event.preventDefault();
    }

    _handleTouchStart(event) {
        event.preventDefault();
        const touch = event.touches[0];
        const pos = this._getMousePosition(touch);
        if (this.module._wasm_send_touch_event) {
            this.module._wasm_send_touch_event(touch.identifier, pos.x, pos.y, 0);
        }
    }

    _handleTouchMove(event) {
        event.preventDefault();
        const touch = event.touches[0];
        const pos = this._getMousePosition(touch);
        if (this.module._wasm_send_touch_event) {
            this.module._wasm_send_touch_event(touch.identifier, pos.x, pos.y, 1);
        }
    }

    _handleTouchEnd(event) {
        event.preventDefault();
        const touch = event.changedTouches[0];
        const pos = this._getMousePosition(touch);
        if (this.module._wasm_send_touch_event) {
            this.module._wasm_send_touch_event(touch.identifier, pos.x, pos.y, 2);
        }
    }

    /**
     * Update custom cursor from QEMU cursor data.
     */
    _updateCustomCursor(width, height, hotX, hotY, dataPtr) {
        // Create cursor from QEMU data (RGBA format)
        try {
            const canvas = document.createElement('canvas');
            canvas.width = width;
            canvas.height = height;
            const ctx = canvas.getContext('2d');
            const imgData = ctx.createImageData(width, height);

            const HEAPU8 = this.module.HEAPU8;
            for (let i = 0; i < width * height * 4; i++) {
                imgData.data[i] = HEAPU8[dataPtr + i];
            }
            ctx.putImageData(imgData, 0, 0);

            this.canvas.style.cursor = `url(${canvas.toDataURL()}) ${hotX} ${hotY}, auto`;
        } catch (e) {
            // Fallback to default cursor
            this.canvas.style.cursor = this.options.cursorStyle;
        }
    }

    /**
     * Get performance statistics.
     */
    getStats() {
        const stats = { ...this.stats };

        // Add QEMU-side stats if available
        if (this.module._wasm_get_perf_stats) {
            const ptr = this.module._wasm_get_perf_stats();
            if (ptr) {
                const HEAP32 = this.module.HEAP32;
                const HEAPF64 = this.module.HEAPF64;
                stats.qemu = {
                    framesRendered: HEAP32[ptr >> 2] | (HEAP32[(ptr + 4) >> 2] << 32),
                    avgCopyTime: HEAPF64[(ptr + 24) >> 3]
                };
            }
        }

        return stats;
    }

    /**
     * Set render backend.
     */
    setRenderBackend(backend) {
        if (this.module._wasm_set_render_backend) {
            this.module._wasm_set_render_backend(backend);
        }
        this.options.renderBackend = backend;
        this._initRenderingContext();
    }

    /**
     * Enable/disable low power mode.
     */
    setLowPowerMode(enabled) {
        this.options.lowPowerMode = enabled;
        if (this.module._wasm_ios_low_power_mode) {
            this.module._wasm_ios_low_power_mode(enabled ? 1 : 0);
        }
    }

    /**
     * Set target FPS.
     */
    setTargetFps(fps) {
        this.options.targetFps = fps;
        this.frameInterval = 1000 / fps;
        if (this.module._wasm_ios_set_target_fps) {
            this.module._wasm_ios_set_target_fps(fps);
        }
    }
}

// For non-module usage
if (typeof window !== 'undefined') {
    window.WasmDisplay = WasmDisplay;
    window.RenderBackend = RenderBackend;
}
