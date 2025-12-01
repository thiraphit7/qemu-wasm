/**
 * QEMU WASM iOS Safari Optimizations
 *
 * Provides iOS Safari-specific optimizations for WebAssembly execution.
 * Addresses memory pressure, audio autoplay, and gesture requirements.
 *
 * Key optimizations:
 * - Memory pressure handling
 * - Audio context resume on gesture
 * - Viewport meta tag management
 * - Touch event optimization
 * - WebAssembly streaming compilation fallback
 *
 * Usage:
 *   import { IOSSafariOptimizer } from './wasm-ios-safari.js';
 *   const optimizer = new IOSSafariOptimizer();
 *   optimizer.init();
 */

export class IOSSafariOptimizer {
    constructor(options = {}) {
        this.options = {
            enableMemoryManagement: options.enableMemoryManagement !== false,
            enableAudioGestureHandler: options.enableAudioGestureHandler !== false,
            enableTouchOptimization: options.enableTouchOptimization !== false,
            enableViewportFix: options.enableViewportFix !== false,
            memoryWarningThreshold: options.memoryWarningThreshold || 0.85,
            targetFPS: options.targetFPS || 60,
            ...options
        };

        this.isIOS = this._detectIOS();
        this.isSafari = this._detectSafari();
        this.isIPad = this._detectIPad();

        this.initialized = false;
        this.audioUnlocked = false;
        this.memoryWarningActive = false;
        this.lastGCTime = 0;

        // Performance tracking
        this.stats = {
            memoryWarnings: 0,
            audioUnlockAttempts: 0,
            gestureEvents: 0
        };
    }

    /**
     * Detect if running on iOS.
     */
    _detectIOS() {
        return /iPhone|iPad|iPod/.test(navigator.userAgent) ||
               (navigator.platform === 'MacIntel' && navigator.maxTouchPoints > 1);
    }

    /**
     * Detect if running Safari.
     */
    _detectSafari() {
        return /^((?!chrome|android).)*safari/i.test(navigator.userAgent);
    }

    /**
     * Detect iPad specifically.
     */
    _detectIPad() {
        return /iPad/.test(navigator.userAgent) ||
               (navigator.platform === 'MacIntel' && navigator.maxTouchPoints > 1);
    }

    /**
     * Initialize all optimizations.
     */
    init() {
        if (this.initialized) return;

        if (!this.isIOS && !this.isSafari) {
            console.log('WASM iOS Safari: Not iOS/Safari, skipping optimizations');
            this.initialized = true;
            return;
        }

        console.log('WASM iOS Safari: Initializing optimizations');
        console.log(`  - iOS: ${this.isIOS}, Safari: ${this.isSafari}, iPad: ${this.isIPad}`);

        if (this.options.enableMemoryManagement) {
            this._initMemoryManagement();
        }

        if (this.options.enableAudioGestureHandler) {
            this._initAudioGestureHandler();
        }

        if (this.options.enableTouchOptimization) {
            this._initTouchOptimization();
        }

        if (this.options.enableViewportFix) {
            this._initViewportFix();
        }

        this._initWasmOptimizations();
        this._initVisibilityHandler();

        this.initialized = true;
        console.log('WASM iOS Safari: Optimizations initialized');
    }

    /**
     * Initialize memory pressure management.
     */
    _initMemoryManagement() {
        // iOS Safari doesn't expose memory API, use indirect monitoring
        const self = this;

        // Monitor for low memory situations
        if ('memory' in performance) {
            setInterval(() => {
                const mem = performance.memory;
                const usageRatio = mem.usedJSHeapSize / mem.jsHeapSizeLimit;

                if (usageRatio > this.options.memoryWarningThreshold) {
                    self._handleMemoryWarning();
                }
            }, 5000);
        }

        // Handle Safari's memory pressure events (if available)
        if ('onmemorywarning' in window) {
            window.addEventListener('memorywarning', () => {
                self._handleMemoryWarning();
            });
        }

        // Fallback: monitor for signs of memory pressure
        let lastFrameTime = performance.now();
        let slowFrameCount = 0;

        const frameMonitor = () => {
            const now = performance.now();
            const delta = now - lastFrameTime;
            lastFrameTime = now;

            // Detect frame drops as potential memory pressure
            if (delta > 50) { // More than ~20fps
                slowFrameCount++;
                if (slowFrameCount > 10) {
                    self._handleMemoryWarning();
                    slowFrameCount = 0;
                }
            } else {
                slowFrameCount = Math.max(0, slowFrameCount - 1);
            }

            requestAnimationFrame(frameMonitor);
        };
        requestAnimationFrame(frameMonitor);
    }

