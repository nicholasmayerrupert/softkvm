#pragma once
#include <stddef.h>
#include <stdint.h>

#define CFG_MAX_DEVICES 16

/// A machine this PC can control (a Mac today; Linux later).
typedef struct {
    char name[128];
    char addr[64];        // last known address (refreshed by LAN discovery)
    int port;
    int platform;         // SK_PLATFORM_*
    int has_key;          // pub is known: from pairing, or seen in discovery before pairing
    uint8_t pub[32];      // the device's identity key; connections must present it
    char monitors[512];   // ';'-separated PC monitor names to use for it; "" = all
} Device;

typedef struct {
    int pc_style_modifiers;
    int active;           // index into devices, or -1
    int ndevices;
    Device devices[CFG_MAX_DEVICES];
} Config;

/// Loads %APPDATA%\SoftKVM\config.ini (migrating the single-Mac format from 0.1).
void config_load(Config *c);
void config_save(const Config *c);
/// Directory holding the executable (log file lives there), with trailing backslash.
const char *exe_dir(void);

/// Cryptographically secure random bytes.
void sk_random(void *buf, size_t n);
/// This PC's private identity key, created on first use and stored with DPAPI
/// (%APPDATA%\SoftKVM\identity.dat, readable only by this Windows user).
void identity_load(uint8_t priv[32]);
