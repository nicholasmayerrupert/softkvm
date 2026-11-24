// Notification-area (system tray) icon and menu for the SoftKVM client.

#include "tray.h"

#include <windows.h>
#include <shellapi.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "app.h"

#define WM_TRAY (WM_APP + 1)
#define TRAY_UID 1
#define HOTKEY_ID 1
#define HOTKEY_DEVICE_BASE 11
// Menu ids: 1000 + cmd * 100 + arg
#define MENU_ID(cmd, arg) (1000 + (cmd) * 100 + (arg))

static HWND g_hwnd;
static UINT g_taskbar_created;
static NOTIFYICONDATAW g_nid;
static HICON g_icon_on, g_icon_off;
static int g_connected = -1;
static int g_hotkey_ok;

// ---------------------------------------------------------------------------
// Icon: two little monitors, drawn at runtime (no resource files needed).

static void fill(uint32_t *px, int size, int x0, int y0, int x1, int y1, uint32_t argb) {
    for (int y = y0; y < y1; y++)
        for (int x = x0; x < x1; x++)
            if (x >= 0 && y >= 0 && x < size && y < size) px[y * size + x] = argb;
}

static void monitor(uint32_t *px, int s, int x, int y, int w, int h, uint32_t frame, uint32_t screen) {
    int t = s >= 24 ? 2 : 1;  // bezel thickness
    fill(px, s, x, y, x + w, y + h, frame);
    fill(px, s, x + t, y + t, x + w - t, y + h - t, screen);
    // stand
    fill(px, s, x + w / 2 - t, y + h, x + w / 2 + t, y + h + t * 2, frame);
    fill(px, s, x + w / 2 - w / 4, y + h + t * 2, x + w / 2 + w / 4, y + h + t * 3, frame);
}

static HICON make_icon(int connected) {
    int s = GetSystemMetrics(SM_CXSMICON);
    if (s < 16) s = 16;
    BITMAPV5HEADER bi = {0};
    bi.bV5Size = sizeof bi;
    bi.bV5Width = s;
    bi.bV5Height = -s;  // top-down
    bi.bV5Planes = 1;
    bi.bV5BitCount = 32;
    bi.bV5Compression = BI_BITFIELDS;
    bi.bV5RedMask = 0x00FF0000;
    bi.bV5GreenMask = 0x0000FF00;
    bi.bV5BlueMask = 0x000000FF;
    bi.bV5AlphaMask = 0xFF000000;
    void *bits = NULL;
    HDC dc = GetDC(NULL);
    HBITMAP color = CreateDIBSection(dc, (BITMAPINFO *)&bi, DIB_RGB_COLORS, &bits, NULL, 0);
    ReleaseDC(NULL, dc);
    uint32_t *px = bits;
    memset(px, 0, (size_t)s * s * 4);

    uint32_t frame = connected ? 0xFF2563EB : 0xFF8A8A8A;   // blue when connected, gray otherwise
    uint32_t screen = connected ? 0xFFBFDBFE : 0xFFE5E5E5;
    int mw = s * 9 / 16, mh = s * 6 / 16;
    monitor(px, s, 0, s / 8, mw, mh, frame, screen);
    monitor(px, s, s - mw, s / 8 + s / 4, mw, mh, frame, screen);

    HBITMAP mask = CreateBitmap(s, s, 1, 1, NULL);
    ICONINFO ii = {TRUE, 0, 0, mask, color};
    HICON icon = CreateIconIndirect(&ii);
    DeleteObject(color);
    DeleteObject(mask);
    return icon;
}

// ---------------------------------------------------------------------------
// Menu

static void utf8_to_wide(const char *s, wchar_t *out, int n) {
    if (!MultiByteToWideChar(CP_UTF8, 0, s, -1, out, n)) out[0] = 0;
}

static void add_item(HMENU m, UINT id, const char *text, int checked, int enabled) {
    wchar_t w[300];
    utf8_to_wide(text, w, 300);
    AppendMenuW(m, MF_STRING | (checked ? MF_CHECKED : 0) | (enabled ? 0 : MF_GRAYED), id, w);
}

static void add_submenu(HMENU m, HMENU sub, const char *text) {
    wchar_t w[300];
    utf8_to_wide(text, w, 300);
    AppendMenuW(m, MF_POPUP, (UINT_PTR)sub, w);
}

