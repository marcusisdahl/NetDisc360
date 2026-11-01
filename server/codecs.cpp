// SPDX-License-Identifier: MIT
// The compression code the server is built with, in one file so the build
// lists name one more source: miniz (deflate, for compressed games and .zip
// archives) and the LZMA SDK's decoders (for .7z archives). Both are in
// third_party/, as published but for the note in miniz.c.
#include "../third_party/miniz/miniz.c"
#if !defined(_WIN32) && defined(FILE_ATTRIBUTE_NORMAL)
#undef FILE_ATTRIBUTE_NORMAL  // tests/posix_compat.h's; 7zTypes.h has its own.
#endif
#include "../third_party/lzma/7zArcIn.c"
#include "../third_party/lzma/7zBuf.c"
#define Z7_NO_METHODS_FILTERS  // ISOs are packed without filters; see compress.cpp.
#include "../third_party/lzma/7zDec.c"
#include "../third_party/lzma/Bcj2.c"
#include "../third_party/lzma/7zCrc.c"
#include "../third_party/lzma/7zCrcOpt.c"
#include "../third_party/lzma/7zStream.c"
#include "../third_party/lzma/CpuArch.c"
#include "../third_party/lzma/LzmaDec.c"
#include "../third_party/lzma/Lzma2Dec.c"
