#pragma once

void*
wrapper_create(const char *const symbol, void *function);

void
wrapper_set_cpp_demangler(void *function);

/* ANDROID_LOG_TAGS is set: the Android log is asked for its verbose and debug lines (wrapper.c) */
#include <stdbool.h>
bool
android_log_below_info_wanted(void);
