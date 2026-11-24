// SoftKVM PC client (Windows).
//
// Connects to the SoftKVM host on a Mac, asks for one virtual display per selected PC
// monitor, and shows each stream in a borderless fullscreen window on that monitor.
// Video is decoded on the GPU (D3D11VA) with a software fallback.
//
//   softkvm.exe HOST [--monitor N[:WxH@FPS]]... [--sw-decode]
//
// Without --monitor, every connected monitor gets a stream at its native resolution.
//
// Ctrl+Alt+Space toggles between Mac mode (Mac windows shown, keyboard/mouse go to the Mac)
// and PC mode (windows hidden, everything back to Windows). Ctrl+Alt+Q quits.

#define SDL_MAIN_HANDLED
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>

#include <SDL.h>
#include <SDL_syswm.h>
#include <libavcodec/avcodec.h>
#include <libavutil/hwcontext.h>

#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "keymap.h"

#define PROTO_VERSION 0
#define DEFAULT_PORT 47900
#define MAX_STREAMS 4
#define MAX_PAYLOAD (64 * 1024 * 1024)

enum {
    MSG_HELLO_ACK = 0x01,
    MSG_STREAM_INFO = 0x02,
    MSG_VIDEO = 0x03,
    MSG_AUDIO = 0x04,
    MSG_PING = 0x05,
    MSG_LAYOUT = 0x06,
    MSG_CURSOR = 0x07,
    MSG_CURSOR_POS = 0x08,
    MSG_HELLO = 0x81,
    MSG_KEYFRAME_REQUEST = 0x82,
    MSG_INPUT = 0x90,
};

enum { CODEC_HEVC = 1, CODEC_H264 = 2 };

typedef struct {
    // configuration
    int monitor;  // SDL display index
    int w, h, fps;

    // decoding (network thread)
    AVCodecContext *dec;
    AVFrame *frame, *sw;
    AVPacket *pkt;
    int codec;
    int waiting_for_key;

    // hand-off to the render thread
    SDL_mutex *lock;
    AVFrame *ready;
    int has_new;

    // rendering (main thread)
    SDL_Window *win;
    SDL_Renderer *ren;
    SDL_Texture *tex;
    int tex_w, tex_h, tex_fmt;
    SDL_Texture *cursor_tex;
    int cursor_tex_version;
    int dirty;  // needs a redraw (e.g. the local cursor moved)
    Uint32 last_present;

    // stats
    int frames, decoded_bytes;
    Uint32 stats_start;
} Stream;

static Stream g_streams[MAX_STREAMS];
static int g_nstreams;
static char g_host[256];
static int g_port = DEFAULT_PORT;
static int g_force_sw;
static SDL_atomic_t g_quit;
static Uint32 g_frame_event;
static SOCKET g_sock = INVALID_SOCKET;
static SDL_mutex *g_send_lock;
static AVBufferRef *g_hw_device;
static FILE *g_logfile;

// Audio: s16le stereo @ 48 kHz pushed into an SDL queue. Playback starts once a small
// cushion is buffered; if the queue grows (clock drift, hiccups) it is reset to stay low-latency.
#define AUDIO_RATE 48000
#define AUDIO_BYTES_PER_MS (AUDIO_RATE * 4 / 1000)
#define AUDIO_START_MS 30
#define AUDIO_MAX_MS 150
static SDL_AudioDeviceID g_audio;
static int g_audio_playing;

// Input / mode switching
enum { IN_MOUSE_MOVE = 1, IN_MOUSE_BUTTON = 2, IN_SCROLL = 3, IN_KEY = 4, IN_RELEASE_ALL = 5,
       IN_MOUSE_MOVE_GLOBAL = 6 };
#define HOTKEY_ID_TOGGLE 1
static int g_mac_mode = 1;
static int g_start_hidden;
static int g_pc_style_modifiers = 1;
static int g_hotkey_registered;
static Uint32 g_last_toggle;
static int g_pending_move;

// Mac display arrangement (global Mac points), written by the net thread.
#define MAX_MAC_DISPLAYS 8
typedef struct { int stream; double x, y, w, h; } MacDisplay;
static SDL_mutex *g_shared_lock;  // guards the g_net_* fields below
static MacDisplay g_net_layout[MAX_MAC_DISPLAYS];
static int g_net_nlayout, g_net_layout_version;
static uint8_t *g_net_cursor_px;
static int g_net_cursor_w, g_net_cursor_h, g_net_cursor_hx, g_net_cursor_hy, g_net_cursor_version;
static int g_net_cursor_pos_pending;
static double g_net_cursor_x, g_net_cursor_y;

// Main-thread copies.
static MacDisplay g_layout[MAX_MAC_DISPLAYS];
static int g_nlayout, g_layout_version;
static double g_cx, g_cy;  // the Mac cursor, in global Mac points
static int g_cursor_valid;
static int g_cursor_w, g_cursor_h, g_cursor_hx, g_cursor_hy, g_cursor_version;
static uint8_t *g_cursor_px;

static void logf_(const char *fmt, ...) {
    char buf[1024];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof buf, fmt, ap);
    va_end(ap);
    SYSTEMTIME t;
    GetLocalTime(&t);
    fprintf(stdout, "%02d:%02d:%02d.%03d %s\n", t.wHour, t.wMinute, t.wSecond, t.wMilliseconds, buf);
    fflush(stdout);
    if (g_logfile) {
        fprintf(g_logfile, "%02d:%02d:%02d.%03d %s\n", t.wHour, t.wMinute, t.wSecond, t.wMilliseconds, buf);
        fflush(g_logfile);
    }
}
#define LOG(...) logf_(__VA_ARGS__)

// ---------------------------------------------------------------------------
// Networking helpers

