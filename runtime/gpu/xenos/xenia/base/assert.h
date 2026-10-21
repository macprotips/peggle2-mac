// Stand-in for Xenia's base/assert.h.
#pragma once
#include <cassert>
#define assert_always(...) assert(false)
#define assert_true(x, ...) assert(x)
#define assert_false(x, ...) assert(!(x))
#define assert_zero(x, ...) assert((x) == 0)
#define assert_not_zero(x, ...) assert((x) != 0)
#define assert_null(x, ...) assert((x) == nullptr)
#define assert_not_null(x, ...) assert((x) != nullptr)
#define assert_unhandled_case(x) assert(false)
#define xenia_assert assert
#define static_assert_size(type, size) static_assert(sizeof(type) == size, "bad definition for " #type ": must be " #size " bytes")
