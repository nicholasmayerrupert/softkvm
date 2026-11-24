#pragma once

typedef struct {
    char host[64];        // Mac address; "" = connect to the first Mac found on the LAN
    char host_name[128];  // its name, for display
    int port;
    char monitors[512];   // ';'-separated monitor names to use; "" = all
    int pc_style_modifiers;
} Config;

void config_load(Config *c);
void config_save(const Config *c);
/// Directory holding the executable (log file lives there), with trailing backslash.
const char *exe_dir(void);
