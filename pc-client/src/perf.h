// Latency instrumentation: host/client clock sync, per-frame stage timestamps, live stats,
// and CSV dumps for benchmarks. See tools/perf_analyze.py.
#pragma once
#include <stdint.h>

enum {
    MSG_FRAME_TIMING = 0x20,  // host -> client, see mac-host/.../Timing.swift
    MSG_TIME_RESPONSE = 0x21,
    MSG_TIME_REQUEST = 0xA0,  // client -> host
};

// Client-side stages of one video frame, in pipeline order.
enum {
    ST_RECV_HDR,   // the VIDEO header was read off the socket
    ST_RECV,       // the whole payload was read
    ST_DECODED,    // avcodec_receive_frame returned it
    ST_XFER,       // GPU->CPU copy done (copy path only)
    ST_FETCH,      // render thread picked it up
    ST_UPLOADED,   // texture upload / GPU copy issued
    ST_PRESENTED,  // Present returned
    ST_GPU_DONE,   // GPU finished the frame (bench mode only; forces a CPU/GPU sync)
    ST_COUNT
};

typedef int (*perf_send_fn)(uint8_t type, uint8_t stream, const uint8_t *payload, uint32_t len);

extern int g_perf_bench_secs;     // --bench N: headless run for N seconds, then exit
extern const char *g_perf_csv;
extern const char *g_perf_snapshot;  // --snapshot FILE.bmp: bench only, one frame of window 0
extern int g_perf_gpu_wait;      // --gpu-wait: bench only, sync with the GPU after each frame (adds stalls)    // --csv PREFIX: write PREFIX-frames.csv and PREFIX-sync.csv

int64_t perf_now_us(void);
void perf_init(perf_send_fn send);
void perf_connected(void);
void perf_disconnected(void);

// Net thread. Returns 1 if the message was a timing message (consumed).
int perf_handle_msg(uint8_t type, uint8_t stream, const uint8_t *p, uint32_t len, int64_t recv_us);
void perf_on_ping(const uint8_t *p, uint32_t len);
void perf_on_video(int stream, uint64_t pts, uint32_t bytes, int keyframe, int64_t hdr_us, int64_t done_us);

// Any thread.
void perf_mark(int stream, uint64_t pts, int stage, int64_t t);
void perf_dropped(int stream, uint64_t pts);  // replaced before it was drawn

// Logs a one-line latency summary for the stream (call every few seconds).
void perf_log_stats(int stream, void (*log)(const char *fmt, ...));

// Bench mode: blocks until the GPU has finished all work submitted on this device.
void perf_gpu_wait_d3d11(void *id3d11device);
struct SDL_Renderer;
void perf_gpu_wait_renderer(struct SDL_Renderer *ren);

// Logs process CPU use since the previous call (first call just starts the clock).
void perf_log_cpu(void (*log)(const char *fmt, ...));

// Writes the CSVs (bench mode). Safe to call once at exit.
void perf_finish(void);
