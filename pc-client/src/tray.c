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
    wchar_t w[256];
    utf8_to_wide(text, w, 256);
    AppendMenuW(m, MF_STRING | (checked ? MF_CHECKED : 0) | (enabled ? 0 : MF_GRAYED), id, w);
}

static void show_menu(void) {
    AppSnapshot st;
    app_snapshot(&st);
    HMENU m = CreatePopupMenu();
    char buf[256];

    if (st.connected)
        snprintf(buf, sizeof buf, "Connected to %s", st.host_name[0] ? st.host_name : "Mac");
    else if (st.host_addr[0])
        snprintf(buf, sizeof buf, "Connecting to %s…", st.host_addr);
    else
        snprintf(buf, sizeof buf, "Looking for a Mac…");
    add_item(m, 1, buf, 0, 0);
    AppendMenuW(m, MF_SEPARATOR, 0, NULL);

    add_item(m, MENU_ID(CMD_TOGGLE_MODE, 0),
             st.mac_mode ? "Switch to PC\tCtrl+Alt+Space" : "Switch to Mac\tCtrl+Alt+Space", 0, st.connected);

    // Monitors: each checked monitor shows one Mac display. At least one must stay on.
    HMENU mons = CreatePopupMenu();
    int selected = 0;
    for (int i = 0; i < st.nmonitors; i++) selected += st.monitors[i].selected;
    for (int i = 0; i < st.nmonitors; i++) {
        AppMonitor *mo = &st.monitors[i];
        snprintf(buf, sizeof buf, "%s — %d×%d", mo->name, mo->w, mo->h);
        add_item(mons, MENU_ID(CMD_TOGGLE_MONITOR, i), buf, mo->selected, !(mo->selected && selected == 1));
    }
    snprintf(buf, sizeof buf, "Mac displays (%d of %d monitors)", selected, st.nmonitors);
    wchar_t wbuf[256];
    utf8_to_wide(buf, wbuf, 256);
    AppendMenuW(m, MF_POPUP, (UINT_PTR)mons, wbuf);

    // Which Mac to connect to.
    HMENU macs = CreatePopupMenu();
    for (int i = 0; i < st.nhosts; i++) {
        snprintf(buf, sizeof buf, "%s (%s)", st.hosts[i].name, st.hosts[i].addr);
        add_item(macs, MENU_ID(CMD_SELECT_HOST, i), buf, !strcmp(st.hosts[i].addr, st.host_addr), 1);
    }
    if (st.nhosts == 0 && st.host_addr[0]) {
        snprintf(buf, sizeof buf, "%s (%s)", st.host_name[0] ? st.host_name : "Mac", st.host_addr);
        add_item(macs, 1, buf, 1, 0);
    }
    if (st.nhosts == 0 && !st.host_addr[0]) add_item(macs, 1, "No Macs found yet", 0, 0);
    AppendMenuW(macs, MF_SEPARATOR, 0, NULL);
    add_item(macs, MENU_ID(CMD_SEARCH, 0), st.searching ? "Searching…" : "Search again", 0, !st.searching);
    AppendMenuW(m, MF_POPUP, (UINT_PTR)macs, L"Mac");

    HMENU keys = CreatePopupMenu();
    add_item(keys, MENU_ID(CMD_SET_MODIFIERS, 1), "PC style: Ctrl acts as Cmd (Ctrl+C copies)", st.pc_style_modifiers, 1);
    add_item(keys, MENU_ID(CMD_SET_MODIFIERS, 0), "Mac style: Windows key acts as Cmd", !st.pc_style_modifiers, 1);
    AppendMenuW(m, MF_POPUP, (UINT_PTR)keys, L"Keyboard");

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
    Shell_NotifyIconW(NIM_DELETE, &g_nid);
    DestroyWindow(g_hwnd);
    g_hwnd = NULL;
}
