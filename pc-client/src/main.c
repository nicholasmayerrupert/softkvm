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
#include <libavcodec/avcodec.h>
#include <libavutil/hwcontext.h>

#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "app.h"
#include "config.h"
#include "discovery.h"
#include "keymap.h"
#include "secure.h"
#include "perf.h"
#include "d3d_present.h"
#include <SDL_syswm.h>  // HWND for the D3D11 presenter
#include "tray.h"

#include <shellapi.h>

#ifndef SOFTKVM_VERSION
#define SOFTKVM_VERSION "dev"
#endif
#define PROTO_VERSION SK_PROTO_VERSION
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
    MSG_SET_MONITORS = 0x09,
    MSG_BYE = 0x0A,
    MSG_AUTH_STATUS = 0x0B,
    MSG_HELLO = 0x81,
    MSG_KEYFRAME_REQUEST = 0x82,
    MSG_MONITORS = 0x83,
    MSG_TAKEOVER = 0x84,
    MSG_GOODBYE = 0x85,
    MSG_CONFIRM = 0x86,
    MSG_VIEWING = 0x87,
    MSG_INPUT = 0x90,
};

enum { CODEC_HEVC = 1, CODEC_H264 = 2 };
enum { AUTH_OK = 0, AUTH_PAIRING_PENDING = 1, AUTH_PAIRING_DENIED = 2, AUTH_BUSY = 3, AUTH_PAIRED = 4 };
enum { BYE_USER = 0, BYE_TAKEN_OVER = 1, BYE_UNPAIRED = 2, BYE_HOST_QUITTING = 3 };
enum { GOODBYE_QUIT = 0, GOODBYE_SWITCHING = 1 };

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
    D3DPWindow *d3d;  // zero-copy presenter (NULL: SDL renderer)
    SDL_Rect bounds;  // the monitor's desktop rect when the window was made
    AVFrame *cur;     // frame on screen (d3d path; keeps the decoder surface alive)

    // stats
    int frames, decoded_bytes;
    Uint32 stats_start;
} Stream;

static Stream g_streams[MAX_STREAMS];
static int g_nstreams;
static Config g_cfg;          // main thread only
static int g_force_sw;
static int g_sdl_render;  // --sdl-render: old copy-based SDL renderer path
static int g_decode_sync; // --decode-sync: wait for each decode to finish on the GPU (experiment)
static int g_present_mode = D3DP_PRESENT_FLIP;
static int g_cli_monitors;    // --monitor given: don't use the saved monitor selection
static SDL_Thread *g_net_thread;
static SDL_atomic_t g_net_stop;
static SDL_atomic_t g_connected;
static SDL_atomic_t g_session_gen;
static SDL_atomic_t g_paused;  // disconnected on purpose: don't reconnect until the user asks
// An address discovery suggested for the active device (g_shared_lock). It's only a hint: the
// net thread tries it, and it's saved only once a handshake there proves the device's key.
static char g_hint_addr[64];
static int g_hint_port;
static SDL_atomic_t g_local_consent;  // pairing a new device: 0 asked, 1 user said yes, -1 no
static int g_gpu_lost;        // Present reported a removed/reset GPU: relaunch to rebuild everything
static HANDLE g_instance_mutex;
static int g_code_asking;              // the pairing window is waiting for that answer (main thread)
static Uint32 g_resume_mac_until;      // a drop while on the device: go back to it if we reconnect by then
static int g_want_mac_on_connect;
static Uint32 g_cmd_event, g_net_event;

// This PC's identity, and the device the current session talks to (copied for the net thread).
static SkKeyPair g_identity;
static Device g_session_dev;
static SkCipher g_cipher;      // send side guarded by g_send_lock; receive side net thread only

// Per-device runtime state (main thread).
typedef struct { int state; char status[160]; Uint32 last_seen; char hint_addr[64]; int hint_port; } DeviceRuntime;
static DeviceRuntime g_devrt[CFG_MAX_DEVICES];

// Every PC monitor (main thread).
typedef struct { char name[64]; int x, y, w, h, hz; int selected; } PCMonitor;
static PCMonitor g_pcmons[MAX_STREAMS];
static int g_npcmons;

// LAN discovery results (g_shared_lock).
static AppHost g_found[APP_MAX_HOSTS];
static int g_nfound;
static SDL_atomic_t g_searching;
static Uint32 g_last_search;

enum { CMD_INTERNAL_DISCOVERY_DONE = 91 };

// Events from the net thread to the main thread.
enum {
    NE_CONNECTING = 1,
    NE_HOST,          // handshake done: name, pub, platform, version
    NE_PAIRING,       // text = code
    NE_PAIRED,
    NE_DENIED,
    NE_BUSY,          // text = PC in control
    NE_CONNECTED,
    NE_DISCONNECTED,
    NE_BYE,           // arg = reason, text = detail
    NE_VERSION,       // text = the device's app version
    NE_KEY_CHANGED,
    NE_ADDR_PROVEN,   // text = address, arg = port: the hint worked
    NE_HINT_BAD,      // the hinted address wasn't the device
};
typedef struct {
    int gen, code, arg, platform;
    char text[160];
    char version[64];
    uint8_t pub[32];
} NetEvent;
static NetEvent g_last_host;  // the device the current session's handshake verified (main thread)
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
static int g_mac_mode = 1;
static int g_start_hidden;
static int g_pc_style_modifiers = 1;
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
static uint64_t get_u64(const uint8_t *p) { return get_u32(p) | (uint64_t)get_u32(p + 4) << 32; }

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

/// Sends one message through the encrypted channel.
static int send_msg(uint8_t type, uint8_t stream, const uint8_t *payload, uint32_t len) {
    size_t plain_len = 8 + (size_t)len;
    uint8_t *rec = malloc(4 + plain_len + SK_TAG);
    uint8_t *plain = malloc(plain_len);
    if (!rec || !plain) { free(rec); free(plain); return -1; }
    plain[0] = type;
    plain[1] = stream;
    put_u16(plain + 2, 0);
    put_u32(plain + 4, len);
    if (len) memcpy(plain + 8, payload, len);
    SDL_LockMutex(g_send_lock);
    int r = -1;
    if (g_sock != INVALID_SOCKET) {
        size_t n = sk_seal(&g_cipher, plain, plain_len, rec);
        r = send_all(g_sock, rec, (int)n);
    }
    SDL_UnlockMutex(g_send_lock);
    sk_wipe(plain, plain_len);
    free(plain);
    free(rec);
    return r;
}

static int net_should_stop(void);

/// connect() with a timeout, checking every 100 ms whether the session is stopping (switching
/// devices or quitting must not wait for an offline device).
static int connect_timeout(SOCKET s, const struct sockaddr *addr, int len, int timeout_ms) {
    u_long nb = 1;
    ioctlsocket(s, FIONBIO, &nb);
    int r = connect(s, addr, len);
    if (r != 0 && WSAGetLastError() == WSAEWOULDBLOCK) {
        r = -1;
        for (int waited = 0; waited < timeout_ms && !net_should_stop(); waited += 100) {
            fd_set wr, ex;
            FD_ZERO(&wr);
            FD_ZERO(&ex);
            FD_SET(s, &wr);
            FD_SET(s, &ex);
            struct timeval tv = {0, 100 * 1000};
            int n = select(0, NULL, &wr, &ex, &tv);
            if (n == 0) continue;
            r = n == 1 && FD_ISSET(s, &wr) ? 0 : -1;
            break;
        }
    }
    nb = 0;
    ioctlsocket(s, FIONBIO, &nb);
    return r;
}

