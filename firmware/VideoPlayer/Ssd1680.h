#pragma once

#include <stddef.h>
#include <stdint.h>

namespace panel {

constexpr unsigned Width = 250;
constexpr unsigned Height = 122;
constexpr unsigned RamSources = 128;
constexpr size_t PlaneBytes = Width * RamSources / 8;

using WaitHook = void (*)();

bool prepare(WaitHook onWait);
uint8_t* darkMask();
uint8_t* lightMask();
void clearMasks();
bool scan();
bool finish(bool healthy);

} // namespace panel
