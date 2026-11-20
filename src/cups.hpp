// Cups: the game's own cups and up to two custom cups.
//
// Custom cups are added by patching main.dol (a small code section, see
// tools/cup_patch.s and docs/cup-format.md) and by appending the cup icons
// to bmp_e/bmp_selgf.tpl. They use the game's unused cup indices 9 and 10 and
// appear on a second page of the cup selection (in every game mode).
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

// A new course: a copy of a template course (and its stage models, CPU line,
// cameras, staff ghost) in one of the game's unused course slots. Its name and
// theme (venue) are written into the game's tables by the main.dol patch.
struct CourseDef
{
  int id = 0;           // free slot, see FreeCourseSlots()
  std::string name;
  int template_id = 1;  // original course it was created from (decides the theme)
};
constexpr int kMaxCustomCourses = 8;

// Unused course ids the game can load (name "---" in its table); a course file
// placed there is raced like any other.
const std::vector<int>& FreeCourseSlots();

// Venues ("themes") of the game: name, background archive tag, the values of
// the game's two per-course venue tables, original courses using it.
struct Theme
{
  const char* name;
  const char* tag;
  std::uint8_t code;   // course -> venue code table (module 1 section 4)
  std::uint8_t venue;  // course -> venue index table (section 5): venue name, background
  std::vector<int> courses;
};
const std::vector<Theme>& Themes();
int ThemeOfCourse(int id);  // index into Themes(), -1 if none

std::vector<CourseDef> LoadCourses(const std::filesystem::path& file);
bool SaveCourses(const std::filesystem::path& file, const std::vector<CourseDef>& courses);
std::string ValidateCourses(const std::vector<CourseDef>& courses);
// Track name of an original or custom course ("" if neither).
std::string CourseLabel(int id, const std::vector<CourseDef>& custom);

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

// Problems that keep the cups from being written (empty = fine). Custom
// courses may be used in cups.
std::string ValidateCups(const std::vector<CupDef>& cups, const std::vector<CourseDef>& courses = {});

// Adds the cup patch to an original GFZE01 main.dol. Throws FormatError if the
// DOL is not the expected version or already patched.
// Custom courses get their name and venue through extra patch-table entries.
Bytes PatchDol(const Bytes& original_dol, const std::vector<CupDef>& cups, int icon_base,
               const std::vector<CourseDef>& courses = {});

// Icons: the Ruby cup shield (bmp_selgf.tpl #2) with the gem recoloured, and the
// page arrow (bmp_sel.tpl #0) mirrored.
Texture CupIcon(const TplFile& selgf, float hue);
Texture LeftArrow(const TplFile& sel);

// Appends RGB5A3 textures to a TPL archive; returns the new archive.
Bytes AppendTextures(const Bytes& tpl, const std::vector<Texture>& textures);
}  // namespace fzgx
