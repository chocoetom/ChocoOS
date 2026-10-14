#pragma once
#include <stdint.h>

namespace choco::serial {
void init();
void write_char(char c);
void write(const char *s);
} // namespace choco::serial
