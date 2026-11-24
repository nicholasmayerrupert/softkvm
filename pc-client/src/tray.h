#pragma once

/// Creates the hidden tray window, the notification-area icon and the global
/// Ctrl+Alt+Space hotkey. Messages are dispatched by SDL's event pump (same thread).
int tray_init(void);
/// Updates the icon (connected = colored) and its tooltip.
void tray_set_status(int connected, const char *tooltip);
void tray_shutdown(void);
/// 1 if Ctrl+Alt+Space was registered globally.
int tray_hotkey_registered(void);
/// Registers global Ctrl+Alt+1..n (n ≤ 9; 0 = none) for picking devices. They're only taken
/// from other apps when there's more than one device to pick from.
void tray_set_device_hotkeys(int n);
/// Balloon notification from the tray icon.
void tray_notify(const char *title, const char *text);
/// Shows (or replaces) a window with the pairing code to compare on the device.
/// With ask, the window has OK/Cancel and reports the answer as CMD_LOCAL_CONSENT.
void tray_show_code(const char *device, const char *code, int ask);
void tray_hide_code(void);
