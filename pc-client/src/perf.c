// Latency instrumentation, see perf.h.
#include "perf.h"

#define COBJMACROS
#include <windows.h>
#include <d3d11.h>
#include <SDL.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

int g_perf_bench_secs;
const char *g_perf_csv;

typedef struct {
    int stream;
    uint64_t pts;
    // host clock (µs)
    uint64_t cap, cb, submit, enc, queued;
    uint32_t pending, hflags;
    // client clock (µs)
    int64_t t[ST_COUNT];
    uint32_t bytes;
    int key, dropped, has_host;
} Rec;

typedef struct { int64_t t0, host, t1; } SyncSample;

static perf_send_fn g_send;
static SDL_mutex *g_lock;
static Rec *g_recs;
static int g_cap, g_n;  // g_n counts every record ever made; ring index = n % cap
static SyncSample *g_sync;
static int g_sync_cap, g_sync_n;
static SDL_atomic_t g_connected, g_host_has_clock;
static int64_t g_offset;  // host_us - client_us, from the best recent sample
static int g_offset_valid;
static int64_t g_qpf;

#define LAT_MAX 1024
typedef struct { int n; float e2e[LAT_MAX], net[LAT_MAX], dec[LAT_MAX], host[LAT_MAX]; } LatWin;
static LatWin g_lat[8];

int64_t perf_now_us(void) {
    LARGE_INTEGER c;
    QueryPerformanceCounter(&c);
    return (int64_t)(c.QuadPart / g_qpf) * 1000000 + (int64_t)(c.QuadPart % g_qpf) * 1000000 / g_qpf;
}

static int sync_thread(void *unused) {
    (void)unused;
    for (;;) {
        SDL_Delay(g_perf_bench_secs ? 100 : 1000);
        if (!SDL_AtomicGet(&g_connected) || !SDL_AtomicGet(&g_host_has_clock)) continue;
        uint8_t p[8];
        int64_t now = perf_now_us();
        memcpy(p, &now, 8);  // little-endian on Windows
        g_send(MSG_TIME_REQUEST, 0, p, 8);
    }
    return 0;
}

void perf_init(perf_send_fn send) {
    LARGE_INTEGER f;
    QueryPerformanceFrequency(&f);
    g_qpf = f.QuadPart;
    g_send = send;
    g_lock = SDL_CreateMutex();
    g_cap = g_perf_bench_secs ? 400000 : 1024;
    g_recs = calloc(g_cap, sizeof *g_recs);
    g_sync_cap = g_perf_bench_secs ? 100000 : 64;
    g_sync = calloc(g_sync_cap, sizeof *g_sync);
    SDL_DetachThread(SDL_CreateThread(sync_thread, "timesync", NULL));
}

void perf_connected(void) { SDL_AtomicSet(&g_connected, 1); }

void perf_disconnected(void) {
    SDL_AtomicSet(&g_connected, 0);
    SDL_AtomicSet(&g_host_has_clock, 0);
}

void perf_on_ping(const uint8_t *p, uint32_t len) {
    (void)p;
    if (len >= 8) SDL_AtomicSet(&g_host_has_clock, 1);
}

static uint64_t rd64(const uint8_t *p) { uint64_t v; memcpy(&v, p, 8); return v; }
static uint32_t rd32(const uint8_t *p) { uint32_t v; memcpy(&v, p, 4); return v; }

// Caller holds g_lock.
static Rec *find(int stream, uint64_t pts) {
    int lo = g_n - g_cap < 0 ? 0 : g_n - g_cap;
    int stop = g_n - 256 > lo ? g_n - 256 : lo;
    for (int i = g_n - 1; i >= stop; i--) {
        Rec *r = &g_recs[i % g_cap];
        if (r->stream == stream && r->pts == pts) return r;
    }
    return NULL;
}

static Rec *find_or_add(int stream, uint64_t pts) {
    Rec *r = find(stream, pts);
    if (r) return r;
    if (g_perf_bench_secs && g_n >= g_cap) return NULL;
    r = &g_recs[g_n % g_cap];
    memset(r, 0, sizeof *r);
    r->stream = stream;
    r->pts = pts;
    g_n++;
    return r;
}

