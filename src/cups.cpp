#include "cups.hpp"

#include "cup_patch_blob.hpp"

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <fstream>
#include <map>
#include <sstream>

namespace fzgx
{
const std::vector<OriginalCup>& OriginalCups()
{
  // Cup -> course tables of the game (module 1, three identical copies).
  static const std::vector<OriginalCup> cups = {
      {"Ruby Cup", {1, 16, 26, 8, 5}},          {"Sapphire Cup", {14, 7, 11, 13, 3}},
      {"Emerald Cup", {15, 10, 29, 9, 27}},     {"Diamond Cup", {24, 25, 17, 21, 28}},
      {"AX Cup", {31, 32, 33, 34, 35, 36}},
  };
  return cups;
}

const char* CourseName(int id)
{
  // Course id -> track name, as listed by the game (main module string table).
  static const std::map<int, const char*> names = {
      {1, "Twist Road"},       {3, "Serial Gaps"},    {5, "Multiplex"},     {7, "Aero Dive"},
      {8, "Loop Cross"},       {9, "Half Pipe"},      {10, "Intersection"}, {11, "Mobius Ring"},
      {13, "Long Pipe"},       {14, "Drift Highway"}, {15, "Cylinder Knot"}, {16, "Split Oval"},
      {17, "Undulation"},      {21, "Dragon Slope"},  {24, "Trident"},      {25, "Lateral Shift"},
      {26, "Surface Slide"},   {27, "Ordeal"},        {28, "Slim-line Slits"}, {29, "Double Branches"},
      {31, "Screw Drive"},     {32, "Meteor Stream"}, {33, "Cylinder Wave"}, {34, "Thunder Road"},
      {35, "Spiral"},          {36, "Sonic Oval"},
  };
  auto it = names.find(id);
  return it != names.end() ? it->second : "";
}

std::vector<std::string> CupsOfCourse(int id, const std::vector<CupDef>& custom)
{
  std::vector<std::string> out;
  for (const auto& c : OriginalCups())
    if (std::find(c.courses.begin(), c.courses.end(), id) != c.courses.end())
      out.push_back(c.name);
  for (const auto& c : custom)
    if (std::find(c.courses.begin(), c.courses.end(), id) != c.courses.end())
      out.push_back(c.name);
  return out;
}

// --- cups.ini --------------------------------------------------------------------
std::vector<CupDef> LoadCups(const std::filesystem::path& file)
{
  std::vector<CupDef> cups;
  std::ifstream in(file);
  std::string line;
  while (std::getline(in, line))
  {
    while (!line.empty() && (line.back() == '\r' || line.back() == ' '))
      line.pop_back();
    if (line == "[cup]")
    {
      cups.emplace_back();
      continue;
    }
    const auto eq = line.find('=');
    if (cups.empty() || eq == std::string::npos || line[0] == '#')
      continue;
    const std::string key = line.substr(0, eq), value = line.substr(eq + 1);
    CupDef& c = cups.back();
    if (key == "name")
      c.name = value;
    else if (key == "hue")
      c.hue = std::strtof(value.c_str(), nullptr);
    else if (key == "courses")
    {
      std::stringstream ss(value);
      std::string tok;
      while (std::getline(ss, tok, ','))
        if (const int id = std::atoi(tok.c_str()); id > 0)
          c.courses.push_back(id);
    }
  }
  if (cups.size() > kMaxCustomCups)
    cups.resize(kMaxCustomCups);
  return cups;
}

bool SaveCups(const std::filesystem::path& file, const std::vector<CupDef>& cups)
{
  std::ofstream out(file);
  out << "# Custom Grand Prix cups (F-Zero GX Course Editor)\n";
  for (const auto& c : cups)
  {
    out << "\n[cup]\nname=" << c.name << "\ncourses=";
    for (std::size_t i = 0; i < c.courses.size(); ++i)
      out << (i ? "," : "") << c.courses[i];
    out << "\nhue=" << c.hue << "\n";
  }
  return bool(out);
}

std::string ValidateCups(const std::vector<CupDef>& cups)
{
  if (cups.size() > kMaxCustomCups)
    return "At most " + std::to_string(kMaxCustomCups) + " custom cups are possible.";
  for (const auto& c : cups)
  {
    const std::string who = c.name.empty() ? "A cup" : "Cup \"" + c.name + "\"";
    if (c.name.empty() || c.name.size() > 24)
      return who + ": the name must have 1 to 24 characters.";
    for (const unsigned char ch : c.name)
      if (ch < 0x20 || ch > 0x7e)
        return who + ": only plain ASCII letters, digits and punctuation can be shown by the game.";
    if (c.courses.empty() || c.courses.size() > kMaxCupCourses)
      return who + ": needs 1 to " + std::to_string(kMaxCupCourses) + " courses.";
    for (const int id : c.courses)
      if (!*CourseName(id))
        return who + ": course " + std::to_string(id) + " is not a race course.";
  }
  return {};
}

// --- main.dol --------------------------------------------------------------------
Bytes PatchDol(const Bytes& original, const std::vector<CupDef>& cups, int icon_base)
{
  namespace cp = cup_patch;
  Check(original.size() >= 0x100, "main.dol: too small");
  Bytes dol = original;
  std::uint32_t offs[18], addrs[18], sizes[18];
  for (int i = 0; i < 18; ++i)
  {
    offs[i] = U32(dol, 4 * i);
    addrs[i] = U32(dol, 0x48 + 4 * i);
    sizes[i] = U32(dol, 0x90 + 4 * i);
  }
  auto file_off = [&](std::uint32_t addr) -> std::size_t {
    for (int i = 0; i < 18; ++i)
      if (sizes[i] && addr >= addrs[i] && addr < addrs[i] + sizes[i])
        return offs[i] + (addr - addrs[i]);
    throw FormatError("main.dol: address not in any section");
  };
  for (const auto& h : cp::kDolHooks)
  {
    const std::size_t o = file_off(h.addr);
    Check(U32(dol, o) == h.original, "main.dol: unexpected code (not F-Zero GX USA, or already patched)");
    const std::int32_t rel = std::int32_t(h.target - h.addr);
    PutU32(dol, o, 0x48000001u | (std::uint32_t(rel) & 0x03FFFFFCu));
  }
  for (const auto& w : cp::kArenaLo)
  {
    const std::size_t o = file_off(w.addr);
    Check(U32(dol, o) == w.original, "main.dol: unexpected code (not F-Zero GX USA, or already patched)");
    PutU32(dol, o, w.replacement);
  }

  Bytes blob(std::begin(cp::kBlob), std::end(cp::kBlob));
  auto at = [&](std::uint32_t addr) { return std::size_t(addr - cp::kBase); };
  blob[at(cp::kCfgCount)] = std::uint8_t(cups.size());
  for (std::size_t i = 0; i < cups.size() && i < kMaxCustomCups; ++i)
  {
    const CupDef& c = cups[i];
    for (int k = 0; k < 6; ++k)
    {
      const int id = k < int(c.courses.size()) ? c.courses[k] : -1;
      PutU16(blob, at(cp::kCfgRows) + 12 * i + 2 * k, std::uint16_t(std::int16_t(id)));
    }
    PutU32(blob, at(cp::kCfgSprites) + 4 * i, 0xA100u + std::uint32_t(icon_base + 1 + int(i)));
    std::string upper = c.name;
    for (auto& ch : upper)
      ch = char(std::toupper(static_cast<unsigned char>(ch)));
    const std::string n = c.name.substr(0, 31), u = upper.substr(0, 31);
    std::copy(n.begin(), n.end(), blob.begin() + at(cp::kCfgNames) + 32 * i);
    std::copy(u.begin(), u.end(), blob.begin() + at(cp::kCfgNamesUc) + 32 * i);
  }
  PutU32(blob, at(cp::kCfgArrows), 0xA100u + std::uint32_t(icon_base));  // left arrow (page 2)
  Check(blob.size() <= cp::kReserved, "cup patch: too large");

  int slot = -1;
  for (int i = 0; i < 7 && slot < 0; ++i)
    if (sizes[i] == 0)
      slot = i;
  Check(slot >= 0, "main.dol: no free text section");
  const std::size_t start = (dol.size() + 31) & ~std::size_t(31);
  dol.resize(start, 0);
  dol.insert(dol.end(), blob.begin(), blob.end());
  dol.resize((dol.size() + 31) & ~std::size_t(31), 0);
  PutU32(dol, 4 * slot, std::uint32_t(start));
  PutU32(dol, 0x48 + 4 * slot, cp::kBase);
  PutU32(dol, 0x90 + 4 * slot, std::uint32_t((blob.size() + 31) & ~std::size_t(31)));
  return dol;
}

// --- icons -------------------------------------------------------------------------
namespace
{
void RgbToHsv(float r, float g, float b, float& h, float& s, float& v)
{
  const float mx = std::max({r, g, b}), mn = std::min({r, g, b}), d = mx - mn;
  v = mx;
  s = mx > 0 ? d / mx : 0;
  if (d <= 0)
    h = 0;
  else if (mx == r)
    h = 60 * std::fmod((g - b) / d + 6, 6.0f);
  else if (mx == g)
    h = 60 * ((b - r) / d + 2);
  else
    h = 60 * ((r - g) / d + 4);
}

void HsvToRgb(float h, float s, float v, float& r, float& g, float& b)
{
  h = std::fmod(std::fmod(h, 360.0f) + 360.0f, 360.0f);
  const float c = v * s, x = c * (1 - std::fabs(std::fmod(h / 60, 2.0f) - 1)), m = v - c;
  float rr = 0, gg = 0, bb = 0;
  if (h < 60)
    rr = c, gg = x;
  else if (h < 120)
    rr = x, gg = c;
  else if (h < 180)
    gg = c, bb = x;
  else if (h < 240)
    gg = x, bb = c;
  else if (h < 300)
    rr = x, bb = c;
  else
    rr = c, bb = x;
  r = rr + m, g = gg + m, b = bb + m;
}
}  // namespace

Texture CupIcon(const TplFile& selgf, float hue)
{
  auto ruby = selgf.Decode(2);
  Check(ruby.has_value(), "bmp_selgf.tpl: Ruby cup icon missing");
  Texture t = *ruby;
  // Only the gem is saturated; the shield is grey metal.
  for (std::size_t i = 0; i + 3 < t.rgba.size(); i += 4)
  {
    float h, s, v;
    RgbToHsv(t.rgba[i] / 255.0f, t.rgba[i + 1] / 255.0f, t.rgba[i + 2] / 255.0f, h, s, v);
    if (s <= 0.25f)
      continue;
    float r, g, b;
    HsvToRgb(hue, s, v, r, g, b);
    t.rgba[i] = std::uint8_t(std::lround(r * 255));
    t.rgba[i + 1] = std::uint8_t(std::lround(g * 255));
    t.rgba[i + 2] = std::uint8_t(std::lround(b * 255));
  }
  return t;
}

Texture LeftArrow(const TplFile& sel)
{
  auto arrow = sel.Decode(0);
  Check(arrow.has_value(), "bmp_sel.tpl: arrow missing");
  Texture t = *arrow;
  for (int y = 0; y < t.height; ++y)
    for (int x = 0; x < t.width / 2; ++x)
      for (int c = 0; c < 4; ++c)
        std::swap(t.rgba[(y * t.width + x) * 4 + c], t.rgba[(y * t.width + t.width - 1 - x) * 4 + c]);
  return t;
}

namespace
{
Bytes EncodeRgb5a3(const Texture& t)
{
  Bytes out;
  for (int ty = 0; ty < t.height; ty += 4)
    for (int tx = 0; tx < t.width; tx += 4)
      for (int y = ty; y < ty + 4; ++y)
        for (int x = tx; x < tx + 4; ++x)
        {
          std::uint8_t r = 0, g = 0, b = 0, a = 0;
          if (x < t.width && y < t.height)
          {
            const std::size_t i = (std::size_t(y) * t.width + x) * 4;
            r = t.rgba[i], g = t.rgba[i + 1], b = t.rgba[i + 2], a = t.rgba[i + 3];
          }
          std::uint16_t v;
          if (a >= 0xE0)
            v = std::uint16_t(0x8000 | (r >> 3) << 10 | (g >> 3) << 5 | (b >> 3));
          else
            v = std::uint16_t((a >> 5) << 12 | (r >> 4) << 8 | (g >> 4) << 4 | (b >> 4));
          out.push_back(std::uint8_t(v >> 8));
          out.push_back(std::uint8_t(v));
        }
  return out;
}
}  // namespace

Bytes AppendTextures(const Bytes& tpl, const std::vector<Texture>& textures)
{
  const std::uint32_t n = U32(tpl, 0);
  struct Entry
  {
    std::uint32_t format, offset;
    std::uint16_t w, h, mips, magic;
  };
  std::vector<Entry> entries;
  std::uint32_t first = std::uint32_t(tpl.size());
  for (std::uint32_t i = 0; i < n; ++i)
  {
    const std::size_t e = 4 + 16 * i;
    entries.push_back({U32(tpl, e), U32(tpl, e + 4), U16(tpl, e + 8), U16(tpl, e + 10), U16(tpl, e + 12),
                       U16(tpl, e + 14)});
    if (entries.back().offset)
      first = std::min(first, entries.back().offset);
  }
  const std::uint32_t count = n + std::uint32_t(textures.size());
  const std::uint32_t header = (4 + 16 * count + 31) & ~31u;
  const std::uint32_t shift = header - first;
  for (auto& e : entries)
    if (e.offset)
      e.offset += shift;
  Bytes data(tpl.begin() + first, tpl.end());
  for (const auto& t : textures)
  {
    data.resize(((header + data.size() + 31) & ~std::size_t(31)) - header, 0);
    entries.push_back({5, std::uint32_t(header + data.size()), std::uint16_t(t.width), std::uint16_t(t.height), 1,
                       0x1234});
    const Bytes px = EncodeRgb5a3(t);
    data.insert(data.end(), px.begin(), px.end());
  }
  Bytes out(header, 0);
  PutU32(out, 0, count);
  for (std::size_t i = 0; i < entries.size(); ++i)
  {
    const std::size_t e = 4 + 16 * i;
    PutU32(out, e, entries[i].format);
    PutU32(out, e + 4, entries[i].offset);
    PutU16(out, e + 8, entries[i].w);
    PutU16(out, e + 10, entries[i].h);
    PutU16(out, e + 12, entries[i].mips);
    PutU16(out, e + 14, entries[i].magic);
  }
  out.insert(out.end(), data.begin(), data.end());
  return out;
}
}  // namespace fzgx