static const char *device_state_text(const AppDevice *d, int is_active, int mac_mode, char *buf, size_t n) {
    switch (d->state) {
    case DEV_CONNECTED: return is_active && mac_mode ? "in control" : "connected";
    case DEV_ONLINE: return "online";
    case DEV_CONNECTING: return "connecting…";
    case DEV_PAIRING: snprintf(buf, n, "approve on the Mac — code %s", d->status); return buf;
    case DEV_BUSY: snprintf(buf, n, "in use by %s — click to take over", d->status); return buf;
    case DEV_PAUSED: snprintf(buf, n, "%s — click to reconnect", d->status); return buf;
    case DEV_ERROR: return d->status;
    default: return "offline";
    }
}

static void show_menu(void) {
    AppSnapshot st;
    app_snapshot(&st);
    HMENU m = CreatePopupMenu();
    char buf[300], sbuf[200];

    snprintf(buf, sizeof buf, "SoftKVM %s", st.version);
    add_item(m, 1, buf, 0, 0);
    AppendMenuW(m, MF_SEPARATOR, 0, NULL);

    // Devices: click to control. Ctrl+Alt+N jumps straight to device N.
    if (st.ndevices == 0) add_item(m, 1, "No devices yet — add one below", 0, 0);
    for (int i = 0; i < st.ndevices; i++) {
        AppDevice *d = &st.devices[i];
        const char *state = device_state_text(d, i == st.active, st.mac_mode, sbuf, sizeof sbuf);
        if (i < 9) snprintf(buf, sizeof buf, "%s — %s\tCtrl+Alt+%d", d->name, state, i + 1);
        else snprintf(buf, sizeof buf, "%s — %s", d->name, state);
        add_item(m, MENU_ID(CMD_SELECT_DEVICE, i), buf, i == st.active, d->state != DEV_OFFLINE || i == st.active);
    }
    AppendMenuW(m, MF_SEPARATOR, 0, NULL);

    int connected = st.active >= 0 && st.devices[st.active].state == DEV_CONNECTED;
    if (st.active >= 0) {
        snprintf(buf, sizeof buf, st.mac_mode ? "Switch to this PC\tCtrl+Alt+Space" : "Switch to %s\tCtrl+Alt+Space",
                 st.devices[st.active].name);
        add_item(m, MENU_ID(CMD_TOGGLE_MODE, 0), buf, 0, connected || st.mac_mode);

        // Which PC monitors show this device. At least one must stay on.
        HMENU mons = CreatePopupMenu();
        int selected = 0;
        for (int i = 0; i < st.nmonitors; i++) selected += st.monitors[i].selected;
        for (int i = 0; i < st.nmonitors; i++) {
            AppMonitor *mo = &st.monitors[i];
            snprintf(buf, sizeof buf, "%s — %d×%d", mo->name, mo->w, mo->h);
            add_item(mons, MENU_ID(CMD_TOGGLE_MONITOR, i), buf, mo->selected, !(mo->selected && selected == 1));
        }
        snprintf(buf, sizeof buf, "Monitors for %s (%d of %d)", st.devices[st.active].name, selected, st.nmonitors);
        add_submenu(m, mons, buf);
    }

    HMENU add = CreatePopupMenu();
    for (int i = 0; i < st.navailable; i++) {
        snprintf(buf, sizeof buf, "%s (%s)%s", st.available[i].name, st.available[i].addr,
                 st.available[i].proto != 1 ? " — different version" : "");
        add_item(add, MENU_ID(CMD_ADD_DEVICE, i), buf, 0, st.available[i].proto == 1);
    }
    if (st.navailable == 0) add_item(add, 1, st.searching ? "Searching…" : "No new devices found", 0, 0);
    AppendMenuW(add, MF_SEPARATOR, 0, NULL);
    add_item(add, MENU_ID(CMD_SEARCH, 0), "Search again", 0, !st.searching);
    add_submenu(m, add, "Add a device");

    if (st.ndevices > 0) {
        HMENU forget = CreatePopupMenu();
        for (int i = 0; i < st.ndevices; i++) {
            snprintf(buf, sizeof buf, "%s…", st.devices[i].name);
            add_item(forget, MENU_ID(CMD_FORGET_DEVICE, i), buf, 0, 1);
        }
        add_submenu(m, forget, "Forget a device");
    }

    HMENU keys = CreatePopupMenu();
    add_item(keys, MENU_ID(CMD_SET_MODIFIERS, 1), "PC style: Ctrl acts as Cmd (Ctrl+C copies)", st.pc_style_modifiers, 1);
    add_item(keys, MENU_ID(CMD_SET_MODIFIERS, 0), "Mac style: Windows key acts as Cmd", !st.pc_style_modifiers, 1);
    add_submenu(m, keys, "Keyboard");

    AppendMenuW(m, MF_SEPARATOR, 0, NULL);
    add_item(m, MENU_ID(CMD_TOGGLE_AUTOSTART, 0), "Start with Windows", st.autostart, 1);
    add_item(m, MENU_ID(CMD_OPEN_LOG, 0), "Open log", 0, 1);
    AppendMenuW(m, MF_SEPARATOR, 0, NULL);
    add_item(m, MENU_ID(CMD_QUIT, 0), "Quit SoftKVM", 0, 1);

    POINT pt;
    GetCursorPos(&pt);
    SetForegroundWindow(g_hwnd);  // required so the menu closes when clicking elsewhere
    UINT id = TrackPopupMenu(m, TPM_RETURNCMD | TPM_RIGHTBUTTON | TPM_BOTTOMALIGN, pt.x, pt.y, 0, g_hwnd, NULL);
    PostMessageW(g_hwnd, WM_NULL, 0, 0);
    DestroyMenu(m);  // destroys submenus too
    if (id >= 1000) app_post_command((id - 1000) / 100, (id - 1000) % 100);
}

