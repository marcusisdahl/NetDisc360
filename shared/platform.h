// SPDX-License-Identifier: MIT
#pragma once
// The Xbox build gets Win32 and Winsock from the SDK; host tests force-include
// tests/posix_compat.h instead.
#ifdef _XBOX
#include <xtl.h>
#include <winsockx.h>
#endif
#include <stddef.h>
typedef unsigned __int64 U64;
