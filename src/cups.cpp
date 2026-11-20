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

// --- custom courses ----------------------------------------------------------------
const std::vector<int>& FreeCourseSlots()
{
  // Ids 22 and 30 have no venue (0xff) and are left out.
  static const std::vector<int> slots = {2, 4, 6, 12, 18, 19, 20, 23};
  return slots;
}

const std::vector<Theme>& Themes()
{
  // Values of the module 1 byte tables course id -> venue code (0x8031e18c) and
  // course id -> venue index (0x80352dd4; index into the venue name table),
  // addresses with the original main.dol.
  static const std::vector<Theme> themes = {
      {"Mute City", "mut", 25, 1, {1, 3, 36}},      {"Casino Palace", "cas", 4, 13, {16, 29}},
      {"Sand Ocean", "san", 41, 8, {26, 25}},       {"Lightning", "lig", 20, 6, {8, 9, 34}},
      {"Aeropolis", "tow", 50, 15, {5, 21, 31}},    {"Port Town", "por", 33, 2, {7, 13, 33}},
      {"Green Plant", "for", 13, 10, {10, 11, 35}}, {"Big Blue", "big", 27, 4, {14, 27}},
      {"Fire Field", "fir", 11, 11, {15, 17}},      {"Cosmo Terminal", "ele", 9, 16, {24}},
      {"Phantom Road", "rai", 35, 19, {28}},        {"Outer Space", "met", 22, 14, {32}},
  };
  return themes;
}

int ThemeOfCourse(int id)
{
  const auto& t = Themes();
  for (int i = 0; i < int(t.size()); ++i)
    if (std::find(t[i].courses.begin(), t[i].courses.end(), id) != t[i].courses.end())
      return i;
  return -1;
}

std::vector<CourseDef> LoadCourses(const std::filesystem::path& file)
{
  std::vector<CourseDef> out;
  std::ifstream in(file);
  std::string line;
  while (std::getline(in, line))
  {
    while (!line.empty() && (line.back() == '\r' || line.back() == ' '))
      line.pop_back();
    if (line == "[course]")
    {
      out.emplace_back();
      continue;
    }
    const auto eq = line.find('=');
    if (out.empty() || eq == std::string::npos || line[0] == '#')
      continue;
    const std::string key = line.substr(0, eq), value = line.substr(eq + 1);
    if (key == "id")
      out.back().id = std::atoi(value.c_str());
    else if (key == "name")
      out.back().name = value;
    else if (key == "template")
      out.back().template_id = std::atoi(value.c_str());
  }
  return out;
}

bool SaveCourses(const std::filesystem::path& file, const std::vector<CourseDef>& courses)
{
  std::ofstream out(file);
  out << "# Custom courses (F-Zero GX Course Editor)\n";
  for (const auto& c : courses)
    out << "\n[course]\nid=" << c.id << "\nname=" << c.name << "\ntemplate=" << c.template_id << "\n";
  return bool(out);
}

std::string ValidateCourses(const std::vector<CourseDef>& courses)
{
  if (courses.size() > std::size_t(kMaxCustomCourses))
    return "At most " + std::to_string(kMaxCustomCourses) + " new courses are possible.";
  std::vector<int> seen;
  for (const auto& c : courses)
  {
    const std::string who = c.name.empty() ? "A course" : "Course \"" + c.name + "\"";
    const auto& free = FreeCourseSlots();
    if (std::find(free.begin(), free.end(), c.id) == free.end())
      return who + ": slot " + std::to_string(c.id) + " is not a free course slot.";
    if (std::find(seen.begin(), seen.end(), c.id) != seen.end())
      return who + ": slot " + std::to_string(c.id) + " is used twice.";
    seen.push_back(c.id);
    if (c.name.empty() || c.name.size() > 24)
      return who + ": the name must have 1 to 24 characters.";
    for (const unsigned char ch : c.name)
      if (ch < 0x20 || ch > 0x7e)
        return who + ": only plain ASCII letters, digits and punctuation can be shown by the game.";
    if (ThemeOfCourse(c.template_id) < 0)
      return who + ": template " + std::to_string(c.template_id) + " is not a race course.";
  }
  return {};
}

