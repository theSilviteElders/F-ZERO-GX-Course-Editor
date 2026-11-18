// Big-endian helpers for GameCube data.
#pragma once
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <optional>
#include <stdexcept>
#include <string>
#include <vector>

namespace fzgx
{
using Bytes = std::vector<std::uint8_t>;

struct FormatError : std::runtime_error
{
  using std::runtime_error::runtime_error;
};

inline void Check(bool ok, const char* what)
{
  if (!ok)
    throw FormatError(what);
}

inline std::uint8_t U8(const Bytes& d, std::size_t o)
{
  Check(o < d.size(), "read past end");
  return d[o];
}
inline std::uint16_t U16(const Bytes& d, std::size_t o)
{
  Check(o + 2 <= d.size(), "read past end");
  return std::uint16_t(d[o] << 8 | d[o + 1]);
}
inline std::int16_t S16(const Bytes& d, std::size_t o) { return std::int16_t(U16(d, o)); }
inline std::uint32_t U32(const Bytes& d, std::size_t o)
{
  Check(o + 4 <= d.size(), "read past end");
  return std::uint32_t(d[o]) << 24 | std::uint32_t(d[o + 1]) << 16 | std::uint32_t(d[o + 2]) << 8 |
         d[o + 3];
}
inline float F32(const Bytes& d, std::size_t o)
{
  const std::uint32_t v = U32(d, o);
  float f;
  std::memcpy(&f, &v, 4);
  return f;
}
inline std::uint32_t U32LE(const Bytes& d, std::size_t o)
{
  Check(o + 4 <= d.size(), "read past end");
  return std::uint32_t(d[o]) | std::uint32_t(d[o + 1]) << 8 | std::uint32_t(d[o + 2]) << 16 |
         std::uint32_t(d[o + 3]) << 24;
}

inline void PutU16(Bytes& d, std::size_t o, std::uint16_t v)
{
  Check(o + 2 <= d.size(), "write past end");
  d[o] = std::uint8_t(v >> 8);
  d[o + 1] = std::uint8_t(v);
}
inline void PutU32(Bytes& d, std::size_t o, std::uint32_t v)
{
  Check(o + 4 <= d.size(), "write past end");
  d[o] = std::uint8_t(v >> 24);
  d[o + 1] = std::uint8_t(v >> 16);
  d[o + 2] = std::uint8_t(v >> 8);
  d[o + 3] = std::uint8_t(v);
}
inline void PutF32(Bytes& d, std::size_t o, float f)
{
  std::uint32_t v;
  std::memcpy(&v, &f, 4);
  PutU32(d, o, v);
}

inline std::string CString(const Bytes& d, std::size_t o)
{
  std::string s;
  while (o < d.size() && d[o] != 0)
    s.push_back(char(d[o++]));
  return s;
}

std::optional<Bytes> ReadFile(const std::filesystem::path& path);
bool WriteFileAtomic(const std::filesystem::path& path, const Bytes& data);
}  // namespace fzgx
