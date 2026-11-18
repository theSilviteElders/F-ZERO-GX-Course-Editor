#include "tpl.hpp"

#include <algorithm>

namespace fzgx
{
namespace
{
struct Rgba
{
  std::uint8_t r, g, b, a;
};

Rgba From565(std::uint16_t c)
{
  const int r = c >> 11 & 31, g = c >> 5 & 63, b = c & 31;
  return {std::uint8_t(r << 3 | r >> 2), std::uint8_t(g << 2 | g >> 4), std::uint8_t(b << 3 | b >> 2), 255};
}

Rgba From5A3(std::uint16_t c)
{
  if (c & 0x8000)
  {
    const int r = c >> 10 & 31, g = c >> 5 & 31, b = c & 31;
    return {std::uint8_t(r << 3 | r >> 2), std::uint8_t(g << 3 | g >> 2), std::uint8_t(b << 3 | b >> 2), 255};
  }
  const int a = c >> 12 & 7, r = c >> 8 & 15, g = c >> 4 & 15, b = c & 15;
  return {std::uint8_t(r * 17), std::uint8_t(g * 17), std::uint8_t(b * 17), std::uint8_t(a << 5 | a << 2 | a >> 1)};
}

// Size of one tile (block) per format: width, height, bytes.
struct Tile
{
  int w, h, bytes;
};

std::optional<Tile> TileOf(std::uint32_t format)
{
  switch (format)
  {
  case 0: return Tile{8, 8, 32};    // I4
  case 1: return Tile{8, 4, 32};    // I8
  case 2: return Tile{8, 4, 32};    // IA4
  case 3: return Tile{4, 4, 32};    // IA8
  case 4: return Tile{4, 4, 32};    // RGB565
  case 5: return Tile{4, 4, 32};    // RGB5A3
  case 6: return Tile{4, 4, 64};    // RGBA8
  case 14: return Tile{8, 8, 32};   // CMPR
  default: return std::nullopt;
  }
}
}  // namespace

std::optional<Texture> DecodeGxTexture(const Bytes& d, std::size_t offset, std::uint32_t format, int width,
                                       int height)
{
  const auto tile = TileOf(format);
  if (!tile || width <= 0 || height <= 0 || width > 4096 || height > 4096)
    return std::nullopt;
  const int tiles_x = (width + tile->w - 1) / tile->w, tiles_y = (height + tile->h - 1) / tile->h;
  if (offset + std::size_t(tiles_x) * tiles_y * tile->bytes > d.size())
    return std::nullopt;

  Texture t;
  t.width = width;
  t.height = height;
  t.rgba.assign(std::size_t(width) * height * 4, 0);
  auto put = [&](int x, int y, Rgba c) {
    if (x >= width || y >= height)
      return;
    std::uint8_t* p = &t.rgba[(std::size_t(y) * width + x) * 4];
    p[0] = c.r;
    p[1] = c.g;
    p[2] = c.b;
    p[3] = c.a;
  };

  std::size_t o = offset;
  for (int ty = 0; ty < tiles_y; ++ty)
    for (int tx = 0; tx < tiles_x; ++tx, o += tile->bytes)
    {
      const int bx = tx * tile->w, by = ty * tile->h;
      switch (format)
      {
      case 0:  // I4
        for (int i = 0; i < 64; ++i)
        {
          const std::uint8_t v = (d[o + i / 2] >> (i & 1 ? 0 : 4)) & 15;
          const std::uint8_t c = std::uint8_t(v * 17);
          put(bx + i % 8, by + i / 8, {c, c, c, 255});
        }
        break;
      case 1:  // I8
        for (int i = 0; i < 32; ++i)
          put(bx + i % 8, by + i / 8, {d[o + i], d[o + i], d[o + i], 255});
        break;
      case 2:  // IA4
        for (int i = 0; i < 32; ++i)
        {
          const std::uint8_t a = std::uint8_t((d[o + i] >> 4) * 17), c = std::uint8_t((d[o + i] & 15) * 17);
          put(bx + i % 8, by + i / 8, {c, c, c, a});
        }
        break;
      case 3:  // IA8
        for (int i = 0; i < 16; ++i)
          put(bx + i % 4, by + i / 4, {d[o + 2 * i + 1], d[o + 2 * i + 1], d[o + 2 * i + 1], d[o + 2 * i]});
        break;
      case 4:  // RGB565
        for (int i = 0; i < 16; ++i)
          put(bx + i % 4, by + i / 4, From565(U16(d, o + 2 * i)));
        break;
      case 5:  // RGB5A3
        for (int i = 0; i < 16; ++i)
          put(bx + i % 4, by + i / 4, From5A3(U16(d, o + 2 * i)));
        break;
      case 6:  // RGBA8: 32 bytes AR pairs, then 32 bytes GB pairs
        for (int i = 0; i < 16; ++i)
          put(bx + i % 4, by + i / 4, {d[o + 2 * i + 1], d[o + 32 + 2 * i], d[o + 32 + 2 * i + 1], d[o + 2 * i]});
        break;
      case 14:  // CMPR: 2x2 DXT1 sub-blocks, big-endian colours, 2-bit indices MSB first
        for (int sb = 0; sb < 4; ++sb)
        {
          const std::size_t s = o + sb * 8;
          const std::uint16_t c0 = U16(d, s), c1 = U16(d, s + 2);
          Rgba pal[4] = {From565(c0), From565(c1), {}, {}};
          if (c0 > c1)
          {
            pal[2] = {std::uint8_t((2 * pal[0].r + pal[1].r) / 3), std::uint8_t((2 * pal[0].g + pal[1].g) / 3),
                      std::uint8_t((2 * pal[0].b + pal[1].b) / 3), 255};
            pal[3] = {std::uint8_t((pal[0].r + 2 * pal[1].r) / 3), std::uint8_t((pal[0].g + 2 * pal[1].g) / 3),
                      std::uint8_t((pal[0].b + 2 * pal[1].b) / 3), 255};
          }
          else
          {
            pal[2] = {std::uint8_t((pal[0].r + pal[1].r) / 2), std::uint8_t((pal[0].g + pal[1].g) / 2),
                      std::uint8_t((pal[0].b + pal[1].b) / 2), 255};
            pal[3] = {0, 0, 0, 0};
          }
          const int sx = bx + (sb & 1) * 4, sy = by + (sb >> 1) * 4;
          for (int row = 0; row < 4; ++row)
          {
            const std::uint8_t bits = d[s + 4 + row];
            for (int col = 0; col < 4; ++col)
              put(sx + col, sy + row, pal[(bits >> (6 - 2 * col)) & 3]);
          }
        }
        break;
      }
    }
  return t;
}

int TplFile::count() const { return data_.size() >= 4 ? int(std::min<std::uint32_t>(U32(data_, 0), 100000)) : 0; }

std::uint32_t TplFile::Format(int index) const
{
  if (index < 0 || index >= count() || 4 + std::size_t(index + 1) * 16 > data_.size())
    return 0xFFFFFFFF;
  return U32(data_, 4 + std::size_t(index) * 16);
}

std::optional<Texture> TplFile::Decode(int index) const
{
  if (index < 0 || index >= count() || 4 + std::size_t(index + 1) * 16 > data_.size())
    return std::nullopt;
  const std::size_t e = 4 + std::size_t(index) * 16;
  const std::uint32_t format = U32(data_, e), offset = U32(data_, e + 4);
  const int w = U16(data_, e + 8), h = U16(data_, e + 10);
  if (!offset)
    return std::nullopt;
  return DecodeGxTexture(data_, offset, format, w, h);
}
}  // namespace fzgx
