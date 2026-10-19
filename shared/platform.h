// SPDX-License-Identifier: MIT
#pragma once
// The Xbox build gets Win32 and Winsock from the SDK, the PC server from the
// Windows SDK; host tests force-include tests/posix_compat.h instead.
#ifdef _XBOX
#include <xtl.h>
#include <winsockx.h>
#elif defined(_WIN32)
// The PC server (server/).
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <winsock2.h>
#include <windows.h>
#endif
#include <stddef.h>
typedef unsigned __int64 U64;
