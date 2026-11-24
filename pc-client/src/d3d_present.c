// Zero-copy D3D11 video presenter, see d3d_present.h.
#define COBJMACROS
#define INITGUID
#include "d3d_present.h"

#include <d3d11.h>
#include <d3d10.h>
#include <d3dcompiler.h>
#include <dxgi1_5.h>
#include <libavutil/hwcontext.h>
#include <libavutil/hwcontext_d3d11va.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define MAX_SRV 48
// Displayed decoder surfaces stay referenced for this many more frames, so the decoder
// doesn't pick a surface the GPU may still be reading (FFmpeg's pool is LIFO, and
// DecoderBeginFrame on a busy surface returns E_PENDING, which FFmpeg answers with 2 ms sleeps).
#define RETIRE 3

struct D3DPWindow {
    HWND hwnd;
    IDXGISwapChain2 *sc;
    HANDLE waitable;
    ID3D11RenderTargetView *rtv;
    UINT w, h;
    // SRVs for slices of the decoder's array texture (one pool per stream).
    ID3D11Texture2D *srv_tex;
    struct { intptr_t idx; ID3D11ShaderResourceView *y, *uv; } srv[MAX_SRV];
    int nsrv;
    ID3D11Texture2D *cursor_tex;
    ID3D11ShaderResourceView *cursor_srv;
    int cursor_version, cursor_w, cursor_h;
    AVFrame *retire[RETIRE];
    int retire_pos;
};

static ID3D11Device *g_dev;
static ID3D11DeviceContext *g_ctx;
static AVD3D11VADeviceContext *g_hw;  // for the shared lock
static IDXGIFactory2 *g_factory;
static int g_tearing_ok, g_mode, g_active;
static ID3D11VertexShader *g_vs;
static ID3D11PixelShader *g_ps_nv12, *g_ps_rgba;
static ID3D11SamplerState *g_sampler;
static ID3D11BlendState *g_blend;
static ID3D11Buffer *g_cb;

static const char g_hlsl[] =
    // dst: NDC left, top, right, bottom. uv: scale to the picture inside a padded decoder
    // surface (xy) and the clamp that keeps linear filtering off the padding (zw).
    "cbuffer CB : register(b0) { float4 dst; float4 uv; };\n"
    "struct V { float4 pos : SV_Position; float2 uv : TEXCOORD0; };\n"
    "V vs(uint id : SV_VertexID) {\n"
    "  V o; float2 t = float2(id & 1, id >> 1); o.uv = t * uv.xy;\n"
    "  o.pos = float4(lerp(dst.x, dst.z, t.x), lerp(dst.y, dst.w, t.y), 0, 1); return o; }\n"
    "Texture2D<float> ty : register(t0);\n"
    "Texture2D<float2> tuv : register(t1);\n"
    "Texture2D trgba : register(t0);\n"
    "SamplerState s : register(s0);\n"
    // BT.709 limited range, same as SDL's BT709 conversion mode.
    "float4 ps_nv12(V i) : SV_Target {\n"
    "  float2 p = min(i.uv, uv.zw);\n"
    "  float y = (ty.Sample(s, p) - 16.0 / 255.0) * (255.0 / 219.0);\n"
    "  float2 c = (tuv.Sample(s, p) - 128.0 / 255.0) * (255.0 / 224.0);\n"
    "  float3 rgb = float3(y + 1.5748 * c.y, y - 0.1873 * c.x - 0.4681 * c.y, y + 1.8556 * c.x);\n"
    "  return float4(saturate(rgb), 1); }\n"
    "float4 ps_rgba(V i) : SV_Target { return trgba.Sample(s, i.uv); }\n";

static void lock(void) { g_hw->lock(g_hw->lock_ctx); }
static void unlock(void) { g_hw->unlock(g_hw->lock_ctx); }