static void put_u16(uint8_t *p, uint16_t v) { p[0] = v; p[1] = v >> 8; }
static void put_u32(uint8_t *p, uint32_t v) { for (int i = 0; i < 4; i++) p[i] = v >> (8 * i); }
static uint32_t get_u32(const uint8_t *p) { return p[0] | p[1] << 8 | p[2] << 16 | (uint32_t)p[3] << 24; }

static int send_all(SOCKET s, const uint8_t *buf, int len) {
    while (len > 0) {
        int n = send(s, (const char *)buf, len, 0);
        if (n <= 0) return -1;
        buf += n;
        len -= n;
    }
    return 0;
}

static int recv_all(SOCKET s, uint8_t *buf, int len) {
    while (len > 0) {
        int n = recv(s, (char *)buf, len, 0);
        if (n <= 0) return -1;
        buf += n;
        len -= n;
    }
    return 0;
}

static int send_msg(uint8_t type, uint8_t stream, const uint8_t *payload, uint32_t len) {
    uint8_t hdr[8];
    hdr[0] = type;
    hdr[1] = stream;
    put_u16(hdr + 2, 0);
    put_u32(hdr + 4, len);
    SDL_LockMutex(g_send_lock);
    int r = -1;
    if (g_sock != INVALID_SOCKET) {
        r = send_all(g_sock, hdr, 8);
        if (r == 0 && len) r = send_all(g_sock, payload, (int)len);
    }
    SDL_UnlockMutex(g_send_lock);
    return r;
}

static SOCKET connect_host(void) {
    char port[16];
    snprintf(port, sizeof port, "%d", g_port);
    struct addrinfo hints = {0}, *res = NULL;
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;
    if (getaddrinfo(g_host, port, &hints, &res) != 0) return INVALID_SOCKET;
    SOCKET s = INVALID_SOCKET;
    for (struct addrinfo *ai = res; ai; ai = ai->ai_next) {
        s = socket(ai->ai_family, ai->ai_socktype, ai->ai_protocol);
        if (s == INVALID_SOCKET) continue;
        if (connect(s, ai->ai_addr, (int)ai->ai_addrlen) == 0) break;
        closesocket(s);
        s = INVALID_SOCKET;
    }
    freeaddrinfo(res);
    if (s != INVALID_SOCKET) {
        BOOL one = TRUE;
        setsockopt(s, IPPROTO_TCP, TCP_NODELAY, (const char *)&one, sizeof one);
        int rcvbuf = 8 * 1024 * 1024;
        setsockopt(s, SOL_SOCKET, SO_RCVBUF, (const char *)&rcvbuf, sizeof rcvbuf);
        // The host pings every second; silence this long means the link is dead
        // (e.g. a Wi-Fi drop swallowed the close) and we should reconnect.
        DWORD timeout_ms = 4000;
        setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, (const char *)&timeout_ms, sizeof timeout_ms);
        DWORD send_timeout_ms = 2000;
        setsockopt(s, SOL_SOCKET, SO_SNDTIMEO, (const char *)&send_timeout_ms, sizeof send_timeout_ms);
    }
    return s;
}

// ---------------------------------------------------------------------------
// Decoding (network thread)

static enum AVPixelFormat pick_hw_format(AVCodecContext *c, const enum AVPixelFormat *fmts) {
    for (const enum AVPixelFormat *f = fmts; *f != AV_PIX_FMT_NONE; f++)
        if (*f == AV_PIX_FMT_D3D11) return *f;
    LOG("decoder: D3D11 surface format not offered, using software");
    return avcodec_default_get_format(c, fmts);
}

static void stream_close_decoder(Stream *s) {
    avcodec_free_context(&s->dec);
}

static int stream_open_decoder(Stream *s, int codec) {
    stream_close_decoder(s);
    const AVCodec *dec = avcodec_find_decoder(codec == CODEC_H264 ? AV_CODEC_ID_H264 : AV_CODEC_ID_HEVC);
    if (!dec) {
        LOG("decoder: codec %d not available", codec);
        return -1;
    }
    AVCodecContext *c = avcodec_alloc_context3(dec);
    c->flags |= AV_CODEC_FLAG_LOW_DELAY;
    if (g_hw_device) {
        c->hw_device_ctx = av_buffer_ref(g_hw_device);
        c->get_format = pick_hw_format;
        c->thread_count = 1;
    } else {
        c->thread_count = 4;
        c->thread_type = FF_THREAD_SLICE;
    }
    if (avcodec_open2(c, dec, NULL) < 0) {
        LOG("decoder: avcodec_open2 failed");
        avcodec_free_context(&c);
        return -1;
    }
    s->dec = c;
    s->codec = codec;
    s->waiting_for_key = 1;
    LOG("decoder: stream %d opened %s (%s)", (int)(s - g_streams), dec->name, g_hw_device ? "d3d11va" : "software");
    return 0;
}

static void request_keyframe(int idx) {
    send_msg(MSG_KEYFRAME_REQUEST, (uint8_t)idx, NULL, 0);
}

static void handoff(Stream *s, AVFrame *f) {
    SDL_LockMutex(s->lock);
    av_frame_unref(s->ready);
    av_frame_move_ref(s->ready, f);
    int notify = !s->has_new;
    s->has_new = 1;
    SDL_UnlockMutex(s->lock);
    if (notify) {
        SDL_Event ev = {0};
        ev.type = g_frame_event;
        SDL_PushEvent(&ev);
    }
}