static void update_offset(void) {
    // Lowest round trip among the recent samples: least queueing, so best symmetric estimate.
    int from = g_sync_n > 32 ? g_sync_n - 32 : 0;
    int64_t best_rtt = INT64_MAX;
    for (int i = from; i < g_sync_n; i++) {
        SyncSample *s = &g_sync[i % g_sync_cap];
        int64_t rtt = s->t1 - s->t0;
        if (rtt < best_rtt) {
            best_rtt = rtt;
            g_offset = s->host - (s->t0 + s->t1) / 2;
        }
    }
    g_offset_valid = best_rtt != INT64_MAX;
}

int perf_handle_msg(uint8_t type, uint8_t stream, const uint8_t *p, uint32_t len, int64_t recv_us) {
    if (type == MSG_TIME_RESPONSE) {
        if (len < 16) return 1;
        SDL_LockMutex(g_lock);
        if (!g_perf_bench_secs || g_sync_n < g_sync_cap) {
            SyncSample *s = &g_sync[g_sync_n % g_sync_cap];
            s->t0 = (int64_t)rd64(p);
            s->host = (int64_t)rd64(p + 8);
            s->t1 = recv_us;
            g_sync_n++;
            update_offset();
        }
        SDL_UnlockMutex(g_lock);
        return 1;
    }
    if (type == MSG_FRAME_TIMING) {
        if (len < 48) return 1;
        SDL_LockMutex(g_lock);
        Rec *r = find_or_add(stream, rd64(p));
        if (r) {
            r->cap = rd64(p + 8);
            r->cb = rd64(p + 16);
            r->enc = rd64(p + 24);
            r->queued = rd64(p + 32);
            r->pending = rd32(p + 40);
            r->hflags = rd32(p + 44);
            r->submit = len >= 56 ? rd64(p + 48) : r->cb;
            r->has_host = 1;
        }
        SDL_UnlockMutex(g_lock);
        return 1;
    }
    return 0;
}

void perf_on_video(int stream, uint64_t pts, uint32_t bytes, int keyframe, int64_t hdr_us, int64_t done_us) {
    SDL_LockMutex(g_lock);
    Rec *r = find_or_add(stream, pts);
    if (r) {
        r->bytes = bytes;
        r->key = keyframe;
        r->t[ST_RECV_HDR] = hdr_us;
        r->t[ST_RECV] = done_us;
    }
    SDL_UnlockMutex(g_lock);
}

void perf_mark(int stream, uint64_t pts, int stage, int64_t t) {
    SDL_LockMutex(g_lock);
    Rec *r = find(stream, pts);
    if (r) {
        r->t[stage] = t;
        if (stage == ST_PRESENTED && stream >= 0 && stream < 8) {
            LatWin *w = &g_lat[stream];
            if (w->n < LAT_MAX && r->has_host && g_offset_valid && r->cap && !(r->hflags & 1)) {
                int64_t cap_client = (int64_t)r->cap - g_offset;
                w->e2e[w->n] = (float)(t - cap_client) / 1000.f;
                w->net[w->n] = (float)(r->t[ST_RECV] - ((int64_t)r->queued - g_offset)) / 1000.f;
                w->host[w->n] = (float)((int64_t)r->queued - (int64_t)r->cap) / 1000.f;
                w->dec[w->n] = (float)(t - r->t[ST_RECV]) / 1000.f;
                w->n++;
            }
        }
    }
    SDL_UnlockMutex(g_lock);
}

void perf_dropped(int stream, uint64_t pts) {
    SDL_LockMutex(g_lock);
    Rec *r = find(stream, pts);
    if (r) r->dropped = 1;
    SDL_UnlockMutex(g_lock);
}

static int cmpf(const void *a, const void *b) {
    float x = *(const float *)a, y = *(const float *)b;
    return x < y ? -1 : x > y;
}

