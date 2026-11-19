#include "project.hpp"

#include "lz.hpp"

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <fcntl.h>
#include <fstream>
#include <regex>
#include <set>
#include <spawn.h>
#include <sys/wait.h>
#include <unistd.h>

extern char** environ;

namespace fzgx
{
namespace
{
std::string Trim(std::string s)
{
  while (!s.empty() && (s.back() == ' ' || s.back() == '\r' || s.back() == '\n'))
    s.pop_back();
  while (!s.empty() && s.front() == ' ')
    s.erase(s.begin());
  return s;
}

// Runs argv detached from the editor (own session, output into a log file).
bool Spawn(const std::vector<std::string>& args, const fs::path& log, std::string& message)
{
  posix_spawn_file_actions_t fa;
  posix_spawn_file_actions_init(&fa);
  posix_spawn_file_actions_addopen(&fa, 1, log.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0644);
  posix_spawn_file_actions_adddup2(&fa, 1, 2);
  posix_spawnattr_t attr;
  posix_spawnattr_init(&attr);
  posix_spawnattr_setflags(&attr, POSIX_SPAWN_SETSID);
  std::vector<char*> argv;
  for (const auto& a : args)
    argv.push_back(const_cast<char*>(a.c_str()));
  argv.push_back(nullptr);
  pid_t pid;
  const int rc = posix_spawn(&pid, argv[0], &fa, &attr, argv.data(), environ);
  posix_spawn_file_actions_destroy(&fa);
  posix_spawnattr_destroy(&attr);
  if (rc != 0)
  {
    message = "Failed to start: " + args[0];
    return false;
  }
  message = "Started, log: " + log.string();
  return true;
}
}  // namespace

fs::path ScriptsDir() { return fs::path(FZGX_SOURCE_DIR) / "scripts"; }

void Settings::Load()
{
  std::ifstream in(file());
  std::string line;
  while (std::getline(in, line))
  {
    const auto eq = line.find('=');
    if (eq == std::string::npos || line[0] == '#')
      continue;
    const std::string key = Trim(line.substr(0, eq)), value = Trim(line.substr(eq + 1));
    if (key == "rom")
      rom = value;
    else if (key == "glinux_appimage")
      glinux_appimage = value;
    else if (key == "glinux_user_dir")
      glinux_user_dir = value;
    else if (key == "launcher")
      launcher = value;
    else if (key == "quickstart")
      quickstart = value != "0" && value != "false";
    else if (key == "textures")
      textures = value != "0" && value != "false";
    else if (key == "magnet")
      magnet = value != "0" && value != "false";
    else if (key == "magnet_radius")
      magnet_radius = std::clamp(std::strtof(value.c_str(), nullptr), 1.0f, 1000.0f);
  }
}

void Settings::Save() const
{
  std::error_code ec;
  fs::create_directories(workspace, ec);
  std::ofstream out(file());
  out << "# F-Zero GX Course Editor\n"
      << "rom=" << rom.string() << "\n"
      << "glinux_appimage=" << glinux_appimage.string() << "\n"
      << "glinux_user_dir=" << glinux_user_dir.string() << "\n"
      << "launcher=" << launcher << "\n"
      << "quickstart=" << (quickstart ? "true" : "false") << "\n"
      << "textures=" << (textures ? "true" : "false") << "\n"
      << "magnet=" << (magnet ? "true" : "false") << "\n"
      << "magnet_radius=" << magnet_radius << "\n";
}

std::string Project::CourseFile(int id)
{
  char buf[32];
  std::snprintf(buf, sizeof buf, "COLI_COURSE%02d.lz", id);
  return buf;
}

bool Project::DiscReady() const { return fs::exists(settings_.disc() / "sys" / "main.dol"); }

std::vector<CourseInfo> Project::ListCourses() const
{
  std::vector<CourseInfo> out;
  const std::regex re(R"(COLI_COURSE(\d+)\.lz)");
  std::error_code ec;
  for (const auto& e : fs::directory_iterator(settings_.disc() / "files" / "stage", ec))
  {
    std::smatch m;
    const std::string name = e.path().filename().string();
    if (std::regex_match(name, m, re))
    {
      CourseInfo c;
      c.id = std::stoi(m[1]);
      const fs::path mod = settings_.mod() / "files" / "stage" / name;
      std::error_code ec2;
      c.modified = fs::exists(mod) && fs::file_size(mod, ec2) != fs::file_size(e.path(), ec2);
      if (fs::exists(mod) && !c.modified)
      {
        auto a = ReadFile(mod), b = ReadFile(e.path());
        c.modified = a != b;
      }
      out.push_back(c);
    }
  }
  std::sort(out.begin(), out.end(), [](auto& a, auto& b) { return a.id < b.id; });
  return out;
}

Course Project::LoadCourse(int id) const
{
  const std::string name = CourseFile(id);
  fs::path path = settings_.mod() / "files" / "stage" / name;
  if (!fs::exists(path))
    path = settings_.disc() / "files" / "stage" / name;
  auto raw = ReadFile(path);
  Check(raw.has_value(), "course file not readable");
  return Course::Parse(LzDecompress(*raw));
}

ModelLibrary Project::BuildModels(int id, const Course& course, std::string* venue) const
{
  ModelLibrary lib;
  char st[32];
  std::snprintf(st, sizeof st, "st%02d.gma.lz", id);
  lib.AddArchive(settings_.disc() / "files" / "stage" / st);

  std::set<std::string> wanted;
  for (const auto& in : course.instances())
    if (!lib.Contains(in.name))
      wanted.insert(in.name);

  std::vector<std::pair<int, fs::path>> ranked;
  std::error_code ec;
  for (const auto& e : fs::directory_iterator(settings_.disc() / "files" / "bg", ec))
  {
    const std::string fn = e.path().filename().string();
    if (fn.size() < 7 || fn.substr(fn.size() - 7) != ".gma.lz")
      continue;
    auto raw = ReadFile(e.path());
    if (!raw)
      continue;
    int hits = 0;
    try
    {
      for (const auto& [name, off] : ListGma(LzDecompress(*raw)))
        hits += int(wanted.count(name));
    }
    catch (const FormatError&)
    {
      continue;
    }
    if (hits)
      ranked.emplace_back(hits, e.path());
  }
  std::sort(ranked.begin(), ranked.end(), [](auto& a, auto& b) {
    // Prefer full-detail archives over the "_s" (small) variants on ties.
    return a.first != b.first ? a.first > b.first : a.second.filename() < b.second.filename();
  });
  for (const auto& [hits, path] : ranked)
    lib.AddArchive(path);
  if (venue)
  {
    *venue = "";
    for (const auto& [hits, path] : ranked)
    {
      const std::string fn = path.filename().string();
      if (fn.rfind("bg_com", 0) == 0)
        continue;  // shared props, not a venue
      *venue = VenueName(fn);
      break;
    }
  }
  return lib;
}

bool Project::EnsureMod(std::string& message)
{
  if (fs::exists(settings_.mod() / "sys" / "main.dol"))
    return true;
  if (!DiscReady())
  {
    message = "The disc has not been extracted.";
    return false;
  }
  std::error_code ec;
  fs::create_directories(settings_.mod().parent_path(), ec);
  // Reflink copy: practically free on btrfs/XFS, falls back to a real copy elsewhere.
  pid_t pid;
  const std::string src = settings_.disc().string(), dst = settings_.mod().string();
  const char* argv[] = {"cp", "-a", "--reflink=auto", src.c_str(), dst.c_str(), nullptr};
  if (posix_spawnp(&pid, "cp", nullptr, nullptr, const_cast<char**>(argv), environ) != 0)
  {
    message = "Could not run cp.";
    return false;
  }
  int status = 0;
  waitpid(pid, &status, 0);
  if (!WIFEXITED(status) || WEXITSTATUS(status) != 0)
  {
    message = "Copying the disc into the mod folder failed.";
    return false;
  }
  return true;
}

bool Project::SaveCourse(int id, const Course& course, std::string& message)
{
  if (!EnsureMod(message))
    return false;
  const Bytes raw = course.Serialize();
  const Bytes packed = LzCompress(raw);
  if (LzDecompress(packed) != raw)
  {
    message = "Internal error: compression was not lossless, nothing saved.";
    return false;
  }
  const fs::path out = settings_.mod() / "files" / "stage" / CourseFile(id);
  if (!WriteFileAtomic(out, packed))
  {
    message = "Write failed: " + out.string();
    return false;
  }
  message = "Saved: " + out.string();
  return true;
}

bool Project::RestoreCourse(int id, std::string& message)
{
  const fs::path out = settings_.mod() / "files" / "stage" / CourseFile(id);
  auto orig = ReadFile(settings_.disc() / "files" / "stage" / CourseFile(id));
  if (!orig || !fs::exists(settings_.mod()))
  {
    message = "Nothing to revert.";
    return false;
  }
  if (!WriteFileAtomic(out, *orig))
  {
    message = "Write failed.";
    return false;
  }
  message = "Original restored.";
  return true;
}

bool Project::Launch(int course, std::string& message)
{
  if (!EnsureMod(message))
    return false;
  const fs::path script = ScriptsDir() / "run-test.sh";
  std::vector<std::string> args = {"/bin/bash", script.string(), settings_.launcher, settings_.mod().string(),
                                   settings_.glinux_appimage.string(), settings_.glinux_user_dir.string(),
                                   (settings_.workspace / "glinux-runtime").string()};
  if (settings_.quickstart && settings_.launcher == "glinux" && course > 0)
    args.push_back(std::to_string(course));
  return Spawn(args, settings_.workspace / "launch.log", message);
}

bool Project::ExtractDisc(std::string& message)
{
  if (settings_.rom.empty() || !fs::exists(settings_.rom))
  {
    message = "ROM path is empty or does not exist.";
    return false;
  }
  const fs::path script = ScriptsDir() / "extract-disc.sh";
  return Spawn({"/bin/bash", script.string(), settings_.rom.string(), settings_.disc().string()},
               settings_.workspace / "extract.log", message);
}

std::string VenueName(const std::string& fn)
{
  static const std::map<std::string, std::string> names = {
      {"mut", "Mute City"},  {"big", "Big Blue"},   {"cas", "Casino Palace"},
      {"fir", "Fire Field"}, {"por", "Port Town"},  {"san", "Sand Ocean"},
      {"lig", "Lightning"},
  };
  if (fn.size() < 6)
    return fn;
  const std::string tag = fn.substr(3, 3);
  auto it = names.find(tag);
  return it != names.end() ? it->second : "bg_" + tag;
}
}  // namespace fzgx
