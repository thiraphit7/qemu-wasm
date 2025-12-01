/**
 * QEMU WASM WebGPU/WebGL Display - JavaScript Integration
 *
 * Provides GPU-accelerated rendering for VirtIO-GPU devices.
 * Supports WebGPU with experimental WebGL fallback (QEMU 10.1+).
 *
 * Features:
 * - Native WebGPU rendering when available
 * - WebGL 2.0 fallback for broad compatibility
 * - WebGPU Compatibility Mode (WebGL backend) experimental support
 * - iOS Safari optimizations
 *
 * Usage:
 *   import { WasmWebGPU } from './wasm-webgpu.js';
 *   const gpu = new WasmWebGPU(canvas, Module, { preferWebGPU: true });
 *   await gpu.init();
 */

// GPU Backend Types (matches C enum)
const GPU_BACKEND = {
    NONE: 0,
    CANVAS2D: 1,
    WEBGL: 2,
    WEBGL2: 3,
    WEBGPU: 4,
    WEBGPU_COMPAT: 5  // WebGPU over WebGL (experimental)
};

export class WasmWebGPU {
    /**
     * Create a new WebGPU/WebGL display instance.
     *
     * @param {HTMLCanvasElement} canvas - Target canvas element
     * @param {Object} module - Emscripten Module object
     * @param {Object} options - Configuration options
     */
    constructor(canvas, module, options = {}) {
        this.canvas = canvas;
        this.module = module;

        this.options = {
            preferWebGPU: options.preferWebGPU !== false,
            enableWebGPUCompat: options.enableWebGPUCompat || false,
            antialias: options.antialias || false,
            powerPreference: options.powerPreference || 'high-performance',
            ...options
        };

        // State
        this.backend = GPU_BACKEND.NONE;
        this.initialized = false;
        this.device = null;
        this.context = null;
        this.gl = null;

        // WebGPU resources
        this.gpuTextures = new Map();
        this.gpuPipeline = null;
        this.gpuBindGroup = null;
        this.gpuSampler = null;

        // WebGL resources
        this.glProgram = null;
        this.glTextures = new Map();
        this.glFramebuffer = null;

        // Performance
        this.stats = {
            framesRendered: 0,
            lastFrameTime: 0,
            avgFrameTime: 0
        };

        // Bind methods
        this._renderFrame = this._renderFrame.bind(this);

        // Setup QEMU callbacks
        this._setupCallbacks();
    }

    /**
     * Initialize GPU backend.
     */
    async init() {
        if (this.initialized) {
            return this.backend;
        }

        // Try WebGPU first
        if (this.options.preferWebGPU && navigator.gpu) {
            try {
                await this._initWebGPU();
                this.backend = GPU_BACKEND.WEBGPU;
                this.initialized = true;
                console.log('WASM WebGPU: Using native WebGPU');
                return this.backend;
            } catch (e) {
                console.warn('WASM WebGPU: Native WebGPU init failed:', e);
            }
        }

        // Try WebGPU compatibility mode (experimental)
        if (this.options.enableWebGPUCompat && navigator.gpu) {
            try {
                await this._initWebGPUCompat();
                this.backend = GPU_BACKEND.WEBGPU_COMPAT;
                this.initialized = true;
                console.log('WASM WebGPU: Using WebGPU compatibility mode');
                return this.backend;
            } catch (e) {
                console.warn('WASM WebGPU: Compatibility mode failed:', e);
            }
        }

        // Fall back to WebGL2
        try {
            this._initWebGL2();
            this.backend = GPU_BACKEND.WEBGL2;
            this.initialized = true;
            console.log('WASM WebGPU: Using WebGL 2.0 fallback');
            return this.backend;
        } catch (e) {
            console.warn('WASM WebGPU: WebGL 2.0 init failed:', e);
        }

        // Last resort: WebGL 1.0
        try {
            this._initWebGL();
            this.backend = GPU_BACKEND.WEBGL;
            this.initialized = true;
            console.log('WASM WebGPU: Using WebGL 1.0 fallback');
            return this.backend;
        } catch (e) {
            console.error('WASM WebGPU: All GPU backends failed');
            this.backend = GPU_BACKEND.NONE;
            return this.backend;
        }
    }