static void decode_video(int idx, uint8_t *payload, uint32_t len) {
    Stream *s = &g_streams[idx];
    if (!s->dec || len < 9) return;
    int keyframe = payload[8] & 1;
    if (s->waiting_for_key && !keyframe) return;
    s->waiting_for_key = 0;

    s->pkt->data = payload + 9;
    s->pkt->size = (int)len - 9;
    if (keyframe) s->pkt->flags |= AV_PKT_FLAG_KEY; else s->pkt->flags &= ~AV_PKT_FLAG_KEY;
    int ret = avcodec_send_packet(s->dec, s->pkt);
    if (ret < 0) {
        LOG("decoder: stream %d send_packet error %d, requesting keyframe", idx, ret);
        s->waiting_for_key = 1;
        request_keyframe(idx);
        return;
    }
    while ((ret = avcodec_receive_frame(s->dec, s->frame)) == 0) {
        AVFrame *out = s->frame;
        if (s->frame->format == AV_PIX_FMT_D3D11) {
            av_frame_unref(s->sw);
            if (av_hwframe_transfer_data(s->sw, s->frame, 0) < 0) {
                LOG("decoder: hw transfer failed");
                av_frame_unref(s->frame);
                continue;
            }
            av_frame_unref(s->frame);
            out = s->sw;
        }
        handoff(s, out);
        s->frames++;
    }
    s->decoded_bytes += (int)len;
    Uint32 now = SDL_GetTicks();
    if (now - s->stats_start >= 5000) {
        double secs = (now - s->stats_start) / 1000.0;
        LOG("stream %d: %.1f fps, %.1f Mbps", idx, s->frames / secs, s->decoded_bytes * 8 / secs / 1e6);
        s->frames = 0;
        s->decoded_bytes = 0;
        s->stats_start = now;
    }
}

static void play_audio(const uint8_t *payload, uint32_t len) {
    if (!g_audio || len <= 8) return;
    const uint8_t *pcm = payload + 8;
    uint32_t n = (len - 8) & ~3u;
    Uint32 queued = SDL_GetQueuedAudioSize(g_audio);
    if (queued > AUDIO_MAX_MS * AUDIO_BYTES_PER_MS) {
        // Too far behind: drop the backlog and re-buffer from here.
        SDL_ClearQueuedAudio(g_audio);
        SDL_PauseAudioDevice(g_audio, 1);
        g_audio_playing = 0;
        queued = 0;
    }
    SDL_QueueAudio(g_audio, pcm, n);
    if (!g_audio_playing && queued + n >= AUDIO_START_MS * AUDIO_BYTES_PER_MS) {
        SDL_PauseAudioDevice(g_audio, 0);
        g_audio_playing = 1;
    } else if (g_audio_playing && queued == 0) {
        // Underrun: pause so the next packets build a fresh cushion.
        SDL_PauseAudioDevice(g_audio, 1);
        g_audio_playing = 0;
    }
}

static void notify_main(void) {
    SDL_Event ev = {0};
    ev.type = g_frame_event;
    SDL_PushEvent(&ev);
}

static void handle_layout(const uint8_t *p, uint32_t len) {
    if (len < 4) return;
    uint32_t n = get_u32(p);
    if (n > MAX_MAC_DISPLAYS || len < 4 + n * 20) return;
    SDL_LockMutex(g_shared_lock);
    for (uint32_t i = 0; i < n; i++) {
        const uint8_t *e = p + 4 + i * 20;
        g_net_layout[i].stream = e[0] == 0xFF ? -1 : e[0];
        g_net_layout[i].x = (int32_t)get_u32(e + 4);
        g_net_layout[i].y = (int32_t)get_u32(e + 8);
        g_net_layout[i].w = (int32_t)get_u32(e + 12);
        g_net_layout[i].h = (int32_t)get_u32(e + 16);
        LOG("layout: %s at %.0f,%.0f %.0fx%.0f", e[0] == 0xFF ? "mac display" : "stream",
            g_net_layout[i].x, g_net_layout[i].y, g_net_layout[i].w, g_net_layout[i].h);
    }
    g_net_nlayout = (int)n;
    g_net_layout_version++;
    SDL_UnlockMutex(g_shared_lock);
    notify_main();
}

static void handle_cursor_shape(const uint8_t *p, uint32_t len) {
    if (len < 16) return;
    int hx = (int32_t)get_u32(p), hy = (int32_t)get_u32(p + 4);
    uint32_t w = get_u32(p + 8), h = get_u32(p + 12);
    if (w == 0 || h == 0 || w > 256 || h > 256 || len < 16 + w * h * 4) return;
    uint8_t *px = malloc(w * h * 4);
    memcpy(px, p + 16, w * h * 4);
    SDL_LockMutex(g_shared_lock);
    free(g_net_cursor_px);
    g_net_cursor_px = px;
    g_net_cursor_w = (int)w;
    g_net_cursor_h = (int)h;
    g_net_cursor_hx = hx;
    g_net_cursor_hy = hy;
    g_net_cursor_version++;
    SDL_UnlockMutex(g_shared_lock);
    notify_main();
}

static int send_hello(void) {
    uint8_t buf[8 + MAX_STREAMS * 20];
    put_u32(buf, PROTO_VERSION);
    put_u32(buf + 4, (uint32_t)g_nstreams);
    for (int i = 0; i < g_nstreams; i++) {
        put_u32(buf + 8 + i * 12, (uint32_t)g_streams[i].w);
        put_u32(buf + 12 + i * 12, (uint32_t)g_streams[i].h);
        put_u32(buf + 16 + i * 12, (uint32_t)g_streams[i].fps);
    }
    // Extension: each monitor's desktop origin so the Mac can arrange its displays alike.
    uint8_t *ext = buf + 8 + g_nstreams * 12;
    for (int i = 0; i < g_nstreams; i++) {
        SDL_Rect b;
        SDL_GetDisplayBounds(g_streams[i].monitor, &b);
        put_u32(ext + i * 8, (uint32_t)b.x);
        put_u32(ext + i * 8 + 4, (uint32_t)b.y);
    }
    return send_msg(MSG_HELLO, 0, buf, 8 + g_nstreams * 20);
}