// ---------------------------------------------------------------------------
// Notifications and the pairing-code window

void tray_notify(const char *title, const char *text) {
    if (!g_hwnd) return;
    NOTIFYICONDATAW n = g_nid;
    n.uFlags = NIF_INFO;
    utf8_to_wide(title, n.szInfoTitle, 64);
    utf8_to_wide(text, n.szInfo, 256);
    n.dwInfoFlags = NIIF_INFO;
    Shell_NotifyIconW(NIM_MODIFY, &n);
}

static const wchar_t *CODE_TITLE = L"SoftKVM — pairing";
static wchar_t g_code_text[600];
static volatile LONG g_code_box_gen;  // bumped when we close the box ourselves

static DWORD WINAPI code_box_thread(LPVOID param) {
    int ask = (int)(INT_PTR)param;
    LONG gen = g_code_box_gen;
    int r = MessageBoxW(NULL, g_code_text, CODE_TITLE,
                        (ask ? MB_OKCANCEL | MB_ICONQUESTION : MB_OK | MB_ICONINFORMATION) | MB_SETFOREGROUND |
                            MB_TOPMOST);
    if (ask && gen == g_code_box_gen) app_post_command(CMD_LOCAL_CONSENT, r == IDOK);
    return 0;
}

void tray_show_code(const char *device, const char *code, int ask) {
    tray_hide_code();
    char text[600];
    if (ask)
        snprintf(text, sizeof text,
                 "Pair with “%s”?\n\nCheck that it shows this same code:\n\n        %s\n\n"
                 "Click Allow on that device, then OK here. Cancel if the codes don't match.", device, code);
    else
        snprintf(text, sizeof text,
                 "Approve this PC on “%s”.\n\nCheck that it shows the same code:\n\n        %s\n\n"
                 "This closes by itself once you approve.", device, code);
    utf8_to_wide(text, g_code_text, 600);
    CloseHandle(CreateThread(NULL, 0, code_box_thread, (LPVOID)(INT_PTR)ask, 0, NULL));
}

void tray_hide_code(void) {
    InterlockedIncrement(&g_code_box_gen);  // a box we close ourselves doesn't count as an answer
    HWND box = FindWindowW(L"#32770", CODE_TITLE);
    if (box) PostMessageW(box, WM_CLOSE, 0, 0);
}

// ---------------------------------------------------------------------------

static void add_icon(void) {
    Shell_NotifyIconW(NIM_ADD, &g_nid);
    g_nid.uVersion = NOTIFYICON_VERSION_4;
    Shell_NotifyIconW(NIM_SETVERSION, &g_nid);
}

