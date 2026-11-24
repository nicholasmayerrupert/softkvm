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
enum { IN_MOUSE_MOVE = 1, IN_MOUSE_BUTTON = 2, IN_SCROLL = 3, IN_KEY = 4, IN_RELEASE_ALL = 5 };
#define HOTKEY_ID_TOGGLE 1
static int g_mac_mode = 1;
static int g_start_hidden;
static int g_pc_style_modifiers = 1;
static int g_hotkey_registered;
static Uint32 g_last_toggle;
static struct { int valid, stream, x, y; } g_pending_move;

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

static int send_hello(void) {
    uint8_t buf[8 + MAX_STREAMS * 12];
    put_u32(buf, PROTO_VERSION);
    put_u32(buf + 4, (uint32_t)g_nstreams);
    for (int i = 0; i < g_nstreams; i++) {
        put_u32(buf + 8 + i * 12, (uint32_t)g_streams[i].w);
        put_u32(buf + 12 + i * 12, (uint32_t)g_streams[i].h);
        put_u32(buf + 16 + i * 12, (uint32_t)g_streams[i].fps);
    }
    return send_msg(MSG_HELLO, 0, buf, 8 + g_nstreams * 12);
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
    SDL_RenderPresent(s->ren);
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
    s->ren = SDL_CreateRenderer(s->win, -1, SDL_RENDERER_ACCELERATED | SDL_RENDERER_PRESENTVSYNC);
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
    if (!g_pending_move.valid) return;
    uint8_t p[10];
    p[0] = IN_MOUSE_MOVE;
    p[1] = (uint8_t)g_pending_move.stream;
    put_u32(p + 2, (uint32_t)g_pending_move.x);
    put_u32(p + 6, (uint32_t)g_pending_move.y);
    send_input(p, sizeof p);
    g_pending_move.valid = 0;
}

static void send_release_all(void) {
    uint8_t p[1] = {IN_RELEASE_ALL};
    g_pending_move.valid = 0;
    send_input(p, 1);
}

static int stream_for_window(Uint32 window_id) {
    for (int i = 0; i < g_nstreams; i++)
        if (g_streams[i].win && SDL_GetWindowID(g_streams[i].win) == window_id) return i;
    return -1;
}

static void set_mac_mode(int on) {
    g_mac_mode = on;
    if (on) {
        // Bring up every Mac window, then focus the one on the monitor the cursor is on.
        int gx, gy, focus = 0;
        SDL_GetGlobalMouseState(&gx, &gy);
        for (int i = 0; i < g_nstreams; i++) {
            SDL_ShowWindow(g_streams[i].win);
            SDL_Rect b;
            SDL_GetDisplayBounds(g_streams[i].monitor, &b);
            if (gx >= b.x && gx < b.x + b.w && gy >= b.y && gy < b.y + b.h) focus = i;
            render_stream(&g_streams[i]);
        }
        SDL_RaiseWindow(g_streams[focus].win);
        SDL_SetWindowKeyboardGrab(g_streams[focus].win, SDL_TRUE);
        SDL_ShowCursor(SDL_DISABLE);
    } else {
        send_release_all();
        for (int i = 0; i < g_nstreams; i++) {
            SDL_SetWindowKeyboardGrab(g_streams[i].win, SDL_FALSE);
            SDL_HideWindow(g_streams[i].win);
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
    case SDL_MOUSEMOTION: {
        int i = stream_for_window(ev->motion.windowID);
        if (i < 0) return 0;
        int ww, wh;
        SDL_GetWindowSize(g_streams[i].win, &ww, &wh);
        if (ww <= 0 || wh <= 0) return 1;
        g_pending_move.valid = 1;
        g_pending_move.stream = i;
        g_pending_move.x = (int)((int64_t)ev->motion.x * g_streams[i].w / ww);
        g_pending_move.y = (int)((int64_t)ev->motion.y * g_streams[i].h / wh);
        return 1;
    }
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
        if (!SDL_WaitEventTimeout(&ev, 100)) continue;
        int check_focus = 0;
        do {
            if (ev.type == SDL_QUIT) {
                SDL_AtomicSet(&g_quit, 1);
            } else if (ev.type == SDL_SYSWMEVENT) {
                if (ev.syswm.msg->msg.win.msg == WM_HOTKEY && ev.syswm.msg->msg.win.wParam == HOTKEY_ID_TOGGLE)
                    toggle_mode();
            } else if (ev.type == SDL_KEYDOWN && (ev.key.keysym.mod & KMOD_CTRL) && (ev.key.keysym.mod & KMOD_ALT) &&
                       (ev.key.keysym.sym == SDLK_q || ev.key.keysym.sym == SDLK_SPACE)) {
                // Local hotkeys are never forwarded.
                if (ev.key.keysym.sym == SDLK_q) SDL_AtomicSet(&g_quit, 1);
                else if (!g_hotkey_registered) toggle_mode();
            } else if (ev.type == SDL_WINDOWEVENT) {
                if (ev.window.event == SDL_WINDOWEVENT_EXPOSED && g_mac_mode) {
                    int i = stream_for_window(ev.window.windowID);
                    if (i >= 0) render_stream(&g_streams[i]);
                } else if (ev.window.event == SDL_WINDOWEVENT_FOCUS_GAINED && g_mac_mode) {
                    SDL_Window *w = SDL_GetWindowFromID(ev.window.windowID);
                    if (w) SDL_SetWindowKeyboardGrab(w, SDL_TRUE);
                } else if (ev.window.event == SDL_WINDOWEVENT_FOCUS_LOST) {
                    check_focus = 1;
                }
            } else if (g_mac_mode) {
                forward_input(&ev);
            }
        } while (SDL_PollEvent(&ev));
        flush_mouse_move();

        // Focus left our windows entirely (e.g. Ctrl+Alt+Del): lift keys held on the Mac.
        if (check_focus && g_mac_mode && stream_for_window(SDL_GetKeyboardFocus()
                ? SDL_GetWindowID(SDL_GetKeyboardFocus()) : 0) < 0) {
            LOG("focus lost; releasing keys on the Mac");
            send_release_all();
        }

        if (!g_mac_mode) continue;  // keep decoding in the background, draw when shown
        for (int i = 0; i < g_nstreams; i++) {
            SDL_LockMutex(g_streams[i].lock);
            int fresh = g_streams[i].has_new;
            SDL_UnlockMutex(g_streams[i].lock);
            if (fresh) render_stream(&g_streams[i]);
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