static ID3DBlob *compile(pD3DCompile fn, const char *entry, const char *target) {
    ID3DBlob *code = NULL, *err = NULL;
    HRESULT hr = fn(g_hlsl, sizeof g_hlsl - 1, "softkvm", NULL, NULL, entry, target,
                    D3DCOMPILE_OPTIMIZATION_LEVEL3, 0, &code, &err);
    if (FAILED(hr)) {
        fprintf(stderr, "d3dp: %s: %s\n", entry, err ? (const char *)ID3D10Blob_GetBufferPointer(err) : "?");
        if (code) ID3D10Blob_Release(code);
        code = NULL;
    }
    if (err) ID3D10Blob_Release(err);
    return code;
}

static int create_pipeline(void) {
    HMODULE dll = LoadLibraryA("d3dcompiler_47.dll");
    pD3DCompile fn = dll ? (pD3DCompile)(void *)GetProcAddress(dll, "D3DCompile") : NULL;
    if (!fn) return -1;
    ID3DBlob *vs = compile(fn, "vs", "vs_4_0"), *p1 = compile(fn, "ps_nv12", "ps_4_0"),
             *p2 = compile(fn, "ps_rgba", "ps_4_0");
    int ok = vs && p1 && p2 &&
             SUCCEEDED(ID3D11Device_CreateVertexShader(g_dev, ID3D10Blob_GetBufferPointer(vs),
                                                       ID3D10Blob_GetBufferSize(vs), NULL, &g_vs)) &&
             SUCCEEDED(ID3D11Device_CreatePixelShader(g_dev, ID3D10Blob_GetBufferPointer(p1),
                                                      ID3D10Blob_GetBufferSize(p1), NULL, &g_ps_nv12)) &&
             SUCCEEDED(ID3D11Device_CreatePixelShader(g_dev, ID3D10Blob_GetBufferPointer(p2),
                                                      ID3D10Blob_GetBufferSize(p2), NULL, &g_ps_rgba));
    if (vs) ID3D10Blob_Release(vs);
    if (p1) ID3D10Blob_Release(p1);
    if (p2) ID3D10Blob_Release(p2);
    if (!ok) return -1;

    D3D11_SAMPLER_DESC sd = {0};
    sd.Filter = D3D11_FILTER_MIN_MAG_MIP_LINEAR;
    sd.AddressU = sd.AddressV = sd.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
    sd.MaxLOD = D3D11_FLOAT32_MAX;
    if (FAILED(ID3D11Device_CreateSamplerState(g_dev, &sd, &g_sampler))) return -1;

    D3D11_BLEND_DESC bd = {0};
    bd.RenderTarget[0].BlendEnable = TRUE;
    bd.RenderTarget[0].SrcBlend = D3D11_BLEND_SRC_ALPHA;
    bd.RenderTarget[0].DestBlend = D3D11_BLEND_INV_SRC_ALPHA;
    bd.RenderTarget[0].BlendOp = D3D11_BLEND_OP_ADD;
    bd.RenderTarget[0].SrcBlendAlpha = D3D11_BLEND_ONE;
    bd.RenderTarget[0].DestBlendAlpha = D3D11_BLEND_INV_SRC_ALPHA;
    bd.RenderTarget[0].BlendOpAlpha = D3D11_BLEND_OP_ADD;
    bd.RenderTarget[0].RenderTargetWriteMask = D3D11_COLOR_WRITE_ENABLE_ALL;
    if (FAILED(ID3D11Device_CreateBlendState(g_dev, &bd, &g_blend))) return -1;

    D3D11_BUFFER_DESC cbd = {32, D3D11_USAGE_DYNAMIC, D3D11_BIND_CONSTANT_BUFFER, D3D11_CPU_ACCESS_WRITE, 0, 0};
    if (FAILED(ID3D11Device_CreateBuffer(g_dev, &cbd, NULL, &g_cb))) return -1;
    return 0;
}

