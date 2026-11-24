// Settings in %APPDATA%\SoftKVM\config.ini as simple key=value lines.

#include "config.h"

#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define DEFAULT_PORT 47900

static void config_path(wchar_t *out, size_t n) {
    const wchar_t *appdata = _wgetenv(L"APPDATA");
    _snwprintf(out, n, L"%ls\\SoftKVM", appdata ? appdata : L".");
    CreateDirectoryW(out, NULL);
    wcsncat(out, L"\\config.ini", n - wcslen(out) - 1);
}

static void set_str(char *dst, size_t n, const char *v) {
    snprintf(dst, n, "%s", v);
}

void config_load(Config *c) {
    memset(c, 0, sizeof *c);
    c->port = DEFAULT_PORT;
    c->pc_style_modifiers = 1;
    wchar_t path[MAX_PATH];
    config_path(path, MAX_PATH);
    FILE *f = _wfopen(path, L"r");
    if (!f) return;
    char line[1024];
    while (fgets(line, sizeof line, f)) {
        line[strcspn(line, "\r\n")] = 0;
        char *eq = strchr(line, '=');
        if (!eq) continue;
        *eq = 0;
        const char *k = line, *v = eq + 1;
        if (!strcmp(k, "host")) set_str(c->host, sizeof c->host, v);
        else if (!strcmp(k, "host_name")) set_str(c->host_name, sizeof c->host_name, v);
        else if (!strcmp(k, "port")) c->port = atoi(v) > 0 ? atoi(v) : DEFAULT_PORT;
        else if (!strcmp(k, "monitors")) set_str(c->monitors, sizeof c->monitors, v);
        else if (!strcmp(k, "pc_style_modifiers")) c->pc_style_modifiers = atoi(v) != 0;
    }
    fclose(f);
}

void config_save(const Config *c) {
    wchar_t path[MAX_PATH];
    config_path(path, MAX_PATH);
    FILE *f = _wfopen(path, L"w");
    if (!f) return;
    fprintf(f, "host=%s\nhost_name=%s\nport=%d\nmonitors=%s\npc_style_modifiers=%d\n", c->host, c->host_name,
            c->port, c->monitors, c->pc_style_modifiers);
    fclose(f);
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
