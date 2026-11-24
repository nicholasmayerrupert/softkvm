// Zero-copy D3D11 video presenter.
//
// Decoded D3D11VA surfaces are sampled directly by a pixel shader (NV12 -> RGB) and drawn
// into a flip-model swap chain per window: no GPU->CPU->GPU copies, no SDL renderer.
// Decoder and presenter share one ID3D11Device, guarded by FFmpeg's device lock.
#pragma once
#include <stdint.h>
#include <windows.h>
#include <libavcodec/avcodec.h>

typedef struct D3DPWindow D3DPWindow;

enum {
    D3DP_PRESENT_FLIP = 0,     // flip model, newest frame shown at the next vblank (no tearing)
    D3DP_PRESENT_TEARING = 1,  // flip model + ALLOW_TEARING: shown immediately (or VRR)
    D3DP_PRESENT_VSYNC = 2,    // flip model, Present(1): classic vsync
};

typedef struct {
    const uint8_t *px;  // RGBA, straight alpha; NULL = no cursor
    int w, h, version;
    int x, y, dw, dh;   // destination rect in window pixels
} D3DPCursor;

// Creates the shared device and an FFmpeg D3D11VA device context around it.
// Returns 0 on success; *hw_device receives a new reference.
int d3dp_init(AVBufferRef **hw_device, int present_mode);
int d3dp_active(void);

// AVCodecContext.get_format: D3D11 frames that shaders can read (falls back to software).
enum AVPixelFormat d3dp_get_format(AVCodecContext *c, const enum AVPixelFormat *fmts);

D3DPWindow *d3dp_window_create(HWND hwnd);
void d3dp_window_destroy(D3DPWindow *w);

// Draws `frame` (AV_PIX_FMT_D3D11, or NULL to clear) scaled to the window, plus the cursor.
int d3dp_render(D3DPWindow *w, const AVFrame *frame, const D3DPCursor *cursor);
// Presents what d3dp_render drew. Returns 0 on success.
/// 0 ok, -1 failed, -2 the GPU was removed/reset (every D3D object must be rebuilt).
int d3dp_present(D3DPWindow *w);

// 1 if the swap chain can take another frame without blocking (frame-latency waitable).
int d3dp_ready(D3DPWindow *w);

// Bench mode: writes the back buffer (after d3dp_render, before d3dp_present) as a BMP.
int d3dp_snapshot(D3DPWindow *w, const char *path);

// Bench mode: wait until the GPU finished everything submitted so far.
void d3dp_gpu_wait(void);