int d3dp_init(AVBufferRef **hw_device, int present_mode) {
    g_mode = present_mode;
    UINT flags = D3D11_CREATE_DEVICE_VIDEO_SUPPORT | D3D11_CREATE_DEVICE_BGRA_SUPPORT;
    D3D_FEATURE_LEVEL levels[] = {D3D_FEATURE_LEVEL_11_1, D3D_FEATURE_LEVEL_11_0};
    if (FAILED(D3D11CreateDevice(NULL, D3D_DRIVER_TYPE_HARDWARE, NULL, flags, levels, 2, D3D11_SDK_VERSION, &g_dev,
                                 NULL, &g_ctx)))
        return -1;
    ID3D10Multithread *mt = NULL;
    if (SUCCEEDED(ID3D11Device_QueryInterface(g_dev, &IID_ID3D10Multithread, (void **)&mt))) {
        ID3D10Multithread_SetMultithreadProtected(mt, TRUE);
        ID3D10Multithread_Release(mt);
    }

    IDXGIDevice *dxgi_dev = NULL;
    IDXGIAdapter *adapter = NULL;
    if (FAILED(ID3D11Device_QueryInterface(g_dev, &IID_IDXGIDevice, (void **)&dxgi_dev)) ||
        FAILED(IDXGIDevice_GetAdapter(dxgi_dev, &adapter)) ||
        FAILED(IDXGIAdapter_GetParent(adapter, &IID_IDXGIFactory2, (void **)&g_factory)))
        goto fail;
    IDXGIAdapter_Release(adapter);
    IDXGIDevice_Release(dxgi_dev);
    IDXGIFactory5 *f5 = NULL;
    if (SUCCEEDED(IDXGIFactory2_QueryInterface(g_factory, &IID_IDXGIFactory5, (void **)&f5))) {
        BOOL t = FALSE;
        if (SUCCEEDED(IDXGIFactory5_CheckFeatureSupport(f5, DXGI_FEATURE_PRESENT_ALLOW_TEARING, &t, sizeof t)))
            g_tearing_ok = t;
        IDXGIFactory5_Release(f5);
    }
    if (g_mode == D3DP_PRESENT_TEARING && !g_tearing_ok) g_mode = D3DP_PRESENT_FLIP;
    if (create_pipeline() < 0) goto fail;

    AVBufferRef *ref = av_hwdevice_ctx_alloc(AV_HWDEVICE_TYPE_D3D11VA);
    if (!ref) goto fail;
    AVHWDeviceContext *dc = (AVHWDeviceContext *)ref->data;
    g_hw = dc->hwctx;
    g_hw->device = g_dev;
    ID3D11Device_AddRef(g_dev);  // FFmpeg releases its reference when the context is freed
    if (av_hwdevice_ctx_init(ref) < 0) {
        av_buffer_unref(&ref);
        goto fail;
    }
    *hw_device = ref;
    g_active = 1;
    return 0;
fail:
    if (g_ctx) ID3D11DeviceContext_Release(g_ctx);
    if (g_dev) ID3D11Device_Release(g_dev);
    g_ctx = NULL;
    g_dev = NULL;
    return -1;
}

int d3dp_active(void) { return g_active; }

enum AVPixelFormat d3dp_get_format(AVCodecContext *c, const enum AVPixelFormat *fmts) {
    for (const enum AVPixelFormat *f = fmts; *f != AV_PIX_FMT_NONE; f++) {
        if (*f != AV_PIX_FMT_D3D11) continue;
        AVBufferRef *fr = NULL;
        if (avcodec_get_hw_frames_parameters(c, c->hw_device_ctx, AV_PIX_FMT_D3D11, &fr) < 0) break;
        AVHWFramesContext *fc = (AVHWFramesContext *)fr->data;
        AVD3D11VAFramesContext *dfc = fc->hwctx;
        dfc->BindFlags |= D3D11_BIND_SHADER_RESOURCE;
        fc->initial_pool_size += 2 + RETIRE + 1;  // held by the renderer: pending, on screen, retiring
        if (av_hwframe_ctx_init(fr) < 0) {
            av_buffer_unref(&fr);
            break;
        }
        av_buffer_unref(&c->hw_frames_ctx);
        c->hw_frames_ctx = fr;
        return AV_PIX_FMT_D3D11;
    }
    return avcodec_default_get_format(c, fmts);
}

static void release_targets(D3DPWindow *w) {
    if (w->rtv) ID3D11RenderTargetView_Release(w->rtv);
    w->rtv = NULL;
}

