// Amusement Vision TPL texture archives (paired with a .gma of the same name).
//   u32 count, then count x 0x10: u32 format, u32 offset, u16 width, u16 height,
//   u16 mipmap levels, u16 0x1234. offset 0 = empty slot.
// Formats are GameCube GX texture formats (tiled).
#pragma once
#include "binary.hpp"

#include <optional>

namespace fzgx
{
struct Texture
{
  int width = 0, height = 0;
  std::vector<std::uint8_t> rgba;  // width * height * 4, top row first
};

class TplFile
{
public:
  explicit TplFile(Bytes data) : data_(std::move(data)) {}
  int count() const;
  // Decodes mip level 0 of a texture; nullopt for empty slots / unknown formats.
  std::optional<Texture> Decode(int index) const;
  std::uint32_t Format(int index) const;

private:
  Bytes data_;
};

// Decodes one GX texture image (format codes: 0 I4, 1 I8, 2 IA4, 3 IA8,
// 4 RGB565, 5 RGB5A3, 6 RGBA8, 14 CMPR).
std::optional<Texture> DecodeGxTexture(const Bytes& data, std::size_t offset, std::uint32_t format, int width,
                                       int height);
}  // namespace fzgx