static LRESULT CALLBACK wndproc(HWND h, UINT msg, WPARAM wp, LPARAM lp) {
    if (msg == WM_TRAY) {
        UINT ev = LOWORD(lp);
        // NOTIFYICON_VERSION_4: left click = NIN_SELECT, right click = WM_CONTEXTMENU.
        if (ev == WM_CONTEXTMENU || ev == NIN_SELECT || ev == NIN_KEYSELECT) show_menu();
        return 0;
    }
    if (msg == WM_HOTKEY && wp == HOTKEY_ID) {
        app_post_command(CMD_TOGGLE_MODE, 0);
        return 0;
    }
    if (msg == WM_HOTKEY && wp >= HOTKEY_DEVICE_BASE && wp < HOTKEY_DEVICE_BASE + 9) {
        app_post_command(CMD_SELECT_DEVICE, (int)(wp - HOTKEY_DEVICE_BASE));
        return 0;
    }
    if ((msg == WM_POWERBROADCAST && wp == PBT_APMSUSPEND) || msg == WM_QUERYENDSESSION) {
        app_release_input_now();  // before the link dies with keys still held on the device
        return msg == WM_QUERYENDSESSION ? TRUE : 0;
    }
    if (msg == WM_CLOSE) {  // the installer/uninstaller asks us to quit
        app_post_command(CMD_QUIT, 0);
        return 0;
    }
    if (msg == g_taskbar_created && g_taskbar_created) {  // Explorer restarted
        add_icon();
        return 0;
    }
    return DefWindowProcW(h, msg, wp, lp);
}

int tray_init(void) {
    HINSTANCE inst = GetModuleHandleW(NULL);
    WNDCLASSEXW wc;
    memset(&wc, 0, sizeof wc);
    wc.cbSize = sizeof wc;
    wc.lpfnWndProc = wndproc;
    wc.hInstance = inst;
    wc.lpszClassName = L"SoftKVMTray";
    RegisterClassExW(&wc);
    g_hwnd = CreateWindowExW(WS_EX_TOOLWINDOW, wc.lpszClassName, L"SoftKVM", WS_POPUP, 0, 0, 0, 0, NULL, NULL,
                             inst, NULL);
    if (!g_hwnd) return -1;
    g_taskbar_created = RegisterWindowMessageW(L"TaskbarCreated");

    g_icon_on = make_icon(1);
    g_icon_off = make_icon(0);
    g_nid.cbSize = sizeof g_nid;
    g_nid.hWnd = g_hwnd;
    g_nid.uID = TRAY_UID;
    g_nid.uFlags = NIF_MESSAGE | NIF_ICON | NIF_TIP | NIF_SHOWTIP;
    g_nid.uCallbackMessage = WM_TRAY;
    g_nid.hIcon = g_icon_off;
    wcscpy(g_nid.szTip, L"SoftKVM");
    add_icon();

    g_hotkey_ok = RegisterHotKey(g_hwnd, HOTKEY_ID, MOD_CONTROL | MOD_ALT | MOD_NOREPEAT, VK_SPACE) != 0;
    for (int i = 0; i < 9; i++)  // Ctrl+Alt+1..9: jump to device N
        RegisterHotKey(g_hwnd, HOTKEY_DEVICE_BASE + i, MOD_CONTROL | MOD_ALT | MOD_NOREPEAT, '1' + i);
    return 0;
}

int tray_hotkey_registered(void) { return g_hotkey_ok; }

void tray_set_status(int connected, const char *tooltip) {
    if (!g_hwnd) return;
    g_connected = connected;
    g_nid.hIcon = connected ? g_icon_on : g_icon_off;
    wchar_t w[128];
    utf8_to_wide(tooltip, w, 128);
    wcsncpy(g_nid.szTip, w, 127);
    g_nid.szTip[127] = 0;
    g_nid.uFlags = NIF_ICON | NIF_TIP | NIF_SHOWTIP;
    Shell_NotifyIconW(NIM_MODIFY, &g_nid);
    g_nid.uFlags = NIF_MESSAGE | NIF_ICON | NIF_TIP | NIF_SHOWTIP;
}

void tray_shutdown(void) {
    if (!g_hwnd) return;
    UnregisterHotKey(g_hwnd, HOTKEY_ID);
    for (int i = 0; i < 9; i++) UnregisterHotKey(g_hwnd, HOTKEY_DEVICE_BASE + i);
    tray_hide_code();
    Shell_NotifyIconW(NIM_DELETE, &g_nid);
    DestroyWindow(g_hwnd);
    g_hwnd = NULL;
}