static int create_targets(D3DPWindow *w) {
    ID3D11Texture2D *bb = NULL;
    if (FAILED(IDXGISwapChain2_GetBuffer(w->sc, 0, &IID_ID3D11Texture2D, (void **)&bb))) return -1;
    HRESULT hr = ID3D11Device_CreateRenderTargetView(g_dev, (ID3D11Resource *)bb, NULL, &w->rtv);
    ID3D11Texture2D_Release(bb);
    return SUCCEEDED(hr) ? 0 : -1;
}

static UINT sc_flags(void) {
    return DXGI_SWAP_CHAIN_FLAG_FRAME_LATENCY_WAITABLE_OBJECT |
           (g_tearing_ok ? DXGI_SWAP_CHAIN_FLAG_ALLOW_TEARING : 0);
}

D3DPWindow *d3dp_window_create(HWND hwnd) {
    if (!g_active) return NULL;
    D3DPWindow *w = calloc(1, sizeof *w);
    w->hwnd = hwnd;
    RECT rc;
    GetClientRect(hwnd, &rc);
    w->w = rc.right > 0 ? (UINT)rc.right : 1;
    w->h = rc.bottom > 0 ? (UINT)rc.bottom : 1;
    DXGI_SWAP_CHAIN_DESC1 d = {0};
    d.Width = w->w;
    d.Height = w->h;
    d.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
    d.SampleDesc.Count = 1;
    d.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    d.BufferCount = 2;
    d.Scaling = DXGI_SCALING_NONE;
    d.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;
    d.AlphaMode = DXGI_ALPHA_MODE_IGNORE;
    d.Flags = sc_flags();
    IDXGISwapChain1 *sc1 = NULL;
    lock();
    HRESULT hr = IDXGIFactory2_CreateSwapChainForHwnd(g_factory, (IUnknown *)g_dev, hwnd, &d, NULL, NULL, &sc1);
    unlock();
    if (FAILED(hr) || FAILED(IDXGISwapChain1_QueryInterface(sc1, &IID_IDXGISwapChain2, (void **)&w->sc))) {
        fprintf(stderr, "d3dp: CreateSwapChainForHwnd failed 0x%lx\n", (unsigned long)hr);
        if (sc1) IDXGISwapChain1_Release(sc1);
        free(w);
        return NULL;
    }
    IDXGISwapChain1_Release(sc1);
    IDXGIFactory2_MakeWindowAssociation(g_factory, hwnd, DXGI_MWA_NO_ALT_ENTER);
    IDXGISwapChain2_SetMaximumFrameLatency(w->sc, 1);
    w->waitable = IDXGISwapChain2_GetFrameLatencyWaitableObject(w->sc);
    for (int i = 0; i < RETIRE; i++) w->retire[i] = av_frame_alloc();
    if (create_targets(w) < 0) {
        d3dp_window_destroy(w);
        return NULL;
    }
    return w;
}

static void flush_srvs(D3DPWindow *w) {
    for (int i = 0; i < w->nsrv; i++) {
        ID3D11ShaderResourceView_Release(w->srv[i].y);
        ID3D11ShaderResourceView_Release(w->srv[i].uv);
    }
    w->nsrv = 0;
    w->srv_tex = NULL;
}

void d3dp_window_destroy(D3DPWindow *w) {
    if (!w) return;
    lock();
    flush_srvs(w);
    release_targets(w);
    if (w->cursor_srv) ID3D11ShaderResourceView_Release(w->cursor_srv);
    if (w->cursor_tex) ID3D11Texture2D_Release(w->cursor_tex);
    if (w->sc) IDXGISwapChain2_Release(w->sc);
    unlock();
    if (w->waitable) CloseHandle(w->waitable);
    for (int i = 0; i < RETIRE; i++) av_frame_free(&w->retire[i]);
    free(w);
}

