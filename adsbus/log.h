#pragma once

#include <stdbool.h>
#include <stdint.h>

#define LOG(id, ...) log_write((log_module), (id), __VA_ARGS__)

void log_init(void);
void log_init2(void);
void log_cleanup(void);
bool log_reopen(const char *);
void log_write(char, const uint8_t *, const char *, ...)
		__attribute__ ((__format__ (__printf__, 3, 4)));
