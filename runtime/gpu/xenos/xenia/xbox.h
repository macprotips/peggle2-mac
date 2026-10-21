#pragma once
// Stand-in: Xenia's xbox.h pulls in the base headers the vendored APU files rely on.
#include <cstdint>
#include "third_party/fmt/include/fmt/format.h"
#include "xenia/base/assert.h"
#include "xenia/base/math.h"
#include "xenia/base/memory.h"
#ifndef static_assert_size
#define static_assert_size(type, size) static_assert(sizeof(type) == (size), #type " has the wrong size")
#endif
