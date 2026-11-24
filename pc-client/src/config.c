// Settings in %APPDATA%\SoftKVM\config.ini, identity in identity.dat (DPAPI).

#include "config.h"

#include <windows.h>
#include <bcrypt.h>
#include <wincrypt.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define DEFAULT_PORT 47900

static void app_dir(wchar_t *out, size_t n) {
    const wchar_t *appdata = _wgetenv(L"APPDATA");
    _snwprintf(out, n, L"%ls\\SoftKVM", appdata ? appdata : L".");
    CreateDirectoryW(out, NULL);
}

static void app_file(const wchar_t *name, wchar_t *out, size_t n) {
    app_dir(out, n);
    wcsncat(out, L"\\", n - wcslen(out) - 1);
    wcsncat(out, name, n - wcslen(out) - 1);
}

static void set_str(char *dst, size_t n, const char *v) { snprintf(dst, n, "%s", v); }

static void hex_encode(const uint8_t *p, size_t n, char *out) {
    for (size_t i = 0; i < n; i++) sprintf(out + i * 2, "%02x", p[i]);
}

static int hex_decode(const char *s, uint8_t *out, size_t n) {
    if (strlen(s) != n * 2) return -1;
    for (size_t i = 0; i < n; i++) {
        unsigned v;
        if (sscanf(s + i * 2, "%2x", &v) != 1) return -1;
        out[i] = (uint8_t)v;
    }
    return 0;
}

void config_load(Config *c) {
    memset(c, 0, sizeof *c);
    c->pc_style_modifiers = 1;
    c->active = -1;
    wchar_t path[MAX_PATH];
    app_file(L"config.ini", path, MAX_PATH);
    FILE *f = _wfopen(path, L"r");
    if (!f) return;
    char line[1024];
    Device legacy = {0};
    int has_legacy = 0;
    Device *d = NULL;
    while (fgets(line, sizeof line, f)) {
        line[strcspn(line, "\r\n")] = 0;
        if (!strcmp(line, "[device]")) {
            d = c->ndevices < CFG_MAX_DEVICES ? &c->devices[c->ndevices++] : NULL;
            if (d) memset(d, 0, sizeof *d), d->port = DEFAULT_PORT;
            continue;
        }
        char *eq = strchr(line, '=');
        if (!eq) continue;
        *eq = 0;
        const char *k = line, *v = eq + 1;
        if (d) {
            if (!strcmp(k, "name")) set_str(d->name, sizeof d->name, v);
            else if (!strcmp(k, "addr")) set_str(d->addr, sizeof d->addr, v);
            else if (!strcmp(k, "port")) d->port = atoi(v) > 0 ? atoi(v) : DEFAULT_PORT;
            else if (!strcmp(k, "platform")) d->platform = atoi(v);
            else if (!strcmp(k, "pub")) d->has_key = hex_decode(v, d->pub, 32) == 0;
            else if (!strcmp(k, "monitors")) set_str(d->monitors, sizeof d->monitors, v);
        } else {
            if (!strcmp(k, "pc_style_modifiers")) c->pc_style_modifiers = atoi(v) != 0;
            else if (!strcmp(k, "active")) c->active = atoi(v);
            // 0.1 format: a single Mac
            else if (!strcmp(k, "host") && v[0]) set_str(legacy.addr, sizeof legacy.addr, v), has_legacy = 1;
            else if (!strcmp(k, "host_name")) set_str(legacy.name, sizeof legacy.name, v);
            else if (!strcmp(k, "port")) legacy.port = atoi(v);
            else if (!strcmp(k, "monitors")) set_str(legacy.monitors, sizeof legacy.monitors, v);
        }
    }
    fclose(f);
    if (has_legacy && c->ndevices == 0) {
        if (!legacy.port) legacy.port = DEFAULT_PORT;
        legacy.platform = 2;
        if (!legacy.name[0]) set_str(legacy.name, sizeof legacy.name, legacy.addr);
        c->devices[c->ndevices++] = legacy;
        c->active = 0;
    }
    if (c->active >= c->ndevices) c->active = c->ndevices ? 0 : -1;
}

void config_save(const Config *c) {
    wchar_t path[MAX_PATH], tmp[MAX_PATH];
    app_file(L"config.ini", path, MAX_PATH);
    app_file(L"config.ini.tmp", tmp, MAX_PATH);
    FILE *f = _wfopen(tmp, L"w");
    if (!f) return;
    fprintf(f, "version=2\npc_style_modifiers=%d\nactive=%d\n", c->pc_style_modifiers, c->active);
    for (int i = 0; i < c->ndevices; i++) {
        const Device *d = &c->devices[i];
        char pub[65] = "";
        if (d->has_key) hex_encode(d->pub, 32, pub);
        fprintf(f, "\n[device]\nname=%s\naddr=%s\nport=%d\nplatform=%d\npub=%s\nmonitors=%s\n", d->name, d->addr,
                d->port, d->platform, pub, d->monitors);
    }
    fclose(f);
    MoveFileExW(tmp, path, MOVEFILE_REPLACE_EXISTING);  // atomic-ish: never leave a half-written file
}

const char *exe_dir(void) {
    static char dir[MAX_PATH];
    if (!dir[0]) {
        GetModuleFileNameA(NULL, dir, MAX_PATH);
        char *slash = strrchr(dir, '\\');
        if (slash) slash[1] = 0;
    }
    return dir;
}

void sk_random(void *buf, size_t n) {
    if (BCryptGenRandom(NULL, buf, (ULONG)n, BCRYPT_USE_SYSTEM_PREFERRED_RNG) != 0) abort();
}

void identity_load(uint8_t priv[32]) {
    wchar_t path[MAX_PATH];
    app_file(L"identity.dat", path, MAX_PATH);
    FILE *f = _wfopen(path, L"rb");
    if (f) {
        BYTE blob[1024];
        size_t n = fread(blob, 1, sizeof blob, f);
        fclose(f);
        DATA_BLOB in = {(DWORD)n, blob}, out = {0};
        if (CryptUnprotectData(&in, NULL, NULL, NULL, NULL, 0, &out) && out.cbData == 32) {
            memcpy(priv, out.pbData, 32);
            SecureZeroMemory(out.pbData, out.cbData);
            LocalFree(out.pbData);
            return;
        }
    }
    sk_random(priv, 32);
    DATA_BLOB in = {32, priv}, out = {0};
    if (CryptProtectData(&in, L"SoftKVM identity", NULL, NULL, NULL, 0, &out)) {
        f = _wfopen(path, L"wb");
        if (f) {
            fwrite(out.pbData, 1, out.cbData, f);
            fclose(f);
        }
        LocalFree(out.pbData);
    }
}