std::string CourseLabel(int id, const std::vector<CourseDef>& custom)
{
  for (const auto& c : custom)
    if (c.id == id)
      return c.name;
  return CourseName(id);
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

std::string ValidateCups(const std::vector<CupDef>& cups, const std::vector<CourseDef>& courses)
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
      if (!*CourseName(id) &&
          std::none_of(courses.begin(), courses.end(), [&](const CourseDef& d) { return d.id == id; }))
        return who + ": course " + std::to_string(id) + " is not a race course.";
  }
  return {};
}

// --- main.dol --------------------------------------------------------------------
namespace
{
// Module 1 section offsets of per-course tables (measured with GLINUX, see
// docs/course-slots.md), indexed by course id:
constexpr std::uint32_t kM1VenueCodes = 0x8031E18C - 0x80319E10;    // section 4: u8 venue code
constexpr std::uint32_t kM1VenueIndex = 0x80352DD4 - 0x80327D00;    // section 5: u8 venue index
constexpr std::uint32_t kM1CourseNames = 0x80352FB8 - 0x80327D00;   // section 5: name pointer
constexpr std::uint32_t kM1CourseTexts = 0x80353A50 - 0x80327D00;   // section 5: 6 name pointers (JP EN DE FR ES IT)
}  // namespace

Bytes PatchDol(const Bytes& original, const std::vector<CupDef>& cups, int icon_base,
               const std::vector<CourseDef>& courses)
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

  // Custom courses: extra "copy" entries (kind 1) into module 1 for the two
  // venue bytes and the name pointers, written over the table's terminator;
  // their data (venue bytes, pointers, names) follows the new terminator.
  if (!courses.empty())
  {
    std::size_t end = U32(blob, 12);  // header: patches - BASE
    while (end + 16 <= blob.size() && blob[end] != 0)
      end += 16;
    Check(end + 16 <= blob.size(), "cup patch: patch table not terminated");
    blob.resize(end);
    constexpr std::size_t kEntries = 4;  // per course
    const std::size_t n = std::min(courses.size(), std::size_t(kMaxCustomCourses));
    const std::size_t data = end + 16 * (kEntries * n + 1);
    // Per course: 2 venue bytes (padded to 4), 6 name pointers, 32-byte name.
    const std::size_t rec = 4 + 24 + 32;
    blob.resize(data + rec * n, 0);
    auto entry = [&](std::size_t o, std::uint8_t section, std::uint32_t offset, std::size_t src, std::uint32_t len) {
      blob[o] = 1;
      blob[o + 1] = 1;  // module 1
      blob[o + 2] = section;
      blob[o + 3] = 0;
      PutU32(blob, o + 4, offset);
      PutU32(blob, o + 8, cp::kBase + std::uint32_t(src));
      PutU32(blob, o + 12, len);
    };
    for (std::size_t i = 0; i < n; ++i)
    {
      const CourseDef& c = courses[i];
      const int theme = ThemeOfCourse(c.template_id);
      Check(theme >= 0 && c.id > 0 && c.id < 64, "custom course: bad slot or template");
      const std::size_t r = data + rec * i, ptrs = r + 4, name = r + 28;
      blob[r] = Themes()[std::size_t(theme)].code;
      blob[r + 1] = Themes()[std::size_t(theme)].venue;
      const std::string text = c.name.substr(0, 31);
      std::copy(text.begin(), text.end(), blob.begin() + std::ptrdiff_t(name));
      for (int k = 0; k < 6; ++k)
        PutU32(blob, ptrs + 4 * std::size_t(k), cp::kBase + std::uint32_t(name));
      const std::uint32_t id = std::uint32_t(c.id);
      const std::size_t e = end + 16 * kEntries * i;
      entry(e, 4, kM1VenueCodes + id, r, 1);
      entry(e + 16, 5, kM1VenueIndex + id, r + 1, 1);
      entry(e + 32, 5, kM1CourseNames + 4 * id, ptrs, 4);
      entry(e + 48, 5, kM1CourseTexts + 24 * id, ptrs, 24);
    }
    // blob[end + 16 * kEntries * n .. data) stays zero: the terminator.
  }
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
