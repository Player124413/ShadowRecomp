// Stands in for the forced precompiled header (ShadowRecomp/stdafx.h) of the real build:
// the Android sources rely on it for these.
#pragma once

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <string>
#include <string_view>
#include <vector>

#define FMT_HEADER_ONLY 1
#include <fmt/format.h>
