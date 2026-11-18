// Workspace layout:
//   <workspace>/disc/{files,sys}        original disc, extracted once (read-only for the editor)
//   <workspace>/mod/GFZE01/{files,sys}  test copy (btrfs/XFS reflink copy of disc) with edited files
#pragma once
#include "coli.hpp"
#include "gma.hpp"

#include <filesystem>
#include <map>
#include <string>

namespace fzgx
{
namespace fs = std::filesystem;

struct Settings
{
  fs::path workspace;
  fs::path rom;              // RVZ/ISO used to (re)extract the disc
  fs::path glinux_appimage;  // F-ZERO GLINUX AppImage for the test button
  fs::path glinux_user_dir;  // GLINUX user dir (saves, config, controller)
  std::string launcher = "glinux";  // "glinux" or "dolphin"
  bool quickstart = true;           // start the edited course directly (Practice)
  bool textures = true;             // load and show the original textures (.tpl)

  fs::path disc() const { return workspace / "disc"; }
  fs::path mod() const { return workspace / "mod" / "GFZE01"; }
  fs::path file() const { return workspace / "editor.ini"; }
  void Load();
  void Save() const;
};

struct CourseInfo
{
  int id = 0;
  bool modified = false;
};

class Project
{
public:
  explicit Project(Settings s) : settings_(std::move(s)) {}
  Settings& settings() { return settings_; }

  bool DiscReady() const;
  std::vector<CourseInfo> ListCourses() const;

  // Loads the edited version if one exists, otherwise the original.
  Course LoadCourse(int id) const;
  // Model library for a course: stage/stNN.gma first, then the bg archives
  // ranked by how many of the course's models they contain.
  ModelLibrary BuildModels(int id, const Course& course, std::string* venue) const;

  // Compresses, verifies and writes the course into the mod copy.
  bool SaveCourse(int id, const Course& course, std::string& message);
  bool RestoreCourse(int id, std::string& message);

  // Starts the game from the mod copy in the background.
  // course >= 0 with settings.quickstart: race starts directly on that course.
  bool Launch(int course, std::string& message);
  // Extracts the ROM into <workspace>/disc (runs in the background).
  bool ExtractDisc(std::string& message);

  static std::string CourseFile(int id);

private:
  bool EnsureMod(std::string& message);
  Settings settings_;
};

std::string VenueName(const std::string& bg_archive);
fs::path ScriptsDir();
}  // namespace fzgx
