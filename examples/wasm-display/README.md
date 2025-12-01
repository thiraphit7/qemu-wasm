# QEMU WASM Display Example

This example demonstrates how to use the WASM display backend to render
QEMU's graphical output in an HTML5 Canvas element.

## Overview

The WASM display backend exports QEMU's framebuffer to JavaScript, allowing
you to render virtual machine graphics directly in a web browser. This works
with any browser that supports WebAssembly, including Safari, Chrome, and Firefox.

## Files

- `wasm-display.js` - JavaScript class for integrating with the display backend
- `index.html` - Example HTML page with full display integration

## Building QEMU with WASM Display

1. Configure QEMU for Emscripten build:

```bash
emconfigure ./configure \
    --target-list=aarch64-softmmu \
    --cpu=wasm32 \
    --cross-prefix= \
    --static \
    --without-default-features \
    --enable-system \
    --with-coroutine=fiber \
    --enable-virtfs
```

2. Build with display support:

```bash
emmake make -j$(nproc)
```

3. Link with exported functions:

```bash
# Add to EXTRA_CFLAGS or emcc flags:
-sEXPORTED_FUNCTIONS="['_main','_wasm_get_framebuffer_info','_wasm_get_framebuffer_data','_wasm_framebuffer_ack','_wasm_framebuffer_is_dirty','_wasm_get_frame_count','_wasm_send_keyboard_event','_wasm_send_mouse_motion','_wasm_send_mouse_button','_wasm_send_mouse_wheel']"
-sEXPORTED_RUNTIME_METHODS="['ccall','cwrap','HEAP32','HEAPU8']"
```

## Usage

### Basic Setup

```html
<canvas id="qemu-display" width="800" height="600"></canvas>
<script type="module">
    import { WasmDisplay } from './wasm-display.js';
    import initEmscriptenModule from './out.js';

    const canvas = document.getElementById('qemu-display');

    // Initialize QEMU module
    const Module = {
        arguments: [
            '-M', 'virt',
            '-cpu', 'cortex-a72',
            '-m', '512M',
            '-device', 'virtio-gpu-pci',
            '-display', 'wasm'
        ]
    };

    const instance = await initEmscriptenModule(Module);

    // Create and start display
    const display = new WasmDisplay(canvas, Module, {
        targetFps: 60,
        enableInput: true,
        scaleToFit: true
    });

    display.start();
</script>
```

### WasmDisplay Options

| Option | Type | Default | Description |
|--------|------|---------|-------------|
| `targetFps` | number | 60 | Target frame rate |
| `enableInput` | boolean | true | Enable keyboard/mouse input |
| `scaleToFit` | boolean | true | Scale canvas to fit container |
| `cursorStyle` | string | 'default' | CSS cursor style |
| `onReady` | function | - | Callback when display is ready |
| `onResize` | function | - | Callback on resolution change |

### JavaScript Callbacks

The display backend calls these global functions when events occur:

```javascript
// Called when display is initialized
window.onWasmDisplayReady = function() {
    console.log('Display ready');
};

// Called when framebuffer is updated
window.onWasmFramebufferUpdate = function() {
    // Trigger canvas update
};

// Called when resolution changes
window.onWasmFramebufferResize = function(width, height) {
    console.log(`New resolution: ${width}x${height}`);
};

// Called when mouse cursor position updates
window.onWasmMouseUpdate = function(x, y, visible) {
    // Update cursor
};

// Called when guest defines custom cursor
window.onWasmCursorDefine = function(width, height, hotX, hotY) {
    // Create custom cursor
};
```

### Input Handling

The display automatically handles:
- Keyboard events (keydown/keyup)
- Mouse movement
- Mouse buttons (left, middle, right)
- Mouse wheel scrolling

To capture mouse for relative mode (like FPS games):

```javascript
canvas.requestPointerLock();
```

## Supported Display Devices

- `virtio-gpu-pci` - Recommended for best compatibility
- `virtio-gpu-device` - For platforms without PCI
- `ramfb` - Simple framebuffer for early boot
- Standard VGA - Works but less efficient

## Performance Tips

1. **Use SharedArrayBuffer** for better performance with threading:
   - Requires COOP/COEP headers
   - Set `Cross-Origin-Opener-Policy: same-origin`
   - Set `Cross-Origin-Embedder-Policy: require-corp`

2. **Reduce resolution** for slower devices:
   - Use `-device virtio-gpu-pci,max_outputs=1,xres=800,yres=600`

3. **Disable unnecessary features**:
   - Run QEMU with `-nographic` for terminal-only guests
   - Disable sound with `-nodefaults`

## Browser Compatibility

| Browser | Status | Notes |
|---------|--------|-------|
| Chrome 80+ | Full | Best performance with SharedArrayBuffer |
| Firefox 78+ | Full | May need COOP/COEP headers |
| Safari 14+ | Full | Works well on M-series Macs |
| Edge 80+ | Full | Same as Chrome |

## Troubleshooting

### Black screen
- Ensure QEMU is started with `-display wasm`
- Check that the guest has graphics drivers loaded
- Verify the display device is enabled

### Low FPS
- Reduce guest resolution
- Enable SharedArrayBuffer for threading
- Check browser console for errors

### Input not working
- Click on the canvas to focus it
- Check that `enableInput: true` is set
- Some key combinations may be captured by the browser

## API Reference

### WasmDisplay Class

```javascript
class WasmDisplay {
    constructor(canvas, module, options)
    start()                 // Start rendering loop
    stop()                  // Stop rendering loop
    getStats()              // Get performance statistics
}
```

### C Functions (for custom integration)

```c
// Get framebuffer info structure
WasmFramebufferInfo *wasm_get_framebuffer_info(void);

// Get raw pixel data pointer
uint8_t *wasm_get_framebuffer_data(void);

// Get current dimensions
bool wasm_get_framebuffer_size(int32_t *width, int32_t *height);

// Acknowledge framebuffer read (clear dirty flag)
void wasm_framebuffer_ack(void);

// Check if framebuffer was updated
bool wasm_framebuffer_is_dirty(void);

// Get frame counter
uint64_t wasm_get_frame_count(void);

// Send input events
void wasm_send_keyboard_event(int keycode, bool down);
void wasm_send_mouse_motion(int x, int y);
void wasm_send_mouse_button(int button, bool down);
void wasm_send_mouse_wheel(int dx, int dy);
```
