// Grand Prix cups: the game's own cups and up to two custom cups.
//
// Custom cups are added by patching main.dol (a small code section, see
// tools/cup_patch.s and docs/cup-format.md) and by appending the cup icons
// to bmp_e/bmp_selgf.tpl. They use the game's unused cup indices 9 and 10 and
// appear on a second page of the Grand Prix cup selection.
#pragma once
#include "binary.hpp"
#include "tpl.hpp"

#include <filesystem>
#include <string>
#include <vector>

namespace fzgx
{
constexpr int kMaxCustomCups = 2;
constexpr int kMaxCupCourses = 5;

struct CupDef
{
  std::string name;
  std::vector<int> courses;  // course ids (COLI_COURSExx), 1..kMaxCupCourses
  float hue = 30;            // gem colour of the icon, degrees
};

struct OriginalCup
{
  const char* name;
  std::vector<int> courses;
};
const std::vector<OriginalCup>& OriginalCups();

// Track name of a course id ("Twist Road"); empty for story/special courses.
const char* CourseName(int id);

// Names of all cups (original and custom) that contain the course.
std::vector<std::string> CupsOfCourse(int id, const std::vector<CupDef>& custom);

std::vector<CupDef> LoadCups(const std::filesystem::path& file);
bool SaveCups(const std::filesystem::path& file, const std::vector<CupDef>& cups);

// Problems that keep the cups from being written (empty = fine).
std::string ValidateCups(const std::vector<CupDef>& cups);

// Adds the cup patch to an original GFZE01 main.dol. Throws FormatError if the
// DOL is not the expected version or already patched.
Bytes PatchDol(const Bytes& original_dol, const std::vector<CupDef>& cups, int icon_base);

// Icons: the Ruby cup shield (bmp_selgf.tpl #2) with the gem recoloured, and the
// page arrow (bmp_sel.tpl #0) mirrored.
Texture CupIcon(const TplFile& selgf, float hue);
Texture LeftArrow(const TplFile& sel);

// Appends RGB5A3 textures to a TPL archive; returns the new archive.
Bytes AppendTextures(const Bytes& tpl, const std::vector<Texture>& textures);
}  // namespace fzgx
