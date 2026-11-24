#pragma once

#include <SDL.h>
#include <stdint.h>

#define KEYMAP_NONE 0xFFFF

/// Maps a physical key to a macOS virtual keycode, or KEYMAP_NONE if unmapped.
/// With pc_style_modifiers, Ctrl sends Command and the Windows key sends Control.
uint16_t mac_keycode(SDL_Scancode sc, int pc_style_modifiers);
