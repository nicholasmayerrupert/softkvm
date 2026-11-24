#pragma once
// Interface between the tray UI and the rest of the client. All functions run on the main thread.

#define APP_MAX_MONITORS 4
#define APP_MAX_HOSTS 8

typedef struct {
    char name[64];
    int w, h, hz;
    int selected;
} AppMonitor;

typedef struct {
    char name[128];
    char addr[64];
    int port;
} AppHost;

typedef struct {
    int connected;
    char host_name[128];  // the Mac's name once connected
    char host_addr[64];   // configured address ("" = pick automatically)
    int mac_mode;
    int pc_style_modifiers;
    int autostart;
    int searching;
    int paused;  // the Mac disconnected us on purpose; waiting for the user to reconnect
    int nmonitors;
    AppMonitor monitors[APP_MAX_MONITORS];
    int nhosts;
    AppHost hosts[APP_MAX_HOSTS];
} AppSnapshot;

enum {
    CMD_TOGGLE_MODE = 1,
    CMD_TOGGLE_MONITOR,  // arg = monitor index
    CMD_SELECT_HOST,     // arg = index into hosts
    CMD_SET_MODIFIERS,   // arg = 1 PC style, 0 Mac style
    CMD_TOGGLE_AUTOSTART,
    CMD_OPEN_LOG,
    CMD_SEARCH,
    CMD_QUIT,
    CMD_SET_MONITOR_MASK,  // arg = bitmask (from the Mac's menu)
    CMD_RECONNECT,
};

void app_snapshot(AppSnapshot *out);
/// Queues a command; it runs on the next main-loop iteration (never inside a window procedure).
void app_post_command(int cmd, int arg);