static int net_thread(void *unused) {
    (void)unused;
    uint8_t *buf = NULL;
    uint32_t cap = 0;
    while (!SDL_AtomicGet(&g_quit)) {
        LOG("net: connecting to %s:%d", g_host, g_port);
        SOCKET s = connect_host();
        if (s == INVALID_SOCKET) {
            LOG("net: connect failed, retrying in 2s");
            SDL_Delay(2000);
            continue;
        }
        SDL_LockMutex(g_send_lock);
        g_sock = s;
        SDL_UnlockMutex(g_send_lock);
        LOG("net: connected");
        if (send_hello() < 0) goto disconnected;

        for (;;) {
            uint8_t hdr[8];
            if (recv_all(s, hdr, 8) < 0) break;
            uint8_t type = hdr[0], stream = hdr[1];
            uint32_t len = get_u32(hdr + 4);
            if (len > MAX_PAYLOAD) {
                LOG("net: payload too large (%u)", len);
                break;
            }
            if (len + AV_INPUT_BUFFER_PADDING_SIZE > cap) {
                cap = len + AV_INPUT_BUFFER_PADDING_SIZE;
                buf = realloc(buf, cap);
            }
            if (len && recv_all(s, buf, (int)len) < 0) break;
            memset(buf + len, 0, AV_INPUT_BUFFER_PADDING_SIZE);

            switch (type) {
            case MSG_HELLO_ACK:
                LOG("net: host protocol v%u", len >= 4 ? get_u32(buf) : 0);
                break;
            case MSG_STREAM_INFO:
                if (stream < g_nstreams && len >= 16) {
                    LOG("net: stream %d is %ux%u@%u codec %u", stream, get_u32(buf), get_u32(buf + 4),
                        get_u32(buf + 8), get_u32(buf + 12));
                    stream_open_decoder(&g_streams[stream], (int)get_u32(buf + 12));
                }
                break;
            case MSG_VIDEO:
                if (stream < g_nstreams) decode_video(stream, buf, len);
                break;
            case MSG_AUDIO:
                play_audio(buf, len);
                break;
            case MSG_LAYOUT:
                handle_layout(buf, len);
                break;
            case MSG_CURSOR:
                handle_cursor_shape(buf, len);
                break;
            case MSG_CURSOR_POS:
                if (len >= 8) {
                    float x, y;
                    uint32_t ux = get_u32(buf), uy = get_u32(buf + 4);
                    memcpy(&x, &ux, 4);
                    memcpy(&y, &uy, 4);
                    SDL_LockMutex(g_shared_lock);
                    g_net_cursor_x = x;
                    g_net_cursor_y = y;
                    g_net_cursor_pos_pending = 1;
                    SDL_UnlockMutex(g_shared_lock);
                    notify_main();
                }
                break;
            default:
                break;
            }
        }
    disconnected:
        LOG("net: disconnected");
        SDL_LockMutex(g_send_lock);
        closesocket(s);
        g_sock = INVALID_SOCKET;
        SDL_UnlockMutex(g_send_lock);
        for (int i = 0; i < g_nstreams; i++) stream_close_decoder(&g_streams[i]);
        if (g_audio) {
            SDL_PauseAudioDevice(g_audio, 1);
            SDL_ClearQueuedAudio(g_audio);
            g_audio_playing = 0;
        }
        if (!SDL_AtomicGet(&g_quit)) SDL_Delay(1000);
    }
    free(buf);
    return 0;
}

// ---------------------------------------------------------------------------
// Rendering (main thread)

static const MacDisplay *layout_for_stream(int stream) {
    for (int i = 0; i < g_nlayout; i++)
        if (g_layout[i].stream == stream) return &g_layout[i];
    return NULL;
}

static int display_at(double x, double y) {
    for (int i = 0; i < g_nlayout; i++)
        if (x >= g_layout[i].x && x < g_layout[i].x + g_layout[i].w && y >= g_layout[i].y &&
            y < g_layout[i].y + g_layout[i].h)
            return i;
    return -1;
}

/// Stream index showing the cursor right now, or -1 (e.g. it's on the MacBook's own screen).
static int cursor_stream(void) {
    if (!g_cursor_valid) return -1;
    int d = display_at(g_cx, g_cy);
    return d >= 0 ? g_layout[d].stream : -1;
}

/// Draws the Mac cursor locally so it moves with zero video latency.
static void draw_cursor(Stream *s) {
    if (!g_mac_mode || !g_cursor_valid || !g_cursor_px) return;
    int idx = (int)(s - g_streams);
    const MacDisplay *d = layout_for_stream(idx);
    if (!d || cursor_stream() != idx) return;
    if (s->cursor_tex_version != g_cursor_version || !s->cursor_tex) {
        if (s->cursor_tex) SDL_DestroyTexture(s->cursor_tex);
        s->cursor_tex = SDL_CreateTexture(s->ren, SDL_PIXELFORMAT_ABGR8888, SDL_TEXTUREACCESS_STATIC,
                                          g_cursor_w, g_cursor_h);
        if (!s->cursor_tex) return;
        SDL_UpdateTexture(s->cursor_tex, NULL, g_cursor_px, g_cursor_w * 4);
        SDL_SetTextureBlendMode(s->cursor_tex, SDL_BLENDMODE_BLEND);
        s->cursor_tex_version = g_cursor_version;
    }
    int ow, oh;
    SDL_GetRendererOutputSize(s->ren, &ow, &oh);
    double sx = ow / d->w, sy = oh / d->h;
    SDL_Rect r = {(int)((g_cx - d->x - g_cursor_hx) * sx), (int)((g_cy - d->y - g_cursor_hy) * sy),
                  (int)(g_cursor_w * sx + 0.5), (int)(g_cursor_h * sy + 0.5)};
    SDL_RenderCopy(s->ren, s->cursor_tex, NULL, &r);
}

