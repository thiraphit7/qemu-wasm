/**
 * QEMU WASM Display - JavaScript Integration
 *
 * This module provides a JavaScript interface for rendering QEMU's
 * framebuffer output to an HTML5 Canvas element.
 *
 * Compatible with Safari, Chrome, Firefox, and other modern browsers.
 *
 * Usage:
 *   import { WasmDisplay } from './wasm-display.js';
 *   const display = new WasmDisplay(canvas, Module);
 *   display.start();
 */

export class WasmDisplay {
    /**
     * Create a new WASM display instance.
     *
     * @param {HTMLCanvasElement} canvas - The canvas element to render to
     * @param {Object} module - The Emscripten Module object
     * @param {Object} options - Optional configuration
     */
    constructor(canvas, module, options = {}) {
        this.canvas = canvas;
        this.module = module;
        this.ctx = canvas.getContext('2d', {
            alpha: false,
            desynchronized: true  // Better performance on some browsers
        });

        // Options with defaults
        this.options = {
            targetFps: options.targetFps || 60,
            enableInput: options.enableInput !== false,
            scaleToFit: options.scaleToFit !== false,
            cursorStyle: options.cursorStyle || 'default',
            ...options
        };

        // State
        this.running = false;
        this.lastFrameCount = 0;
        this.frameRequest = null;
        this.imageData = null;
        this.width = 0;
        this.height = 0;

        // Performance metrics
        this.stats = {
            fps: 0,
            frameTime: 0,
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

        // Setup global callbacks for QEMU notifications
        this._setupCallbacks();
    }

    /**
     * Setup global callback functions that QEMU calls.
     */
    _setupCallbacks() {
        const self = this;

        window.onWasmDisplayReady = function() {
            console.log('WASM Display: Ready');
            if (self.options.onReady) {
                self.options.onReady();
            }
        };

        window.onWasmFramebufferUpdate = function() {
            // Framebuffer was updated - next render will pick it up
        };

        window.onWasmFramebufferResize = function(width, height) {
            console.log(`WASM Display: Resize to ${width}x${height}`);
            self._handleResize(width, height);
        };

        window.onWasmMouseUpdate = function(x, y, visible) {
            // Update cursor position if needed
        };

        window.onWasmCursorDefine = function(width, height, hotX, hotY) {
            // Custom cursor definition (could create CSS cursor)
        };
    }

    /**
     * Start the display rendering loop.
     */
    start() {
        if (this.running) return;

        this.running = true;

        // Setup input handlers
        if (this.options.enableInput) {
            this._setupInputHandlers();
        }

        // Start render loop
        this._render();

        console.log('WASM Display: Started');
    }

    /**
     * Stop the display rendering loop.
     */
    stop() {
        this.running = false;

        if (this.frameRequest) {
            cancelAnimationFrame(this.frameRequest);
            this.frameRequest = null;
        }

        this._removeInputHandlers();

        console.log('WASM Display: Stopped');
    }

    /**
     * Main render loop.
     */
    _render() {
        if (!this.running) return;

        const now = performance.now();

        try {
            // Get framebuffer info from QEMU
            const fbInfo = this._getFramebufferInfo();

            if (fbInfo && fbInfo.dirty) {
                this._updateCanvas(fbInfo);
                this._ackFramebuffer();

                // Update stats
                this.stats.frameCount++;
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

        // Schedule next frame
        this.frameRequest = requestAnimationFrame(this._render);
    }

    /**
     * Get framebuffer information from QEMU.
     */
    _getFramebufferInfo() {
        if (!this.module._wasm_get_framebuffer_info) {
            return null;
        }

        const infoPtr = this.module._wasm_get_framebuffer_info();
        if (!infoPtr) return null;

        // Read WasmFramebufferInfo structure
        // struct WasmFramebufferInfo {
        //     uint8_t *data;      // offset 0
        //     int32_t width;      // offset 4
        //     int32_t height;     // offset 8
        //     int32_t stride;     // offset 12
        //     int32_t bpp;        // offset 16
        //     uint32_t format;    // offset 20
        //     bool dirty;         // offset 24
        //     uint64_t frame_count; // offset 28 (aligned to 8)
        // }

        const HEAP32 = this.module.HEAP32;
        const HEAPU8 = this.module.HEAPU8;

        const dataPtr = HEAP32[infoPtr >> 2];
        const width = HEAP32[(infoPtr + 4) >> 2];
        const height = HEAP32[(infoPtr + 8) >> 2];
        const stride = HEAP32[(infoPtr + 12) >> 2];
        const bpp = HEAP32[(infoPtr + 16) >> 2];
        const dirty = HEAPU8[infoPtr + 24] !== 0;

        if (width <= 0 || height <= 0 || !dataPtr) {
            return null;
        }

        return {
            dataPtr,
            width,
            height,
            stride,
            bpp,
            dirty
        };
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
     * Update the canvas with new framebuffer data.
     */
    _updateCanvas(fbInfo) {
        const { dataPtr, width, height, stride } = fbInfo;

        // Resize canvas if needed
        if (this.width !== width || this.height !== height) {
            this._handleResize(width, height);
        }

        // Get framebuffer data
        const HEAPU8 = this.module.HEAPU8;
        const size = height * stride;

        // Create ImageData if needed
        if (!this.imageData || this.imageData.width !== width || this.imageData.height !== height) {
            this.imageData = this.ctx.createImageData(width, height);
        }

        // Copy pixel data (RGBA format from QEMU)
        const srcData = HEAPU8.subarray(dataPtr, dataPtr + size);
        const dstData = this.imageData.data;

        // Direct copy since QEMU already converts to RGBA
        for (let y = 0; y < height; y++) {
            const srcOffset = y * stride;
            const dstOffset = y * width * 4;
            for (let x = 0; x < width * 4; x++) {
                dstData[dstOffset + x] = srcData[srcOffset + x];
            }
        }

        // Draw to canvas
        this.ctx.putImageData(this.imageData, 0, 0);
    }

    /**
     * Handle resolution change.
     */
    _handleResize(width, height) {
        this.width = width;
        this.height = height;

        if (this.options.scaleToFit) {
            // Scale canvas to fit container while maintaining aspect ratio
            const container = this.canvas.parentElement;
            if (container) {
                const containerWidth = container.clientWidth;
                const containerHeight = container.clientHeight;
                const scale = Math.min(containerWidth / width, containerHeight / height);

                this.canvas.style.width = `${width * scale}px`;
                this.canvas.style.height = `${height * scale}px`;
            }
        }

        // Set actual canvas resolution
        this.canvas.width = width;
        this.canvas.height = height;

        // Clear ImageData cache
        this.imageData = null;

        if (this.options.onResize) {
            this.options.onResize(width, height);
        }
    }

    /**
     * Setup input event handlers.
     */
    _setupInputHandlers() {
        this.canvas.tabIndex = 0;  // Make canvas focusable
        this.canvas.style.cursor = this.options.cursorStyle;

        this.canvas.addEventListener('keydown', this._handleKeyDown);
        this.canvas.addEventListener('keyup', this._handleKeyUp);
        this.canvas.addEventListener('mousemove', this._handleMouseMove);
        this.canvas.addEventListener('mousedown', this._handleMouseDown);
        this.canvas.addEventListener('mouseup', this._handleMouseUp);
        this.canvas.addEventListener('wheel', this._handleWheel);
        this.canvas.addEventListener('contextmenu', this._handleContextMenu);

        // Focus canvas on click
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
    }

    /**
     * Convert browser keycode to QEMU QKeyCode.
     */
    _keyToQKeyCode(event) {
        // This is a simplified mapping - full implementation would need complete keymap
        const keyMap = {
            'Escape': 1, 'F1': 67, 'F2': 68, 'F3': 69, 'F4': 70,
            'F5': 71, 'F6': 72, 'F7': 73, 'F8': 74, 'F9': 75, 'F10': 76,
            'Backquote': 41, 'Digit1': 2, 'Digit2': 3, 'Digit3': 4,
            'Digit4': 5, 'Digit5': 6, 'Digit6': 7, 'Digit7': 8,
            'Digit8': 9, 'Digit9': 10, 'Digit0': 11, 'Minus': 12, 'Equal': 13,
            'Backspace': 14, 'Tab': 15, 'KeyQ': 16, 'KeyW': 17, 'KeyE': 18,
            'KeyR': 19, 'KeyT': 20, 'KeyU': 22, 'KeyI': 23, 'KeyO': 24,
            'KeyP': 25, 'BracketLeft': 26, 'BracketRight': 27, 'Enter': 28,
            'ControlLeft': 29, 'KeyA': 30, 'KeyS': 31, 'KeyD': 32, 'KeyF': 33,
            'KeyG': 34, 'KeyH': 35, 'KeyJ': 36, 'KeyK': 37, 'KeyL': 38,
            'Semicolon': 39, 'Quote': 40, 'ShiftLeft': 42, 'Backslash': 43,
            'KeyZ': 44, 'KeyX': 45, 'KeyC': 46, 'KeyV': 47, 'KeyB': 48,
            'KeyN': 49, 'KeyM': 50, 'Comma': 51, 'Period': 52, 'Slash': 53,
            'ShiftRight': 54, 'AltLeft': 56, 'Space': 57, 'CapsLock': 58,
            'ArrowUp': 103, 'ArrowLeft': 105, 'ArrowRight': 106, 'ArrowDown': 108,
            'Delete': 111, 'Home': 102, 'End': 107, 'PageUp': 104, 'PageDown': 109,
            'Insert': 110
        };

        return keyMap[event.code] || 0;
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
            this.module._wasm_send_mouse_wheel(dx, -dy);  // Invert Y for natural scrolling
        }
    }

    _handleContextMenu(event) {
        event.preventDefault();
    }

    /**
     * Get current performance statistics.
     */
    getStats() {
        return { ...this.stats };
    }
}

// For non-module usage
if (typeof window !== 'undefined') {
    window.WasmDisplay = WasmDisplay;
}
