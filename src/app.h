#pragma once

#include <stdint.h>

void app_setup();

void app_addr( uint8_t value );
uint8_t app_get_addr();

void app_breathe( bool value );
bool app_get_breathe();

const char *app_send( bool on );
const char *app_send_to( bool on, uint8_t addr );