static void mark_cursor_dirty(void) {
    int i = cursor_stream();
    if (i >= 0) g_streams[i].dirty = 1;
}

static void render_stream(Stream *s) {
    AVFrame *f = av_frame_alloc();
    SDL_LockMutex(s->lock);
    int fresh = s->has_new;
    if (fresh) av_frame_move_ref(f, s->ready);
    s->has_new = 0;
    SDL_UnlockMutex(s->lock);

    if (fresh) {
        int fmt = f->format == AV_PIX_FMT_NV12 ? SDL_PIXELFORMAT_NV12
                : f->format == AV_PIX_FMT_YUV420P ? SDL_PIXELFORMAT_IYUV
                : 0;
        if (!fmt) {
            LOG("render: unsupported pixel format %d", f->format);
        } else {
            if (!s->tex || s->tex_w != f->width || s->tex_h != f->height || s->tex_fmt != fmt) {
                if (s->tex) SDL_DestroyTexture(s->tex);
                s->tex = SDL_CreateTexture(s->ren, fmt, SDL_TEXTUREACCESS_STREAMING, f->width, f->height);
                s->tex_w = f->width;
                s->tex_h = f->height;
                s->tex_fmt = fmt;
            }
            if (fmt == SDL_PIXELFORMAT_NV12)
                SDL_UpdateNVTexture(s->tex, NULL, f->data[0], f->linesize[0], f->data[1], f->linesize[1]);
            else
                SDL_UpdateYUVTexture(s->tex, NULL, f->data[0], f->linesize[0], f->data[1], f->linesize[1],
                                     f->data[2], f->linesize[2]);
        }
    }
    av_frame_free(&f);

    SDL_SetRenderDrawColor(s->ren, 0, 0, 0, 255);
    SDL_RenderClear(s->ren);
    if (s->tex) SDL_RenderCopy(s->ren, s->tex, NULL, NULL);
    draw_cursor(s);
    SDL_RenderPresent(s->ren);
    s->dirty = 0;
    s->last_present = SDL_GetTicks();
}

static int create_window(Stream *s, int idx) {
    SDL_Rect b;
    SDL_GetDisplayBounds(s->monitor, &b);
    char title[64];
    snprintf(title, sizeof title, "SoftKVM %d", idx + 1);
    s->win = SDL_CreateWindow(title, b.x, b.y, b.w, b.h, SDL_WINDOW_BORDERLESS | SDL_WINDOW_ALLOW_HIGHDPI);
    if (!s->win) {
        LOG("window: %s", SDL_GetError());
        return -1;
    }
    SDL_SetWindowFullscreen(s->win, SDL_WINDOW_FULLSCREEN_DESKTOP);
    s->ren = SDL_CreateRenderer(s->win, -1, SDL_RENDERER_ACCELERATED);
    if (!s->ren) {
        LOG("renderer: %s", SDL_GetError());
        return -1;
    }
    SDL_RendererInfo info;
    SDL_GetRendererInfo(s->ren, &info);
    LOG("window %d on monitor %d (%dx%d at %d,%d), renderer %s", idx, s->monitor, b.w, b.h, b.x, b.y, info.name);
    SDL_SetRenderDrawColor(s->ren, 0, 0, 0, 255);
    SDL_RenderClear(s->ren);
    SDL_RenderPresent(s->ren);
    return 0;
}

// ---------------------------------------------------------------------------
// Input forwarding (main thread)

static void send_input(const uint8_t *p, uint32_t n) { send_msg(MSG_INPUT, 0, p, n); }

static void flush_mouse_move(void) {
    if (!g_pending_move) return;
    g_pending_move = 0;
    float x = (float)g_cx, y = (float)g_cy;
    uint32_t ux, uy;
    memcpy(&ux, &x, 4);
    memcpy(&uy, &y, 4);
    uint8_t p[9];
    p[0] = IN_MOUSE_MOVE_GLOBAL;
    put_u32(p + 1, ux);
    put_u32(p + 5, uy);
    send_input(p, sizeof p);
}

static void send_release_all(void) {
    uint8_t p[1] = {IN_RELEASE_ALL};
    send_input(p, 1);
}

static double clampd(double v, double lo, double hi) { return v < lo ? lo : v > hi ? hi : v; }

/// Moves the Mac cursor by a relative amount, sliding along the edges of the Mac's display
/// arrangement like macOS does, so it can travel onto any Mac display (including the built-in one).
static void move_cursor(double dx, double dy) {
    if (!g_cursor_valid || g_nlayout == 0) return;
    int cur = display_at(g_cx, g_cy);
    if (cur < 0) {
        cur = 0;
        g_cx = clampd(g_cx, g_layout[0].x, g_layout[0].x + g_layout[0].w - 1);
        g_cy = clampd(g_cy, g_layout[0].y, g_layout[0].y + g_layout[0].h - 1);
    }
    mark_cursor_dirty();
    double nx = g_cx + dx;
    if (display_at(nx, g_cy) < 0) nx = clampd(nx, g_layout[cur].x, g_layout[cur].x + g_layout[cur].w - 1);
    int c2 = display_at(nx, g_cy);
    if (c2 < 0) c2 = cur;
    double ny = g_cy + dy;
    if (display_at(nx, ny) < 0) ny = clampd(ny, g_layout[c2].y, g_layout[c2].y + g_layout[c2].h - 1);
    g_cx = nx;
    g_cy = ny;
    mark_cursor_dirty();
    g_pending_move = 1;
}

