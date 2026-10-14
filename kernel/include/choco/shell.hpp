#pragma once
#include <stddef.h>
#include <stdint.h>

namespace choco::shell {

void start_workers(); // spawn the demo counter threads (scheduler must be init)
[[noreturn]] void run(); // interactive loop: line editing, history, commands

} // namespace choco::shell