    /**
     * Initialize native WebGPU.
     */
    async _initWebGPU() {
        const adapter = await navigator.gpu.requestAdapter({
            powerPreference: this.options.powerPreference
        });

        if (!adapter) {
            throw new Error('No WebGPU adapter found');
        }

        this.device = await adapter.requestDevice();
        this.context = this.canvas.getContext('webgpu');

        const format = navigator.gpu.getPreferredCanvasFormat();
        this.context.configure({
            device: this.device,
            format: format,
            alphaMode: 'opaque'
        });

        // Create sampler
        this.gpuSampler = this.device.createSampler({
            magFilter: 'linear',
            minFilter: 'linear'
        });

        // Create render pipeline for texture display
        await this._createWebGPUPipeline(format);
    }

    /**
     * Initialize WebGPU compatibility mode (WebGL backend).
     * Experimental feature in Chrome.
     */
    async _initWebGPUCompat() {
        const adapter = await navigator.gpu.requestAdapter({
            powerPreference: this.options.powerPreference,
            // Request compatibility mode (WebGL fallback)
            compatibilityMode: true
        });

        if (!adapter) {
            throw new Error('No WebGPU compat adapter found');
        }

        // Check if this is actually a compat adapter
        const info = await adapter.requestAdapterInfo();
        console.log('WASM WebGPU Compat adapter:', info);

        this.device = await adapter.requestDevice();
        this.context = this.canvas.getContext('webgpu');

        const format = navigator.gpu.getPreferredCanvasFormat();
        this.context.configure({
            device: this.device,
            format: format,
            alphaMode: 'opaque'
        });

        this.gpuSampler = this.device.createSampler({
            magFilter: 'linear',
            minFilter: 'linear'
        });

        await this._createWebGPUPipeline(format);
    }

    /**
     * Create WebGPU render pipeline.
     */
    async _createWebGPUPipeline(format) {
        const shaderCode = `
            struct VertexOutput {
                @builtin(position) position: vec4f,
                @location(0) texCoord: vec2f,
            }

            @vertex
            fn vertexMain(@builtin(vertex_index) vertexIndex: u32) -> VertexOutput {
                var pos = array<vec2f, 4>(
                    vec2f(-1.0, -1.0),
                    vec2f( 1.0, -1.0),
                    vec2f(-1.0,  1.0),
                    vec2f( 1.0,  1.0)
                );
                var texCoord = array<vec2f, 4>(
                    vec2f(0.0, 1.0),
                    vec2f(1.0, 1.0),
                    vec2f(0.0, 0.0),
                    vec2f(1.0, 0.0)
                );

                var output: VertexOutput;
                output.position = vec4f(pos[vertexIndex], 0.0, 1.0);
                output.texCoord = texCoord[vertexIndex];
                return output;
            }

            @group(0) @binding(0) var texSampler: sampler;
            @group(0) @binding(1) var tex: texture_2d<f32>;

            @fragment
            fn fragmentMain(@location(0) texCoord: vec2f) -> @location(0) vec4f {
                return textureSample(tex, texSampler, texCoord);
            }
        `;

        const shaderModule = this.device.createShaderModule({
            code: shaderCode
        });

        this.gpuPipeline = this.device.createRenderPipeline({
            layout: 'auto',
            vertex: {
                module: shaderModule,
                entryPoint: 'vertexMain'
            },
            fragment: {
                module: shaderModule,
                entryPoint: 'fragmentMain',
                targets: [{ format: format }]
            },
            primitive: {
                topology: 'triangle-strip',
                stripIndexFormat: undefined
            }
        });
    }

    /**
     * Initialize WebGL 2.0.
     */
    _initWebGL2() {
        this.gl = this.canvas.getContext('webgl2', {
            antialias: this.options.antialias,
            powerPreference: this.options.powerPreference,
            alpha: false,
            depth: false,
            stencil: false
        });

        if (!this.gl) {
            throw new Error('WebGL 2.0 not available');
        }

        this._initWebGLCommon();
    }