/// Pulls layout, cursor shape and host-reported cursor position from the net thread.
static void sync_shared_state(void) {
    SDL_LockMutex(g_shared_lock);
    if (g_layout_version != g_net_layout_version) {
        memcpy(g_layout, g_net_layout, sizeof g_layout);
        g_nlayout = g_net_nlayout;
        g_layout_version = g_net_layout_version;
        for (int i = 0; i < g_nstreams; i++) g_streams[i].dirty = 1;
    }
    if (g_cursor_version != g_net_cursor_version && g_net_cursor_px) {
        free(g_cursor_px);
        g_cursor_px = malloc((size_t)g_net_cursor_w * g_net_cursor_h * 4);
        memcpy(g_cursor_px, g_net_cursor_px, (size_t)g_net_cursor_w * g_net_cursor_h * 4);
        g_cursor_w = g_net_cursor_w;
        g_cursor_h = g_net_cursor_h;
        g_cursor_hx = g_net_cursor_hx;
        g_cursor_hy = g_net_cursor_hy;
        g_cursor_version = g_net_cursor_version;
        mark_cursor_dirty();
    }
    if (g_net_cursor_pos_pending) {
        g_net_cursor_pos_pending = 0;
        mark_cursor_dirty();
        g_cx = g_net_cursor_x;
        g_cy = g_net_cursor_y;
        g_cursor_valid = 1;
        mark_cursor_dirty();
    }
    SDL_UnlockMutex(g_shared_lock);
}

static int stream_for_window(Uint32 window_id) {
    for (int i = 0; i < g_nstreams; i++)
        if (g_streams[i].win && SDL_GetWindowID(g_streams[i].win) == window_id) return i;
    return -1;
}

static void set_mac_mode(int on) {
    g_mac_mode = on;
    if (on) {
        // Bring up every Mac window and focus the one on the monitor the PC cursor is on.
        // The Mac cursor jumps to the same spot, so switching feels continuous.
        int gx, gy, focus = 0;
        SDL_GetGlobalMouseState(&gx, &gy);
        for (int i = 0; i < g_nstreams; i++) {
            SDL_ShowWindow(g_streams[i].win);
            SDL_Rect b;
            SDL_GetDisplayBounds(g_streams[i].monitor, &b);
            if (gx >= b.x && gx < b.x + b.w && gy >= b.y && gy < b.y + b.h) {
                focus = i;
                const MacDisplay *d = layout_for_stream(i);
                if (d) {
                    g_cx = d->x + (double)(gx - b.x) / b.w * d->w;
                    g_cy = d->y + (double)(gy - b.y) / b.h * d->h;
                    g_cursor_valid = 1;
                    g_pending_move = 1;
                }
            }
            g_streams[i].dirty = 1;
        }
        SDL_RaiseWindow(g_streams[focus].win);
        SDL_SetWindowKeyboardGrab(g_streams[focus].win, SDL_TRUE);
        // Relative mode: Windows' cursor is hidden and pinned; we get raw movement (with the
        // system pointer speed applied) and move the Mac cursor ourselves.
        if (SDL_SetRelativeMouseMode(SDL_TRUE) != 0) LOG("relative mouse: %s (will retry on focus)", SDL_GetError());
        for (int i = 0; i < g_nstreams; i++) render_stream(&g_streams[i]);
    } else {
        send_release_all();
        SDL_SetRelativeMouseMode(SDL_FALSE);
        // Put the PC cursor where the Mac cursor was, if it was on one of the monitors.
        int cs = cursor_stream();
        const MacDisplay *d = cs >= 0 ? layout_for_stream(cs) : NULL;
        for (int i = 0; i < g_nstreams; i++) {
            SDL_SetWindowKeyboardGrab(g_streams[i].win, SDL_FALSE);
            SDL_HideWindow(g_streams[i].win);
        }
        if (d) {
            SDL_Rect b;
            SDL_GetDisplayBounds(g_streams[cs].monitor, &b);
            SDL_WarpMouseGlobal(b.x + (int)((g_cx - d->x) / d->w * b.w), b.y + (int)((g_cy - d->y) / d->h * b.h));
        }
        SDL_ShowCursor(SDL_ENABLE);
    }
    LOG("mode: %s", on ? "Mac" : "PC");
}

static void toggle_mode(void) {
    Uint32 now = SDL_GetTicks();
    if (now - g_last_toggle < 300) return;  // hotkey can arrive twice (system + window)
    g_last_toggle = now;
    set_mac_mode(!g_mac_mode);
}

static int mouse_button_index(Uint8 b) {
    switch (b) {
    case SDL_BUTTON_LEFT: return 0;
    case SDL_BUTTON_RIGHT: return 1;
    case SDL_BUTTON_MIDDLE: return 2;
    case SDL_BUTTON_X1: return 3;
    case SDL_BUTTON_X2: return 4;
    default: return -1;
    }
}

