#pragma once
#include <cstddef>
#include <cstdint>
#include <cstdio>
struct MockConsole {
  template<typename... Args> void printf(const char*,Args...) {}
};
extern MockConsole Serial;