    /**
     * Handle memory warning.
     */
    _handleMemoryWarning() {
        if (this.memoryWarningActive) return;

        const now = performance.now();
        if (now - this.lastGCTime < 10000) return; // Rate limit

        this.memoryWarningActive = true;
        this.stats.memoryWarnings++;
        this.lastGCTime = now;

        console.warn('WASM iOS Safari: Memory pressure detected');

        // Notify WASM module to reduce memory usage
        if (typeof Module !== 'undefined' && Module._wasm_handle_memory_pressure) {
            Module._wasm_handle_memory_pressure();
        }

        // Trigger garbage collection if possible
        if (window.gc) {
            window.gc();
        }

        // Notify application
        if (window.onWasmMemoryWarning) {
            window.onWasmMemoryWarning();
        }

        setTimeout(() => {
            this.memoryWarningActive = false;
        }, 1000);
    }

    /**
     * Initialize audio gesture handler for iOS autoplay restrictions.
     */
    _initAudioGestureHandler() {
        const self = this;

        const unlockAudio = (event) => {
            if (self.audioUnlocked) return;

            self.stats.audioUnlockAttempts++;

            // Resume any Web Audio contexts
            if (window._wasmAudio && window._wasmAudio.context) {
                const ctx = window._wasmAudio.context;
                if (ctx.state === 'suspended') {
                    ctx.resume().then(() => {
                        console.log('WASM iOS Safari: Audio context resumed');
                        self.audioUnlocked = true;
                    }).catch(e => {
                        console.warn('WASM iOS Safari: Audio resume failed', e);
                    });
                }
            }

            // Call WASM audio resume
            if (typeof Module !== 'undefined' && Module._wasm_audio_resume) {
                Module._wasm_audio_resume();
            }

            // Notify application
            if (window.onWasmAudioUnlocked) {
                window.onWasmAudioUnlocked();
            }
        };

        // Listen for various user gestures
        const gestureEvents = ['touchstart', 'touchend', 'click', 'keydown'];
        gestureEvents.forEach(eventType => {
            document.addEventListener(eventType, unlockAudio, { once: false, passive: true });
        });

        // Create silent audio element to help unlock audio
        const silentAudio = document.createElement('audio');
        silentAudio.src = 'data:audio/wav;base64,UklGRigAAABXQVZFZm10IBIAAAABAAEARKwAAIhYAQACABAAAABkYXRhAgAAAAEA';
        silentAudio.setAttribute('playsinline', '');
        silentAudio.volume = 0.001;

        document.addEventListener('touchstart', () => {
            silentAudio.play().catch(() => {});
        }, { once: true });
    }

    /**
     * Initialize touch event optimizations.
     */
    _initTouchOptimization() {
        // Disable iOS Safari's delayed click
        document.addEventListener('touchstart', () => {}, { passive: true });

        // Prevent double-tap zoom on canvas elements
        document.querySelectorAll('canvas').forEach(canvas => {
            canvas.style.touchAction = 'manipulation';
        });

        // Optimize scroll events
        document.addEventListener('touchmove', (e) => {
            if (e.target.tagName === 'CANVAS') {
                e.preventDefault();
            }
        }, { passive: false });

        // Prevent pull-to-refresh on iOS
        document.body.style.overscrollBehavior = 'none';

        // Prevent context menu on long press
        document.addEventListener('contextmenu', (e) => {
            if (e.target.tagName === 'CANVAS') {
                e.preventDefault();
            }
        });
    }

