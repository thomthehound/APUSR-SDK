#pragma once

// Linux-friendly precompiled header bridging the Windows-focused implementation.
// Provides portable standard library and DirectX Shader Compiler includes while
// avoiding dependencies on the Win32 SDK.

#include <algorithm>
#include <bitset>
#include <cassert>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <exception>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iosfwd>
#include <iostream>
#include <limits>
#include <locale>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <set>
#include <span>
#include <sstream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>
#include <type_traits>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

namespace fs = std::filesystem;

// DXC ships a Win32 compatibility shim (WinAdapter) that defines COM-style smart
// pointers and ABI types in a cross-platform way. Always include it before the
// public DXC headers so that downstream code can reuse the original Windows
// implementation with minimal edits.
#include <dxc/WinAdapter.h>
#include <dxc/dxcapi.h>
#include <dxc/dxcerrors.h>
#include "ffx_sc/d3d12shader_stub.hpp"

// The legacy implementation expected the D3DCompiler entry points to exist, but
// our Linux port never links against D3DCompiler_47. Guard the include with a
// feature test so the code paths are compiled out.
#ifdef FFX_SC_ENABLE_D3DCOMPILER
    #include <d3dcompiler.h>
#endif

// pthread is needed for std::thread on glibc.
#include <pthread.h>

