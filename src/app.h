#pragma once

#include <stdint.h>

void app_setup();

void app_addr( uint8_t value );
uint8_t app_get_addr();

void app_breathe( bool value );
bool app_get_breathe();

void app_name( uint8_t addr, const String &name );
const char *app_get_name( uint8_t addr );

const char *app_send( bool on );
const char *app_send_to( bool on, uint8_t addr );

// Non-blocking and safe from any task: remember the latest wanted state per switch.
void app_request( bool on, uint8_t addr );
// Call from loop(): sends at most one pending command (round robin), returns its change code or nullptr.
const char *app_handle();
