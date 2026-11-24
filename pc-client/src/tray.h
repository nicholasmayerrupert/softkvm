#pragma once

/// Creates the hidden tray window, the notification-area icon and the global
/// Ctrl+Alt+Space hotkey. Messages are dispatched by SDL's event pump (same thread).
int tray_init(void);
/// Updates the icon (connected = colored) and its tooltip.
void tray_set_status(int connected, const char *tooltip);
void tray_shutdown(void);
/// 1 if Ctrl+Alt+Space was registered globally.
int tray_hotkey_registered(void);