    /**
     * Initialize viewport fixes for iOS Safari.
     */
    _initViewportFix() {
        // Check for existing viewport meta
        let viewportMeta = document.querySelector('meta[name="viewport"]');

        if (!viewportMeta) {
            viewportMeta = document.createElement('meta');
            viewportMeta.name = 'viewport';
            document.head.appendChild(viewportMeta);
        }

        // Set optimal viewport for iOS
        viewportMeta.content = [
            'width=device-width',
            'initial-scale=1.0',
            'maximum-scale=1.0',
            'user-scalable=no',
            'viewport-fit=cover'
        ].join(', ');

        // Handle iOS Safari's dynamic toolbar height
        const setVH = () => {
            const vh = window.innerHeight * 0.01;
            document.documentElement.style.setProperty('--vh', `${vh}px`);
        };

        setVH();
        window.addEventListener('resize', setVH);
        window.addEventListener('orientationchange', () => {
            setTimeout(setVH, 100);
        });

        // Prevent iOS Safari bounce scrolling
        document.body.style.position = 'fixed';
        document.body.style.width = '100%';
        document.body.style.height = '100%';
        document.body.style.overflow = 'hidden';
    }

    /**
     * Initialize WASM-specific optimizations.
     */
    _initWasmOptimizations() {
        // Optimize WebAssembly instantiation for iOS
        if (typeof WebAssembly !== 'undefined') {
            // Check if streaming compilation is supported
            const supportsStreaming = typeof WebAssembly.instantiateStreaming === 'function';

            if (!supportsStreaming || this.isIOS) {
                // iOS Safari sometimes has issues with streaming compilation
                // Provide fallback instantiation
                window._wasmInstantiateWithFallback = async (response, imports) => {
                    try {
                        // Try streaming first
                        if (supportsStreaming) {
                            return await WebAssembly.instantiateStreaming(response, imports);
                        }
                    } catch (e) {
                        console.warn('WASM iOS Safari: Streaming compilation failed, using fallback');
                    }

                    // Fallback to array buffer instantiation
                    const bytes = await response.arrayBuffer();
                    return await WebAssembly.instantiate(bytes, imports);
                };
            }
        }

        // Set optimal memory growth for iOS
        if (typeof Module !== 'undefined') {
            // Limit initial memory on iOS to reduce pressure
            if (!Module.INITIAL_MEMORY && this.isIOS) {
                Module.INITIAL_MEMORY = 256 * 1024 * 1024; // 256MB
            }

            // Enable memory growth but with smaller increments
            if (!Module.MAXIMUM_MEMORY && this.isIOS) {
                Module.MAXIMUM_MEMORY = 1024 * 1024 * 1024; // 1GB max
            }
        }
    }

    /**
     * Initialize visibility change handler.
     */
    _initVisibilityHandler() {
        document.addEventListener('visibilitychange', () => {
            if (document.hidden) {
                this._handleAppBackground();
            } else {
                this._handleAppForeground();
            }
        });

        // iOS-specific events
        window.addEventListener('pagehide', () => {
            this._handleAppBackground();
        });

        window.addEventListener('pageshow', (event) => {
            if (event.persisted) {
                this._handleAppForeground();
            }
        });
    }

    /**
     * Handle app going to background.
     */
    _handleAppBackground() {
        console.log('WASM iOS Safari: App backgrounded');

        // Suspend audio to save battery
        if (window._wasmAudio && window._wasmAudio.context) {
            window._wasmAudio.context.suspend();
        }

        // Notify WASM module
        if (typeof Module !== 'undefined' && Module._wasm_handle_background) {
            Module._wasm_handle_background();
        }

        if (window.onWasmBackground) {
            window.onWasmBackground();
        }
    }

    /**
     * Handle app returning to foreground.
     */
    _handleAppForeground() {
        console.log('WASM iOS Safari: App foregrounded');

        // Resume audio if it was playing
        if (this.audioUnlocked && window._wasmAudio && window._wasmAudio.context) {
            window._wasmAudio.context.resume();
        }

        // Notify WASM module
        if (typeof Module !== 'undefined' && Module._wasm_handle_foreground) {
            Module._wasm_handle_foreground();
        }

        if (window.onWasmForeground) {
            window.onWasmForeground();
        }
    }

