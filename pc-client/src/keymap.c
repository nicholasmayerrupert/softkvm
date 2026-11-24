// SDL scancode (physical key position) -> macOS virtual keycode (kVK_*).
// Using physical positions means the Mac's own keyboard layout decides what gets typed.

#include "keymap.h"

#define MAC_COMMAND 0x37
#define MAC_RIGHT_COMMAND 0x36
#define MAC_CONTROL 0x3B
#define MAC_RIGHT_CONTROL 0x3E

uint16_t mac_keycode(SDL_Scancode sc, int pc_style_modifiers) {
    switch (sc) {
    // letters
    case SDL_SCANCODE_A: return 0x00;
    case SDL_SCANCODE_S: return 0x01;
    case SDL_SCANCODE_D: return 0x02;
    case SDL_SCANCODE_F: return 0x03;
    case SDL_SCANCODE_H: return 0x04;
    case SDL_SCANCODE_G: return 0x05;
    case SDL_SCANCODE_Z: return 0x06;
    case SDL_SCANCODE_X: return 0x07;
    case SDL_SCANCODE_C: return 0x08;
    case SDL_SCANCODE_V: return 0x09;
    case SDL_SCANCODE_B: return 0x0B;
    case SDL_SCANCODE_Q: return 0x0C;
    case SDL_SCANCODE_W: return 0x0D;
    case SDL_SCANCODE_E: return 0x0E;
    case SDL_SCANCODE_R: return 0x0F;
    case SDL_SCANCODE_Y: return 0x10;
    case SDL_SCANCODE_T: return 0x11;
    case SDL_SCANCODE_O: return 0x1F;
    case SDL_SCANCODE_U: return 0x20;
    case SDL_SCANCODE_I: return 0x22;
    case SDL_SCANCODE_P: return 0x23;
    case SDL_SCANCODE_L: return 0x25;
    case SDL_SCANCODE_J: return 0x26;
    case SDL_SCANCODE_K: return 0x28;
    case SDL_SCANCODE_N: return 0x2D;
    case SDL_SCANCODE_M: return 0x2E;
    // number row
    case SDL_SCANCODE_1: return 0x12;
    case SDL_SCANCODE_2: return 0x13;
    case SDL_SCANCODE_3: return 0x14;
    case SDL_SCANCODE_4: return 0x15;
    case SDL_SCANCODE_6: return 0x16;
    case SDL_SCANCODE_5: return 0x17;
    case SDL_SCANCODE_EQUALS: return 0x18;
    case SDL_SCANCODE_9: return 0x19;
    case SDL_SCANCODE_7: return 0x1A;
    case SDL_SCANCODE_MINUS: return 0x1B;
    case SDL_SCANCODE_8: return 0x1C;
    case SDL_SCANCODE_0: return 0x1D;
    // punctuation
    case SDL_SCANCODE_RIGHTBRACKET: return 0x1E;
    case SDL_SCANCODE_LEFTBRACKET: return 0x21;
    case SDL_SCANCODE_APOSTROPHE: return 0x27;
    case SDL_SCANCODE_SEMICOLON: return 0x29;
    case SDL_SCANCODE_BACKSLASH: return 0x2A;
    case SDL_SCANCODE_NONUSHASH: return 0x2A;
    case SDL_SCANCODE_COMMA: return 0x2B;
    case SDL_SCANCODE_SLASH: return 0x2C;
    case SDL_SCANCODE_PERIOD: return 0x2F;
    case SDL_SCANCODE_GRAVE: return 0x32;
    case SDL_SCANCODE_NONUSBACKSLASH: return 0x0A;
    // editing / whitespace
    case SDL_SCANCODE_RETURN: return 0x24;
    case SDL_SCANCODE_TAB: return 0x30;
    case SDL_SCANCODE_SPACE: return 0x31;
    case SDL_SCANCODE_BACKSPACE: return 0x33;
    case SDL_SCANCODE_ESCAPE: return 0x35;
    case SDL_SCANCODE_CAPSLOCK: return 0x39;
    case SDL_SCANCODE_DELETE: return 0x75;
    case SDL_SCANCODE_INSERT: return 0x72;  // Help
    case SDL_SCANCODE_HOME: return 0x73;
    case SDL_SCANCODE_END: return 0x77;
    case SDL_SCANCODE_PAGEUP: return 0x74;
    case SDL_SCANCODE_PAGEDOWN: return 0x79;
    case SDL_SCANCODE_LEFT: return 0x7B;
    case SDL_SCANCODE_RIGHT: return 0x7C;
    case SDL_SCANCODE_DOWN: return 0x7D;
    case SDL_SCANCODE_UP: return 0x7E;
    case SDL_SCANCODE_APPLICATION: return 0x6E;  // context menu
    // function keys
    case SDL_SCANCODE_F1: return 0x7A;
    case SDL_SCANCODE_F2: return 0x78;
    case SDL_SCANCODE_F3: return 0x63;
    case SDL_SCANCODE_F4: return 0x76;
    case SDL_SCANCODE_F5: return 0x60;
    case SDL_SCANCODE_F6: return 0x61;
    case SDL_SCANCODE_F7: return 0x62;
    case SDL_SCANCODE_F8: return 0x64;
    case SDL_SCANCODE_F9: return 0x65;
    case SDL_SCANCODE_F10: return 0x6D;
    case SDL_SCANCODE_F11: return 0x67;
    case SDL_SCANCODE_F12: return 0x6F;
    case SDL_SCANCODE_PRINTSCREEN: return 0x69;  // F13
    case SDL_SCANCODE_SCROLLLOCK: return 0x6B;   // F14
    case SDL_SCANCODE_PAUSE: return 0x71;        // F15
    // keypad
    case SDL_SCANCODE_NUMLOCKCLEAR: return 0x47;
    case SDL_SCANCODE_KP_DIVIDE: return 0x4B;
    case SDL_SCANCODE_KP_MULTIPLY: return 0x43;
    case SDL_SCANCODE_KP_MINUS: return 0x4E;
    case SDL_SCANCODE_KP_PLUS: return 0x45;
    case SDL_SCANCODE_KP_ENTER: return 0x4C;
    case SDL_SCANCODE_KP_EQUALS: return 0x51;
    case SDL_SCANCODE_KP_PERIOD: return 0x41;
    case SDL_SCANCODE_KP_0: return 0x52;
    case SDL_SCANCODE_KP_1: return 0x53;
    case SDL_SCANCODE_KP_2: return 0x54;
    case SDL_SCANCODE_KP_3: return 0x55;
    case SDL_SCANCODE_KP_4: return 0x56;
    case SDL_SCANCODE_KP_5: return 0x57;
    case SDL_SCANCODE_KP_6: return 0x58;
    case SDL_SCANCODE_KP_7: return 0x59;
    case SDL_SCANCODE_KP_8: return 0x5B;
    case SDL_SCANCODE_KP_9: return 0x5C;
    // modifiers. "PC style": Ctrl acts as Cmd (Ctrl+C copies) and the Windows key as Control.
    case SDL_SCANCODE_LSHIFT: return 0x38;
    case SDL_SCANCODE_RSHIFT: return 0x3C;
    case SDL_SCANCODE_LALT: return 0x3A;   // Option
    case SDL_SCANCODE_RALT: return 0x3D;
    case SDL_SCANCODE_LCTRL: return pc_style_modifiers ? MAC_COMMAND : MAC_CONTROL;
    case SDL_SCANCODE_RCTRL: return pc_style_modifiers ? MAC_RIGHT_COMMAND : MAC_RIGHT_CONTROL;
    case SDL_SCANCODE_LGUI: return pc_style_modifiers ? MAC_CONTROL : MAC_COMMAND;
    case SDL_SCANCODE_RGUI: return pc_style_modifiers ? MAC_RIGHT_CONTROL : MAC_RIGHT_COMMAND;
    default: return KEYMAP_NONE;
    }
}