static float pct(float *v, int n, double q) {
    if (n == 0) return 0;
    qsort(v, n, sizeof *v, cmpf);
    int i = (int)(q * (n - 1) + 0.5);
    return v[i];
}

void perf_log_stats(int stream, void (*log)(const char *fmt, ...)) {
    if (stream < 0 || stream >= 8) return;
    SDL_LockMutex(g_lock);
    LatWin w = g_lat[stream];
    g_lat[stream].n = 0;
    SDL_UnlockMutex(g_lock);
    if (w.n == 0) return;
    log("stream %d latency ms (n=%d): capture->present p50 %.1f p95 %.1f p99 %.1f | host %.1f | net %.1f/%.1f | "
        "client %.1f/%.1f (p50/p95)",
        stream, w.n, pct(w.e2e, w.n, .5), pct(w.e2e, w.n, .95), pct(w.e2e, w.n, .99), pct(w.host, w.n, .5),
        pct(w.net, w.n, .5), pct(w.net, w.n, .95), pct(w.dec, w.n, .5), pct(w.dec, w.n, .95));
}

void perf_finish(void) {
    if (!g_perf_csv) return;
    char path[512];
    SDL_LockMutex(g_lock);
    snprintf(path, sizeof path, "%s-frames.csv", g_perf_csv);
    FILE *f = fopen(path, "w");
    if (f) {
        fprintf(f, "stream,pts,key,bytes,refresh,pending,cap,cb,submit,enc,queued,recv_hdr,recv,decoded,xfer,fetch,"
                   "uploaded,presented,gpu_done,dropped\n");
        int from = g_n > g_cap ? g_n - g_cap : 0;
        for (int i = from; i < g_n; i++) {
            Rec *r = &g_recs[i % g_cap];
            fprintf(f, "%d,%llu,%d,%u,%u,%u,%llu,%llu,%llu,%llu,%llu", r->stream, (unsigned long long)r->pts, r->key,
                    r->bytes, r->hflags & 1, r->pending, (unsigned long long)r->cap, (unsigned long long)r->cb, (unsigned long long)r->submit,
                    (unsigned long long)r->enc, (unsigned long long)r->queued);
            for (int s = 0; s < ST_COUNT; s++) fprintf(f, ",%lld", (long long)r->t[s]);
            fprintf(f, ",%d\n", r->dropped);
        }
        fclose(f);
    }
    snprintf(path, sizeof path, "%s-sync.csv", g_perf_csv);
    f = fopen(path, "w");
    if (f) {
        fprintf(f, "t0,host,t1\n");
        int from = g_sync_n > g_sync_cap ? g_sync_n - g_sync_cap : 0;
        for (int i = from; i < g_sync_n; i++) {
            SyncSample *s = &g_sync[i % g_sync_cap];
            fprintf(f, "%lld,%lld,%lld\n", (long long)s->t0, (long long)s->host, (long long)s->t1);
        }
        fclose(f);
    }
    SDL_UnlockMutex(g_lock);
}

void perf_gpu_wait_d3d11(void *device) {
    ID3D11Device *dev = device;
    if (!dev) return;
    ID3D11DeviceContext *ctx = NULL;
    ID3D11Device_GetImmediateContext(dev, &ctx);
    D3D11_QUERY_DESC qd = {D3D11_QUERY_EVENT, 0};
    ID3D11Query *q = NULL;
    if (SUCCEEDED(ID3D11Device_CreateQuery(dev, &qd, &q))) {
        ID3D11DeviceContext_End(ctx, (ID3D11Asynchronous *)q);
        BOOL done = FALSE;
        while (ID3D11DeviceContext_GetData(ctx, (ID3D11Asynchronous *)q, &done, sizeof done, 0) == S_FALSE) YieldProcessor();
        ID3D11Query_Release(q);
    }
    ID3D11DeviceContext_Release(ctx);
}

void perf_gpu_wait_renderer(SDL_Renderer *ren) {
    ID3D11Device *dev = SDL_RenderGetD3D11Device(ren);
    if (!dev) return;
    perf_gpu_wait_d3d11(dev);
    ID3D11Device_Release(dev);
}