static int frame_srvs(D3DPWindow *w, ID3D11Texture2D *tex, intptr_t idx, ID3D11ShaderResourceView **y,
                      ID3D11ShaderResourceView **uv) {
    if (w->srv_tex != tex) flush_srvs(w);  // new decoder pool
    w->srv_tex = tex;
    for (int i = 0; i < w->nsrv; i++)
        if (w->srv[i].idx == idx) {
            *y = w->srv[i].y;
            *uv = w->srv[i].uv;
            return 0;
        }
    if (w->nsrv == MAX_SRV) flush_srvs(w), w->srv_tex = tex;
    D3D11_SHADER_RESOURCE_VIEW_DESC sd = {0};
    sd.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2DARRAY;
    sd.Texture2DArray.MipLevels = 1;
    sd.Texture2DArray.FirstArraySlice = (UINT)idx;
    sd.Texture2DArray.ArraySize = 1;
    sd.Format = DXGI_FORMAT_R8_UNORM;
    if (FAILED(ID3D11Device_CreateShaderResourceView(g_dev, (ID3D11Resource *)tex, &sd, y))) return -1;
    sd.Format = DXGI_FORMAT_R8G8_UNORM;
    if (FAILED(ID3D11Device_CreateShaderResourceView(g_dev, (ID3D11Resource *)tex, &sd, uv))) {
        ID3D11ShaderResourceView_Release(*y);
        return -1;
    }
    w->srv[w->nsrv].idx = idx;
    w->srv[w->nsrv].y = *y;
    w->srv[w->nsrv].uv = *uv;
    w->nsrv++;
    return 0;
}

static void set_dst(float l, float t, float r, float b, float su, float sv, float mu, float mv) {
    D3D11_MAPPED_SUBRESOURCE m;
    if (SUCCEEDED(ID3D11DeviceContext_Map(g_ctx, (ID3D11Resource *)g_cb, 0, D3D11_MAP_WRITE_DISCARD, 0, &m))) {
        float *p = m.pData;
        p[0] = l, p[1] = t, p[2] = r, p[3] = b;
        p[4] = su, p[5] = sv, p[6] = mu, p[7] = mv;
        ID3D11DeviceContext_Unmap(g_ctx, (ID3D11Resource *)g_cb, 0);
    }
}

static void update_cursor(D3DPWindow *w, const D3DPCursor *c) {
    if (w->cursor_tex && w->cursor_version == c->version && w->cursor_w == c->w && w->cursor_h == c->h) return;
    if (w->cursor_srv) ID3D11ShaderResourceView_Release(w->cursor_srv);
    if (w->cursor_tex) ID3D11Texture2D_Release(w->cursor_tex);
    w->cursor_srv = NULL;
    w->cursor_tex = NULL;
    D3D11_TEXTURE2D_DESC td = {0};
    td.Width = c->w;
    td.Height = c->h;
    td.MipLevels = td.ArraySize = 1;
    td.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    td.SampleDesc.Count = 1;
    td.Usage = D3D11_USAGE_IMMUTABLE;
    td.BindFlags = D3D11_BIND_SHADER_RESOURCE;
    D3D11_SUBRESOURCE_DATA init = {c->px, (UINT)c->w * 4, 0};
    if (FAILED(ID3D11Device_CreateTexture2D(g_dev, &td, &init, &w->cursor_tex))) return;
    if (FAILED(ID3D11Device_CreateShaderResourceView(g_dev, (ID3D11Resource *)w->cursor_tex, NULL, &w->cursor_srv)))
        return;
    w->cursor_version = c->version;
    w->cursor_w = c->w;
    w->cursor_h = c->h;
}

int d3dp_ready(D3DPWindow *w) {
    return !w || !w->waitable || WaitForSingleObjectEx(w->waitable, 0, FALSE) == WAIT_OBJECT_0;
}