    /**
     * Initialize WebGL 1.0.
     */
    _initWebGL() {
        this.gl = this.canvas.getContext('webgl', {
            antialias: this.options.antialias,
            powerPreference: this.options.powerPreference,
            alpha: false,
            depth: false,
            stencil: false
        }) || this.canvas.getContext('experimental-webgl');

        if (!this.gl) {
            throw new Error('WebGL not available');
        }

        this._initWebGLCommon();
    }

    /**
     * Common WebGL initialization.
     */
    _initWebGLCommon() {
        const gl = this.gl;

        // Vertex shader
        const vsSource = `
            attribute vec2 aPosition;
            attribute vec2 aTexCoord;
            varying vec2 vTexCoord;
            void main() {
                gl_Position = vec4(aPosition, 0.0, 1.0);
                vTexCoord = aTexCoord;
            }
        `;

        // Fragment shader
        const fsSource = `
            precision mediump float;
            varying vec2 vTexCoord;
            uniform sampler2D uTexture;
            void main() {
                gl_FragColor = texture2D(uTexture, vTexCoord);
            }
        `;

        // Compile shaders
        const vs = this._compileShader(gl.VERTEX_SHADER, vsSource);
        const fs = this._compileShader(gl.FRAGMENT_SHADER, fsSource);

        // Link program
        this.glProgram = gl.createProgram();
        gl.attachShader(this.glProgram, vs);
        gl.attachShader(this.glProgram, fs);
        gl.linkProgram(this.glProgram);

        if (!gl.getProgramParameter(this.glProgram, gl.LINK_STATUS)) {
            throw new Error('WebGL program link failed');
        }

        gl.useProgram(this.glProgram);

        // Create vertex buffer
        const vertices = new Float32Array([
            -1, -1,  0, 1,
             1, -1,  1, 1,
            -1,  1,  0, 0,
             1,  1,  1, 0
        ]);

        const vbo = gl.createBuffer();
        gl.bindBuffer(gl.ARRAY_BUFFER, vbo);
        gl.bufferData(gl.ARRAY_BUFFER, vertices, gl.STATIC_DRAW);

        const aPosition = gl.getAttribLocation(this.glProgram, 'aPosition');
        const aTexCoord = gl.getAttribLocation(this.glProgram, 'aTexCoord');

        gl.enableVertexAttribArray(aPosition);
        gl.vertexAttribPointer(aPosition, 2, gl.FLOAT, false, 16, 0);
        gl.enableVertexAttribArray(aTexCoord);
        gl.vertexAttribPointer(aTexCoord, 2, gl.FLOAT, false, 16, 8);
    }

    /**
     * Compile WebGL shader.
     */
    _compileShader(type, source) {
        const gl = this.gl;
        const shader = gl.createShader(type);
        gl.shaderSource(shader, source);
        gl.compileShader(shader);

        if (!gl.getShaderParameter(shader, gl.COMPILE_STATUS)) {
            const info = gl.getShaderInfoLog(shader);
            gl.deleteShader(shader);
            throw new Error('Shader compile error: ' + info);
        }

        return shader;
    }

    /**
     * Create or update a GPU texture.
     */
    createTexture(resourceId, width, height, format) {
        if (this.backend === GPU_BACKEND.WEBGPU ||
            this.backend === GPU_BACKEND.WEBGPU_COMPAT) {
            return this._createWebGPUTexture(resourceId, width, height, format);
        } else if (this.backend >= GPU_BACKEND.WEBGL) {
            return this._createWebGLTexture(resourceId, width, height, format);
        }
        return false;
    }

    /**
     * Create WebGPU texture.
     */
    _createWebGPUTexture(resourceId, width, height, format) {
        if (!this.device) return false;

        // Destroy existing texture
        const existing = this.gpuTextures.get(resourceId);
        if (existing) {
            existing.texture.destroy();
        }

        const texture = this.device.createTexture({
            size: [width, height],
            format: 'rgba8unorm',
            usage: GPUTextureUsage.TEXTURE_BINDING |
                   GPUTextureUsage.COPY_DST |
                   GPUTextureUsage.RENDER_ATTACHMENT
        });

        this.gpuTextures.set(resourceId, {
            texture: texture,
            view: texture.createView(),
            width: width,
            height: height
        });

        return true;
    }

