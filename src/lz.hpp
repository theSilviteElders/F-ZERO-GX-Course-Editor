// Amusement Vision ".lz" container used by F-Zero GX:
//   u32 LE  compressed payload size (without this 8-byte header)
//   u32 LE  decompressed size
//   LZSS payload (4 KiB ring buffer starting at 0xFEE, flag bit 1 = literal,
//   reference = 12-bit ring position + 4-bit length - 3).
#pragma once
#include "binary.hpp"

namespace fzgx
{
Bytes LzDecompress(const Bytes& file);
Bytes LzCompress(const Bytes& data);
}  // namespace fzgx
