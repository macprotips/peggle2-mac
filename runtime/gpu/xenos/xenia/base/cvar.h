// Stand-in for Xenia's cvar system: flags become plain variables with their defaults.
#pragma once
#include <cstdint>
#include <filesystem>
#define DEFINE_bool(name, value, description, category) namespace cvars { bool name = value; }
#define DECLARE_bool(name) namespace cvars { extern bool name; }
#define DEFINE_int32(name, value, description, category) namespace cvars { int32_t name = value; }
#define DECLARE_int32(name) namespace cvars { extern int32_t name; }