int d3dp_render(D3DPWindow *w, const AVFrame *frame, const D3DPCursor *cursor) {
    if (!w) return -1;
    RECT rc;
    GetClientRect(w->hwnd, &rc);
    UINT cw = rc.right > 0 ? (UINT)rc.right : 1, ch = rc.bottom > 0 ? (UINT)rc.bottom : 1;
    lock();
    if (cw != w->w || ch != w->h) {
        release_targets(w);
        ID3D11DeviceContext_ClearState(g_ctx);
        ID3D11DeviceContext_Flush(g_ctx);
        if (SUCCEEDED(IDXGISwapChain2_ResizeBuffers(w->sc, 0, cw, ch, DXGI_FORMAT_UNKNOWN, sc_flags()))) {
            w->w = cw;
            w->h = ch;
        }
        create_targets(w);
    }
    if (!w->rtv) {
        unlock();
        return -1;
    }
    static const float black[4] = {0, 0, 0, 1};
    ID3D11DeviceContext_OMSetRenderTargets(g_ctx, 1, &w->rtv, NULL);
    ID3D11DeviceContext_ClearRenderTargetView(g_ctx, w->rtv, black);
    D3D11_VIEWPORT vp = {0, 0, (float)w->w, (float)w->h, 0, 1};
    ID3D11DeviceContext_RSSetViewports(g_ctx, 1, &vp);
    ID3D11DeviceContext_IASetPrimitiveTopology(g_ctx, D3D11_PRIMITIVE_TOPOLOGY_TRIANGLESTRIP);
    ID3D11DeviceContext_IASetInputLayout(g_ctx, NULL);
    ID3D11DeviceContext_VSSetShader(g_ctx, g_vs, NULL, 0);
    ID3D11DeviceContext_VSSetConstantBuffers(g_ctx, 0, 1, &g_cb);
    ID3D11DeviceContext_PSSetConstantBuffers(g_ctx, 0, 1, &g_cb);
    ID3D11DeviceContext_PSSetSamplers(g_ctx, 0, 1, &g_sampler);

    if (frame && frame->format == AV_PIX_FMT_D3D11) {
        AVFrame *last = w->retire[(w->retire_pos + RETIRE - 1) % RETIRE];
        if (last->data[0] != frame->data[0] || last->data[1] != frame->data[1] || !last->buf[0]) {
            av_frame_unref(w->retire[w->retire_pos]);
            av_frame_ref(w->retire[w->retire_pos], frame);
            w->retire_pos = (w->retire_pos + 1) % RETIRE;
        }
        ID3D11ShaderResourceView *srv[2];
        if (frame_srvs(w, (ID3D11Texture2D *)frame->data[0], (intptr_t)frame->data[1], &srv[0], &srv[1]) == 0) {
            // Decoder surfaces are padded (HEVC: 128-row alignment, 1440 -> 1536).
            D3D11_TEXTURE2D_DESC td;
            ID3D11Texture2D_GetDesc((ID3D11Texture2D *)frame->data[0], &td);
            float su = (float)frame->width / td.Width, sv = (float)frame->height / td.Height;
            set_dst(-1, 1, 1, -1, su, sv, (frame->width - 1.f) / td.Width, (frame->height - 1.f) / td.Height);
            ID3D11DeviceContext_OMSetBlendState(g_ctx, NULL, NULL, 0xffffffff);
            ID3D11DeviceContext_PSSetShader(g_ctx, g_ps_nv12, NULL, 0);
            ID3D11DeviceContext_PSSetShaderResources(g_ctx, 0, 2, srv);
            ID3D11DeviceContext_Draw(g_ctx, 4, 0);
            ID3D11ShaderResourceView *none[2] = {NULL, NULL};
            ID3D11DeviceContext_PSSetShaderResources(g_ctx, 0, 2, none);
        }
    }
    if (cursor && cursor->px && cursor->w > 0 && cursor->h > 0) {
        update_cursor(w, cursor);
        if (w->cursor_srv) {
            float l = 2.f * cursor->x / w->w - 1, t = 1 - 2.f * cursor->y / w->h;
            float r = 2.f * (cursor->x + cursor->dw) / w->w - 1, b = 1 - 2.f * (cursor->y + cursor->dh) / w->h;
            set_dst(l, t, r, b, 1, 1, 1, 1);
            ID3D11DeviceContext_OMSetBlendState(g_ctx, g_blend, NULL, 0xffffffff);
            ID3D11DeviceContext_PSSetShader(g_ctx, g_ps_rgba, NULL, 0);
            ID3D11DeviceContext_PSSetShaderResources(g_ctx, 0, 1, &w->cursor_srv);
            ID3D11DeviceContext_Draw(g_ctx, 4, 0);
            ID3D11ShaderResourceView *none = NULL;
            ID3D11DeviceContext_PSSetShaderResources(g_ctx, 0, 1, &none);
        }
    }
    unlock();
    return 0;
}