/// Handles keyboard/mouse events while in Mac mode. Returns 1 if the event was consumed.
static int forward_input(const SDL_Event *ev) {
    switch (ev->type) {
    case SDL_MOUSEMOTION:
        move_cursor(ev->motion.xrel, ev->motion.yrel);
        return 1;
    case SDL_MOUSEBUTTONDOWN:
    case SDL_MOUSEBUTTONUP: {
        int b = mouse_button_index(ev->button.button);
        if (b < 0) return 1;
        flush_mouse_move();
        uint8_t p[3] = {IN_MOUSE_BUTTON, (uint8_t)b, ev->type == SDL_MOUSEBUTTONDOWN};
        send_input(p, sizeof p);
        return 1;
    }
    case SDL_MOUSEWHEEL: {
        flush_mouse_move();
        float dx = ev->wheel.preciseX, dy = ev->wheel.preciseY;
        if (ev->wheel.direction == SDL_MOUSEWHEEL_FLIPPED) { dx = -dx; dy = -dy; }
        uint8_t p[9];
        uint32_t ux, uy;
        memcpy(&ux, &dx, 4);
        memcpy(&uy, &dy, 4);
        p[0] = IN_SCROLL;
        put_u32(p + 1, ux);
        put_u32(p + 5, uy);
        send_input(p, sizeof p);
        return 1;
    }
    case SDL_KEYDOWN:
    case SDL_KEYUP: {
        uint16_t code = mac_keycode(ev->key.keysym.scancode, g_pc_style_modifiers);
        if (code == KEYMAP_NONE) return 1;
        uint8_t p[5];
        p[0] = IN_KEY;
        put_u16(p + 1, code);
        p[3] = ev->type == SDL_KEYDOWN;
        p[4] = ev->key.repeat ? 1 : 0;
        send_input(p, sizeof p);
        return 1;
    }
    default:
        return 0;
    }
}

static void usage(void) {
    fprintf(stderr,
            "usage: softkvm HOST [--port P] [--monitor N[:WxH@FPS]]... [--sw-decode]\n"
            "                    [--start-hidden] [--mac-modifiers]\n"
            "  Without --monitor every PC monitor gets a stream at its native mode (max 60 fps).\n"
            "  --mac-modifiers  Ctrl stays Control and the Windows key is Command\n"
            "                   (default: Ctrl acts as Command, so Ctrl+C/V copy and paste).\n"
            "  Ctrl+Alt+Space switches between Mac and PC. Ctrl+Alt+Q quits.\n");
}

static int add_monitor(const char *spec) {
    if (g_nstreams >= MAX_STREAMS) return -1;
    Stream *s = &g_streams[g_nstreams];
    memset(s, 0, sizeof *s);
    s->monitor = atoi(spec);
    if (s->monitor < 0 || s->monitor >= SDL_GetNumVideoDisplays()) {
        fprintf(stderr, "no monitor %d (have %d)\n", s->monitor, SDL_GetNumVideoDisplays());
        return -1;
    }
    SDL_DisplayMode m;
    SDL_GetDesktopDisplayMode(s->monitor, &m);
    s->w = m.w;
    s->h = m.h;
    s->fps = m.refresh_rate > 0 && m.refresh_rate < 60 ? m.refresh_rate : 60;
    const char *colon = strchr(spec, ':');
    if (colon && sscanf(colon + 1, "%dx%d@%d", &s->w, &s->h, &s->fps) < 2) return -1;
    g_nstreams++;
    return 0;
}

