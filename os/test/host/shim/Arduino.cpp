// test/host/shim/Arduino.cpp - the fake clock behind millis(). See Arduino.h.
#include "Arduino.h"

static uint32_t s_millis = 0;

extern "C" {

unsigned long millis(void) { return s_millis; }
unsigned long micros(void) { return (unsigned long)(uint32_t)(s_millis * 1000u); }
void delay(unsigned long ms) { s_millis += (uint32_t)ms; }
void yield(void) {}
void vk_host_set_millis(uint32_t ms) { s_millis = ms; }

}  // extern "C"
