#pragma once

// Feature-test macros for POSIX translation units. Include this header first
// so the macros are in effect before any system header is pulled in. The CMake
// target defines the same values on the command line; the guards keep the
// headers usable in standalone translation units too.

#if defined(__linux__)
#if !defined(_GNU_SOURCE)
#define _GNU_SOURCE 1
#endif
#elif defined(__APPLE__)
// The deprecated ucontext declarations on macOS are gated on the X/Open
// feature set; _DARWIN_C_SOURCE keeps the rest of the BSD/POSIX surface.
#if !defined(_XOPEN_SOURCE)
#define _XOPEN_SOURCE 700
#endif
#if !defined(_DARWIN_C_SOURCE)
#define _DARWIN_C_SOURCE 1
#endif
#else
#if !defined(_XOPEN_SOURCE)
#define _XOPEN_SOURCE 700
#endif
#endif
