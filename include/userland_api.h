#ifndef USERLAND_API_H
#define USERLAND_API_H

#include <stdint.h>

enum syscall_id {
    SYSCALL_GFX_CLEAR = 1,
    SYSCALL_GFX_RECT = 2,
    SYSCALL_GFX_TEXT = 3,
    SYSCALL_INPUT_MOUSE = 4,
    SYSCALL_INPUT_KEY = 5,
    SYSCALL_SLEEP = 6,
    SYSCALL_TIME_TICKS = 7
};

struct mouse_state {
    int x;
    int y;
    uint8_t buttons;
};

typedef void (*userland_entry_t)(void);

#endif