    /**
     * Create WebGL texture.
     */
    _createWebGLTexture(resourceId, width, height, format) {
        const gl = this.gl;
        if (!gl) return false;

        // Destroy existing
        const existing = this.glTextures.get(resourceId);
        if (existing) {
            gl.deleteTexture(existing.texture);
        }

        const texture = gl.createTexture();
        gl.bindTexture(gl.TEXTURE_2D, texture);
        gl.texImage2D(gl.TEXTURE_2D, 0, gl.RGBA, width, height, 0,
                      gl.RGBA, gl.UNSIGNED_BYTE, null);
        gl.texParameteri(gl.TEXTURE_2D, gl.TEXTURE_MIN_FILTER, gl.LINEAR);
        gl.texParameteri(gl.TEXTURE_2D, gl.TEXTURE_MAG_FILTER, gl.LINEAR);
        gl.texParameteri(gl.TEXTURE_2D, gl.TEXTURE_WRAP_S, gl.CLAMP_TO_EDGE);
        gl.texParameteri(gl.TEXTURE_2D, gl.TEXTURE_WRAP_T, gl.CLAMP_TO_EDGE);

        this.glTextures.set(resourceId, {
            texture: texture,
            width: width,
            height: height
        });

        return true;
    }

    /**
     * Upload texture data.
     */
    uploadTexture(resourceId, x, y, width, height, data) {
        if (this.backend === GPU_BACKEND.WEBGPU ||
            this.backend === GPU_BACKEND.WEBGPU_COMPAT) {
            return this._uploadWebGPUTexture(resourceId, x, y, width, height, data);
        } else if (this.backend >= GPU_BACKEND.WEBGL) {
            return this._uploadWebGLTexture(resourceId, x, y, width, height, data);
        }
        return false;
    }

    /**
     * Upload to WebGPU texture.
     */
    _uploadWebGPUTexture(resourceId, x, y, width, height, data) {
        const texInfo = this.gpuTextures.get(resourceId);
        if (!texInfo || !this.device) return false;

        this.device.queue.writeTexture(
            { texture: texInfo.texture, origin: [x, y] },
            data,
            { bytesPerRow: width * 4 },
            [width, height]
        );

        return true;
    }

    /**
     * Upload to WebGL texture.
     */
    _uploadWebGLTexture(resourceId, x, y, width, height, data) {
        const texInfo = this.glTextures.get(resourceId);
        const gl = this.gl;
        if (!texInfo || !gl) return false;

        gl.bindTexture(gl.TEXTURE_2D, texInfo.texture);
        gl.texSubImage2D(gl.TEXTURE_2D, 0, x, y, width, height,
                         gl.RGBA, gl.UNSIGNED_BYTE, data);

        return true;
    }

    /**
     * Render a texture to the canvas.
     */
    renderTexture(resourceId) {
        if (this.backend === GPU_BACKEND.WEBGPU ||
            this.backend === GPU_BACKEND.WEBGPU_COMPAT) {
            return this._renderWebGPUTexture(resourceId);
        } else if (this.backend >= GPU_BACKEND.WEBGL) {
            return this._renderWebGLTexture(resourceId);
        }
        return false;
    }

    /**
     * Render with WebGPU.
     */
    _renderWebGPUTexture(resourceId) {
        const texInfo = this.gpuTextures.get(resourceId);
        if (!texInfo || !this.device || !this.context) return false;

        // Resize canvas if needed
        if (this.canvas.width !== texInfo.width ||
            this.canvas.height !== texInfo.height) {
            this.canvas.width = texInfo.width;
            this.canvas.height = texInfo.height;
        }

        // Create bind group for this texture
        const bindGroup = this.device.createBindGroup({
            layout: this.gpuPipeline.getBindGroupLayout(0),
            entries: [
                { binding: 0, resource: this.gpuSampler },
                { binding: 1, resource: texInfo.view }
            ]
        });

        const commandEncoder = this.device.createCommandEncoder();
        const renderPass = commandEncoder.beginRenderPass({
            colorAttachments: [{
                view: this.context.getCurrentTexture().createView(),
                loadOp: 'clear',
                storeOp: 'store',
                clearValue: { r: 0, g: 0, b: 0, a: 1 }
            }]
        });

        renderPass.setPipeline(this.gpuPipeline);
        renderPass.setBindGroup(0, bindGroup);
        renderPass.draw(4);
        renderPass.end();

        this.device.queue.submit([commandEncoder.finish()]);
        this.stats.framesRendered++;

        return true;
    }

