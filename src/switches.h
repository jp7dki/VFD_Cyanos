#pragma once
#include <stdint.h>

typedef enum { SW_A = 0, SW_B = 1, SW_C = 2 } SwitchId;

// Initialize switch inputs and start polling task
void switches_init();

// Register a callback invoked when a switch changes state.
// callback: void cb(SwitchId id, bool pressed) where pressed==true when button is pressed (LOW)
typedef void (*switch_cb_t)(SwitchId id, bool pressed);
void switches_register_callback(switch_cb_t cb);

// Query whether the given switch was detected pressed (held LOW) at the last
// debounced sample. Safe to call immediately after `switches_init()` to
// detect power-on held buttons.
bool switches_is_pressed(SwitchId id);

// Read the physical pin state immediately (with pull-up) to detect a button
// held at power-on. Returns true if pressed (LOW). Safe to call from setup().
bool switches_was_held_at_boot(SwitchId id);
