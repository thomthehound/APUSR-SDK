#pragma once

// =============================================================================================
// PIX stub shim
// ---------------------------------------------------------------------------------------------
// Cross-platform builds rarely have Microsoft's PIX headers installed.  Rather than force every
// contributor to wrangle that dependency we expose a tiny shim that mirrors the PIX macro
// surface.  The real runtime dynamically resolves the PIX entry points at execution time, so the
// compile-time hooks can degrade to no-ops without affecting correctness.  Keeping the macro
// signatures intact means the backend continues to compile regardless of whether the developer has
// PIX Graphics Tools installed locally.
// =============================================================================================

#ifndef PIX_COLOR
#define PIX_COLOR(...) 0u
#endif

#ifndef PIXBeginEvent
#define PIXBeginEvent(...) do { } while (false)
#endif

#ifndef PIXEndEvent
#define PIXEndEvent(...) do { } while (false)
#endif

#ifndef PIXScopedEvent
#define PIXScopedEvent(...) do { } while (false)
#endif

#if !defined(PIX_STUB_WARNING_EMITTED)
#define PIX_STUB_WARNING_EMITTED 1

// We emit a build-time diagnostic once per translation unit so developers immediately
// understand why PIX markers vanished.  The stub keeps pipelines hermetic, but the
// experience stays transparent and nudges power users towards installing PIX locally.
#if defined(_MSC_VER)
#pragma message("PIX WinPixEventRuntime headers not found; FidelityFX GPU markers compiled as no-ops. Install Microsoft PIX to restore capture markers.")
#elif defined(__clang__) || defined(__GNUC__)
#warning "PIX WinPixEventRuntime headers not found; FidelityFX GPU markers compiled as no-ops. Install Microsoft PIX to restore capture markers."
#else
#pragma message("PIX WinPixEventRuntime headers not found; FidelityFX GPU markers compiled as no-ops. Install Microsoft PIX to restore capture markers.")
#endif
#endif