static SOCKET connect_host(const char *host, int port_num) {
    char port[16];
    snprintf(port, sizeof port, "%d", port_num);
    struct addrinfo hints = {0}, *res = NULL;
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;
    if (getaddrinfo(host, port, &hints, &res) != 0) return INVALID_SOCKET;
    SOCKET s = INVALID_SOCKET;
    for (struct addrinfo *ai = res; ai; ai = ai->ai_next) {
        s = socket(ai->ai_family, ai->ai_socktype, ai->ai_protocol);
        if (s == INVALID_SOCKET) continue;
        if (connect_timeout(s, ai->ai_addr, (int)ai->ai_addrlen, 3000) == 0) break;
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
        c->get_format = d3dp_active() ? d3dp_get_format : pick_hw_format;
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
    if (s->has_new) perf_dropped((int)(s - g_streams), (uint64_t)s->ready->pts);
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
    s->pkt->pts = (int64_t)get_u64(payload);
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
        int64_t pts = s->frame->pts;
        if (g_decode_sync && d3dp_active() && s->frame->format == AV_PIX_FMT_D3D11) d3dp_gpu_wait();
        perf_mark(idx, (uint64_t)pts, ST_DECODED, perf_now_us());
        if (s->frame->format == AV_PIX_FMT_D3D11 && !d3dp_active()) {
            av_frame_unref(s->sw);
            if (av_hwframe_transfer_data(s->sw, s->frame, 0) < 0) {
                LOG("decoder: hw transfer failed");
                av_frame_unref(s->frame);
                continue;
            }
            av_frame_unref(s->frame);
            out = s->sw;
            out->pts = pts;
            perf_mark(idx, (uint64_t)pts, ST_XFER, perf_now_us());
        }
        handoff(s, out);
        s->frames++;
    }
    s->decoded_bytes += (int)len;
    Uint32 now = SDL_GetTicks();
    if (now - s->stats_start >= 5000) {
        double secs = (now - s->stats_start) / 1000.0;
        LOG("stream %d: %.1f fps, %.1f Mbps", idx, s->frames / secs, s->decoded_bytes * 8 / secs / 1e6);
        perf_log_stats(idx, logf_);
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

/// MONITORS: u8 nameLen, PC name, u8 n, then n × {u8 index, u8 selected, u32 w, h, hz, u8 len, name}
static int send_monitors(void) {
    uint8_t buf[2048];
    int o = 0;
    char pc[MAX_COMPUTERNAME_LENGTH + 1] = "PC";
    DWORD pclen = sizeof pc;
    GetComputerNameA(pc, &pclen);
    buf[o++] = (uint8_t)strlen(pc);
    memcpy(buf + o, pc, strlen(pc));
    o += (int)strlen(pc);
    buf[o++] = (uint8_t)g_npcmons;
    for (int i = 0; i < g_npcmons; i++) {
        buf[o++] = (uint8_t)i;
        buf[o++] = (uint8_t)g_pcmons[i].selected;
        put_u32(buf + o, (uint32_t)g_pcmons[i].w);
        put_u32(buf + o + 4, (uint32_t)g_pcmons[i].h);
        put_u32(buf + o + 8, (uint32_t)g_pcmons[i].hz);
        o += 12;
        size_t n = strlen(g_pcmons[i].name);
        buf[o++] = (uint8_t)n;
        memcpy(buf + o, g_pcmons[i].name, n);
        o += (int)n;
    }
    return send_msg(MSG_MONITORS, 0, buf, (uint32_t)o);
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
        SDL_Rect b = {0, 0, 0, 0};
        SDL_GetDisplayBounds(g_streams[i].monitor, &b);
        put_u32(ext + i * 8, (uint32_t)b.x);
        put_u32(ext + i * 8 + 4, (uint32_t)b.y);
    }
    return send_msg(MSG_HELLO, 0, buf, 8 + g_nstreams * 20);
}

static int g_trying_hint;  // net thread: the current attempt is at an unproven (discovered) address

static int net_should_stop(void) { return SDL_AtomicGet(&g_quit) || SDL_AtomicGet(&g_net_stop); }

/// Sleeps up to ms, waking early if the session is stopping.
static void net_sleep(int ms) {
    for (int t = 0; t < ms && !net_should_stop(); t += 50) SDL_Delay(50);
}

static void post_net(int gen, int code, int arg, const char *text, const SkHello *host) {
    NetEvent *e = calloc(1, sizeof *e);
    e->gen = gen;
    e->code = code;
    e->arg = arg;
    if (text) snprintf(e->text, sizeof e->text, "%s", text);
    if (host) {
        memcpy(e->pub, host->static_pub, 32);
        e->platform = host->platform;
        snprintf(e->version, sizeof e->version, "%s", host->version);
        if (!text) snprintf(e->text, sizeof e->text, "%s", host->name);
    }
    SDL_Event ev = {0};
    ev.type = g_net_event;
    ev.user.data1 = e;
    SDL_PushEvent(&ev);
}

static void pc_name(char *out, size_t n) {
    char name[MAX_COMPUTERNAME_LENGTH + 1] = "PC";
    DWORD len = sizeof name;
    GetComputerNameA(name, &len);
    snprintf(out, n, "%s", name);
}

/// Runs the handshake on a fresh socket. Returns 0 when the channel is up; on failure posts
/// the reason (and pauses for problems the user must fix).
static int client_handshake(SOCKET s, int gen) {
    SkKeyPair eph;
    sk_random(eph.priv, 32);
    sk_keypair_complete(&eph);
    SkHello ch = {0};
    ch.proto = SK_PROTO_VERSION;
    memcpy(ch.static_pub, g_identity.pub, 32);
    memcpy(ch.eph_pub, eph.pub, 32);
    pc_name(ch.name, sizeof ch.name);
    snprintf(ch.version, sizeof ch.version, "%s", SOFTKVM_VERSION);
    ch.platform = SK_PLATFORM_WINDOWS;
    uint8_t cbuf[SK_MAX_HELLO + 4], hbuf[SK_MAX_HELLO];
    size_t cl = sk_hello_encode(&ch, cbuf + 4, SK_MAX_HELLO);
    put_u32(cbuf, (uint32_t)cl);
    // v2: commit to our hello before seeing the device's, so neither side can grind the code.
    uint8_t commit[4 + 72];
    put_u32(commit, 72);
    sk_commit(cbuf + 4, cl, commit + 4);
    if (send_all(s, commit, sizeof commit) < 0) return -1;

    uint8_t lenb[4];
    if (recv_all(s, lenb, 4) < 0) return -1;
    uint32_t hl = get_u32(lenb);
    if (hl == 0 || hl > SK_MAX_HELLO || recv_all(s, hbuf, (int)hl) < 0) return -1;
    SkHello hh;
    if (sk_hello_decode(hbuf, hl, &hh) < 0) {
        LOG("net: the device didn't answer like SoftKVM");
        return -1;
    }
    if (hh.proto != SK_PROTO_VERSION || hh.status == SK_STATUS_VERSION_MISMATCH) {
        LOG("net: version mismatch: %s runs SoftKVM %s (protocol v%u), this PC runs %s (v%d)", hh.name, hh.version,
            hh.proto, SOFTKVM_VERSION, SK_PROTO_VERSION);
        if (g_trying_hint) {  // an unproven address: forget the hint, keep trying the known one
            post_net(gen, NE_HINT_BAD, 0, NULL, NULL);
            return -1;
        }
        SDL_AtomicSet(&g_paused, 1);
        post_net(gen, NE_VERSION, 0, NULL, &hh);
        return -1;
    }
    if (g_session_dev.has_key && memcmp(g_session_dev.pub, hh.static_pub, 32) != 0) {
        // Not the device we paired with (reinstalled, or something impersonating it).
        char want[17], got[17];
        sk_device_id(g_session_dev.pub, want);
        sk_device_id(hh.static_pub, got);
        LOG("net: the device at that address presented key %s, expected %s; refusing", got, want);
        if (g_trying_hint) {
            post_net(gen, NE_HINT_BAD, 0, NULL, NULL);
            return -1;
        }
        SDL_AtomicSet(&g_paused, 1);
        post_net(gen, NE_KEY_CHANGED, 0, NULL, &hh);
        return -1;
    }
    if (send_all(s, cbuf, (int)(cl + 4)) < 0) return -1;  // reveal the hello we committed to
    SDL_LockMutex(g_send_lock);
    sk_client_keys(&g_cipher, cbuf + 4, cl, hbuf, hl, &g_identity, &eph, &hh);
    SDL_UnlockMutex(g_send_lock);
    sk_wipe(&eph, sizeof eph);
    // Key confirmation (and, for a device we haven't paired with, ask it to show its pairing
    // prompt even if it already trusts this PC, so the user confirms the code on both screens).
    uint8_t flags = g_session_dev.paired ? 0 : 1;
    if (send_msg(MSG_CONFIRM, 0, &flags, 1) < 0) return -1;
    char id[17];
    sk_device_id(hh.static_pub, id);
    LOG("net: secure channel to %s (%s, SoftKVM %s)", hh.name, id, hh.version);
    if (g_trying_hint && g_session_dev.has_key) {  // the key matched: the hinted address is real
        char addr[64];
        int port;
        SDL_LockMutex(g_shared_lock);
        snprintf(addr, sizeof addr, "%s", g_hint_addr);
        port = g_hint_port;
        SDL_UnlockMutex(g_shared_lock);
        post_net(gen, NE_ADDR_PROVEN, port, addr, NULL);
    }
    post_net(gen, NE_HOST, 0, NULL, &hh);
    return 0;
}

static int net_thread(void *unused) {
    (void)unused;
    uint8_t *buf = NULL;
    uint32_t cap = 0;
    int gen = SDL_AtomicGet(&g_session_gen);
    while (!net_should_stop()) {
        if (SDL_AtomicGet(&g_paused)) {  // disconnected on purpose: wait for the user
            net_sleep(300);
            continue;
        }
        char host[64];
        int port;
        SDL_LockMutex(g_shared_lock);
        // Alternate between the last proven address and a discovery hint, if there is one.
        static int attempt;
        int use_hint = g_hint_addr[0] && (attempt++ % 2 == 1 || !g_session_dev.addr[0]);
        snprintf(host, sizeof host, "%s", use_hint ? g_hint_addr : g_session_dev.addr);
        port = use_hint ? g_hint_port : g_session_dev.port;
        g_trying_hint = use_hint;
        SDL_UnlockMutex(g_shared_lock);
        if (!host[0]) {
            net_sleep(500);
            continue;
        }
        post_net(gen, NE_CONNECTING, 0, NULL, NULL);
        LOG("net: connecting to %s (%s:%d)", g_session_dev.name, host, port);
        SOCKET s = connect_host(host, port);
        if (s == INVALID_SOCKET) {
            LOG("net: connect failed, retrying in 2s");
            post_net(gen, NE_DISCONNECTED, 0, NULL, NULL);
            net_sleep(2000);
            continue;
        }
        SDL_LockMutex(g_send_lock);
        g_sock = s;
        SDL_UnlockMutex(g_send_lock);
        int authorized = 0, host_ok = 0, got_paired = 0;
        int need_consent = !g_session_dev.paired;  // a new device: the user must confirm the code here too
        SDL_AtomicSet(&g_local_consent, 0);
        if (net_should_stop() || client_handshake(s, gen) < 0) goto disconnected;

        for (;;) {
            uint8_t hdr[4];
            if (recv_all(s, hdr, 4) < 0) break;
            int64_t hdr_us = perf_now_us();
            uint32_t rl = get_u32(hdr);
            if (rl < SK_TAG + 8 || rl > MAX_PAYLOAD + 8 + SK_TAG) {
                LOG("net: bad record length (%u)", rl);
                break;
            }
            if (rl + AV_INPUT_BUFFER_PADDING_SIZE > cap) {
                cap = rl + AV_INPUT_BUFFER_PADDING_SIZE;
                buf = realloc(buf, cap);
            }
            if (recv_all(s, buf, (int)rl) < 0) break;
            long pl = sk_open(&g_cipher, hdr, buf, rl);
            if (pl < 8) {
                LOG("net: record failed authentication; dropping the connection");
                break;
            }
            uint8_t type = buf[0], stream = buf[1];
            uint32_t len = get_u32(buf + 4);
            if (len > (uint32_t)pl - 8) break;
            int64_t done_us = perf_now_us();
            uint8_t *msg = buf + 8;  // decoders read past the end: keep the padding zeroed
            memset(msg + len, 0, AV_INPUT_BUFFER_PADDING_SIZE);  // fits: cap >= rl + padding

            if (type == MSG_AUTH_STATUS && len >= 1) {
                char text[160] = "";
                if (len >= 2) {
                    int n = msg[1] < len - 2 ? msg[1] : (int)len - 2;
                    if (n > (int)sizeof text - 1) n = sizeof text - 1;
                    memcpy(text, msg + 2, n);
                    text[n] = 0;
                }
                switch (msg[0]) {
                case AUTH_OK:
                    if (need_consent && !got_paired) {
                        // We asked a new device to show its pairing prompt and it skipped it:
                        // not a SoftKVM we can trust with the keyboard.
                        LOG("net: the device skipped pairing confirmation; refusing");
                        SDL_AtomicSet(&g_paused, 1);
                        post_net(gen, NE_KEY_CHANGED, 1, NULL, NULL);
                        goto disconnected;
                    }
                    host_ok = 1;
                    perf_connected();  // time sync keeps the link alive while we wait for consent
                    LOG(need_consent ? "net: approved on the device; waiting for confirmation here" : "net: authorized");
                    break;
                case AUTH_PAIRED:
                    LOG("net: paired");
                    got_paired = 1;
                    post_net(gen, NE_PAIRED, 0, NULL, NULL);
                    break;
                case AUTH_PAIRING_PENDING:
                    LOG("net: waiting for approval on the device (code %s)", g_cipher.code);
                    post_net(gen, NE_PAIRING, need_consent, g_cipher.code, NULL);
                    break;
                case AUTH_PAIRING_DENIED:
                    LOG("net: pairing was refused on the device");
                    SDL_AtomicSet(&g_paused, 1);
                    post_net(gen, NE_DENIED, 0, NULL, NULL);
                    goto disconnected;
                case AUTH_BUSY:
                    LOG("net: in use by %s", text);
                    post_net(gen, NE_BUSY, 0, text, NULL);
                    break;
                }
                continue;
            }
            if (type == MSG_BYE) {
                int reason = len >= 1 ? msg[0] : BYE_USER;
                char text[160] = "";
                if (len >= 2) {
                    int n = msg[1] < len - 2 ? msg[1] : (int)len - 2;
                    if (n > (int)sizeof text - 1) n = sizeof text - 1;
                    memcpy(text, msg + 2, n);
                    text[n] = 0;
                }
                LOG("net: the device said goodbye (reason %d %s)", reason, text);
                if (reason != BYE_HOST_QUITTING) SDL_AtomicSet(&g_paused, 1);
                post_net(gen, NE_BYE, reason, text, NULL);
                goto disconnected;
            }
            if (host_ok && !authorized) {
                int consent = need_consent ? SDL_AtomicGet(&g_local_consent) : 1;
                if (consent < 0) {
                    LOG("net: pairing cancelled on this PC");
                    SDL_AtomicSet(&g_paused, 1);
                    post_net(gen, NE_DENIED, 1, NULL, NULL);
                    goto disconnected;
                }
                if (consent > 0) {
                    authorized = 1;
                    if (send_hello() < 0 || send_monitors() < 0) goto disconnected;
                }
            }
            if (!authorized) continue;
            if (perf_handle_msg(type, stream, msg, len, done_us)) continue;  // latency instrumentation

            switch (type) {
            case MSG_HELLO_ACK:
                SDL_AtomicSet(&g_connected, 1);
                post_net(gen, NE_CONNECTED, 0, NULL, NULL);
                break;
            case MSG_STREAM_INFO:
                if (stream < g_nstreams && len >= 16) {
                    LOG("net: stream %d is %ux%u@%u codec %u", stream, get_u32(msg), get_u32(msg + 4),
                        get_u32(msg + 8), get_u32(msg + 12));
                    stream_open_decoder(&g_streams[stream], (int)get_u32(msg + 12));
                }
                break;
            case MSG_VIDEO:
                if (stream < g_nstreams && len >= 9)
                    perf_on_video(stream, get_u64(msg), len, msg[8] & 1, hdr_us, done_us);
                if (stream < g_nstreams) decode_video(stream, msg, len);
                break;
            case MSG_PING:
                perf_on_ping(msg, len);
                break;
            case MSG_AUDIO:
                play_audio(msg, len);
                break;
            case MSG_LAYOUT:
                handle_layout(msg, len);
                break;
            case MSG_CURSOR:
                handle_cursor_shape(msg, len);
                break;
            case MSG_CURSOR_POS:
                if (len >= 8) {
                    float x, y;
                    uint32_t ux = get_u32(msg), uy = get_u32(msg + 4);
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
            case MSG_SET_MONITORS:
                if (len >= 4) app_post_command(CMD_SET_MONITOR_MASK, (int)get_u32(msg));
                break;
            default:
                break;
            }
        }
    disconnected:
        LOG("net: disconnected");
        perf_disconnected();
        SDL_AtomicSet(&g_connected, 0);
        post_net(gen, NE_DISCONNECTED, 0, NULL, NULL);
        SDL_LockMutex(g_send_lock);
        closesocket(s);
        g_sock = INVALID_SOCKET;
        sk_wipe(&g_cipher, sizeof g_cipher);
        SDL_UnlockMutex(g_send_lock);
        for (int i = 0; i < g_nstreams; i++) stream_close_decoder(&g_streams[i]);
        if (g_audio) {
            SDL_PauseAudioDevice(g_audio, 1);
            SDL_ClearQueuedAudio(g_audio);
            g_audio_playing = 0;
        }
        net_sleep(1000);
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

/// Where the local cursor goes in a window of ow x oh pixels; 0 if it isn't on this stream.
static int cursor_dst(Stream *s, int ow, int oh, SDL_Rect *r) {
    if (!g_mac_mode || !g_cursor_valid || !g_cursor_px) return 0;
    int idx = (int)(s - g_streams);
    const MacDisplay *d = layout_for_stream(idx);
    if (!d || cursor_stream() != idx) return 0;
    double sx = ow / d->w, sy = oh / d->h;
    r->x = (int)((g_cx - d->x - g_cursor_hx) * sx);
    r->y = (int)((g_cy - d->y - g_cursor_hy) * sy);
    r->w = (int)(g_cursor_w * sx + 0.5);
    r->h = (int)(g_cursor_h * sy + 0.5);
    return 1;
}

/// Draws the Mac cursor locally so it moves with zero video latency.
static void draw_cursor(Stream *s) {
    int ow, oh;
    SDL_Rect r;
    SDL_GetRendererOutputSize(s->ren, &ow, &oh);
    if (!cursor_dst(s, ow, oh, &r)) return;
    if (s->cursor_tex_version != g_cursor_version || !s->cursor_tex) {
        if (s->cursor_tex) SDL_DestroyTexture(s->cursor_tex);
        s->cursor_tex = SDL_CreateTexture(s->ren, SDL_PIXELFORMAT_ABGR8888, SDL_TEXTUREACCESS_STATIC,
                                          g_cursor_w, g_cursor_h);
        if (!s->cursor_tex) return;
        SDL_UpdateTexture(s->cursor_tex, NULL, g_cursor_px, g_cursor_w * 4);
        SDL_SetTextureBlendMode(s->cursor_tex, SDL_BLENDMODE_BLEND);
        s->cursor_tex_version = g_cursor_version;
    }
    SDL_RenderCopy(s->ren, s->cursor_tex, NULL, &r);
}

static void mark_cursor_dirty(void) {
    int i = cursor_stream();
    if (i >= 0) g_streams[i].dirty = 1;
}

/// Zero-copy path: draws the decoder's D3D11 surface directly. If the swap chain still has a
/// frame queued, leaves the new one pending (newest wins) and retries on the next loop pass.
static void render_stream_d3d(Stream *s) {
    if (!d3dp_ready(s->d3d)) {
        s->dirty = 1;
        return;
    }
    int idx = (int)(s - g_streams);
    SDL_LockMutex(s->lock);
    int fresh = s->has_new;
    if (fresh) {
        av_frame_unref(s->cur);
        av_frame_move_ref(s->cur, s->ready);
    }
    s->has_new = 0;
    SDL_UnlockMutex(s->lock);
    uint64_t pts = (uint64_t)s->cur->pts;
    if (fresh) perf_mark(idx, pts, ST_FETCH, perf_now_us());

    int ow = 0, oh = 0;
    SDL_GetWindowSizeInPixels(s->win, &ow, &oh);
    SDL_Rect r;
    D3DPCursor c = {0};
    if (cursor_dst(s, ow, oh, &r)) {
        c.px = g_cursor_px;
        c.w = g_cursor_w;
        c.h = g_cursor_h;
        c.version = g_cursor_version;
        c.x = r.x, c.y = r.y, c.dw = r.w, c.dh = r.h;
    }
    if (fresh && s->cur->format != AV_PIX_FMT_D3D11) {
        static int warned;
        if (!warned++) LOG("render: decoder fell back to software frames; restart with --sdl-render");
    }
    d3dp_render(s->d3d, s->cur->buf[0] ? s->cur : NULL, &c);
    if (fresh) perf_mark(idx, pts, ST_UPLOADED, perf_now_us());
    static int snap_frames;
    if (g_perf_snapshot && idx == 0 && fresh && ++snap_frames > 150) {
        LOG("bench: snapshot %s: %s", g_perf_snapshot, d3dp_snapshot(s->d3d, g_perf_snapshot) == 0 ? "ok" : "failed");
        g_perf_snapshot = NULL;
    }
    if (d3dp_present(s->d3d) == -2) g_gpu_lost = 1;
    if (fresh) {
        perf_mark(idx, pts, ST_PRESENTED, perf_now_us());
        if (g_perf_gpu_wait) {
            d3dp_gpu_wait();
            perf_mark(idx, pts, ST_GPU_DONE, perf_now_us());
        }
    }
    s->dirty = 0;
    s->last_present = SDL_GetTicks();
}

static void render_stream(Stream *s) {
    if (s->d3d) {
        render_stream_d3d(s);
        return;
    }
    AVFrame *f = av_frame_alloc();
    SDL_LockMutex(s->lock);
    int fresh = s->has_new;
    if (fresh) av_frame_move_ref(f, s->ready);
    s->has_new = 0;
    SDL_UnlockMutex(s->lock);
    int idx = (int)(s - g_streams);
    uint64_t pts = (uint64_t)f->pts;
    if (fresh) perf_mark(idx, pts, ST_FETCH, perf_now_us());

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
            perf_mark(idx, pts, ST_UPLOADED, perf_now_us());
        }
    }
    av_frame_free(&f);

    SDL_SetRenderDrawColor(s->ren, 0, 0, 0, 255);
    SDL_RenderClear(s->ren);
    if (s->tex) SDL_RenderCopy(s->ren, s->tex, NULL, NULL);
    draw_cursor(s);
    SDL_RenderPresent(s->ren);
    if (fresh) {
        perf_mark(idx, pts, ST_PRESENTED, perf_now_us());
        if (g_perf_gpu_wait) {
            perf_gpu_wait_renderer(s->ren);
            perf_mark(idx, pts, ST_GPU_DONE, perf_now_us());
        }
    }
    s->dirty = 0;
    s->last_present = SDL_GetTicks();
}

static int create_window(Stream *s, int idx) {
    SDL_Rect b = {0, 0, 0, 0};
    SDL_GetDisplayBounds(s->monitor, &b);
    s->bounds = b;
    char title[64];
    snprintf(title, sizeof title, "SoftKVM %d", idx + 1);
    if (g_perf_bench_secs) {
        // Headless benchmark: a hidden window of the stream's size, never shown or focused.
        b.x = b.y = 0;
        b.w = s->w;
        b.h = s->h;
    }
    s->win = SDL_CreateWindow(title, b.x, b.y, b.w, b.h, SDL_WINDOW_BORDERLESS | SDL_WINDOW_ALLOW_HIGHDPI |
                              (g_perf_bench_secs ? SDL_WINDOW_HIDDEN : 0));
    if (!s->win) {
        LOG("window: %s", SDL_GetError());
        return -1;
    }
    if (!g_perf_bench_secs) SDL_SetWindowFullscreen(s->win, SDL_WINDOW_FULLSCREEN_DESKTOP);
    if (d3dp_active()) {
        SDL_SysWMinfo wm;
        SDL_VERSION(&wm.version);
        if (SDL_GetWindowWMInfo(s->win, &wm)) s->d3d = d3dp_window_create(wm.info.win.window);
        if (!s->d3d) {
            LOG("window %d: D3D11 presenter failed", idx);
            return -1;
        }
        s->cur = av_frame_alloc();
        LOG("window %d on monitor %d (%dx%d at %d,%d), zero-copy D3D11 presenter", idx, s->monitor, b.w, b.h, b.x,
            b.y);
        d3dp_ready(s->d3d);  // every present consumes one frame-latency slot
        d3dp_render(s->d3d, NULL, NULL);
        d3dp_present(s->d3d);
        return 0;
    }
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

static void raise_stream_window(SDL_Window *win) {
    SDL_SysWMinfo wm;
    SDL_VERSION(&wm.version);
    if (!SDL_GetWindowWMInfo(win, &wm)) return;

    // Moving through the topmost group puts this window above other desktop windows even
    // when reconnecting took long enough for Windows to restrict foreground activation.
    HWND hwnd = wm.info.win.window;
    UINT flags = SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE;
    if (!SetWindowPos(hwnd, HWND_TOPMOST, 0, 0, 0, 0, flags) ||
        !SetWindowPos(hwnd, HWND_NOTOPMOST, 0, 0, 0, 0, flags)) {
        LOG("window: could not raise stream (%lu)", GetLastError());
    }
}

static void set_mac_mode(int on) {
    if (g_perf_bench_secs) {  // benchmarks never show windows or grab input
        g_mac_mode = on;
        return;
    }
    g_mac_mode = on;
    if (SDL_AtomicGet(&g_connected)) {
        uint8_t viewing = on != 0;
        send_msg(MSG_VIEWING, 0, &viewing, 1);
    }
    if (on) {
        // Bring up every Mac window and focus the one on the monitor the PC cursor is on.
        // The Mac cursor jumps to the same spot, so switching feels continuous.
        int gx, gy, focus = 0;
        SDL_GetGlobalMouseState(&gx, &gy);
        for (int i = 0; i < g_nstreams; i++) {
            SDL_ShowWindow(g_streams[i].win);
            SDL_Rect b = {0, 0, 0, 0};
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
        for (int i = 0; i < g_nstreams; i++) raise_stream_window(g_streams[i].win);
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
            SDL_Rect b = {0, 0, 0, 0};
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

static void stop_session(int goodbye);
static void restart_session(void);

/// True if a monitor we're showing the device on went away or moved/resized.
static int streamed_monitors_changed(void) {
    for (int i = 0; i < g_nstreams; i++) {
        SDL_Rect b = {0, 0, 0, 0};
        if (g_streams[i].monitor >= SDL_GetNumVideoDisplays() || SDL_GetDisplayBounds(g_streams[i].monitor, &b) != 0)
            return 1;
        if (b.x != g_streams[i].bounds.x || b.y != g_streams[i].bounds.y || b.w != g_streams[i].bounds.w ||
            b.h != g_streams[i].bounds.h)
            return 1;
    }
    return 0;
}

/// The GPU was reset (driver crash/update): every D3D object is dead. Rebuilding all of them in
/// place is fiddly, so start a fresh copy (it reconnects, and the device keeps its displays).
static void relaunch_after_gpu_loss(void) {
    LOG("render: the GPU was reset; restarting SoftKVM");
    if (g_mac_mode) set_mac_mode(0);
    stop_session(GOODBYE_SWITCHING);  // the device keeps its displays for the new copy
    if (g_instance_mutex) {
        ReleaseMutex(g_instance_mutex);
        CloseHandle(g_instance_mutex);
        g_instance_mutex = NULL;
    }
    wchar_t exe[MAX_PATH];
    GetModuleFileNameW(NULL, exe, MAX_PATH);
    STARTUPINFOW si;
    memset(&si, 0, sizeof si);
    si.cb = sizeof si;
    PROCESS_INFORMATION pi;
    if (CreateProcessW(exe, NULL, NULL, NULL, FALSE, 0, NULL, NULL, &si, &pi)) {
        CloseHandle(pi.hThread);
        CloseHandle(pi.hProcess);
    }
    SDL_AtomicSet(&g_quit, 1);
}

/// Ctrl + left Alt + Q / Space / 1..9 by physical key. Right Alt is excluded: on many layouts it's
/// AltGr (= Ctrl+Alt to Windows), so AltGr+Q (@) or AltGr+7 ({) must keep typing characters.
static int is_local_hotkey(const SDL_Keysym *k) {
    if (!(k->mod & KMOD_CTRL) || !(k->mod & KMOD_LALT) || (k->mod & KMOD_RALT)) return 0;
    if (k->scancode == SDL_SCANCODE_Q || k->scancode == SDL_SCANCODE_SPACE) return 1;
    if (k->scancode >= SDL_SCANCODE_1 && k->scancode <= SDL_SCANCODE_9)
        return g_cfg.ndevices >= 2 && (int)(k->scancode - SDL_SCANCODE_1) < g_cfg.ndevices;
    return 0;
}

/// Peer-supplied names go into menus and config.ini: no control characters (a newline would
/// inject config lines), at most 40 characters.
static void sanitize_name(char *s) {
    for (char *p = s; *p; p++)
        if ((unsigned char)*p < 0x20 || *p == 0x7f) *p = ' ';
    if (strlen(s) > 40) s[40] = 0;
}

static void usage(void) {
    fprintf(stderr,
            "usage: softkvm [DEVICE_ADDRESS] [--port P] [--monitor N[:WxH@FPS]]... [--sw-decode]\n"
            "               [--start-hidden] [--mac-modifiers]\n"
            "  Devices, monitors and the keyboard style are normally chosen from the tray icon and saved.\n"
            "  --present flip|tearing|vsync  how frames reach the screen (default flip: newest frame\n"
            "                   at the next refresh; tearing: immediately / VRR)\n"
            "  --sdl-render     old SDL renderer path (copies every frame through the CPU)\n"
            "  Ctrl+Alt+Space switches between this PC and the active device; Ctrl+Alt+1..9 picks a device.\n");
}

// ---------------------------------------------------------------------------
// Devices, monitors and sessions (main thread)

/// Saves settings, except in benchmark runs (which use a throwaway device entry).
static void save_config(void) {
    if (!g_perf_bench_secs) config_save(&g_cfg);
}

static Device *active_dev(void) {
    return g_cfg.active >= 0 && g_cfg.active < g_cfg.ndevices ? &g_cfg.devices[g_cfg.active] : NULL;
}

static void set_dev_state(int i, int state, const char *status) {
    if (i < 0 || i >= g_cfg.ndevices) return;
    g_devrt[i].state = state;
    snprintf(g_devrt[i].status, sizeof g_devrt[i].status, "%s", status ? status : "");
}

static int find_device_by_pub(const uint8_t pub[32]) {
    for (int i = 0; i < g_cfg.ndevices; i++)
        if (g_cfg.devices[i].has_key && !memcmp(g_cfg.devices[i].pub, pub, 32)) return i;
    return -1;
}

/// Reads every PC monitor and marks which ones the active device uses.
static void refresh_pc_monitors(void) {
    Device *d = active_dev();
    const char *sel = d ? d->monitors : "";
    g_npcmons = 0;
    int any_selected = 0;
    for (int i = 0; i < SDL_GetNumVideoDisplays() && g_npcmons < MAX_STREAMS; i++) {
        PCMonitor *m = &g_pcmons[g_npcmons++];
        memset(m, 0, sizeof *m);
        snprintf(m->name, sizeof m->name, "%s", SDL_GetDisplayName(i) ? SDL_GetDisplayName(i) : "Monitor");
        SDL_Rect b = {0, 0, 0, 0};
        SDL_GetDisplayBounds(i, &b);
        m->x = b.x;
        m->y = b.y;
        SDL_DisplayMode dm;
        SDL_GetDesktopDisplayMode(i, &dm);
        m->w = dm.w;
        m->h = dm.h;
        m->hz = dm.refresh_rate;
        // The selection is a ';'-separated list of monitor names.
        char list[520], key[80];
        snprintf(list, sizeof list, ";%s;", sel);
        snprintf(key, sizeof key, ";%s;", m->name);
        m->selected = sel[0] == 0 || strstr(list, key) != NULL;
        any_selected |= m->selected;
    }
    if (!any_selected)
        for (int i = 0; i < g_npcmons; i++) g_pcmons[i].selected = 1;
}

static void save_monitor_selection(void) {
    Device *d = active_dev();
    if (!d) return;
    char list[512] = "";
    for (int i = 0; i < g_npcmons; i++) {
        if (!g_pcmons[i].selected) continue;
        if (list[0]) strncat(list, ";", sizeof list - strlen(list) - 1);
        strncat(list, g_pcmons[i].name, sizeof list - strlen(list) - 1);
    }
    snprintf(d->monitors, sizeof d->monitors, "%s", list);
    save_config();
}

static int add_stream(int monitor, int w, int h, int fps) {
    if (g_nstreams >= MAX_STREAMS) return -1;
    Stream *s = &g_streams[g_nstreams];
    memset(s, 0, sizeof *s);
    s->monitor = monitor;
    SDL_DisplayMode m;
    SDL_GetDesktopDisplayMode(monitor, &m);
    s->w = w > 0 ? w : m.w;
    s->h = h > 0 ? h : m.h;
    s->fps = fps > 0 ? fps : (m.refresh_rate > 0 && m.refresh_rate < 60 ? m.refresh_rate : 60);
    g_nstreams++;
    return 0;
}

static int parse_monitor_arg(const char *spec) {
    int idx = atoi(spec), w = 0, h = 0, fps = 0;
    if (idx < 0 || idx >= SDL_GetNumVideoDisplays()) {
        fprintf(stderr, "no monitor %d (have %d)\n", idx, SDL_GetNumVideoDisplays());
        return -1;
    }
    const char *colon = strchr(spec, ':');
    if (colon && sscanf(colon + 1, "%dx%d@%d", &w, &h, &fps) < 2) return -1;
    g_cli_monitors = 1;
    return add_stream(idx, w, h, fps);
}

static void reset_remote_state(void) {
    SDL_LockMutex(g_shared_lock);
    g_net_nlayout = 0;
    g_net_layout_version++;
    g_net_cursor_pos_pending = 0;
    SDL_UnlockMutex(g_shared_lock);
    g_cursor_valid = 0;
}

/// Starts talking to the active device (if any): windows for its monitors plus the net thread.
static void start_session(void) {
    Device *d = active_dev();
    if (!d) return;
    if (!g_cli_monitors) {
        g_nstreams = 0;
        for (int i = 0; i < g_npcmons; i++)
            if (g_pcmons[i].selected) add_stream(i, 0, 0, 0);
    }
    for (int i = 0; i < g_nstreams; i++) {
        Stream *s = &g_streams[i];
        s->lock = SDL_CreateMutex();
        s->ready = av_frame_alloc();
        s->frame = av_frame_alloc();
        s->sw = av_frame_alloc();
        s->pkt = av_packet_alloc();
        s->stats_start = SDL_GetTicks();
        create_window(s, i);
        if (!g_mac_mode) SDL_HideWindow(s->win);
        LOG("stream %d -> monitor %d (%s), requesting %dx%d@%d", i, s->monitor, SDL_GetDisplayName(s->monitor),
            s->w, s->h, s->fps);
    }
    SDL_LockMutex(g_shared_lock);
    g_session_dev = *d;
    snprintf(g_hint_addr, sizeof g_hint_addr, "%s", g_devrt[g_cfg.active].hint_addr);
    g_hint_port = g_devrt[g_cfg.active].hint_port;
    SDL_UnlockMutex(g_shared_lock);
    set_dev_state(g_cfg.active, DEV_CONNECTING, "");
    SDL_AtomicSet(&g_net_stop, 0);
    g_net_thread = SDL_CreateThread(net_thread, "net", NULL);
}

/// Stops the session. goodbye >= 0 tells the device why we're leaving (GOODBYE_*).
static void stop_session(int goodbye) {
    if (goodbye >= 0 && SDL_AtomicGet(&g_connected)) {
        uint8_t reason = (uint8_t)goodbye;
        send_msg(MSG_GOODBYE, 0, &reason, 1);
        // Half-close and let the device read the goodbye before we stop reading: closing with
        // video still inbound can reset the connection and lose it (displays would linger).
        SDL_LockMutex(g_send_lock);
        if (g_sock != INVALID_SOCKET) shutdown(g_sock, SD_SEND);
        SDL_UnlockMutex(g_send_lock);
        for (int t = 0; t < 500 && SDL_AtomicGet(&g_connected); t += 20) SDL_Delay(20);  // host closes → net thread sees EOF
    }
    SDL_AtomicIncRef(&g_session_gen);  // events from the old session are ignored
    SDL_AtomicSet(&g_net_stop, 1);
    SDL_LockMutex(g_send_lock);
    if (g_sock != INVALID_SOCKET) shutdown(g_sock, SD_BOTH);
    SDL_UnlockMutex(g_send_lock);
    if (g_net_thread) SDL_WaitThread(g_net_thread, NULL);
    g_net_thread = NULL;
    SDL_AtomicSet(&g_connected, 0);
    for (int i = 0; i < g_nstreams; i++) {
        Stream *s = &g_streams[i];
        stream_close_decoder(s);
        av_frame_free(&s->ready);
        av_frame_free(&s->frame);
        av_frame_free(&s->sw);
        av_packet_free(&s->pkt);
        if (s->tex) SDL_DestroyTexture(s->tex);
        if (s->cursor_tex) SDL_DestroyTexture(s->cursor_tex);
        if (s->ren) SDL_DestroyRenderer(s->ren);
        d3dp_window_destroy(s->d3d);
        av_frame_free(&s->cur);
        if (s->win) SDL_DestroyWindow(s->win);
        if (s->lock) SDL_DestroyMutex(s->lock);
        memset(s, 0, sizeof *s);
    }
    if (!g_cli_monitors) g_nstreams = 0;
    reset_remote_state();
    g_code_asking = 0;
    tray_hide_code();
}

/// Applies a new monitor selection: the device recreates its displays to match.
static void restart_session(void) {
    int was_mac = g_mac_mode;
    if (g_mac_mode) set_mac_mode(0);
    stop_session(-1);
    start_session();
    if (was_mac) g_want_mac_on_connect = 1;  // back to the device once it reconnects
}

static void switch_to_device(int n) {
    if (n < 0 || n >= g_cfg.ndevices) return;
    g_resume_mac_until = 0;
    if (g_mac_mode) set_mac_mode(0);
    stop_session(GOODBYE_SWITCHING);
    g_cfg.active = n;
    save_config();
    refresh_pc_monitors();
    SDL_AtomicSet(&g_paused, 0);
    g_want_mac_on_connect = 1;
    LOG("device: switching to %s", g_cfg.devices[n].name);
    start_session();
}

// ---------------------------------------------------------------------------
// Tray / app commands

static const wchar_t *RUN_KEY = L"Software\\Microsoft\\Windows\\CurrentVersion\\Run";

static int autostart_enabled(void) {
    return RegGetValueW(HKEY_CURRENT_USER, RUN_KEY, L"SoftKVM", RRF_RT_REG_SZ, NULL, NULL, NULL) == ERROR_SUCCESS;
}

static void set_autostart(int on) {
    if (on) {
        wchar_t exe[MAX_PATH], cmd[MAX_PATH + 32];
        GetModuleFileNameW(NULL, exe, MAX_PATH);
        _snwprintf(cmd, MAX_PATH + 32, L"\"%ls\" --start-hidden", exe);
        RegSetKeyValueW(HKEY_CURRENT_USER, RUN_KEY, L"SoftKVM", REG_SZ, cmd,
                        (DWORD)((wcslen(cmd) + 1) * sizeof(wchar_t)));
    } else {
        RegDeleteKeyValueW(HKEY_CURRENT_USER, RUN_KEY, L"SoftKVM");
    }
    LOG("autostart: %s", on ? "on" : "off");
}

static int ask_yes_no(const char *text) {
    wchar_t w[600];
    MultiByteToWideChar(CP_UTF8, 0, text, -1, w, 600);
    return MessageBoxW(NULL, w, L"SoftKVM", MB_YESNO | MB_ICONQUESTION | MB_SETFOREGROUND | MB_TOPMOST) == IDYES;
}

void app_release_input_now(void) {
    if (!g_mac_mode) return;
    LOG("power: suspending or shutting down; releasing input on the device");
    if (SDL_AtomicGet(&g_connected)) send_release_all();
    app_post_command(CMD_GO_TO_PC, 0);
}

void app_post_command(int cmd, int arg) {
    SDL_Event ev = {0};
    ev.type = g_cmd_event;
    ev.user.code = cmd;
    ev.user.data1 = (void *)(intptr_t)arg;
    SDL_PushEvent(&ev);
}

/// Devices found on the LAN that aren't added yet (g_shared_lock held by caller).
static int available_hosts(AppHost *out) {
    int n = 0;
    for (int i = 0; i < g_nfound && n < APP_MAX_HOSTS; i++)
        if (find_device_by_pub(g_found[i].pub) < 0) out[n++] = g_found[i];
    return n;
}

void app_snapshot(AppSnapshot *out) {
    memset(out, 0, sizeof *out);
    out->mac_mode = g_mac_mode;
    out->active = g_cfg.active;
    out->pc_style_modifiers = g_pc_style_modifiers;
    out->autostart = autostart_enabled();
    out->searching = SDL_AtomicGet(&g_searching);
    snprintf(out->version, sizeof out->version, "%s", SOFTKVM_VERSION);
    Uint32 now = SDL_GetTicks();
    out->ndevices = g_cfg.ndevices < APP_MAX_DEVICES ? g_cfg.ndevices : APP_MAX_DEVICES;
    for (int i = 0; i < out->ndevices; i++) {
        AppDevice *d = &out->devices[i];
        snprintf(d->name, sizeof d->name, "%s", g_cfg.devices[i].name);
        snprintf(d->addr, sizeof d->addr, "%s", g_cfg.devices[i].addr);
        d->platform = g_cfg.devices[i].platform;
        int seen = g_devrt[i].last_seen && now - g_devrt[i].last_seen < 45000;
        if (i == g_cfg.active) {
            d->state = g_devrt[i].state;
            if (d->state == DEV_CONNECTING && !seen) d->state = DEV_OFFLINE;
        } else {
            d->state = seen ? DEV_ONLINE : DEV_OFFLINE;
        }
        snprintf(d->status, sizeof d->status, "%s", g_devrt[i].status);
    }
    out->nmonitors = g_npcmons;
    for (int i = 0; i < g_npcmons && i < APP_MAX_MONITORS; i++) {
        snprintf(out->monitors[i].name, sizeof out->monitors[i].name, "%s", g_pcmons[i].name);
        out->monitors[i].w = g_pcmons[i].w;
        out->monitors[i].h = g_pcmons[i].h;
        out->monitors[i].hz = g_pcmons[i].hz;
        out->monitors[i].selected = g_pcmons[i].selected;
    }
    SDL_LockMutex(g_shared_lock);
    out->navailable = available_hosts(out->available);
    SDL_UnlockMutex(g_shared_lock);
}

static int discovery_thread(void *unused) {
    (void)unused;
    AppHost found[APP_MAX_HOSTS];
    int n = discovery_run(found, APP_MAX_HOSTS, 1500);
    SDL_LockMutex(g_shared_lock);
    memcpy(g_found, found, sizeof found);
    g_nfound = n;
    SDL_UnlockMutex(g_shared_lock);
    SDL_AtomicSet(&g_searching, 0);
    app_post_command(CMD_INTERNAL_DISCOVERY_DONE, 0);
    return 0;
}

static void start_discovery(void) {
    if (!SDL_AtomicCAS(&g_searching, 0, 1)) return;
    g_last_search = SDL_GetTicks();
    SDL_DetachThread(SDL_CreateThread(discovery_thread, "discovery", NULL));
}

static int add_device(const AppHost *h) {
    if (g_cfg.ndevices >= CFG_MAX_DEVICES) return -1;
    int i = g_cfg.ndevices++;
    Device *d = &g_cfg.devices[i];
    memset(d, 0, sizeof *d);
    snprintf(d->name, sizeof d->name, "%s", h->name);
    snprintf(d->addr, sizeof d->addr, "%s", h->addr);
    d->port = h->port;
    d->platform = h->platform;
    memcpy(d->pub, h->pub, 32);  // pin the key it announced; pairing confirms it on the device
    d->has_key = 1;
    memset(&g_devrt[i], 0, sizeof g_devrt[i]);
    g_devrt[i].last_seen = SDL_GetTicks();
    save_config();
    LOG("device: added %s (%s)", d->name, d->addr);
    return i;
}

/// Updates online status and addresses of known devices from discovery results.
static void merge_discovery(void) {
    AppHost found[APP_MAX_HOSTS];
    SDL_LockMutex(g_shared_lock);
    int n = g_nfound;
    memcpy(found, g_found, sizeof found);
    SDL_UnlockMutex(g_shared_lock);
    int changed = 0;
    for (int f = 0; f < n; f++) {
        int i = find_device_by_pub(found[f].pub);
        if (i < 0) {  // a device from 0.1 (no key yet) at the same address
            for (int k = 0; k < g_cfg.ndevices; k++)
                if (!g_cfg.devices[k].has_key && !strcmp(g_cfg.devices[k].addr, found[f].addr)) i = k;
        }
        if (i < 0) continue;
        Device *d = &g_cfg.devices[i];
        g_devrt[i].last_seen = SDL_GetTicks();
        if (strcmp(d->addr, found[f].addr) || d->port != found[f].port) {
            // Only a hint (anyone can answer discovery): tried, and saved once proven.
            if (strcmp(g_devrt[i].hint_addr, found[f].addr))
                LOG("device: %s may have moved %s -> %s; will try it", d->name, d->addr, found[f].addr);
            snprintf(g_devrt[i].hint_addr, sizeof g_devrt[i].hint_addr, "%s", found[f].addr);
            g_devrt[i].hint_port = found[f].port;
            if (i == g_cfg.active) {  // the net thread tries it on its next attempts
                SDL_LockMutex(g_shared_lock);
                snprintf(g_hint_addr, sizeof g_hint_addr, "%s", found[f].addr);
                g_hint_port = found[f].port;
                SDL_UnlockMutex(g_shared_lock);
            }
        }
    }
    if (changed) save_config();
    // First run: nothing added yet, so start pairing with the first device found.
    if (g_cfg.ndevices == 0) {
        AppHost avail[APP_MAX_HOSTS];
        SDL_LockMutex(g_shared_lock);
        int na = available_hosts(avail);
        SDL_UnlockMutex(g_shared_lock);
        if (na > 0 && avail[0].proto == SK_PROTO_VERSION) {
            int i = add_device(&avail[0]);
            if (i >= 0) switch_to_device(i);
        }
    }
}

static void update_tray(void) {
    AppSnapshot st;
    app_snapshot(&st);
    char tip[128];
    int connected = st.active >= 0 && st.devices[st.active].state == DEV_CONNECTED;
    if (st.active < 0) snprintf(tip, sizeof tip, "SoftKVM — no device yet");
    else if (connected)
        snprintf(tip, sizeof tip, "SoftKVM — %s (%s)", st.devices[st.active].name, st.mac_mode ? "in control" : "ready");
    else snprintf(tip, sizeof tip, "SoftKVM — %s: not connected", st.devices[st.active].name);
    tray_set_status(connected, tip);
    tray_set_device_hotkeys(g_cfg.ndevices >= 2 ? g_cfg.ndevices : 0);
}

static void offer_takeover(void) {
    Device *d = active_dev();
    if (!d) return;
    char q[400];
    snprintf(q, sizeof q, "%s is being controlled by %s.\n\nTake over?", d->name, g_devrt[g_cfg.active].status);
    if (ask_yes_no(q)) {
        LOG("device: taking over %s", d->name);
        send_msg(MSG_TAKEOVER, 0, NULL, 0);
        g_want_mac_on_connect = 1;
    }
}

/// What the hotkey / clicking the active device does, depending on its state.
static void use_active_device(void) {
    int i = g_cfg.active;
    if (i < 0) return;
    switch (g_devrt[i].state) {
    case DEV_CONNECTED:
        toggle_mode();
        break;
    case DEV_BUSY:
        offer_takeover();
        break;
    case DEV_PAUSED:
    case DEV_ERROR:
        LOG("device: reconnecting to %s", g_cfg.devices[i].name);
        SDL_AtomicSet(&g_paused, 0);
        set_dev_state(i, DEV_CONNECTING, "");
        g_want_mac_on_connect = 1;
        break;
    default:
        if (g_mac_mode) set_mac_mode(0);
        break;
    }
}

static void handle_command(int cmd, int arg) {
    switch (cmd) {
    case CMD_TOGGLE_MODE:
        g_resume_mac_until = 0;  // the user decides now
        if (g_mac_mode) set_mac_mode(0);
        else use_active_device();
        break;
    case CMD_SELECT_DEVICE:
        if (arg == g_cfg.active) use_active_device();
        else switch_to_device(arg);
        break;
    case CMD_RECONNECT:
        use_active_device();
        break;
    case CMD_LOCAL_CONSENT:
        g_code_asking = 0;
        SDL_AtomicSet(&g_local_consent, arg ? 1 : -1);
        break;
    case CMD_GO_TO_PC:
        if (g_mac_mode) set_mac_mode(0);
        break;
    case CMD_ADD_DEVICE: {
        AppHost avail[APP_MAX_HOSTS];
        SDL_LockMutex(g_shared_lock);
        int na = available_hosts(avail);
        SDL_UnlockMutex(g_shared_lock);
        if (arg < 0 || arg >= na) break;
        int i = add_device(&avail[arg]);
        if (i >= 0) switch_to_device(i);
        break;
    }
    case CMD_FORGET_DEVICE: {
        if (arg < 0 || arg >= g_cfg.ndevices) break;
        char q[300];
        snprintf(q, sizeof q, "Forget %s?\n\nYou'll have to pair it again (approve on the device) to use it.",
                 g_cfg.devices[arg].name);
        if (!ask_yes_no(q)) break;
        LOG("device: forgetting %s", g_cfg.devices[arg].name);
        int was_active = arg == g_cfg.active;
        if (was_active) {
            if (g_mac_mode) set_mac_mode(0);
            stop_session(GOODBYE_QUIT);
        }
        for (int k = arg; k < g_cfg.ndevices - 1; k++) {
            g_cfg.devices[k] = g_cfg.devices[k + 1];
            g_devrt[k] = g_devrt[k + 1];
        }
        g_cfg.ndevices--;
        if (was_active) g_cfg.active = g_cfg.ndevices ? 0 : -1;
        else if (arg < g_cfg.active) g_cfg.active--;
        save_config();
        if (was_active) {
            refresh_pc_monitors();
            g_want_mac_on_connect = 0;
            start_session();
        }
        break;
    }
    case CMD_TOGGLE_MONITOR: {
        if (arg < 0 || arg >= g_npcmons) break;
        int selected = 0;
        for (int i = 0; i < g_npcmons; i++) selected += g_pcmons[i].selected;
        if (g_pcmons[arg].selected && selected == 1) break;  // keep at least one
        g_pcmons[arg].selected = !g_pcmons[arg].selected;
        g_cli_monitors = 0;
        save_monitor_selection();
        restart_session();
        break;
    }
    case CMD_SET_MONITOR_MASK: {
        unsigned mask = (unsigned)arg;
        int any = 0, changed = 0;
        for (int i = 0; i < g_npcmons; i++) any |= (mask >> i) & 1;
        if (!any) break;
        for (int i = 0; i < g_npcmons; i++) {
            int on = (mask >> i) & 1;
            changed |= on != g_pcmons[i].selected;
            g_pcmons[i].selected = on;
        }
        if (!changed) break;
        LOG("monitors: the device asked for mask %u", mask);
        g_cli_monitors = 0;
        save_monitor_selection();
        restart_session();
        break;
    }
    case CMD_SET_MODIFIERS:
        g_pc_style_modifiers = arg != 0;
        g_cfg.pc_style_modifiers = g_pc_style_modifiers;
        save_config();
        if (g_mac_mode) send_release_all();
        break;
    case CMD_TOGGLE_AUTOSTART:
        set_autostart(!autostart_enabled());
        break;
    case CMD_OPEN_LOG: {
        char path[MAX_PATH];
        snprintf(path, sizeof path, "%ssoftkvm.log", exe_dir());
        ShellExecuteA(NULL, "open", path, NULL, NULL, SW_SHOWNORMAL);
        break;
    }
    case CMD_SEARCH:
        start_discovery();
        break;
    case CMD_QUIT:
        SDL_AtomicSet(&g_quit, 1);
        break;
    case CMD_INTERNAL_DISCOVERY_DONE:
        merge_discovery();
        break;
    default:
        break;
    }
    update_tray();
}

/// Reacts to what the net thread saw.
static void handle_net_event(NetEvent *e) {
    int i = g_cfg.active;
    Device *d = active_dev();
    if (e->gen != SDL_AtomicGet(&g_session_gen) || !d) return;
    char msg[300];
    switch (e->code) {
    case NE_CONNECTING:
        if (g_devrt[i].state != DEV_PAIRING && g_devrt[i].state != DEV_BUSY) set_dev_state(i, DEV_CONNECTING, "");
        break;
    case NE_HOST:
        g_devrt[i].last_seen = SDL_GetTicks();
        g_last_host = *e;
        break;
    case NE_PAIRING:
        set_dev_state(i, DEV_PAIRING, e->text);
        g_code_asking = e->arg;
        tray_show_code(d->name, e->text, e->arg);
        break;
    case NE_PAIRED:
        if (!g_code_asking) tray_hide_code();  // otherwise it stays until the user answers here
        snprintf(msg, sizeof msg, "%s approved this PC.", d->name);
        tray_notify("Paired", msg);
        break;
    case NE_DENIED:
        tray_hide_code();
        set_dev_state(i, DEV_PAUSED, e->arg ? "pairing cancelled" : "pairing refused on the device");
        if (!e->arg) {
            snprintf(msg, sizeof msg, "%s refused to pair with this PC.", d->name);
            tray_notify("Not paired", msg);
        }
        break;
    case NE_BUSY:
        if (!g_code_asking) tray_hide_code();
        set_dev_state(i, DEV_BUSY, e->text);
        if (g_want_mac_on_connect) offer_takeover();  // the user just asked for this device
        break;
    case NE_CONNECTED:
        tray_hide_code();
        set_dev_state(i, DEV_CONNECTED, "");
        g_devrt[i].last_seen = SDL_GetTicks();
        // Remember the device: the key its handshake proved, and its real name.
        memcpy(d->pub, g_last_host.pub, 32);
        d->has_key = 1;
        d->paired = 1;
        d->platform = g_last_host.platform;
        if (!strcmp(d->name, d->addr) && g_last_host.text[0]) {
            snprintf(d->name, sizeof d->name, "%s", g_last_host.text);
            sanitize_name(d->name);
        }
        SDL_LockMutex(g_shared_lock);
        memcpy(g_session_dev.pub, d->pub, 32);
        g_session_dev.has_key = 1;
        SDL_UnlockMutex(g_shared_lock);
        save_config();
        if (g_want_mac_on_connect || (g_resume_mac_until && SDL_GetTicks() < g_resume_mac_until)) {
            if (!g_want_mac_on_connect) LOG("mode: reconnected quickly; back to %s", d->name);
            g_want_mac_on_connect = 0;
            set_mac_mode(1);
        }
        g_resume_mac_until = 0;
        break;
    case NE_DISCONNECTED:
        if (!SDL_AtomicGet(&g_paused) && g_devrt[i].state != DEV_BUSY) set_dev_state(i, DEV_CONNECTING, "");
        if (g_mac_mode && !g_perf_bench_secs) {
            LOG("mode: lost %s; back to the PC", d->name);
            set_mac_mode(0);
            // A blip (Wi-Fi, host restart): if it's back within 15 s, return to it by itself.
            // Not after a deliberate goodbye (paused).
            if (!SDL_AtomicGet(&g_paused)) g_resume_mac_until = SDL_GetTicks() + 15000;
        }
        break;
    case NE_BYE:
        tray_hide_code();
        if (e->arg == BYE_TAKEN_OVER) {
            snprintf(msg, sizeof msg, "taken over by %s", e->text);
            set_dev_state(i, DEV_PAUSED, msg);
            snprintf(msg, sizeof msg, "%s is now controlled by %s.", d->name, e->text);
            tray_notify("Taken over", msg);
        } else if (e->arg == BYE_UNPAIRED) {
            set_dev_state(i, DEV_PAUSED, "this PC was removed on the device — click to pair again");
            snprintf(msg, sizeof msg, "%s forgot this PC. Click it in the tray to pair again.", d->name);
            tray_notify("Unpaired", msg);
        } else if (e->arg == BYE_USER) {
            set_dev_state(i, DEV_PAUSED, "disconnected from the device");
        } else {
            set_dev_state(i, DEV_CONNECTING, "");
        }
        break;
    case NE_ADDR_PROVEN:
        LOG("device: %s is now at %s", d->name, e->text);
        snprintf(d->addr, sizeof d->addr, "%s", e->text);
        d->port = e->arg;
        g_devrt[i].hint_addr[0] = 0;
        SDL_LockMutex(g_shared_lock);
        snprintf(g_session_dev.addr, sizeof g_session_dev.addr, "%s", d->addr);
        g_session_dev.port = d->port;
        g_hint_addr[0] = 0;
        SDL_UnlockMutex(g_shared_lock);
        save_config();
        break;
    case NE_HINT_BAD:
        LOG("device: the discovered address for %s wasn't it; ignoring it", d->name);
        g_devrt[i].hint_addr[0] = 0;
        SDL_LockMutex(g_shared_lock);
        g_hint_addr[0] = 0;
        SDL_UnlockMutex(g_shared_lock);
        break;
    case NE_VERSION:
        snprintf(msg, sizeof msg, "needs an update (device runs SoftKVM %s, this PC %s)", e->version, SOFTKVM_VERSION);
        set_dev_state(i, DEV_ERROR, msg);
        snprintf(msg, sizeof msg, "%s runs SoftKVM %s and this PC runs %s. Update both to the same version.", d->name,
                 e->version, SOFTKVM_VERSION);
        tray_notify("Version mismatch", msg);
        break;
    case NE_KEY_CHANGED:
        if (e->arg) {  // skipped the pairing prompt we asked for
            set_dev_state(i, DEV_ERROR, "didn't confirm pairing — not trusted");
            snprintf(msg, sizeof msg, "The device at %s answered without showing a pairing code. "
                     "SoftKVM didn't connect to it.", d->addr);
            tray_notify("Security warning", msg);
            break;
        }
        set_dev_state(i, DEV_ERROR, "its identity changed — forget it and add it again");
        snprintf(msg, sizeof msg,
                 "The device at %s isn't the %s this PC paired with (it may have been reinstalled). "
                 "Forget it and add it again if that's expected.", d->addr, d->name);
        tray_notify("Security warning", msg);
        break;
    }
    update_tray();
}

int main(int argc, char **argv) {
    char logpath[MAX_PATH];
    snprintf(logpath, sizeof logpath, "%ssoftkvm.log", exe_dir());
    // Rotate at 5 MB so the log doesn't grow forever (one previous file kept).
    WIN32_FILE_ATTRIBUTE_DATA fa;
    if (GetFileAttributesExA(logpath, GetFileExInfoStandard, &fa) && (fa.nFileSizeHigh || fa.nFileSizeLow > 5u << 20)) {
        char old[MAX_PATH];
        snprintf(old, sizeof old, "%s.1", logpath);
        MoveFileExA(logpath, old, MOVEFILE_REPLACE_EXISTING);
    }
    g_logfile = fopen(logpath, "a");

    // One instance per user: a second launch just exits (benchmarks run next to the real one).
    int bench_run = 0;
    for (int i = 1; i < argc; i++) bench_run |= !strcmp(argv[i], "--bench");
    if (!bench_run) g_instance_mutex = CreateMutexW(NULL, TRUE, L"Local\\SoftKVMClient");
    if (!bench_run && GetLastError() == ERROR_ALREADY_EXISTS) {
        LOG("another SoftKVM is already running; exiting");
        return 0;
    }

    SDL_SetHint(SDL_HINT_WINDOWS_DPI_AWARENESS, "permonitorv2");
    SDL_SetHint(SDL_HINT_VIDEO_MINIMIZE_ON_FOCUS_LOSS, "0");
    SDL_SetHint(SDL_HINT_RENDER_DRIVER, "direct3d11");
    SDL_SetHint(SDL_HINT_RENDER_SCALE_QUALITY, "linear");
    SDL_SetHint(SDL_HINT_MOUSE_FOCUS_CLICKTHROUGH, "1");
    SDL_SetHint(SDL_HINT_ALLOW_ALT_TAB_WHILE_GRABBED, "0");
    SDL_SetHint(SDL_HINT_MOUSE_RELATIVE_MODE_WARP, "0");
    SDL_SetHint(SDL_HINT_MOUSE_RELATIVE_SYSTEM_SCALE, "1");  // honor Windows pointer speed/acceleration
    for (int i = 1; i + 1 < argc; i++)
        if (!strcmp(argv[i], "--bench")) g_perf_bench_secs = atoi(argv[i + 1]);
    if (SDL_Init(SDL_INIT_VIDEO | SDL_INIT_EVENTS | (g_perf_bench_secs ? 0 : SDL_INIT_AUDIO)) != 0) {
        fprintf(stderr, "SDL_Init: %s\n", SDL_GetError());
        return 1;
    }
    SDL_SetYUVConversionMode(SDL_YUV_CONVERSION_BT709);

    identity_load(g_identity.priv);
    sk_keypair_complete(&g_identity);
    char my_id[17];
    sk_device_id(g_identity.pub, my_id);
    LOG("SoftKVM %s starting (protocol v%d, this PC is %s)", SOFTKVM_VERSION, SK_PROTO_VERSION, my_id);

    config_load(&g_cfg);
    g_pc_style_modifiers = g_cfg.pc_style_modifiers;
    int cli_port = 0;
    const char *cli_host = NULL;
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--port") && i + 1 < argc) cli_port = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--monitor") && i + 1 < argc) {
            if (parse_monitor_arg(argv[++i]) < 0) { usage(); return 1; }
        } else if (!strcmp(argv[i], "--sw-decode")) g_force_sw = 1;
        else if (!strcmp(argv[i], "--start-hidden")) g_start_hidden = 1;
        else if (!strcmp(argv[i], "--mac-modifiers")) g_pc_style_modifiers = 0;
        else if (!strcmp(argv[i], "--bench") && i + 1 < argc) i++;  // parsed above
        else if (!strcmp(argv[i], "--csv") && i + 1 < argc) g_perf_csv = argv[++i];
        else if (!strcmp(argv[i], "--sdl-render")) g_sdl_render = 1;
        else if (!strcmp(argv[i], "--gpu-wait")) g_perf_gpu_wait = 1;
        else if (!strcmp(argv[i], "--decode-sync")) g_decode_sync = 1;
        else if (!strcmp(argv[i], "--snapshot") && i + 1 < argc) g_perf_snapshot = argv[++i];
        else if (!strcmp(argv[i], "--present") && i + 1 < argc) {
            const char *m = argv[++i];
            g_present_mode = !strcmp(m, "tearing") ? D3DP_PRESENT_TEARING
                           : !strcmp(m, "vsync")   ? D3DP_PRESENT_VSYNC
                                                   : D3DP_PRESENT_FLIP;
        }
        else if (argv[i][0] != '-') cli_host = argv[i];
        else { usage(); return 1; }
    }
    if (cli_host) {  // connect to this address (pairing it if it's new)
        int found = -1;
        for (int i = 0; i < g_cfg.ndevices; i++)
            if (!strcmp(g_cfg.devices[i].addr, cli_host)) found = i;
        if (found < 0 && g_cfg.ndevices < CFG_MAX_DEVICES) {
            found = g_cfg.ndevices++;
            memset(&g_cfg.devices[found], 0, sizeof(Device));
            snprintf(g_cfg.devices[found].addr, sizeof g_cfg.devices[found].addr, "%s", cli_host);
            snprintf(g_cfg.devices[found].name, sizeof g_cfg.devices[found].name, "%s", cli_host);
            g_cfg.devices[found].port = DEFAULT_PORT;
        }
        if (found >= 0) {
            if (cli_port) g_cfg.devices[found].port = cli_port;
            g_cfg.active = found;
            save_config();  // a benchmark never touches saved settings (see save_config)
        }
    }
    refresh_pc_monitors();

    SDL_AudioSpec want = {0}, have;
    if (!g_perf_bench_secs) {
    want.freq = AUDIO_RATE;
    want.format = AUDIO_S16LSB;
    want.channels = 2;
    want.samples = 256;  // ~5 ms device buffer
    g_audio = SDL_OpenAudioDevice(NULL, 0, &want, &have, 0);
    if (g_audio) LOG("audio: output %d Hz, %d ch, %d-sample buffer", have.freq, have.channels, have.samples);
    else LOG("audio: no output device (%s)", SDL_GetError());
    }

    WSADATA wsa;
    WSAStartup(MAKEWORD(2, 2), &wsa);

    if (!g_force_sw && !g_sdl_render && d3dp_init(&g_hw_device, g_present_mode) == 0) {
        LOG("decoder: D3D11VA, zero-copy presenter (%s)",
            g_present_mode == D3DP_PRESENT_TEARING ? "tearing" : g_present_mode == D3DP_PRESENT_VSYNC ? "vsync" : "flip");
    } else if (!g_force_sw && av_hwdevice_ctx_create(&g_hw_device, AV_HWDEVICE_TYPE_D3D11VA, NULL, NULL, 0) < 0) {
        LOG("decoder: D3D11VA unavailable, using software decoding");
        g_hw_device = NULL;
    }

    g_frame_event = SDL_RegisterEvents(3);
    g_cmd_event = g_frame_event + 1;
    g_net_event = g_frame_event + 2;
    g_send_lock = SDL_CreateMutex();
    g_shared_lock = SDL_CreateMutex();
    SDL_StopTextInput();

    if (!g_perf_bench_secs) {
        if (tray_init() < 0) LOG("tray: could not create the notification icon");
        LOG(tray_hotkey_registered() ? "hotkey: Ctrl+Alt+Space registered"
                                     : "hotkey: Ctrl+Alt+Space is taken by another app; it only works in Mac mode");
    }

    // Start on the PC; switch to the device once connected (unless started hidden).
    // Bench: render into hidden windows, never show them or grab input; no tray, no discovery.
    g_mac_mode = g_perf_bench_secs ? 1 : 0;
    g_want_mac_on_connect = !g_start_hidden && !g_perf_bench_secs;
    perf_init(send_msg);
    start_session();
    if (g_perf_bench_secs) {
        LOG("bench: headless run for %d s", g_perf_bench_secs);
    } else {
        start_discovery();
        update_tray();
    }
    Uint32 bench_end = SDL_GetTicks() + (Uint32)g_perf_bench_secs * 1000;

    while (!SDL_AtomicGet(&g_quit)) {
        SDL_Event ev;
        int any_dirty = 0;
        for (int i = 0; i < g_nstreams; i++) any_dirty |= g_streams[i].dirty;
        int got = SDL_WaitEventTimeout(&ev, g_mac_mode && any_dirty ? 2 : 100);
        int check_focus = 0;
        if (got) do {
            if (ev.type == SDL_QUIT) {
                SDL_AtomicSet(&g_quit, 1);
            } else if (ev.type == g_cmd_event) {
                handle_command(ev.user.code, (int)(intptr_t)ev.user.data1);
            } else if (ev.type == g_net_event) {
                handle_net_event(ev.user.data1);
                free(ev.user.data1);
            } else if (ev.type == SDL_KEYDOWN && is_local_hotkey(&ev.key.keysym)) {
                // Local hotkeys are never forwarded. In Mac mode the keyboard grab swallows them
                // before Windows' hotkeys see them (toggle_mode debounces duplicates).
                SDL_Scancode sc = ev.key.keysym.scancode;
                if (sc == SDL_SCANCODE_Q) SDL_AtomicSet(&g_quit, 1);
                else if (sc == SDL_SCANCODE_SPACE) handle_command(CMD_TOGGLE_MODE, 0);
                else handle_command(CMD_SELECT_DEVICE, sc - SDL_SCANCODE_1);
            } else if (ev.type == SDL_WINDOWEVENT) {
                if (ev.window.event == SDL_WINDOWEVENT_EXPOSED && g_mac_mode) {
                    int i = stream_for_window(ev.window.windowID);
                    if (i >= 0) render_stream(&g_streams[i]);
                } else if (ev.window.event == SDL_WINDOWEVENT_FOCUS_GAINED && g_mac_mode) {
                    SDL_Window *w = SDL_GetWindowFromID(ev.window.windowID);
                    if (w && !g_perf_bench_secs) SDL_SetWindowKeyboardGrab(w, SDL_TRUE);
                    if (!SDL_GetRelativeMouseMode() && !g_perf_bench_secs) SDL_SetRelativeMouseMode(SDL_TRUE);
                } else if (ev.window.event == SDL_WINDOWEVENT_FOCUS_LOST) {
                    check_focus = 1;
                }
            } else if (ev.type == SDL_DISPLAYEVENT) {
                refresh_pc_monitors();  // monitor plugged/unplugged/rearranged
                if (streamed_monitors_changed()) {
                    LOG("monitors: a monitor in use changed; rebuilding the session");
                    if (!g_cli_monitors) restart_session();
                }
            } else if (g_mac_mode && !g_perf_bench_secs) {
                forward_input(&ev);
            }
        } while (SDL_PollEvent(&ev));
        sync_shared_state();
        if (g_gpu_lost && !g_perf_bench_secs) {
            relaunch_after_gpu_loss();
            break;
        }
        if (g_perf_bench_secs && layout_for_stream(0)) {
            // Bench: pin the local cursor to the middle of stream 0 so cursor drawing is
            // exercised (the host reports wherever the Mac's real cursor is).
            const MacDisplay *d = layout_for_stream(0);
            g_cx = d->x + d->w / 2;
            g_cy = d->y + d->h / 2;
            g_cursor_valid = 1;
        }
        if (!g_perf_bench_secs) flush_mouse_move();

        // Focus left our windows entirely (e.g. Ctrl+Alt+Del): lift keys held on the device.
        if (check_focus && g_mac_mode && stream_for_window(SDL_GetKeyboardFocus()
                ? SDL_GetWindowID(SDL_GetKeyboardFocus()) : 0) < 0) {
            LOG("focus lost; releasing keys on the device");
            send_release_all();
        }

        // Keep the device list's online status and addresses fresh.
        if (!g_mac_mode && SDL_GetTicks() - g_last_search > 15000) start_discovery();

        if (!g_mac_mode) continue;  // keep decoding in the background, draw when shown
        Uint32 now = SDL_GetTicks();
        if (g_perf_bench_secs && (Sint32)(now - bench_end) >= 0) {
            perf_log_cpu(logf_);
            SDL_AtomicSet(&g_quit, 1);
        }
        static int cpu_started;
        if (g_perf_bench_secs && !cpu_started && now + (Uint32)(g_perf_bench_secs - 3) * 1000 >= bench_end) {
            cpu_started = 1;  // measure CPU after the first 3 s (connect, display setup)
            perf_log_cpu(NULL);
        }
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
    if (g_mac_mode && !g_perf_bench_secs) send_release_all();
    stop_session(g_perf_bench_secs ? -1 : GOODBYE_QUIT);
    perf_finish();
    if (!g_perf_bench_secs) tray_shutdown();
    SDL_Quit();
    WSACleanup();
    return 0;
}