int main(int argc, char **argv) {
    g_logfile = fopen("softkvm.log", "a");

    SDL_SetHint(SDL_HINT_WINDOWS_DPI_AWARENESS, "permonitorv2");
    SDL_SetHint(SDL_HINT_VIDEO_MINIMIZE_ON_FOCUS_LOSS, "0");
    SDL_SetHint(SDL_HINT_RENDER_DRIVER, "direct3d11");
    SDL_SetHint(SDL_HINT_RENDER_SCALE_QUALITY, "linear");
    SDL_SetHint(SDL_HINT_MOUSE_FOCUS_CLICKTHROUGH, "1");
    SDL_SetHint(SDL_HINT_ALLOW_ALT_TAB_WHILE_GRABBED, "0");
    SDL_SetHint(SDL_HINT_MOUSE_RELATIVE_MODE_WARP, "0");
    SDL_SetHint(SDL_HINT_MOUSE_RELATIVE_SYSTEM_SCALE, "1");  // honor Windows pointer speed/acceleration
    if (SDL_Init(SDL_INIT_VIDEO | SDL_INIT_EVENTS | SDL_INIT_AUDIO) != 0) {
        fprintf(stderr, "SDL_Init: %s\n", SDL_GetError());
        return 1;
    }
    SDL_SetYUVConversionMode(SDL_YUV_CONVERSION_BT709);

    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--port") && i + 1 < argc) g_port = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--monitor") && i + 1 < argc) {
            if (add_monitor(argv[++i]) < 0) { usage(); return 1; }
        } else if (!strcmp(argv[i], "--sw-decode")) g_force_sw = 1;
        else if (!strcmp(argv[i], "--start-hidden")) g_start_hidden = 1;
        else if (!strcmp(argv[i], "--mac-modifiers")) g_pc_style_modifiers = 0;
        else if (argv[i][0] != '-' && !g_host[0]) snprintf(g_host, sizeof g_host, "%s", argv[i]);
        else { usage(); return 1; }
    }
    if (!g_host[0]) { usage(); return 1; }
    if (g_nstreams == 0) {
        for (int i = 0; i < SDL_GetNumVideoDisplays() && i < MAX_STREAMS; i++) {
            char spec[8];
            snprintf(spec, sizeof spec, "%d", i);
            add_monitor(spec);
        }
    }
    for (int i = 0; i < g_nstreams; i++)
        LOG("stream %d -> monitor %d (%s), requesting %dx%d@%d", i, g_streams[i].monitor,
            SDL_GetDisplayName(g_streams[i].monitor), g_streams[i].w, g_streams[i].h, g_streams[i].fps);

    SDL_AudioSpec want = {0}, have;
    want.freq = AUDIO_RATE;
    want.format = AUDIO_S16LSB;
    want.channels = 2;
    want.samples = 256;  // ~5 ms device buffer
    g_audio = SDL_OpenAudioDevice(NULL, 0, &want, &have, 0);
    if (g_audio) LOG("audio: output %d Hz, %d ch, %d-sample buffer", have.freq, have.channels, have.samples);
    else LOG("audio: no output device (%s)", SDL_GetError());

    WSADATA wsa;
    WSAStartup(MAKEWORD(2, 2), &wsa);

    if (!g_force_sw && av_hwdevice_ctx_create(&g_hw_device, AV_HWDEVICE_TYPE_D3D11VA, NULL, NULL, 0) < 0) {
        LOG("decoder: D3D11VA unavailable, using software decoding");
        g_hw_device = NULL;
    }

    g_frame_event = SDL_RegisterEvents(1);
    g_send_lock = SDL_CreateMutex();
    g_shared_lock = SDL_CreateMutex();
    for (int i = 0; i < g_nstreams; i++) {
        Stream *s = &g_streams[i];
        s->lock = SDL_CreateMutex();
        s->ready = av_frame_alloc();
        s->frame = av_frame_alloc();
        s->sw = av_frame_alloc();
        s->pkt = av_packet_alloc();
        s->stats_start = SDL_GetTicks();
        if (create_window(s, i) < 0) return 1;
    }

    // Global hotkey: works in PC mode too, when our windows are hidden.
    SDL_EventState(SDL_SYSWMEVENT, SDL_ENABLE);
    SDL_StopTextInput();
    {
        SDL_SysWMinfo wm;
        SDL_VERSION(&wm.version);
        if (SDL_GetWindowWMInfo(g_streams[0].win, &wm) &&
            RegisterHotKey(wm.info.win.window, HOTKEY_ID_TOGGLE, MOD_CONTROL | MOD_ALT | MOD_NOREPEAT, VK_SPACE)) {
            g_hotkey_registered = 1;
            LOG("hotkey: Ctrl+Alt+Space registered");
        } else {
            LOG("hotkey: could not register Ctrl+Alt+Space globally (in use?); it only works in Mac mode");
        }
    }
    set_mac_mode(!g_start_hidden);

    SDL_Thread *net = SDL_CreateThread(net_thread, "net", NULL);

    while (!SDL_AtomicGet(&g_quit)) {
        SDL_Event ev;
        int any_dirty = 0;
        for (int i = 0; i < g_nstreams; i++) any_dirty |= g_streams[i].dirty;
        int got = SDL_WaitEventTimeout(&ev, g_mac_mode && any_dirty ? 2 : 100);
        int check_focus = 0;
        if (got) do {
            if (ev.type == SDL_QUIT) {
                SDL_AtomicSet(&g_quit, 1);
            } else if (ev.type == SDL_SYSWMEVENT) {
                if (ev.syswm.msg->msg.win.msg == WM_HOTKEY && ev.syswm.msg->msg.win.wParam == HOTKEY_ID_TOGGLE)
                    toggle_mode();
            } else if (ev.type == SDL_KEYDOWN && (ev.key.keysym.mod & KMOD_CTRL) && (ev.key.keysym.mod & KMOD_ALT) &&
                       (ev.key.keysym.sym == SDLK_q || ev.key.keysym.sym == SDLK_SPACE)) {
                // Local hotkeys are never forwarded.
                // In Mac mode the keyboard grab swallows Ctrl+Alt+Space before Windows' hotkey
                // sees it, so handle it here too (toggle_mode debounces duplicates).
                if (ev.key.keysym.sym == SDLK_q) SDL_AtomicSet(&g_quit, 1);
                else toggle_mode();
            } else if (ev.type == SDL_WINDOWEVENT) {
                if (ev.window.event == SDL_WINDOWEVENT_EXPOSED && g_mac_mode) {
                    int i = stream_for_window(ev.window.windowID);
                    if (i >= 0) render_stream(&g_streams[i]);
                } else if (ev.window.event == SDL_WINDOWEVENT_FOCUS_GAINED && g_mac_mode) {
                    SDL_Window *w = SDL_GetWindowFromID(ev.window.windowID);
                    if (w) SDL_SetWindowKeyboardGrab(w, SDL_TRUE);
                    if (!SDL_GetRelativeMouseMode()) SDL_SetRelativeMouseMode(SDL_TRUE);
                } else if (ev.window.event == SDL_WINDOWEVENT_FOCUS_LOST) {
                    check_focus = 1;
                }
            } else if (g_mac_mode) {
                forward_input(&ev);
            }
        } while (SDL_PollEvent(&ev));
        sync_shared_state();
        flush_mouse_move();

        // Focus left our windows entirely (e.g. Ctrl+Alt+Del): lift keys held on the Mac.
        if (check_focus && g_mac_mode && stream_for_window(SDL_GetKeyboardFocus()
                ? SDL_GetWindowID(SDL_GetKeyboardFocus()) : 0) < 0) {
            LOG("focus lost; releasing keys on the Mac");
            send_release_all();
        }

        if (!g_mac_mode) continue;  // keep decoding in the background, draw when shown
        Uint32 now = SDL_GetTicks();
        for (int i = 0; i < g_nstreams; i++) {
            Stream *st = &g_streams[i];
            SDL_LockMutex(st->lock);
            int fresh = st->has_new;
            SDL_UnlockMutex(st->lock);
            // New video draws immediately; cursor-only redraws are capped at ~250 Hz.
            if (fresh || (st->dirty && now - st->last_present >= 4)) render_stream(st);
        }
    }

    LOG("quitting");
    send_release_all();
    SDL_LockMutex(g_send_lock);
    if (g_sock != INVALID_SOCKET) shutdown(g_sock, SD_BOTH);
    SDL_UnlockMutex(g_send_lock);
    SDL_WaitThread(net, NULL);
    SDL_Quit();
    WSACleanup();
    return 0;
}