    /**
     * Get device info.
     */
    getDeviceInfo() {
        return {
            isIOS: this.isIOS,
            isSafari: this.isSafari,
            isIPad: this.isIPad,
            userAgent: navigator.userAgent,
            platform: navigator.platform,
            maxTouchPoints: navigator.maxTouchPoints,
            devicePixelRatio: window.devicePixelRatio,
            screenWidth: window.screen.width,
            screenHeight: window.screen.height,
            availableWidth: window.screen.availWidth,
            availableHeight: window.screen.availHeight
        };
    }

    /**
     * Get optimization statistics.
     */
    getStats() {
        return { ...this.stats };
    }

    /**
     * Check if audio is unlocked.
     */
    isAudioUnlocked() {
        return this.audioUnlocked;
    }

    /**
     * Request fullscreen (with iOS Safari workaround).
     */
    requestFullscreen(element) {
        const el = element || document.documentElement;

        if (el.requestFullscreen) {
            return el.requestFullscreen();
        } else if (el.webkitRequestFullscreen) {
            return el.webkitRequestFullscreen();
        } else if (el.webkitEnterFullscreen) {
            // iOS Safari video fullscreen
            return el.webkitEnterFullscreen();
        }

        // iOS Safari doesn't support true fullscreen
        // Apply pseudo-fullscreen styles
        if (this.isIOS) {
            el.style.position = 'fixed';
            el.style.top = '0';
            el.style.left = '0';
            el.style.width = '100vw';
            el.style.height = '100vh';
            el.style.zIndex = '9999';
            return Promise.resolve();
        }

        return Promise.reject(new Error('Fullscreen not supported'));
    }

    /**
     * Exit fullscreen.
     */
    exitFullscreen() {
        if (document.exitFullscreen) {
            return document.exitFullscreen();
        } else if (document.webkitExitFullscreen) {
            return document.webkitExitFullscreen();
        }
        return Promise.resolve();
    }
}

// Helper: Check WebGL support level
export function checkWebGLSupport() {
    const canvas = document.createElement('canvas');
    let support = {
        webgl2: false,
        webgl: false,
        renderer: null,
        vendor: null
    };

    // Try WebGL 2
    let gl = canvas.getContext('webgl2');
    if (gl) {
        support.webgl2 = true;
        const dbg = gl.getExtension('WEBGL_debug_renderer_info');
        if (dbg) {
            support.renderer = gl.getParameter(dbg.UNMASKED_RENDERER_WEBGL);
            support.vendor = gl.getParameter(dbg.UNMASKED_VENDOR_WEBGL);
        }
    }

    // Try WebGL 1
    if (!gl) {
        gl = canvas.getContext('webgl') || canvas.getContext('experimental-webgl');
        if (gl) {
            support.webgl = true;
            const dbg = gl.getExtension('WEBGL_debug_renderer_info');
            if (dbg) {
                support.renderer = gl.getParameter(dbg.UNMASKED_RENDERER_WEBGL);
                support.vendor = gl.getParameter(dbg.UNMASKED_VENDOR_WEBGL);
            }
        }
    }

    return support;
}

// Helper: Check WebGPU support
export async function checkWebGPUSupport() {
    let support = {
        available: false,
        adapter: null,
        features: [],
        limits: null
    };

    if (!navigator.gpu) {
        return support;
    }

    try {
        const adapter = await navigator.gpu.requestAdapter();
        if (adapter) {
            support.available = true;
            support.adapter = await adapter.requestAdapterInfo();
            support.features = [...adapter.features];
            support.limits = { ...adapter.limits };
        }
    } catch (e) {
        console.warn('WebGPU check failed:', e);
    }

    return support;
}

// Export for non-module usage
if (typeof window !== 'undefined') {
    window.IOSSafariOptimizer = IOSSafariOptimizer;
    window.checkWebGLSupport = checkWebGLSupport;
    window.checkWebGPUSupport = checkWebGPUSupport;
}
