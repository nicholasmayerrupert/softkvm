#pragma once
// Interface between the tray UI and the rest of the client. All functions run on the main thread.

#include <stdint.h>

#define APP_MAX_MONITORS 4
#define APP_MAX_DEVICES 16
#define APP_MAX_HOSTS 16

typedef struct {
    char name[64];
    int w, h, hz;
    int selected;
} AppMonitor;

/// A machine found on the LAN by discovery.
typedef struct {
    char name[128];
    char addr[64];
    int port;
    int proto;
    int platform;
    uint8_t pub[32];
} AppHost;

enum {
    DEV_OFFLINE = 0,
    DEV_ONLINE,
    DEV_CONNECTING,
    DEV_PAIRING,     // waiting for approval on the device; status holds the code
    DEV_BUSY,        // another PC is in control; status holds its name
    DEV_CONNECTED,
    DEV_PAUSED,      // disconnected on purpose; status says why
    DEV_ERROR,       // version mismatch, key changed, …; status says why
};

typedef struct {
    char name[128];
    char addr[64];
    int platform;
    int state;
    char status[160];
} AppDevice;

typedef struct {
    int mac_mode;        // showing/controlling the active device
    int active;          // index into devices, or -1
    int pc_style_modifiers;
    int autostart;
    int searching;
    int ndevices;
    AppDevice devices[APP_MAX_DEVICES];
    int nmonitors;
    AppMonitor monitors[APP_MAX_MONITORS];
    int navailable;      // found on the LAN but not added yet
    AppHost available[APP_MAX_HOSTS];
    char version[32];
} AppSnapshot;

enum {
    CMD_TOGGLE_MODE = 1,
    CMD_TOGGLE_MONITOR,   // arg = monitor index
    CMD_SELECT_DEVICE,    // arg = device index: connect/control it (Ctrl+Alt+1..9)
    CMD_SET_MODIFIERS,    // arg = 1 PC style, 0 Mac style
    CMD_TOGGLE_AUTOSTART,
    CMD_OPEN_LOG,
    CMD_SEARCH,
    CMD_QUIT,
    CMD_SET_MONITOR_MASK, // arg = bitmask (from the device's menu)
    CMD_RECONNECT,
    CMD_ADD_DEVICE,       // arg = index into available
    CMD_FORGET_DEVICE,    // arg = device index
    CMD_GO_TO_PC,         // leave Mac mode (e.g. the PC is suspending)
};

void app_snapshot(AppSnapshot *out);
/// The PC is about to suspend or shut down: lift any keys/buttons held on the device right now
/// (safe to call from a window procedure), then leave Mac mode.
void app_release_input_now(void);
/// Queues a command; it runs on the next main-loop iteration (never inside a window procedure).
void app_post_command(int cmd, int arg);