    /**
     * Render with WebGL.
     */
    _renderWebGLTexture(resourceId) {
        const texInfo = this.glTextures.get(resourceId);
        const gl = this.gl;
        if (!texInfo || !gl) return false;

        // Resize canvas if needed
        if (this.canvas.width !== texInfo.width ||
            this.canvas.height !== texInfo.height) {
            this.canvas.width = texInfo.width;
            this.canvas.height = texInfo.height;
        }

        gl.viewport(0, 0, texInfo.width, texInfo.height);
        gl.bindTexture(gl.TEXTURE_2D, texInfo.texture);
        gl.drawArrays(gl.TRIANGLE_STRIP, 0, 4);

        this.stats.framesRendered++;
        return true;
    }

    /**
     * Destroy a texture.
     */
    destroyTexture(resourceId) {
        if (this.backend === GPU_BACKEND.WEBGPU ||
            this.backend === GPU_BACKEND.WEBGPU_COMPAT) {
            const texInfo = this.gpuTextures.get(resourceId);
            if (texInfo) {
                texInfo.texture.destroy();
                this.gpuTextures.delete(resourceId);
            }
        } else if (this.backend >= GPU_BACKEND.WEBGL) {
            const texInfo = this.glTextures.get(resourceId);
            if (texInfo && this.gl) {
                this.gl.deleteTexture(texInfo.texture);
                this.glTextures.delete(resourceId);
            }
        }
    }

    /**
     * Setup QEMU callbacks.
     */
    _setupCallbacks() {
        const self = this;

        window.onWasmGpuInit = function(backend, features) {
            console.log(`WASM GPU Init: backend=${backend}, features=0x${features.toString(16)}`);
        };

        window.onWasmGpuResourceCreate = function(resourceId, width, height, format) {
            self.createTexture(resourceId, width, height, format);
        };

        window.onWasmGpuResourceDestroy = function(resourceId) {
            self.destroyTexture(resourceId);
        };

        window.onWasmGpuTextureUpload = function(resourceId, x, y, width, height, data) {
            self.uploadTexture(resourceId, x, y, width, height, data);
        };

        window.onWasmGpuFlush = function(scanoutId, resourceId) {
            self.renderTexture(resourceId);
        };

        window.onWasmGpuFrame = function(scanoutId) {
            // Frame notification
        };
    }

    /**
     * Get current backend type.
     */
    getBackend() {
        return this.backend;
    }

    /**
     * Get backend name string.
     */
    getBackendName() {
        switch (this.backend) {
            case GPU_BACKEND.WEBGPU: return 'WebGPU';
            case GPU_BACKEND.WEBGPU_COMPAT: return 'WebGPU (Compat)';
            case GPU_BACKEND.WEBGL2: return 'WebGL 2.0';
            case GPU_BACKEND.WEBGL: return 'WebGL 1.0';
            case GPU_BACKEND.CANVAS2D: return 'Canvas 2D';
            default: return 'None';
        }
    }

    /**
     * Get performance statistics.
     */
    getStats() {
        return { ...this.stats };
    }

    /**
     * Destroy and cleanup.
     */
    destroy() {
        // Cleanup all textures
        for (const resourceId of this.gpuTextures.keys()) {
            this.destroyTexture(resourceId);
        }
        for (const resourceId of this.glTextures.keys()) {
            this.destroyTexture(resourceId);
        }

        if (this.device) {
            this.device.destroy();
            this.device = null;
        }

        this.initialized = false;
        this.backend = GPU_BACKEND.NONE;
    }

    /**
     * Internal render frame callback.
     */
    _renderFrame() {
        // Called from animation frame if needed
    }
}

// Export for non-module usage
if (typeof window !== 'undefined') {
    window.WasmWebGPU = WasmWebGPU;
    window.GPU_BACKEND = GPU_BACKEND;
}