int d3dp_present(D3DPWindow *w) {
    if (!w) return -1;
    // Present outside the decoder lock: in vsync mode it may block until the next vblank.
    UINT sync = g_mode == D3DP_PRESENT_VSYNC ? 1 : 0;
    UINT pflags = g_mode == D3DP_PRESENT_TEARING ? DXGI_PRESENT_ALLOW_TEARING : 0;
    HRESULT hr = IDXGISwapChain2_Present(w->sc, sync, pflags);
    if (hr == DXGI_ERROR_DEVICE_REMOVED || hr == DXGI_ERROR_DEVICE_RESET) return -2;  // caller must rebuild
    return FAILED(hr) ? -1 : 0;
}

void d3dp_gpu_wait(void) {
    if (!g_dev) return;
    D3D11_QUERY_DESC qd = {D3D11_QUERY_EVENT, 0};
    ID3D11Query *q = NULL;
    if (FAILED(ID3D11Device_CreateQuery(g_dev, &qd, &q))) return;
    lock();
    ID3D11DeviceContext_End(g_ctx, (ID3D11Asynchronous *)q);
    ID3D11DeviceContext_Flush(g_ctx);
    unlock();
    BOOL done = FALSE;
    for (;;) {
        lock();
        HRESULT hr = ID3D11DeviceContext_GetData(g_ctx, (ID3D11Asynchronous *)q, &done, sizeof done,
                                                 D3D11_ASYNC_GETDATA_DONOTFLUSH);
        unlock();
        if (hr != S_FALSE) break;
        SwitchToThread();
    }
    ID3D11Query_Release(q);
}

int d3dp_snapshot(D3DPWindow *w, const char *path) {
    if (!w || !w->sc) return -1;
    ID3D11Texture2D *bb = NULL, *st = NULL;
    if (FAILED(IDXGISwapChain2_GetBuffer(w->sc, 0, &IID_ID3D11Texture2D, (void **)&bb))) return -1;
    D3D11_TEXTURE2D_DESC td;
    ID3D11Texture2D_GetDesc(bb, &td);
    td.Usage = D3D11_USAGE_STAGING;
    td.BindFlags = 0;
    td.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    td.MiscFlags = 0;
    int ret = -1;
    lock();
    if (SUCCEEDED(ID3D11Device_CreateTexture2D(g_dev, &td, NULL, &st))) {
        ID3D11DeviceContext_CopyResource(g_ctx, (ID3D11Resource *)st, (ID3D11Resource *)bb);
        D3D11_MAPPED_SUBRESOURCE m;
        if (SUCCEEDED(ID3D11DeviceContext_Map(g_ctx, (ID3D11Resource *)st, 0, D3D11_MAP_READ, 0, &m))) {
            FILE *f = fopen(path, "wb");
            if (f) {  // 32-bit top-down BMP (BGRA)
                uint32_t W = td.Width, H = td.Height, sz = W * H * 4;
                uint8_t h[54] = {'B', 'M'};
                uint32_t v[] = {54 + sz, 0, 54, 40, W, (uint32_t)-(int32_t)H};
                memcpy(h + 2, &v[0], 4), memcpy(h + 10, &v[2], 4), memcpy(h + 14, &v[3], 4);
                memcpy(h + 18, &v[4], 4), memcpy(h + 22, &v[5], 4);
                h[26] = 1, h[28] = 32;
                fwrite(h, 1, 54, f);
                for (uint32_t y = 0; y < H; y++) fwrite((uint8_t *)m.pData + (size_t)y * m.RowPitch, 1, W * 4, f);
                fclose(f);
                ret = 0;
            }
            ID3D11DeviceContext_Unmap(g_ctx, (ID3D11Resource *)st, 0);
        }
        ID3D11Texture2D_Release(st);
    }
    unlock();
    ID3D11Texture2D_Release(bb);
    return ret;
}
