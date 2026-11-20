#include "editor.hpp"

#include <epoxy/gl.h>
#include <glm/gtc/matrix_transform.hpp>
#include <glm/gtc/type_ptr.hpp>
#include <imgui.h>
#include <imgui_stdlib.h>
// ImGuizmo.h needs imgui.h first.
#include <ImGuizmo.h>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <set>
#include <unordered_map>

namespace fzgx
{
namespace
{
constexpr float kLeftWidth = 300, kRightWidth = 330, kStatusHeight = 26;

glm::mat4 ToGlm(const Mat34& m)
{
  glm::mat4 r(1.0f);
  for (int row = 0; row < 3; ++row)
    for (int col = 0; col < 4; ++col)
      r[col][row] = m[row * 4 + col];
  return r;
}

Mat34 FromGlm(const glm::mat4& r)
{
  Mat34 m{};
  for (int row = 0; row < 3; ++row)
    for (int col = 0; col < 4; ++col)
      m[row * 4 + col] = r[col][row];
  return m;
}

std::uint32_t Rgba(int r, int g, int b, int a = 255) { return std::uint32_t(r | g << 8 | b << 16 | a << 24); }

std::uint32_t SurfaceColor(int s)
{
  static const std::uint32_t pal[] = {
      Rgba(230, 80, 80, 170),  Rgba(80, 200, 90, 170),  Rgba(80, 140, 240, 170), Rgba(240, 200, 60, 170),
      Rgba(200, 90, 220, 170), Rgba(60, 210, 210, 170), Rgba(250, 140, 50, 170), Rgba(160, 160, 160, 170),
      Rgba(120, 90, 50, 170),  Rgba(250, 250, 250, 170), Rgba(255, 60, 160, 170), Rgba(120, 220, 160, 170),
      Rgba(90, 90, 230, 170),  Rgba(200, 230, 90, 170),
  };
  return pal[std::clamp(s, 0, 13)];
}

bool RayTriangle(const glm::vec3& o, const glm::vec3& d, const glm::vec3& a, const glm::vec3& b,
                 const glm::vec3& c, float& t)
{
  const glm::vec3 e1 = b - a, e2 = c - a, p = glm::cross(d, e2);
  const float det = glm::dot(e1, p);
  if (std::fabs(det) < 1e-9f)
    return false;
  const float inv = 1.0f / det;
  const glm::vec3 s = o - a;
  const float u = glm::dot(s, p) * inv;
  if (u < 0 || u > 1)
    return false;
  const glm::vec3 q = glm::cross(s, e1);
  const float v = glm::dot(d, q) * inv;
  if (v < 0 || u + v > 1)
    return false;
  t = glm::dot(e2, q) * inv;
  return t > 0;
}

bool RayBox(const glm::vec3& o, const glm::vec3& d, const glm::vec3& mn, const glm::vec3& mx, float& t)
{
  float t0 = 0, t1 = 1e30f;
  for (int k = 0; k < 3; ++k)
  {
    if (std::fabs(d[k]) < 1e-12f)
    {
      if (o[k] < mn[k] || o[k] > mx[k])
        return false;
      continue;
    }
    float a = (mn[k] - o[k]) / d[k], b = (mx[k] - o[k]) / d[k];
    if (a > b)
      std::swap(a, b);
    t0 = std::max(t0, a);
    t1 = std::min(t1, b);
    if (t0 > t1)
      return false;
  }
  t = t0;
  return true;
}

const glm::vec3 kMissingMin(-8, -8, -8), kMissingMax(8, 8, 8);

const char* const kStructureCategories[] = {
    "Boost & special zones",       // groups with collision polygons (dash plates ...)
    "Object groups",               // several linked objects
    "Single objects",
    "Track pieces (visual only)",  // road/guardrail models placed at the origin
};

// Placeholder boxes keep a fixed size: strip the scale from the instance matrix.
glm::mat4 WithoutScale(const glm::mat4& m)
{
  glm::mat4 r = m;
  for (int c = 0; c < 3; ++c)
  {
    const float len = glm::length(glm::vec3(m[c]));
    if (len > 1e-6f)
      r[c] = m[c] / len;
  }
  return r;
}
}  // namespace

glm::vec3 Camera::Forward() const
{
  const float y = glm::radians(yaw), p = glm::radians(pitch);
  return glm::normalize(glm::vec3(std::cos(y) * std::cos(p), std::sin(p), std::sin(y) * std::cos(p)));
}

glm::mat4 Camera::View() const { return glm::lookAt(pos, pos + Forward(), glm::vec3(0, 1, 0)); }

Editor::Editor(Settings settings) : project_(std::move(settings)) {}

Editor::~Editor()
{
  for (auto& t : cup_icons_)
    renderer_.FreeTexture(t);
  CloseCourse();
  renderer_.Shutdown();
}

bool Editor::Init()
{
  if (!renderer_.Init())
    return false;
  RefreshCourses();
  cups_ = LoadCups(project_.settings().cups_file());
  if (!project_.DiscReady())
  {
    show_settings_ = true;
    Status("No extracted disc found. Choose your ROM in Settings and extract it.", true);
  }
  else if (!courses_.empty())
    OpenCourse(courses_.front().id);
  return true;
}

void Editor::Status(std::string msg, bool error)
{
  status_ = std::move(msg);
  status_error_ = error;
  status_time_ = ImGui::GetTime();
}

void Editor::RefreshCourses() { courses_ = project_.DiscReady() ? project_.ListCourses() : std::vector<CourseInfo>{}; }

void Editor::FreeModels()
{
  for (auto& [name, mesh] : gpu_)
    renderer_.Free(mesh);
  gpu_.clear();
  for (auto& [key, tex] : textures_)
    renderer_.FreeTexture(tex);
  textures_.clear();
}

void Editor::UploadModels()
{
  FreeModels();
  if (!course_)
    return;
  const bool textured = project_.settings().textures;
  for (const auto& in : course_->instances())
  {
    if (gpu_.count(in.name))
      continue;
    const ModelMesh* m = models_->Find(in.name);
    if (!m || m->positions.empty())
    {
      gpu_[in.name] = GpuMesh{};
      continue;
    }
    GpuMesh g = renderer_.Upload(*m, textured);
    if (textured)
    {
      const TplFile* tpl = models_->Textures(m->archive);
      for (const auto& part : m->parts)
      {
        GpuPart gp{part.first, part.count, 0};
        if (tpl && part.tpl >= 0)
        {
          const std::uint64_t key = std::uint64_t(m->archive) << 32 | std::uint64_t(part.tpl) << 8 | part.wrap;
          auto it = textures_.find(key);
          if (it == textures_.end())
          {
            const auto tex = tpl->Decode(part.tpl);
            it = textures_.emplace(key, tex ? renderer_.CreateTexture(*tex, part.wrap) : 0u).first;
          }
          gp.texture = it->second;
        }
        g.parts.push_back(gp);
      }
    }
    gpu_[in.name] = std::move(g);
  }
}

void Editor::CloseCourse()
{
  FreeModels();
  renderer_.Free(collision_mesh_);
  course_.reset();
  models_.reset();
  original_.clear();
  original_polys_.clear();
  background_.clear();
  structures_.clear();
  spawn_groups_.clear();
  vertex_cache_.clear();
  connector_cache_.clear();
  snap_line_ = snap_line_draw_ = {};
  handles_.clear();
  handle_ = -1;
  armed_ = -1;
  hover_valid_ = false;
  undo_.clear();
  redo_.clear();
  selected_ = -1;
  dirty_ = false;
  course_id_ = -1;
}

void Editor::OpenCourse(int id)
{
  CloseCourse();
  try
  {
    course_ = project_.LoadCourse(id);
    models_ = std::make_unique<ModelLibrary>(project_.BuildModels(id, *course_, &venue_));
  }
  catch (const std::exception& e)
  {
    course_.reset();
    Status(std::string("Could not load course: ") + e.what(), true);
    return;
  }
  course_id_ = id;
  for (int i = 0; i < int(course_->instances().size()); ++i)
    original_.push_back(Get(i));
  for (const auto& p : course_->polys())
    original_polys_.push_back(p.v);
  UploadModels();
  background_.assign(course_->instances().size(), 0);
  for (int i = 0; i < int(course_->instances().size()); ++i)
  {
    const auto& in = course_->instances()[i];
    if (const ModelMesh* m = models_->Find(in.name); m && !m->positions.empty())
      for (int k = 0; k < 3; ++k)
        if ((m->bounds_max[k] - m->bounds_min[k]) * std::fabs(in.scale[k]) > 8000)
          background_[i] = 1;
  }
  const auto& col = course_->collision();
  std::vector<std::uint32_t> colors;
  for (int s : col.surface)
    colors.push_back(SurfaceColor(s));
  collision_mesh_ = renderer_.UploadColored(col.triangles, colors);
  BuildStructures();
  RefreshHandles();

  bool from_mod = false;
  for (const auto& c : courses_)
    from_mod |= c.id == id && c.modified;
  Status("Course " + std::to_string(id) + (venue_.empty() ? "" : " (" + venue_ + ")") + " loaded: " +
         std::to_string(course_->instances().size()) + " objects" +
         (from_mod ? ", edited version from the mod folder." : "."));
  FrameCourse();
}

// ---------------------------------------------------------------------------
// Editing

Transform Editor::Get(int i) const
{
  const auto& in = course_->instances()[i];
  return {in.position, in.rotation, in.scale, in.matrix};
}

void Editor::Set(int i, const Transform& t)
{
  auto& in = course_->instances()[i];
  in.position = t.position;
  in.rotation = t.rotation;
  in.scale = t.scale;
  in.matrix = t.matrix;
  dirty_ = true;
}

void Editor::SetFromMatrix(int i, const Mat34& m)
{
  Vec3 pos, rot, scale;
  if (!DecomposeMatrix(m, pos, rot, scale))
    return;
  Transform t;
  t.position = pos;
  t.rotation = {DegToAngle(rot[0]), DegToAngle(rot[1]), DegToAngle(rot[2])};
  t.scale = scale;
  // Re-derive the matrix from the quantised angles: exactly what the game stores.
  t.matrix = ComposeMatrix(t.position, t.rotation, t.scale);
  Set(i, t);
}

bool Editor::IsWorldPiece(int i) const
{
  const auto& in = course_->instances()[i];
  const glm::vec3 p(in.matrix[3], in.matrix[7], in.matrix[11]);
  if (glm::length(p) < 1.0f)
    return true;
  const ModelMesh* m = models_->Find(in.name);
  if (!m || m->positions.empty())
    return false;
  const glm::vec3 a(m->bounds_min[0], m->bounds_min[1], m->bounds_min[2]);
  const glm::vec3 b(m->bounds_max[0], m->bounds_max[1], m->bounds_max[2]);
  return glm::length((a + b) * 0.5f) > std::max(100.0f, glm::length(b - a) * 0.5f);
}

std::vector<int> Editor::Linked(int i) const
{
  std::vector<int> out;
  if (!link_ || !course_ || i < 0)
    return out;
  const auto& list = course_->instances();
  auto prefix = [](const std::string& n) { return n.substr(0, n.find('_')); };
  const glm::vec3 p(list[i].matrix[3], list[i].matrix[7], list[i].matrix[11]);
  // Track pieces (roads, guardrails) mostly sit at the world origin: never link those.
  if (list[i].name.empty() || IsWorldPiece(i))
    return out;
  const std::string pre = prefix(list[i].name);
  for (int j = 0; j < int(list.size()); ++j)
  {
    if (j == i || list[j].removed || IsBackground(j) || prefix(list[j].name) != pre)
      continue;
    const glm::vec3 q(list[j].matrix[3], list[j].matrix[7], list[j].matrix[11]);
    if (glm::distance(p, q) <= link_radius_ && !IsWorldPiece(j))
      out.push_back(j);
  }
  return out;
}

std::vector<int> Editor::AttachedPolys(const std::vector<int>& objects, bool force) const
{
  std::vector<int> out;
  if (!course_ || (!move_collision_ && !force))
    return out;
  const auto& polys = course_->polys();
  std::vector<char> taken(polys.size(), 0);
  for (int i : objects)
  {
    const auto& in = course_->instances()[i];
    if (IsWorldPiece(i) || IsBackground(i))
      continue;  // track pieces / huge scenery: never grab collision
    const ModelMesh* m = models_->Find(in.name);
    if (!m || m->positions.empty())
      continue;
    const glm::mat4 inv = glm::inverse(ToGlm(in.matrix));
    // Local bounds, a little larger (collision sits slightly above/below the model).
    glm::vec3 mn(m->bounds_min[0], m->bounds_min[1], m->bounds_min[2]);
    glm::vec3 mx(m->bounds_max[0], m->bounds_max[1], m->bounds_max[2]);
    const glm::vec3 pad = (mx - mn) * 0.05f + glm::vec3(1.0f, 3.0f, 1.0f);
    mn -= pad;
    mx += pad;
    for (int p = 0; p < int(polys.size()); ++p)
    {
      if (taken[p] || polys[p].removed)
        continue;
      bool inside = true;
      for (const auto& v : polys[p].v)
      {
        const glm::vec3 l = glm::vec3(inv * glm::vec4(v[0], v[1], v[2], 1));
        if (glm::any(glm::lessThan(l, mn)) || glm::any(glm::greaterThan(l, mx)))
        {
          inside = false;
          break;
        }
      }
      if (inside)
      {
        taken[p] = 1;
        out.push_back(p);
      }
    }
  }
  return out;
}

void Editor::SetPoly(int p, const std::vector<Vec3>& v)
{
  auto& poly = course_->polys()[p];
  poly.v = v;
  poly.modified = v != original_polys_[p] || poly.modified;
  collision_dirty_ = true;
  dirty_ = true;
}

void Editor::BeginEdit()
{
  edit_snapshot_.clear();
  poly_snapshot_.clear();
  if (selected_ < 0)
    return;
  edit_snapshot_.emplace_back(selected_, Get(selected_));
  for (int j : Linked(selected_))
    edit_snapshot_.emplace_back(j, Get(j));
  std::vector<int> members;
  for (const auto& [i, t] : edit_snapshot_)
    members.push_back(i);
  for (int p : AttachedPolys(members))
    poly_snapshot_.emplace_back(p, course_->polys()[p].v);
  editing_ = true;
}

glm::vec3 Editor::LocalCenter(int i) const
{
  const ModelMesh* m = models_->Find(course_->instances()[i].name);
  if (!m || m->positions.empty())
    return glm::vec3(0);
  return glm::vec3(m->bounds_min[0] + m->bounds_max[0], m->bounds_min[1] + m->bounds_max[1],
                   m->bounds_min[2] + m->bounds_max[2]) * 0.5f;
}

Transform Editor::ScaledInPlace(int i, const Transform& from, const Vec3& factor) const
{
  const glm::vec3 c = LocalCenter(i);
  const glm::vec3 centre = glm::vec3(ToGlm(from.matrix) * glm::vec4(c, 1));
  Transform t = from;
  for (int k = 0; k < 3; ++k)
    t.scale[k] = from.scale[k] * factor[k];
  t.matrix = ComposeMatrix(t.position, t.rotation, t.scale);
  const glm::vec3 moved = glm::vec3(ToGlm(t.matrix) * glm::vec4(c, 1));
  for (int k = 0; k < 3; ++k)
    t.position[k] += centre[k] - moved[k];
  t.matrix = ComposeMatrix(t.position, t.rotation, t.scale);
  return t;
}

void Editor::ApplyPrimary(const Transform& t, bool scale_in_place)
{
  if (!editing_)
    BeginEdit();
  if (edit_snapshot_.empty())
    return;
  const Transform& start = edit_snapshot_[0].second;
  const glm::mat4 before = ToGlm(start.matrix);
  Set(edit_snapshot_[0].first, t);
  // Same world-space change for every linked object (relative to the start of the edit).
  const glm::mat4 delta = ToGlm(t.matrix) * glm::inverse(before);
  Vec3 factor{1, 1, 1};
  for (int k = 0; k < 3; ++k)
    factor[k] = std::fabs(start.scale[k]) > 1e-6f ? t.scale[k] / start.scale[k] : 1.0f;
  for (std::size_t k = 1; k < edit_snapshot_.size(); ++k)
  {
    const auto& [j, from] = edit_snapshot_[k];
    if (scale_in_place)
      Set(j, ScaledInPlace(j, from, factor));
    else
      SetFromMatrix(j, FromGlm(delta * ToGlm(from.matrix)));
  }
  for (const auto& [p, verts] : poly_snapshot_)
  {
    std::vector<Vec3> moved;
    for (const auto& v : verts)
    {
      const glm::vec3 w = glm::vec3(delta * glm::vec4(v[0], v[1], v[2], 1));
      moved.push_back({w.x, w.y, w.z});
    }
    SetPoly(p, moved);
  }
}

void Editor::EndEdit()
{
  if (!editing_)
    return;
  editing_ = false;
  UndoEntry e;
  for (const auto& [i, before] : edit_snapshot_)
  {
    const Transform after = Get(i);
    if (!(after == before))
      e.items.push_back({i, before, after});
  }
  for (const auto& [p, before] : poly_snapshot_)
    if (course_->polys()[p].v != before)
      e.polys.push_back({p, before, course_->polys()[p].v});
  edit_snapshot_.clear();
  poly_snapshot_.clear();
  if (e.items.empty() && e.polys.empty())
    return;
  undo_.push_back(std::move(e));
  redo_.clear();
}

void Editor::Undo()
{
  if (undo_.empty())
    return;
  auto e = undo_.back();
  undo_.pop_back();
  for (const auto& it : e.items)
    Set(it.index, it.before);
  for (const auto& it : e.polys)
    SetPoly(it.poly, it.before);
  for (const auto& it : e.alive)
    SetAlive(it, !it.alive);
  for (const auto& it : e.curves)
    ApplyCurves(it.segment, it.before);
  if (!e.items.empty())
    selected_ = e.items.front().index;
  redo_.push_back(e);
}

void Editor::Redo()
{
  if (redo_.empty())
    return;
  auto e = redo_.back();
  redo_.pop_back();
  for (const auto& it : e.items)
    Set(it.index, it.after);
  for (const auto& it : e.polys)
    SetPoly(it.poly, it.after);
  for (const auto& it : e.alive)
    SetAlive(it, it.alive);
  for (const auto& it : e.curves)
    ApplyCurves(it.segment, it.after);
  if (!e.items.empty())
    selected_ = e.items.front().index;
  else if (!e.alive.empty() && !e.alive.front().poly && e.alive.front().alive)
    selected_ = e.alive.front().index;
  undo_.push_back(e);
}

bool Editor::Modified(int i) const { return !(Get(i) == original_[i]); }

// ---------------------------------------------------------------------------
// Magnetism

bool Editor::MagnetActive() { return project_.settings().magnet && !ImGui::GetIO().KeyAlt; }

const std::vector<glm::vec3>& Editor::BorderVertices(const std::string& name)
{
  auto it = vertex_cache_.find(name);
  if (it != vertex_cache_.end())
    return it->second;
  std::vector<glm::vec3>& out = vertex_cache_[name];
  if (const ModelMesh* m = models_->Find(name))
  {
    // Tolerance: 1 % of the extent (at least half a unit) along x and z.
    const float tx = std::max(0.5f, (m->bounds_max[0] - m->bounds_min[0]) * 0.01f);
    const float tz = std::max(0.5f, (m->bounds_max[2] - m->bounds_min[2]) * 0.01f);
    std::set<std::array<long, 3>> seen;
    for (const auto& p : m->positions)
    {
      const bool border = p[0] - m->bounds_min[0] <= tx || m->bounds_max[0] - p[0] <= tx ||
                          p[2] - m->bounds_min[2] <= tz || m->bounds_max[2] - p[2] <= tz;
      if (border && seen.insert({std::lround(p[0] * 100), std::lround(p[1] * 100), std::lround(p[2] * 100)}).second)
        out.emplace_back(p[0], p[1], p[2]);
    }
  }
  return out;
}

bool Editor::SnapVertices(const std::vector<int>& sources, const std::vector<int>& exclude, const glm::mat4& pre,
                          glm::vec3& offset)
{
  const float r = project_.settings().magnet_radius;
  const auto& list = course_->instances();
  std::vector<glm::vec3> from;
  glm::vec3 lo(1e30f), hi(-1e30f);
  for (int i : sources)
  {
    const glm::mat4 M = pre * ToGlm(list[i].matrix);
    for (const auto& v : BorderVertices(list[i].name))
    {
      const glm::vec3 w = glm::vec3(M * glm::vec4(v, 1));
      from.push_back(w);
      lo = glm::min(lo, w);
      hi = glm::max(hi, w);
    }
  }
  if (from.empty())
    return false;
  lo -= glm::vec3(r);
  hi += glm::vec3(r);

  // Vertices of other objects near the moved ones, hashed into cells of size r.
  auto key = [&](const glm::vec3& p, int dx = 0, int dy = 0, int dz = 0) {
    const long long x = (long long)std::floor(p.x / r) + dx, y = (long long)std::floor(p.y / r) + dy,
                    z = (long long)std::floor(p.z / r) + dz;
    return (x * 73856093LL) ^ (y * 19349663LL) ^ (z * 83492791LL);
  };
  std::unordered_map<long long, std::vector<glm::vec3>> cells;
  for (int j = 0; j < int(list.size()); ++j)
  {
    const auto& in = list[j];
    if (in.removed || IsBackground(j) || std::find(exclude.begin(), exclude.end(), j) != exclude.end())
      continue;
    const ModelMesh* m = models_->Find(in.name);
    if (!m || m->positions.empty())
      continue;
    const glm::mat4 M = ToGlm(in.matrix);
    glm::vec3 blo(1e30f), bhi(-1e30f);
    for (int c = 0; c < 8; ++c)
    {
      const glm::vec3 corner(c & 1 ? m->bounds_max[0] : m->bounds_min[0], c & 2 ? m->bounds_max[1] : m->bounds_min[1],
                             c & 4 ? m->bounds_max[2] : m->bounds_min[2]);
      const glm::vec3 w = glm::vec3(M * glm::vec4(corner, 1));
      blo = glm::min(blo, w);
      bhi = glm::max(bhi, w);
    }
    if (glm::any(glm::lessThan(bhi, lo)) || glm::any(glm::greaterThan(blo, hi)))
      continue;
    for (const auto& v : BorderVertices(in.name))
    {
      const glm::vec3 w = glm::vec3(M * glm::vec4(v, 1));
      if (glm::all(glm::greaterThanEqual(w, lo)) && glm::all(glm::lessThanEqual(w, hi)))
        cells[key(w)].push_back(w);
    }
  }
  if (cells.empty())
    return false;
  float best = r * r;
  bool found = false;
  for (const auto& p : from)
    for (int dx = -1; dx <= 1; ++dx)
      for (int dy = -1; dy <= 1; ++dy)
        for (int dz = -1; dz <= 1; ++dz)
        {
          auto it = cells.find(key(p, dx, dy, dz));
          if (it == cells.end())
            continue;
          for (const auto& q : it->second)
          {
            const glm::vec3 d = q - p;
            const float d2 = glm::dot(d, d);
            if (d2 < best)
            {
              best = d2;
              found = true;
              offset = d;
              snap_line_ = {true, p, q, {}};
            }
          }
        }
  return found;
}

const std::vector<Connector>& Editor::Connectors(int i)
{
  static const std::vector<Connector> none;
  const auto& in = course_->instances()[i];
  if (in.name.find("ROAD") == std::string::npos)
    return none;
  auto it = connector_cache_.find(in.name);
  if (it != connector_cache_.end())
    return it->second;
  std::vector<Connector>& out = connector_cache_[in.name];
  if (const ModelMesh* m = models_->Find(in.name); m && !m->positions.empty())
    out = FindConnectors(*m);
  return out;
}

std::vector<Connector> Editor::WorldConnectors(int i, const glm::mat4& pre)
{
  std::vector<Connector> out;
  const glm::mat4 M = pre * ToGlm(course_->instances()[i].matrix);
  for (const auto& c : Connectors(i))
    out.push_back(TransformConnector(c, M));
  return out;
}

std::vector<std::pair<int, Connector>> Editor::AllConnectors()
{
  std::vector<std::pair<int, Connector>> out;
  const auto& list = course_->instances();
  for (int j = 0; j < int(list.size()); ++j)
    if (!list[j].removed && !Connectors(j).empty())
      for (auto& c : WorldConnectors(j))
        out.emplace_back(j, std::move(c));
  return out;
}

bool Editor::ConnectorOpen(int owner, const Connector& c, const std::vector<std::pair<int, Connector>>& all)
{
  for (const auto& [j, o] : all)
    if (j != owner && ConnectorsFit(c, o, -0.7f, 3.0f))
      return false;
  return true;
}

bool Editor::SnapConnectors(const std::vector<int>& sources, const std::vector<int>& exclude, const glm::mat4& pre,
                            glm::vec3& offset)
{
  const float r = project_.settings().magnet_radius;
  const auto& list = course_->instances();
  std::vector<Connector> from;
  for (int i : sources)
    for (auto& c : WorldConnectors(i, pre))
      from.push_back(std::move(c));
  if (from.empty())
    return false;
  float best = r;
  bool found = false;
  for (int j = 0; j < int(list.size()); ++j)
  {
    if (list[j].removed || Connectors(j).empty() || std::find(exclude.begin(), exclude.end(), j) != exclude.end())
      continue;
    for (const auto& to : WorldConnectors(j))
      for (const auto& f : from)
      {
        const float d = glm::distance(f.centre, to.centre);
        if (d < best && ConnectorsFit(f, to, -0.7f, r))
        {
          best = d;
          found = true;
          offset = to.centre - f.centre;
          snap_line_ = {true, f.centre, to.centre, f.lines};
          snap_line_.edges.insert(snap_line_.edges.end(), to.lines.begin(), to.lines.end());
        }
      }
  }
  return found;
}

bool Editor::Snap(const std::vector<int>& sources, const std::vector<int>& exclude, const glm::mat4& pre,
                  glm::vec3& offset)
{
  for (int i : sources)
    if (!Connectors(i).empty())
      return SnapConnectors(sources, exclude, pre, offset);
  return SnapVertices(sources, exclude, pre, offset);
}

bool Editor::ConnectSelection()
{
  if (!course_ || selected_ < 0 || Connectors(selected_).empty())
  {
    Status("Connect: select a road piece (a model with road edges) first.", true);
    return false;
  }
  std::vector<int> exclude = Linked(selected_);
  exclude.push_back(selected_);
  const auto& list = course_->instances();
  // Nearest pair (open edge of the selection, open fitting edge of another road).
  float best = 1e30f;
  Connector src, dst;
  int target = -1;
  const auto all = AllConnectors();
  std::vector<Connector> mine;
  for (auto& c : WorldConnectors(selected_))
    if (ConnectorOpen(selected_, c, all))
      mine.push_back(std::move(c));
  for (const auto& [j, to] : all)
  {
    if (std::find(exclude.begin(), exclude.end(), j) != exclude.end())
      continue;
    for (const auto& f : mine)
    {
      const float d = glm::distance(f.centre, to.centre);
      if (d < best && ConnectorsFit(f, to, 1.0f, 1e30f) && ConnectorOpen(j, to, all))
        best = d, src = f, dst = to, target = j;
    }
  }
  if (target < 0)
  {
    Status("Connect: no open road edge of a similar width found.", true);
    return false;
  }
  const glm::mat4 T = ConnectTransform(src, dst);
  Vec3 pos, rot, scl;
  if (!DecomposeMatrix(FromGlm(T * ToGlm(list[selected_].matrix)), pos, rot, scl))
    return false;
  Transform t;
  t.position = pos;
  t.rotation = {DegToAngle(rot[0]), DegToAngle(rot[1]), DegToAngle(rot[2])};
  t.scale = scl;
  t.matrix = ComposeMatrix(t.position, t.rotation, t.scale);
  // The quantised angles turn the piece a tiny bit: put the edge centre back exactly.
  const glm::mat4 pre = ToGlm(t.matrix) * glm::inverse(ToGlm(list[selected_].matrix));
  for (const auto& c : WorldConnectors(selected_, pre))
    if (glm::distance(c.centre, dst.centre) < 50)
    {
      const glm::vec3 d = dst.centre - c.centre;
      for (int k = 0; k < 3; ++k)
        t.position[k] += d[k];
      t.matrix = ComposeMatrix(t.position, t.rotation, t.scale);
      break;
    }
  BeginEdit();
  ApplyPrimary(t);
  EndEdit();
  snap_line_ = {true, src.centre, dst.centre, dst.lines};
  Status("Connected " + list[selected_].name + " to " + list[target].name + " (#" + std::to_string(target) +
         "). Ctrl+Z undoes.");
  return true;
}

// ---------------------------------------------------------------------------
// Track spline

void Editor::RefreshHandles()
{
  TrackHandle keep = handle_ >= 0 && handle_ < int(handles_.size()) ? handles_[handle_] : TrackHandle{};
  handles_ = course_ ? course_->TrackHandles() : std::vector<TrackHandle>{};
  handle_ = -1;
  for (int i = 0; i < int(handles_.size()); ++i)
    if (handles_[i].segment == keep.segment && std::fabs(handles_[i].time - keep.time) < 1e-4f)
      handle_ = i;
}

void Editor::SetTrackMode(bool on)
{
  track_mode_ = on && course_;
  handle_ = -1;
  armed_ = -1;
  if (track_mode_)
  {
    selected_ = -1;
    show_track_ = true;
    Status("Track editing: click a point on the track line, move it with the gizmo. "
           "Checkpoints, distances and the checkpoint grid follow; road models and walls do not.");
  }
}

void Editor::ApplyCurves(int segment, const Curves& c)
{
  course_->SetCurves(segment, c);
  course_->UpdateTrack();
  RefreshHandles();
  dirty_ = true;
}

void Editor::ResetTrack()
{
  UndoEntry e;
  for (int s = 0; s < int(course_->segments().size()); ++s)
  {
    const auto& seg = course_->segments()[s];
    if (seg.curves != seg.curves_orig)
      e.curves.push_back({s, seg.curves, seg.curves_orig});
  }
  for (const auto& it : e.curves)
    ApplyCurves(it.segment, it.after);
  if (!e.curves.empty())
  {
    undo_.push_back(std::move(e));
    redo_.clear();
  }
}

// ---------------------------------------------------------------------------
// Structures (placing copies of object groups)

void Editor::BuildStructures()
{
  structures_.clear();
  const auto& list = course_->instances();
  const bool link = link_;
  link_ = true;  // groups are found the same way as linked objects
  std::map<std::string, int> seen;  // signature -> structure
  for (int i = 0; i < int(list.size()); ++i)
  {
    const auto& in = list[i];
    if (in.added || in.name.empty() || IsBackground(i))
      continue;
    std::vector<int> members{i};
    for (int j : Linked(i))
      if (!list[j].added)
        members.push_back(j);
    // The same group seen from another member: identical sorted name list.
    std::vector<std::string> names;
    for (int m : members)
      names.push_back(list[m].name);
    std::sort(names.begin(), names.end());
    std::string sig;
    for (const auto& n : names)
      sig += n + '|';
    if (auto it = seen.find(sig); it != seen.end())
    {
      // Count each occurrence once (only from its lowest-index member).
      if (*std::min_element(members.begin(), members.end()) == i)
        ++structures_[it->second].uses;
      continue;
    }
    Structure st;
    st.members = members;
    // Only special zones (boost etc.) come along; types 10-13 make up the bulk of
    // all polygons (walls/borders, ~13,000 over all courses) and stay with the track.
    for (int p : AttachedPolys(members, true))
    {
      const auto& sf = course_->polys()[p].surfaces;
      if (std::none_of(sf.begin(), sf.end(), [](int t) { return t >= 10; }))
        st.polys.push_back(p);
    }
    st.uses = 1;
    const glm::vec3 pos(in.matrix[3], in.matrix[7], in.matrix[11]);
    const bool at_origin = IsWorldPiece(i);
    st.anchor = pos;
    if (at_origin)
    {
      // World-space road pieces: anchor at the bottom centre of their bounds.
      if (const ModelMesh* m = models_->Find(in.name); m && !m->positions.empty())
      {
        const glm::mat4 M = ToGlm(in.matrix);
        const glm::vec3 a = glm::vec3(M * glm::vec4(m->bounds_min[0], m->bounds_min[1], m->bounds_min[2], 1));
        const glm::vec3 b = glm::vec3(M * glm::vec4(m->bounds_max[0], m->bounds_max[1], m->bounds_max[2], 1));
        const glm::vec3 lo = glm::min(a, b), hi = glm::max(a, b);
        st.anchor = {(lo.x + hi.x) * 0.5f, lo.y, (lo.z + hi.z) * 0.5f};
      }
    }
    st.category = at_origin ? 3 : !st.polys.empty() ? 0 : members.size() > 1 ? 1 : 2;
    st.label = in.name;
    if (members.size() > 1)
      st.label += " + " + std::to_string(members.size() - 1);
    seen[sig] = int(structures_.size());
    structures_.push_back(std::move(st));
  }
  link_ = link;
  std::stable_sort(structures_.begin(), structures_.end(), [](const Structure& a, const Structure& b) {
    return a.category != b.category ? a.category < b.category : a.label < b.label;
  });
}

std::vector<int> Editor::Spawn(const std::vector<int>& members, const std::vector<int>& polys, const glm::vec3& offset)
{
  UndoEntry e;
  std::vector<int> made, made_polys;
  for (int m : members)
  {
    const int n = course_->AddInstance(m);
    auto& in = course_->instances()[n];
    for (int k = 0; k < 3; ++k)
    {
      in.position[k] += offset[k];
      in.matrix[k * 4 + 3] += offset[k];
    }
    original_.push_back(Get(n));
    background_.push_back(IsBackground(m));
    made.push_back(n);
    e.alive.push_back({n, false, true});
  }
  for (int p : polys)
  {
    const int q = course_->AddPoly(p);
    for (auto& v : course_->polys()[q].v)
      for (int k = 0; k < 3; ++k)
        v[k] += offset[k];
    original_polys_.push_back(course_->polys()[q].v);
    made_polys.push_back(q);
    e.alive.push_back({q, true, true});
  }
  for (int n : made)
    spawn_groups_[n] = {made, made_polys};
  collision_dirty_ = !polys.empty() || collision_dirty_;
  dirty_ = true;
  undo_.push_back(std::move(e));
  redo_.clear();
  if (!made.empty())
  {
    selected_ = made.front();
    scroll_to_selection_ = true;
  }
  return made;
}

void Editor::PlaceStructure(int s, const glm::vec3& offset)
{
  const Structure& st = structures_.at(std::size_t(s));
  Spawn(st.members, st.polys, offset);
  Status("Placed " + st.label + (st.polys.empty() ? "" : " with " + std::to_string(st.polys.size()) + " collision polygons") +
         ". Ctrl+Z undoes, Del deletes it again.");
}

void Editor::DuplicateSelection()
{
  if (!course_ || selected_ < 0)
    return;
  std::vector<int> members{selected_};
  for (int j : Linked(selected_))
    members.push_back(j);
  const std::vector<int> polys = AttachedPolys(members);
  // Shift the copy sideways (camera right) by the object's width so it does not overlap.
  float width = 20;
  if (const ModelMesh* m = models_->Find(course_->instances()[selected_].name); m && !m->positions.empty())
    width = std::max(width, glm::length(glm::vec3(ToGlm(course_->instances()[selected_].matrix) *
                                                  glm::vec4(m->bounds_max[0] - m->bounds_min[0], 0,
                                                            m->bounds_max[2] - m->bounds_min[2], 0))));
  const glm::vec3 right = glm::normalize(glm::cross(cam_.Forward(), glm::vec3(0, 1, 0)));
  Spawn(members, polys, right * (width * 1.1f));
  Status("Duplicated " + std::to_string(members.size()) + " object(s)" +
         (polys.empty() ? "" : " and " + std::to_string(polys.size()) + " collision polygons") + ".");
}

void Editor::DeleteSelection()
{
  if (!course_ || selected_ < 0)
    return;
  if (!course_->instances()[selected_].added)
  {
    Status("Only objects placed in this session can be deleted (original objects cannot be removed yet).", true);
    return;
  }
  UndoEntry e;
  auto it = spawn_groups_.find(selected_);
  std::vector<int> objects = it != spawn_groups_.end() ? it->second.first : std::vector<int>{selected_};
  const std::vector<int> polys = it != spawn_groups_.end() ? it->second.second : std::vector<int>{};
  for (int i : objects)
    if (Alive(i))
      e.alive.push_back({i, false, false});
  for (int p : polys)
    if (!course_->polys()[p].removed)
      e.alive.push_back({p, true, false});
  for (const auto& a : e.alive)
    SetAlive(a, false);
  undo_.push_back(std::move(e));
  redo_.clear();
  selected_ = -1;
  Status("Deleted " + std::to_string(objects.size()) + " placed object(s).");
}

void Editor::SetAlive(const AliveItem& item, bool alive)
{
  if (item.poly)
  {
    course_->polys()[item.index].removed = !alive;
    collision_dirty_ = true;
  }
  else
  {
    course_->instances()[item.index].removed = !alive;
    if (!alive && selected_ == item.index)
      selected_ = -1;
  }
  dirty_ = true;
}

bool Editor::Save()
{
  if (!course_)
    return false;
  std::string msg;
  const bool ok = project_.SaveCourse(course_id_, *course_, msg);
  Status(msg, !ok);
  if (ok)
  {
    dirty_ = false;
    RefreshCourses();
  }
  return ok;
}

void Editor::Launch()
{
  if (dirty_ && !Save())
    return;
  if (!ApplyCups())
    return;
  std::string msg;
  const bool quick = project_.settings().quickstart && project_.settings().launcher == "glinux";
  const bool ok = project_.Launch(course_id_, msg);
  Status(!ok      ? msg
         : quick ? "Starting the game directly on course " + std::to_string(course_id_) +
                       " (Practice). The first time it navigates the menus automatically for about 30 s. " + msg
                 : "Starting the game from the mod folder, pick the course in the game. " + msg,
         !ok);
}

void Editor::DebugStart(int course, int select, bool collision, int place, int handle, float lift)
{
  if (course >= 0)
    OpenCourse(course);
  if (course_ && handle >= 0 && handle < int(handles_.size()))
  {
    SetTrackMode(true);
    handle_ = handle;
    const TrackHandle h = handles_[handle];
    const Curves before = course_->segments()[h.segment].curves;
    course_->MoveHandle(h, before, {0, lift, 0});
    UndoEntry e;
    e.curves.push_back({h.segment, before, course_->segments()[h.segment].curves});
    ApplyCurves(h.segment, e.curves.back().after);
    undo_.push_back(std::move(e));
    const Vec3 p = course_->HandlePosition(handles_[handle_]);
    cam_.yaw = -90;
    cam_.pitch = -25;
    cam_.pos = glm::vec3(p[0], p[1], p[2]) - cam_.Forward() * 900.0f;
    return;
  }
  if (course_ && place >= 0 && place < int(structures_.size()))
  {
    // Like a click next to the original; magnetism applies as in the 3D view.
    glm::vec3 offset(60, 0, 0);
    if (const char* env = std::getenv("FZGX_PLACE_OFFSET"))
      std::sscanf(env, "%f %f %f", &offset.x, &offset.y, &offset.z);
    if (glm::vec3 snap; MagnetActive() && Snap(structures_[place].members, {}, glm::translate(glm::mat4(1), offset), snap))
    {
      std::printf("magnet: offset %.2f %.2f %.2f snapped by %.2f %.2f %.2f\n", offset.x, offset.y, offset.z, snap.x, snap.y, snap.z);
      offset += snap;
    }
    PlaceStructure(place, offset);
    open_structures_tab_ = true;
    FocusSelection();
    return;
  }
  if (course_ && select >= 0 && select < int(course_->instances().size()))
  {
    selected_ = select;
    scroll_to_selection_ = true;
    FocusSelection();
  }
  show_collision_ |= collision;
}

void Editor::DebugScale(float factor)
{
  if (!course_ || selected_ < 0)
    return;
  std::vector<int> objs{selected_};
  for (int j : Linked(selected_))
    objs.push_back(j);
  auto centre = [&](int i) { return glm::vec3(ToGlm(course_->instances()[i].matrix) * glm::vec4(LocalCenter(i), 1)); };
  std::vector<glm::vec3> before;
  for (int i : objs)
    before.push_back(centre(i));
  BeginEdit();
  ApplyPrimary(ScaledInPlace(selected_, Get(selected_), {factor, factor, factor}), true);
  EndEdit();
  for (std::size_t k = 0; k < objs.size(); ++k)
  {
    const glm::vec3 a = before[k], b = centre(objs[k]);
    std::printf("scale: #%d %s centre %.2f %.2f %.2f -> %.2f %.2f %.2f, scale x %.3f\n", objs[k],
                course_->instances()[objs[k]].name.c_str(), a.x, a.y, a.z, b.x, b.y, b.z,
                course_->instances()[objs[k]].scale[0]);
  }
}

void Editor::DebugConnect(float yaw)
{
  if (!course_ || selected_ < 0)
    return;
  DuplicateSelection();
  Transform t = Get(selected_);
  t.rotation[1] = std::int16_t(t.rotation[1] + DegToAngle(yaw));
  t.matrix = ComposeMatrix(t.position, t.rotation, t.scale);
  BeginEdit();
  ApplyPrimary(t);
  EndEdit();
  if (!ConnectSelection())
  {
    std::printf("connect: %s\n", status_.c_str());
    return;
  }
  // Report how well the joined edges match.
  float best = 1e30f, facing = 0;
  for (const auto& a : WorldConnectors(selected_))
    for (int j = 0; j < int(course_->instances().size()); ++j)
      if (j != selected_ && !course_->instances()[j].removed)
        for (const auto& b : WorldConnectors(j))
          if (const float d = glm::distance(a.centre, b.centre); d < best)
            best = d, facing = glm::dot(a.normal, b.normal);
  std::printf("connect: %s\nconnect: edge distance %.3f, normal dot %.4f\n", status_.c_str(), best, facing);
  FocusSelection();
}

void Editor::RequestQuit()
{
  if (dirty_)
    confirm_quit_ = true;
  else
    quit_ = true;
}

// ---------------------------------------------------------------------------
// Camera helpers

void Editor::FrameCourse()
{
  if (!course_)
    return;
  glm::vec3 mn(1e30f), mx(-1e30f);
  bool any = false;
  for (const auto& cp : course_->checkpoints())
  {
    mn = glm::min(mn, glm::vec3(cp.start[0], cp.start[1], cp.start[2]));
    mx = glm::max(mx, glm::vec3(cp.start[0], cp.start[1], cp.start[2]));
    any = true;
  }
  if (!any)
    for (const auto& in : course_->instances())
    {
      mn = glm::min(mn, glm::vec3(in.position[0], in.position[1], in.position[2]));
      mx = glm::max(mx, glm::vec3(in.position[0], in.position[1], in.position[2]));
    }
  const glm::vec3 c = (mn + mx) * 0.5f;
  const float r = std::max(200.0f, glm::length(mx - mn) * 0.6f);
  cam_.yaw = -90;
  cam_.pitch = -45;
  cam_.pos = c - cam_.Forward() * r;
  cam_.speed = std::clamp(r * 0.4f, 200.0f, 5000.0f);
}

void Editor::FocusSelection()
{
  if (!course_ || selected_ < 0)
    return;
  const auto& in = course_->instances()[selected_];
  glm::vec3 c(in.matrix[3], in.matrix[7], in.matrix[11]);
  float r = 30;
  if (const ModelMesh* m = models_->Find(in.name))
  {
    const glm::mat4 M = ToGlm(in.matrix);
    const glm::vec3 a(m->bounds_min[0], m->bounds_min[1], m->bounds_min[2]);
    const glm::vec3 b(m->bounds_max[0], m->bounds_max[1], m->bounds_max[2]);
    c = glm::vec3(M * glm::vec4((a + b) * 0.5f, 1));
    r = std::max(20.0f, glm::length(glm::vec3(M * glm::vec4(b - a, 0))) * 0.5f);
  }
  cam_.pos = c - cam_.Forward() * (r * 2.5f + 20);
}

int Editor::Pick(const glm::vec3& o, const glm::vec3& d, float* t_out, bool background)
{
  int best = -1;
  float best_t = 1e30f;
  const auto& list = course_->instances();
  for (int i = 0; i < int(list.size()); ++i)
  {
    const auto& in = list[i];
    if (in.removed || (IsBackground(i) && (!background || !show_background_)))
      continue;  // ground/sky planes would swallow every click
    const ModelMesh* mm = models_->Find(in.name);
    const bool solid = mm && !mm->positions.empty();
    const glm::mat4 M = solid || (mm && mm->skinned) ? ToGlm(in.matrix) : WithoutScale(ToGlm(in.matrix));
    const glm::mat4 inv = glm::inverse(M);
    const glm::vec3 lo = glm::vec3(inv * glm::vec4(o, 1));
    const glm::vec3 ld = glm::vec3(inv * glm::vec4(d, 0));
    const ModelMesh* m = mm;
    const bool has_tris = solid;
    if (!has_tris && !show_missing_)
      continue;
    if (has_tris && !show_models_)
      continue;
    const glm::vec3 mn = m ? glm::vec3(m->bounds_min[0], m->bounds_min[1], m->bounds_min[2]) : kMissingMin;
    const glm::vec3 mx = m ? glm::vec3(m->bounds_max[0], m->bounds_max[1], m->bounds_max[2]) : kMissingMax;
    float t;
    if (!RayBox(lo, ld, mn, mx, t))
      continue;
    if (!has_tris)
    {
      // ld is not normalised, so t is in world units along d.
      if (t < best_t)
        best_t = t, best = i;
      continue;
    }
    for (std::size_t k = 0; k + 2 < m->positions.size(); k += 3)
    {
      const auto& a = m->positions[k];
      const auto& b = m->positions[k + 1];
      const auto& c = m->positions[k + 2];
      if (RayTriangle(lo, ld, {a[0], a[1], a[2]}, {b[0], b[1], b[2]}, {c[0], c[1], c[2]}, t) && t < best_t)
        best_t = t, best = i;
    }
  }
  if (t_out)
    *t_out = best_t;
  return best;
}

bool Editor::RayHit(const glm::vec3& o, const glm::vec3& d, glm::vec3& hit)
{
  float best = 1e30f;
  if (show_models_)
  {
    float t;
    const int i = Pick(o, d, &t, true);
    // Placeholder boxes are no surface to place on.
    if (i >= 0)
      if (const ModelMesh* m = models_->Find(course_->instances()[i].name); m && !m->positions.empty())
        best = t;
  }
  const auto& tris = course_->collision().triangles;
  for (std::size_t k = 0; k + 2 < tris.size(); k += 3)
  {
    float t;
    const glm::vec3 a(tris[k][0], tris[k][1], tris[k][2]), b(tris[k + 1][0], tris[k + 1][1], tris[k + 1][2]),
        c(tris[k + 2][0], tris[k + 2][1], tris[k + 2][2]);
    if (RayTriangle(o, d, a, b, c, t) && t < best)
      best = t;
  }
  if (best >= 1e29f)
    return false;
  hit = o + d * best;
  return true;
}

// ---------------------------------------------------------------------------
// Frame

void Editor::Frame(int fb_w, int fb_h, float scale, float dt)
{
  ImGuiIO& io = ImGui::GetIO();
  const float menu_h = ImGui::GetFrameHeight();
  const float W = io.DisplaySize.x, H = io.DisplaySize.y;
  const float vx = kLeftWidth, vy = menu_h, vw = std::max(50.0f, W - kLeftWidth - kRightWidth),
              vh = std::max(50.0f, H - menu_h - kStatusHeight);

  // Global shortcuts
  if (!io.WantTextInput)
  {
    const bool ctrl = io.KeyCtrl;
    if (ctrl && ImGui::IsKeyPressed(ImGuiKey_S))
      Save();
    if (ctrl && ImGui::IsKeyPressed(ImGuiKey_Z) && !io.KeyShift)
      Undo();
    if (ctrl && (ImGui::IsKeyPressed(ImGuiKey_Y) || (io.KeyShift && ImGui::IsKeyPressed(ImGuiKey_Z))))
      Redo();
    if (ImGui::IsKeyPressed(ImGuiKey_F5) && course_)
      Launch();
    if (!look_active_ && !ctrl)
    {
      if (ImGui::IsKeyPressed(ImGuiKey_W))
        gizmo_op_ = 0;
      if (ImGui::IsKeyPressed(ImGuiKey_E))
        gizmo_op_ = 1;
      if (ImGui::IsKeyPressed(ImGuiKey_R))
        gizmo_op_ = 2;
      if (ImGui::IsKeyPressed(ImGuiKey_F))
        FocusSelection();
      if (ImGui::IsKeyPressed(ImGuiKey_T) && course_)
        SetTrackMode(!track_mode_);
      if (ImGui::IsKeyPressed(ImGuiKey_C) && course_ && !track_mode_ && selected_ >= 0)
        ConnectSelection();
      if (ImGui::IsKeyPressed(ImGuiKey_Escape))
      {
        if (armed_ >= 0)
          armed_ = -1;
        else if (track_mode_ && handle_ >= 0)
          handle_ = -1;
        else
          selected_ = -1;
      }
      if (ImGui::IsKeyPressed(ImGuiKey_Delete) && course_ && !track_mode_)
        DeleteSelection();
    }
    if (ctrl && ImGui::IsKeyPressed(ImGuiKey_D) && course_ && !track_mode_)
      DuplicateSelection();
  }

  snap_line_draw_ = snap_line_;
  snap_line_ = {};
  ImGuizmo::BeginFrame();
  DrawMenu();
  DrawCoursePanel(0, menu_h, kLeftWidth, H - menu_h - kStatusHeight);
  DrawInspector(W - kRightWidth, menu_h, kRightWidth, H - menu_h - kStatusHeight);
  DrawStatus(0, H - kStatusHeight, W);
  DrawSettings();
  DrawCups();
  DrawPopups();

  HandleViewportInput(vx, vy, vw, vh, dt);
  RenderViewport(fb_w, fb_h, scale, vx, vy, vw, vh);
}

void Editor::HandleViewportInput(float vx, float vy, float vw, float vh, float dt)
{
  ImGuiIO& io = ImGui::GetIO();
  const ImVec2 mp = io.MousePos;
  const bool over = mp.x >= vx && mp.x < vx + vw && mp.y >= vy && mp.y < vy + vh && !io.WantCaptureMouse;

  proj_ = glm::perspective(glm::radians(60.0f), vw / vh, 2.0f, 60000.0f);
  if (over && ImGui::IsMouseClicked(ImGuiMouseButton_Right))
    look_active_ = true;
  if (!ImGui::IsMouseDown(ImGuiMouseButton_Right))
    look_active_ = false;

  if (look_active_)
  {
    cam_.yaw += io.MouseDelta.x * 0.2f;
    cam_.pitch = std::clamp(cam_.pitch - io.MouseDelta.y * 0.2f, -89.0f, 89.0f);
    const glm::vec3 f = cam_.Forward();
    const glm::vec3 r = glm::normalize(glm::cross(f, glm::vec3(0, 1, 0)));
    float s = cam_.speed * dt * (io.KeyShift ? 4.0f : 1.0f) * (io.KeyCtrl ? 0.25f : 1.0f);
    if (ImGui::IsKeyDown(ImGuiKey_W)) cam_.pos += f * s;
    if (ImGui::IsKeyDown(ImGuiKey_S)) cam_.pos -= f * s;
    if (ImGui::IsKeyDown(ImGuiKey_D)) cam_.pos += r * s;
    if (ImGui::IsKeyDown(ImGuiKey_A)) cam_.pos -= r * s;
    if (ImGui::IsKeyDown(ImGuiKey_E)) cam_.pos.y += s;
    if (ImGui::IsKeyDown(ImGuiKey_Q)) cam_.pos.y -= s;
    if (io.MouseWheel != 0)
      cam_.speed = std::clamp(cam_.speed * (io.MouseWheel > 0 ? 1.25f : 0.8f), 20.0f, 20000.0f);
  }
  else if (over)
  {
    if (io.MouseWheel != 0)
      cam_.pos += cam_.Forward() * io.MouseWheel * cam_.speed * 0.15f;
    if (ImGui::IsMouseDragging(ImGuiMouseButton_Middle))
    {
      const glm::vec3 f = cam_.Forward();
      const glm::vec3 r = glm::normalize(glm::cross(f, glm::vec3(0, 1, 0)));
      const glm::vec3 u = glm::cross(r, f);
      const float k = cam_.speed * 0.002f;
      cam_.pos += (-r * io.MouseDelta.x + u * io.MouseDelta.y) * k;
    }
  }
  view_ = cam_.View();

  const float nx = (mp.x - vx) / vw * 2 - 1, ny = 1 - (mp.y - vy) / vh * 2;
  const glm::mat4 inv = glm::inverse(proj_ * view_);
  const glm::vec4 a = inv * glm::vec4(nx, ny, -1, 1), b = inv * glm::vec4(nx, ny, 1, 1);
  const glm::vec3 ray_o = glm::vec3(a) / a.w, ray_d = glm::normalize(glm::vec3(b) / b.w - ray_o);

  // Placing a structure: preview under the mouse, left click places it
  // (Shift keeps the tool for the next one), Esc / right click cancels.
  hover_valid_ = false;
  if (course_ && armed_ >= 0)
  {
    if (over && !look_active_)
    {
      hover_valid_ = RayHit(ray_o, ray_d, hover_);
      if (!hover_valid_)
      {
        hover_ = ray_o + ray_d * 300.0f;
        hover_valid_ = true;
      }
      const Structure& st = structures_[armed_];
      place_offset_ = hover_ - st.anchor;
      if (glm::vec3 snap; MagnetActive() && Snap(st.members, {}, glm::translate(glm::mat4(1), place_offset_), snap))
        place_offset_ += snap;
      if (ImGui::IsMouseClicked(ImGuiMouseButton_Left))
      {
        PlaceStructure(armed_, place_offset_);
        if (!io.KeyShift)
          armed_ = -1;
      }
    }
    return;
  }

  // Track mode: click picks the nearest track point on screen.
  if (track_mode_)
  {
    if (over && course_ && ImGui::IsMouseClicked(ImGuiMouseButton_Left) && !(handle_ >= 0 && ImGuizmo::IsOver()))
    {
      const glm::mat4 vp = proj_ * view_;
      float best = 14;  // pixels
      int pick = -1;
      for (int i = 0; i < int(handles_.size()); ++i)
      {
        const Vec3 p = course_->HandlePosition(handles_[i]);
        const glm::vec4 c = vp * glm::vec4(p[0], p[1], p[2], 1);
        if (c.w <= 0)
          continue;
        const float sx = vx + (c.x / c.w * 0.5f + 0.5f) * vw, sy = vy + (0.5f - c.y / c.w * 0.5f) * vh;
        const float d = std::hypot(sx - mp.x, sy - mp.y);
        if (d < best)
          best = d, pick = i;
      }
      handle_ = pick;
    }
    return;
  }

  // Click to select (unless the gizmo takes the click).
  if (over && course_ && ImGui::IsMouseClicked(ImGuiMouseButton_Left) && !ImGuizmo::IsOver())
  {
    selected_ = Pick(ray_o, ray_d);
    scroll_to_selection_ = true;
  }
}

void Editor::RenderViewport(int fb_w, int fb_h, float scale, float vx, float vy, float vw, float vh)
{
  const int px = int(vx * scale), pw = int(vw * scale), ph = int(vh * scale);
  const int py = fb_h - int((vy + vh) * scale);
  (void)fb_w;
  const glm::mat4 vp = proj_ * view_;
  renderer_.BeginFrame(px, py, pw, ph, vp, cam_.pos);

  std::vector<LineVertex> lines;
  // Ground grid
  {
    const float step = 500;
    const glm::vec3 c = glm::floor(cam_.pos / step) * step;
    for (int i = -20; i <= 20; ++i)
    {
      const std::uint32_t col = i == 0 ? Rgba(70, 75, 90) : Rgba(40, 44, 55);
      lines.push_back({{c.x + i * step, 0, c.z - 20 * step}, col});
      lines.push_back({{c.x + i * step, 0, c.z + 20 * step}, col});
      lines.push_back({{c.x - 20 * step, 0, c.z + i * step}, col});
      lines.push_back({{c.x + 20 * step, 0, c.z + i * step}, col});
    }
  }
  renderer_.DrawLines(lines);
  lines.clear();

  if (course_ && collision_dirty_)
  {
    course_->RebuildCollision();
    renderer_.Free(collision_mesh_);
    std::vector<std::uint32_t> colors;
    for (int s : course_->collision().surface)
      colors.push_back(SurfaceColor(s));
    collision_mesh_ = renderer_.UploadColored(course_->collision().triangles, colors);
    collision_dirty_ = false;
  }
  if (course_)
  {
    const auto& list = course_->instances();
    const std::vector<int> linked_now = Linked(selected_);
    for (int i = 0; i < int(list.size()); ++i)
    {
      const auto& in = list[i];
      if (in.removed)
        continue;
      const glm::mat4 M = ToGlm(in.matrix);
      const GpuMesh& g = gpu_[in.name];
      const bool sel = i == selected_ || (selected_ >= 0 && std::find(linked_now.begin(), linked_now.end(), i) != linked_now.end());
      if (g.count)
      {
        const bool bg = IsBackground(i);
        if (show_models_ && (!bg || show_background_ || sel))
          renderer_.Draw(g, M, bg ? glm::vec4(0.22f, 0.24f, 0.3f, 1) : glm::vec4(1),
                         sel ? glm::vec4(1.0f, 0.55f, 0.1f, 0.45f) : glm::vec4(0));
      }
      else if (show_missing_)
      {
        const ModelMesh* m = models_->Find(in.name);
        glm::vec3 mn = kMissingMin, mx = kMissingMax;
        if (m && m->skinned)
          mn = {m->bounds_min[0], m->bounds_min[1], m->bounds_min[2]},
          mx = {m->bounds_max[0], m->bounds_max[1], m->bounds_max[2]};
        AddBox(lines, m && m->skinned ? M : WithoutScale(M), mn, mx,
               m && m->skinned ? Rgba(120, 200, 255) : Rgba(255, 90, 90));
      }
    }
    if (show_collision_)
    {
      glEnable(GL_POLYGON_OFFSET_FILL);
      glPolygonOffset(-1, -1);
      renderer_.Draw(collision_mesh_, glm::mat4(1), glm::vec4(1, 1, 1, 0.75f));
      glDisable(GL_POLYGON_OFFSET_FILL);
    }
    if (show_track_)
    {
      const auto& cps = course_->checkpoints();
      auto v3 = [](const Vec3& v) { return glm::vec3(v[0], v[1], v[2]); };
      for (std::size_t k = 0; k < cps.size(); ++k)
      {
        const auto& cp = cps[k];
        const glm::vec3 a = v3(cp.start), b = v3(cp.end);
        // Centre line, track edges (width along the segment's x axis) and a cross line.
        const glm::vec3 sa = v3(cp.side0) * (cp.width * 0.5f), sb = v3(cp.side1) * (cp.width * 0.5f);
        lines.push_back({a, Rgba(255, 220, 40)});
        lines.push_back({b, Rgba(255, 220, 40)});
        const std::uint32_t edge = Rgba(60, 200, 255, 200);
        lines.push_back({a - sa, edge});
        lines.push_back({b - sb, edge});
        lines.push_back({a + sa, edge});
        lines.push_back({b + sb, edge});
        const std::uint32_t col = k == 0 ? Rgba(80, 255, 120) : Rgba(255, 220, 40, 110);
        lines.push_back({a - sa, col});
        lines.push_back({a + sa, col});
      }
      if (track_mode_)
        for (int i = 0; i < int(handles_.size()); ++i)
        {
          const glm::vec3 p = v3(course_->HandlePosition(handles_[i]));
          const float r = std::max(2.0f, glm::distance(p, cam_.pos) * 0.008f);
          AddBox(lines, glm::translate(glm::mat4(1), p), glm::vec3(-r), glm::vec3(r),
                 i == handle_ ? Rgba(255, 255, 255) : Rgba(255, 140, 40));
        }
    }
    if (show_edges_ && !track_mode_)
    {
      // Road edges: open ones orange, connected ones green, those of the selection brighter.
      const auto all = AllConnectors();
      for (const auto& [i, c] : all)
      {
        const bool sel = i == selected_;
        {
          const bool open = ConnectorOpen(i, c, all);
          const std::uint32_t col = open ? (sel ? Rgba(255, 170, 40) : Rgba(255, 120, 30, 220))
                                         : (sel ? Rgba(120, 255, 140) : Rgba(60, 200, 90, 150));
          for (const auto& p : c.lines)
            lines.push_back({p + c.normal * 0.3f, col});
          if (open && sel)
          {
            lines.push_back({c.centre, col});
            lines.push_back({c.centre + c.normal * (c.width * 0.25f), col});
          }
        }
      }
    }
    if (snap_line_draw_.valid)
    {
      for (const auto& p : snap_line_draw_.edges)
        lines.push_back({p, Rgba(255, 60, 255)});
      // Magnetism: the vertex that snapped and its target.
      const glm::vec3 a = snap_line_draw_.from, b = snap_line_draw_.to;
      const float r = std::max(1.5f, glm::distance(b, cam_.pos) * 0.006f);
      lines.push_back({a, Rgba(255, 60, 255)});
      lines.push_back({b, Rgba(255, 60, 255)});
      AddBox(lines, glm::translate(glm::mat4(1), b), glm::vec3(-r), glm::vec3(r), Rgba(255, 60, 255));
    }
    if (armed_ >= 0 && hover_valid_)
    {
      // Preview of the structure being placed.
      const Structure& st = structures_[armed_];
      const glm::vec3 off = place_offset_;
      const glm::mat4 T = glm::translate(glm::mat4(1), off);
      for (int m : st.members)
      {
        const auto& in = list[m];
        const GpuMesh& g = gpu_[in.name];
        if (g.count)
          renderer_.Draw(g, T * ToGlm(in.matrix), glm::vec4(1), glm::vec4(0.2f, 0.9f, 0.4f, 0.5f));
        else
          AddBox(lines, T * WithoutScale(ToGlm(in.matrix)), kMissingMin, kMissingMax, Rgba(80, 255, 120));
      }
      for (int p : st.polys)
      {
        const auto& v = course_->polys()[p].v;
        for (std::size_t k = 0; k < v.size(); ++k)
        {
          const glm::vec3 a = glm::vec3(v[k][0], v[k][1] + 0.3f, v[k][2]) + off;
          const glm::vec3 b = glm::vec3(v[(k + 1) % v.size()][0], v[(k + 1) % v.size()][1] + 0.3f,
                                        v[(k + 1) % v.size()][2]) + off;
          lines.push_back({a, Rgba(255, 235, 0)});
          lines.push_back({b, Rgba(255, 235, 0)});
        }
      }
      // Cross marking the spot.
      for (int k = 0; k < 3; ++k)
      {
        glm::vec3 e(0);
        e[k] = 15;
        lines.push_back({hover_ - e, Rgba(80, 255, 120)});
        lines.push_back({hover_ + e, Rgba(80, 255, 120)});
      }
    }
    if (selected_ >= 0)
    {
      // Outline the collision polygons that move with the selection (e.g. the boost zone).
      std::vector<int> members = linked_now;
      members.push_back(selected_);
      for (int p : AttachedPolys(members))
      {
        const auto& v = course_->polys()[p].v;
        for (std::size_t k = 0; k < v.size(); ++k)
        {
          const auto& a = v[k];
          const auto& b = v[(k + 1) % v.size()];
          lines.push_back({{a[0], a[1] + 0.3f, a[2]}, Rgba(255, 235, 0)});
          lines.push_back({{b[0], b[1] + 0.3f, b[2]}, Rgba(255, 235, 0)});
        }
      }
      const auto& in = list[selected_];
      const ModelMesh* m = models_->Find(in.name);
      if (m)
        AddBox(lines, ToGlm(in.matrix), {m->bounds_min[0], m->bounds_min[1], m->bounds_min[2]},
               {m->bounds_max[0], m->bounds_max[1], m->bounds_max[2]}, Rgba(255, 150, 30));
    }
  }
  renderer_.DrawLines(lines, false);
  glViewport(0, 0, fb_w, fb_h);

  // Track point gizmo: moves the selected key of the position curves.
  if (course_ && track_mode_ && handle_ >= 0 && handle_ < int(handles_.size()))
  {
    ImGuizmo::SetDrawlist(ImGui::GetBackgroundDrawList());
    ImGuizmo::SetRect(vx, vy, vw, vh);
    const TrackHandle h = handles_[handle_];
    const Vec3 p = course_->HandlePosition(h);
    glm::mat4 M = glm::translate(glm::mat4(1), glm::vec3(p[0], p[1], p[2]));
    float snap[3] = {snap_move_, snap_move_, snap_move_};
    const bool changed = ImGuizmo::Manipulate(glm::value_ptr(view_), glm::value_ptr(proj_), ImGuizmo::TRANSLATE,
                                              ImGuizmo::WORLD, glm::value_ptr(M), nullptr, snap_ ? snap : nullptr);
    const bool using_now = ImGuizmo::IsUsing();
    if (using_now && !gizmo_was_using_)
    {
      handle_base_ = course_->segments()[h.segment].curves;
      handle_start_ = p;
    }
    if (changed && using_now)
    {
      Vec3 delta = {M[3][0] - handle_start_[0], M[3][1] - handle_start_[1], M[3][2] - handle_start_[2]};
      if (MagnetActive())
      {
        // Snap onto another track point (e.g. to join two segments).
        const glm::vec3 target(M[3]);
        float best = project_.settings().magnet_radius;
        for (int i = 0; i < int(handles_.size()); ++i)
        {
          if (i == handle_)
            continue;
          const Vec3 q = course_->HandlePosition(handles_[i]);
          const float d = glm::distance(target, glm::vec3(q[0], q[1], q[2]));
          if (d < best)
          {
            best = d;
            delta = {q[0] - handle_start_[0], q[1] - handle_start_[1], q[2] - handle_start_[2]};
            snap_line_ = {true, target, glm::vec3(q[0], q[1], q[2]), {}};
          }
        }
      }
      course_->MoveHandle(h, handle_base_, delta);
      course_->UpdateTrack();
      dirty_ = true;
    }
    if (!using_now && gizmo_was_using_)
    {
      const Curves& now = course_->segments()[h.segment].curves;
      if (now != handle_base_)
      {
        UndoEntry e;
        e.curves.push_back({h.segment, handle_base_, now});
        undo_.push_back(std::move(e));
        redo_.clear();
      }
      RefreshHandles();
    }
    gizmo_was_using_ = using_now;
    return;
  }

  // Gizmo (drawn by ImGui on the background draw list, inside the viewport rect).
  if (course_ && selected_ >= 0 && !track_mode_)
  {
    ImGuizmo::SetDrawlist(ImGui::GetBackgroundDrawList());
    ImGuizmo::SetRect(vx, vy, vw, vh);
    // The gizmo sits at the model centre, so rotating and scaling keep the
    // object in place (track pieces have their origin far away at 0,0,0).
    const glm::vec3 pivot = LocalCenter(selected_);
    glm::mat4 M = ToGlm(course_->instances()[selected_].matrix) * glm::translate(glm::mat4(1), pivot);
    const ImGuizmo::OPERATION op = gizmo_op_ == 0 ? ImGuizmo::TRANSLATE
                                   : gizmo_op_ == 1 ? ImGuizmo::ROTATE
                                                    : ImGuizmo::SCALE;
    float snap[3];
    const float sv = gizmo_op_ == 0 ? snap_move_ : gizmo_op_ == 1 ? snap_rot_ : snap_scale_;
    snap[0] = snap[1] = snap[2] = sv;
    const bool changed = ImGuizmo::Manipulate(glm::value_ptr(view_), glm::value_ptr(proj_), op,
                                              gizmo_local_ || gizmo_op_ == 2 ? ImGuizmo::LOCAL : ImGuizmo::WORLD,
                                              glm::value_ptr(M), nullptr, snap_ ? snap : nullptr);
    const bool using_now = ImGuizmo::IsUsing();
    if (using_now && !gizmo_was_using_)
      BeginEdit();
    if (changed)
    {
      M = M * glm::translate(glm::mat4(1), -pivot);
      Vec3 pos, rot, scl;
      if (DecomposeMatrix(FromGlm(M), pos, rot, scl))
      {
        Transform t;
        t.position = pos;
        t.rotation = {DegToAngle(rot[0]), DegToAngle(rot[1]), DegToAngle(rot[2])};
        t.scale = scl;
        t.matrix = ComposeMatrix(t.position, t.rotation, t.scale);
        if (gizmo_op_ == 2)
        {
          // Only the scale changes: rebuild from the current transform so that
          // the quantised rotation cannot shift the centre.
          const Transform cur = Get(selected_);
          Vec3 f{1, 1, 1};
          for (int k = 0; k < 3; ++k)
            f[k] = std::fabs(cur.scale[k]) > 1e-6f ? std::fabs(scl[k] / cur.scale[k]) : 1.0f;  // decomposition may flip signs
          t = ScaledInPlace(selected_, cur, f);
        }
        if (gizmo_op_ == 0 && MagnetActive())
        {
          // Snap a vertex of the moved object onto a vertex of another one.
          std::vector<int> exclude = Linked(selected_);
          exclude.push_back(selected_);
          const glm::mat4 pre = ToGlm(t.matrix) * glm::inverse(ToGlm(course_->instances()[selected_].matrix));
          if (glm::vec3 snap; Snap({selected_}, exclude, pre, snap))
          {
            for (int k = 0; k < 3; ++k)
              t.position[k] += snap[k];
            t.matrix = ComposeMatrix(t.position, t.rotation, t.scale);
          }
        }
        ApplyPrimary(t, gizmo_op_ == 2);
      }
    }
    if (!using_now && gizmo_was_using_)
      EndEdit();
    gizmo_was_using_ = using_now;
  }
  else
    gizmo_was_using_ = false;
}

// ---------------------------------------------------------------------------
// UI panels

void Editor::DrawMenu()
{
  if (!ImGui::BeginMainMenuBar())
    return;
  if (ImGui::BeginMenu("File"))
  {
    if (ImGui::MenuItem("Save", "Ctrl+S", false, course_.has_value()))
      Save();
    if (ImGui::MenuItem("Test in game", "F5", false, course_.has_value()))
      Launch();
    ImGui::Separator();
    if (ImGui::MenuItem("Revert course to original", nullptr, false, course_.has_value()))
    {
      std::string msg;
      const bool ok = project_.RestoreCourse(course_id_, msg);
      Status(msg, !ok);
      RefreshCourses();
      OpenCourse(course_id_);
    }
    if (ImGui::MenuItem("Settings…"))
      show_settings_ = true;
    if (ImGui::MenuItem("Cups…"))
      show_cups_ = true;
    ImGui::Separator();
    if (ImGui::MenuItem("Quit"))
      RequestQuit();
    ImGui::EndMenu();
  }
  if (ImGui::BeginMenu("Edit"))
  {
    if (ImGui::MenuItem("Undo", "Ctrl+Z", false, !undo_.empty()))
      Undo();
    if (ImGui::MenuItem("Redo", "Ctrl+Y", false, !redo_.empty()))
      Redo();
    ImGui::Separator();
    if (ImGui::MenuItem("Edit track spline", "T", track_mode_, course_.has_value()))
      SetTrackMode(!track_mode_);
    if (ImGui::MenuItem("Reset track spline", nullptr, false, course_ && course_->track_modified()))
      ResetTrack();
    ImGui::Separator();
    if (ImGui::MenuItem("Duplicate selection", "Ctrl+D", false, selected_ >= 0))
      DuplicateSelection();
    if (ImGui::MenuItem("Delete placed object", "Del", false, selected_ >= 0 && course_->instances()[selected_].added))
      DeleteSelection();
    if (ImGui::MenuItem("Connect road to nearest open edge", "C", false, selected_ >= 0 && !track_mode_))
      ConnectSelection();
    ImGui::EndMenu();
  }
  if (ImGui::BeginMenu("View"))
  {
    ImGui::MenuItem("Models", nullptr, &show_models_);
    if (ImGui::MenuItem("Textures", nullptr, &project_.settings().textures))
    {
      UploadModels();
      project_.settings().Save();
    }
    ImGui::MenuItem("Background (ground/sky)", nullptr, &show_background_);
    ImGui::MenuItem("Placeholders (missing/animated models)", nullptr, &show_missing_);
    ImGui::MenuItem("Track path (checkpoints)", nullptr, &show_track_);
    ImGui::MenuItem("Collision (static, yellow = boost)", nullptr, &show_collision_);
    ImGui::MenuItem("Road edges (orange = open, green = connected)", nullptr, &show_edges_);
    ImGui::Separator();
    if (ImGui::MenuItem("Frame whole course"))
      FrameCourse();
    if (ImGui::MenuItem("Focus selection", "F", false, selected_ >= 0))
      FocusSelection();
    ImGui::EndMenu();
  }
  if (ImGui::BeginMenu("Help"))
  {
    ImGui::TextUnformatted(
        "Camera:    hold right mouse button + WASD/QE to fly, mouse wheel = speed\n"
        "           mouse wheel = forward/back, middle mouse button = pan\n"
        "Selection: left click in the 3D view or in the object list\n"
        "Gizmo:     W move, E rotate, R scale, F focus\n"
        "Track:     T toggles track editing: click a point on the track line,\n"
        "           move it with the gizmo (checkpoints follow, road models do not)\n"
        "Magnetism: road pieces snap edge to edge (orange = open road edge),\n"
        "           other objects onto nearby vertices, track points onto other\n"
        "           track points (Tool panel), hold Alt to move freely\n"
        "Connect:   C joins the selected road to the nearest open road edge\n"
        "           (moves and turns it)\n"
        "Place:     Structures tab, pick an entry, click in the 3D view\n"
        "           (Shift+click places several), Ctrl+D duplicate, Del delete\n"
        "File:      Ctrl+S save (into the mod folder), F5 test in game\n\n"
        "Note: road models are visuals only. The driving surface and physics come\n"
        "from the track spline (yellow line), which cannot be edited yet.\n\n"
        "AI GENERATED: this tool's source code and documentation were generated\n"
        "with AI (Claude by Anthropic) based on the maintainer's instructions.");
    ImGui::EndMenu();
  }
  ImGui::SameLine(ImGui::GetWindowWidth() - 360);
  if (course_)
    ImGui::TextDisabled("%s  |  %s", Project::CourseFile(course_id_).c_str(), dirty_ ? "unsaved *" : "saved");
  ImGui::EndMainMenuBar();
}

void Editor::DrawCoursePanel(float x, float y, float w, float h)
{
  ImGui::SetNextWindowPos({x, y});
  ImGui::SetNextWindowSize({w, h});
  ImGui::Begin("##left", nullptr, ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove |
                                      ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoBringToFrontOnFocus);
  ImGui::SeparatorText("Courses");
  if (ImGui::BeginListBox("##courses", {-1, std::min(220.0f, h * 0.35f)}))
  {
    for (const auto& c : courses_)
    {
      char label[96];
      std::snprintf(label, sizeof label, "%s %02d  %s##course%d", c.modified ? "*" : " ", c.id,
                    *CourseName(c.id) ? CourseName(c.id) : "(special)", c.id);
      if (ImGui::Selectable(label, c.id == course_id_) && c.id != course_id_)
      {
        if (dirty_)
          pending_course_ = c.id;
        else
          OpenCourse(c.id);
      }
      std::string cups;
      for (const auto& name : CupsOfCourse(c.id, cups_))
        cups += (cups.empty() ? "" : ", ") + name;
      if (!cups.empty())
      {
        ImGui::SameLine(ImGui::GetContentRegionAvail().x * 0.55f);
        ImGui::TextDisabled("%s", cups.c_str());
        if (ImGui::IsItemHovered())
          ImGui::SetTooltip("%s", cups.c_str());
      }
    }
    ImGui::EndListBox();
  }
  ImGui::TextDisabled("* = changed in the mod folder");

  if (!course_)
  {
    ImGui::SeparatorText("Objects");
    ImGui::TextDisabled("No course loaded.");
    ImGui::End();
    return;
  }
  if (ImGui::BeginTabBar("##lefttabs"))
  {
    if (ImGui::BeginTabItem("Objects"))
    {
      DrawObjectList();
      ImGui::EndTabItem();
    }
    if (ImGui::BeginTabItem("Structures", nullptr, open_structures_tab_ ? ImGuiTabItemFlags_SetSelected : 0))
    {
      DrawStructurePanel();
      ImGui::EndTabItem();
    }
    open_structures_tab_ = false;
    ImGui::EndTabBar();
  }
  ImGui::End();
}

void Editor::DrawObjectList()
{
  ImGui::SetNextItemWidth(-1);
  ImGui::InputTextWithHint("##filter", "Filter (name)…", &filter_);
  std::string needle = filter_;
  std::transform(needle.begin(), needle.end(), needle.begin(), ::toupper);
  std::vector<int> rows;
  const auto& list = course_->instances();
  int alive = 0;
  for (int i = 0; i < int(list.size()); ++i)
  {
    if (list[i].removed)
      continue;
    ++alive;
    if (needle.empty() || list[i].name.find(needle) != std::string::npos)
      rows.push_back(i);
  }
  ImGui::TextDisabled("%zu of %d", rows.size(), alive);
  ImGui::BeginChild("##objects", {-1, -1}, ImGuiChildFlags_Borders);
  ImGuiListClipper clip;
  clip.Begin(int(rows.size()));
  if (scroll_to_selection_ && selected_ >= 0)
  {
    auto it = std::find(rows.begin(), rows.end(), selected_);
    if (it != rows.end())
      clip.IncludeItemByIndex(int(it - rows.begin()));
  }
  while (clip.Step())
    for (int r = clip.DisplayStart; r < clip.DisplayEnd; ++r)
    {
      const int i = rows[r];
      const auto& in = list[i];
      const GpuMesh& g = gpu_[in.name];
      char label[160];
      std::snprintf(label, sizeof label, "%4d  %s%s", i, in.name.empty() ? "(unnamed)" : in.name.c_str(),
                    in.added ? "  +" : Modified(i) ? "  *" : "");
      if (!g.count)
        ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.6f, 0.6f, 0.65f, 1));
      if (ImGui::Selectable(label, i == selected_))
        selected_ = i;
      if (!g.count)
        ImGui::PopStyleColor();
      if (ImGui::IsItemHovered() && ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left))
        FocusSelection();
      if (scroll_to_selection_ && i == selected_)
      {
        ImGui::SetScrollHereY();
        scroll_to_selection_ = false;
      }
    }
  ImGui::EndChild();
  scroll_to_selection_ = false;
}

void Editor::DrawStructurePanel()
{
  if (armed_ >= 0)
  {
    ImGui::TextColored({0.4f, 1, 0.55f, 1}, "Placing: %s", structures_[armed_].label.c_str());
    ImGui::TextDisabled("Click in the 3D view. Shift+click\nplaces several, Esc cancels.");
    if (ImGui::Button("Cancel", {-1, 0}))
      armed_ = -1;
  }
  else
    ImGui::TextDisabled("Pick a structure, then click in the\n3D view to place a copy there.");
  ImGui::SetNextItemWidth(-1);
  ImGui::InputTextWithHint("##sfilter", "Filter (name)…", &structure_filter_);
  std::string needle = structure_filter_;
  std::transform(needle.begin(), needle.end(), needle.begin(), ::toupper);
  ImGui::BeginChild("##structures", {-1, -1}, ImGuiChildFlags_Borders);
  const auto& list = course_->instances();
  for (int c = 0; c < int(std::size(kStructureCategories)); ++c)
  {
    std::vector<int> rows;
    for (int k = 0; k < int(structures_.size()); ++k)
      if (structures_[k].category == c && (needle.empty() || structures_[k].label.find(needle) != std::string::npos))
        rows.push_back(k);
    if (rows.empty())
      continue;
    char header[96];
    std::snprintf(header, sizeof header, "%s (%zu)###cat%d", kStructureCategories[c], rows.size(), c);
    if (!ImGui::CollapsingHeader(header, c == 0 ? ImGuiTreeNodeFlags_DefaultOpen : 0))
      continue;
    for (int k : rows)
    {
      const Structure& st = structures_[k];
      ImGui::PushID(k);
      if (ImGui::Selectable(st.label.c_str(), armed_ == k))
      {
        armed_ = armed_ == k ? -1 : k;
        selected_ = -1;
      }
      if (st.uses > 1)
      {
        ImGui::SameLine(ImGui::GetContentRegionAvail().x - 30);
        ImGui::TextDisabled("x%d", st.uses);
      }
      if (ImGui::BeginItemTooltip())
      {
        for (int m : st.members)
          ImGui::Text("#%d  %s", m, list[m].name.c_str());
        if (!st.polys.empty())
        {
          int boost = 0;
          for (int p : st.polys)
          {
            const auto& sf = course_->polys()[p].surfaces;
            boost += std::find(sf.begin(), sf.end(), 3) != sf.end();
          }
          ImGui::TextColored({1, 0.85f, 0.3f, 1}, "%zu collision polygons (%d boost)", st.polys.size(), boost);
        }
        if (st.category == 3)
          ImGui::TextColored({1, 0.6f, 0.4f, 1}, "Road models are visuals only: they do not\nchange where the track can be driven.");
        ImGui::TextDisabled("Occurs %d time(s) in this course.", st.uses);
        ImGui::EndTooltip();
      }
      ImGui::PopID();
    }
  }
  ImGui::EndChild();
}

void Editor::DrawInspector(float x, float y, float w, float h)
{
  ImGui::SetNextWindowPos({x, y});
  ImGui::SetNextWindowSize({w, h});
  ImGui::Begin("##right", nullptr, ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove |
                                       ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoBringToFrontOnFocus);
  ImGui::SeparatorText("Tool");
  ImGui::RadioButton("Move (W)", &gizmo_op_, 0);
  ImGui::RadioButton("Rotate (E)", &gizmo_op_, 1);
  ImGui::RadioButton("Scale (R)", &gizmo_op_, 2);
  ImGui::Checkbox("Local axes", &gizmo_local_);
  ImGui::Checkbox("Snap", &snap_);
  {
    Settings& st = project_.settings();
    if (ImGui::Checkbox("Magnetism", &st.magnet))
      st.Save();
    ImGui::SetItemTooltip("Moved objects and track points snap onto nearby vertices / track points.\n"
                          "Hold Alt to move freely.");
    if (st.magnet)
    {
      ImGui::SameLine();
      ImGui::SetNextItemWidth(80);
      if (ImGui::DragFloat("Range", &st.magnet_radius, 0.5f, 1, 1000, "%.0f"))
        st.magnet_radius = std::clamp(st.magnet_radius, 1.0f, 1000.0f);
      if (ImGui::IsItemDeactivatedAfterEdit())
        st.Save();
      if (ImGui::GetIO().KeyAlt)
        ImGui::TextColored({1, 0.6f, 1, 1}, "Alt held: magnetism off");
    }
  }
  if (snap_)
  {
    ImGui::SetNextItemWidth(100);
    ImGui::DragFloat("Step", gizmo_op_ == 0 ? &snap_move_ : gizmo_op_ == 1 ? &snap_rot_ : &snap_scale_, 0.1f, 0.01f, 1000);
  }

  if (course_ && ImGui::Checkbox("Edit track spline (T)", &track_mode_))
    SetTrackMode(track_mode_);
  if (course_ && track_mode_)
  {
    ImGui::SeparatorText("Track point");
    if (handle_ >= 0 && handle_ < int(handles_.size()))
    {
      const TrackHandle& h = handles_[handle_];
      const auto& seg = course_->segments()[h.segment];
      const Vec3 p = course_->HandlePosition(h);
      ImGui::Text("Segment #%d (0x%05X)  t = %.3f", h.segment, seg.offset, h.time);
      ImGui::Text("Position %.1f  %.1f  %.1f", p[0], p[1], p[2]);
      ImGui::TextDisabled("Keys: x %zu  y %zu  z %zu", seg.curves[6].size(), seg.curves[7].size(), seg.curves[8].size());
      if (seg.curves != seg.curves_orig && ImGui::Button("Reset this segment"))
      {
        UndoEntry e;
        e.curves.push_back({h.segment, seg.curves, seg.curves_orig});
        ApplyCurves(e.curves.back().segment, e.curves.back().after);
        undo_.push_back(std::move(e));
        redo_.clear();
      }
    }
    else
      ImGui::TextDisabled("Click an orange point on the track\nline in the 3D view.");
    ImGui::SeparatorText("Track");
    ImGui::Text("Length %.1f%s", course_->track_length(), course_->track_modified() ? " (changed)" : "");
    ImGui::TextDisabled("%zu segments, %zu checkpoints,\n%zu points", course_->segments().size(),
                        course_->checkpoints().size(), handles_.size());
    ImGui::BeginDisabled(!course_->track_modified());
    if (ImGui::Button("Reset whole track", {-1, 0}))
      ResetTrack();
    ImGui::EndDisabled();
    ImGui::Spacing();
    ImGui::TextColored({1, 0.8f, 0.3f, 1}, "Moves the drivable path and checkpoints.\n"
                                           "Road models and static collision (walls)\n"
                                           "stay where they are.");
    ImGui::SeparatorText("Course");
    if (ImGui::Button("Save", {-1, 0}))
      Save();
    if (ImGui::Button("Test in game (F5)", {-1, 0}))
      Launch();
    ImGui::End();
    return;
  }

  ImGui::SeparatorText("Object");
  if (!course_ || selected_ < 0)
  {
    ImGui::TextDisabled("Nothing selected.\nLeft click in the 3D view\nor in the object list.");
    ImGui::End();
    return;
  }
  auto& in = course_->instances()[selected_];
  ImGui::Text("#%d  %s", selected_, in.name.c_str());
  if (in.added)
    ImGui::TextColored({0.4f, 1, 0.55f, 1}, "Placed in the editor (Del deletes it)");
  else
    ImGui::TextDisabled("Offset 0x%05X  Flags 0x%08X", in.offset, in.flags);
  const ModelMesh* m = models_->Find(in.name);
  if (!m)
    ImGui::TextColored({1, 0.5f, 0.4f, 1}, "Model not found (placeholder)");
  else if (m->skinned)
    ImGui::TextColored({0.5f, 0.8f, 1, 1}, "Animated/skinned model (box only)");
  else
    ImGui::TextDisabled("%zu triangles", m->positions.size() / 3);
  if (!in.HasMatrix())
    ImGui::TextColored({1, 0.8f, 0.3f, 1}, "No matrix: the game uses its own logic,\nchanges may have no effect.");

  Transform t = Get(selected_);
  Vec3 rot_deg = {AngleToDeg(t.rotation[0]), AngleToDeg(t.rotation[1]), AngleToDeg(t.rotation[2])};
  bool changed = false, activated = false, deactivated = false, scale_changed = false;
  const Vec3 scale_before = t.scale;
  auto track = [&](bool c) {
    changed |= c;
    activated |= ImGui::IsItemActivated();
    deactivated |= ImGui::IsItemDeactivatedAfterEdit();
  };
  ImGui::PushItemWidth(-90);
  track(ImGui::DragFloat3("Position", t.position.data(), 1.0f, 0, 0, "%.2f"));
  track(ImGui::DragFloat3("Rotation°", rot_deg.data(), 0.5f, -360, 360, "%.2f"));
  static bool uniform = true;
  if (uniform)
  {
    float s = t.scale[0];
    if (ImGui::DragFloat("Scale", &s, 0.01f, 0.001f, 1000, "%.3f"))
    {
      const float f = t.scale[0] != 0 ? s / t.scale[0] : 1;
      for (auto& v : t.scale)
        v = v * f;
      scale_changed = true;
    }
    activated |= ImGui::IsItemActivated();
    deactivated |= ImGui::IsItemDeactivatedAfterEdit();
  }
  else
  {
    scale_changed = ImGui::DragFloat3("Scale", t.scale.data(), 0.01f, 0.001f, 1000, "%.3f");
    activated |= ImGui::IsItemActivated();
    deactivated |= ImGui::IsItemDeactivatedAfterEdit();
  }
  ImGui::PopItemWidth();
  ImGui::Checkbox("Uniform scale", &uniform);

  if (activated)
    BeginEdit();
  if (scale_changed && !changed)
  {
    // Scaling keeps the model centre (and linked objects) in place.
    Vec3 f{1, 1, 1};
    for (int k = 0; k < 3; ++k)
      f[k] = std::fabs(scale_before[k]) > 1e-6f ? t.scale[k] / scale_before[k] : 1.0f;
    ApplyPrimary(ScaledInPlace(selected_, Get(selected_), f), true);
  }
  else if (changed || scale_changed)
  {
    t.rotation = {DegToAngle(rot_deg[0]), DegToAngle(rot_deg[1]), DegToAngle(rot_deg[2])};
    t.matrix = ComposeMatrix(t.position, t.rotation, t.scale);
    ApplyPrimary(t);
  }
  if (deactivated || (editing_ && !ImGui::IsAnyItemActive() && !ImGuizmo::IsUsing()))
    EndEdit();

  // Linked objects (e.g. dash plate + arrow marker + ring) move together.
  const std::vector<int> linked = Linked(selected_);
  ImGui::Checkbox("Move linked objects along", &link_);
  ImGui::Checkbox("Move collision/boost areas along", &move_collision_);
  {
    std::vector<int> members = linked;
    members.push_back(selected_);
    const std::vector<int> attached = AttachedPolys(members);
    if (move_collision_ && !attached.empty())
    {
      int dash = 0;
      bool outside = false;
      for (int p : attached)
      {
        const auto& poly = course_->polys()[p];
        dash += std::find(poly.surfaces.begin(), poly.surfaces.end(), 3) != poly.surfaces.end();
        bool o = false;
        CellsCovered(course_->grid(), poly, &o);
        outside |= o;
      }
      ImGui::TextColored({1, 0.85f, 0.3f, 1}, "%zu collision polygons attached (%d boost)", attached.size(), dash);
      if (outside)
        ImGui::TextColored({1, 0.45f, 0.4f, 1}, "Warning: polygon extends outside the\ncollision grid, may not work there.");
    }
  }
  if (link_)
  {
    ImGui::SetNextItemWidth(120);
    ImGui::DragFloat("Radius", &link_radius_, 1, 1, 500, "%.0f");
    if (linked.empty())
      ImGui::TextDisabled("No linked objects.");
    for (int j : linked)
    {
      ImGui::PushID(j);
      char label[96];
      std::snprintf(label, sizeof label, "  #%d %s", j, course_->instances()[j].name.c_str());
      if (ImGui::Selectable(label, false))
      {
        selected_ = j;
        scroll_to_selection_ = true;
      }
      ImGui::PopID();
    }
  }

  if (!Connectors(selected_).empty())
  {
    int open = 0;
    const auto all = AllConnectors();
    const auto edges = WorldConnectors(selected_);
    for (const auto& c : edges)
      open += ConnectorOpen(selected_, c, all);
    ImGui::SeparatorText("Road edges");
    ImGui::Text("%zu edges, %d open", edges.size(), open);
    ImGui::SetItemTooltip("Open ends of this road model where another piece attaches.\n"
                          "With magnetism, moving the piece snaps an edge onto a fitting edge.");
    if (ImGui::Button("Connect to nearest open edge (C)", {-1, 0}))
      ConnectSelection();
  }

  ImGui::Spacing();
  if (ImGui::Button("Duplicate (Ctrl+D)"))
    DuplicateSelection();
  ImGui::SameLine();
  ImGui::BeginDisabled(!in.added);
  if (ImGui::Button("Delete (Del)"))
    DeleteSelection();
  ImGui::EndDisabled();
  if (!in.added)
    ImGui::SetItemTooltip("Only objects placed in this session can be deleted.");
  if (ImGui::Button("Bring in front of camera"))
  {
    // Moves the object 300 units in front of the camera.
    Transform n = Get(selected_);
    const glm::vec3 p = cam_.pos + cam_.Forward() * 300.0f;
    n.position = {p.x, p.y, p.z};
    n.matrix = ComposeMatrix(n.position, n.rotation, n.scale);
    BeginEdit();
    ApplyPrimary(n);
    EndEdit();
  }
  bool any_modified = Modified(selected_);
  for (int j : linked)
    any_modified |= Modified(j);
  ImGui::BeginDisabled(!any_modified);
  if (ImGui::Button(linked.empty() ? "Reset object" : "Reset object + linked"))
  {
    BeginEdit();
    for (const auto& [i, before] : edit_snapshot_)
      Set(i, original_[i]);
    for (const auto& [p, before] : poly_snapshot_)
      SetPoly(p, original_polys_[p]);
    EndEdit();
  }
  ImGui::EndDisabled();

  ImGui::SeparatorText("Course");
  ImGui::Text("Course %02d  %s", course_id_, venue_.c_str());
  ImGui::TextDisabled("%zu objects, %zu checkpoints", std::size_t(std::count_if(course_->instances().begin(), course_->instances().end(), [](const SceneInstance& o) { return !o.removed; })), course_->checkpoints().size());
  int changed_count = 0, added_count = 0;
  for (int i = 0; i < int(course_->instances().size()); ++i)
  {
    const auto& o = course_->instances()[i];
    if (o.added)
      added_count += !o.removed;
    else
      changed_count += Modified(i);
  }
  ImGui::Text("Changed objects: %d, placed: %d", changed_count, added_count);
  if (ImGui::Button("Save", {-1, 0}))
    Save();
  if (ImGui::Button("Test in game (F5)", {-1, 0}))
    Launch();
  ImGui::End();
}

void Editor::DrawStatus(float x, float y, float w)
{
  ImGui::SetNextWindowPos({x, y});
  ImGui::SetNextWindowSize({w, kStatusHeight});
  ImGui::Begin("##status", nullptr, ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoSavedSettings |
                                        ImGuiWindowFlags_NoBringToFrontOnFocus);
  const bool fresh = ImGui::GetTime() - status_time_ < 8;
  if (status_error_)
    ImGui::TextColored({1, 0.45f, 0.4f, 1}, "%s", status_.c_str());
  else
    ImGui::TextColored(fresh ? ImVec4(0.6f, 0.95f, 0.6f, 1) : ImVec4(0.7f, 0.7f, 0.75f, 1), "%s", status_.c_str());
  ImGui::End();
}

void Editor::DrawSettings()
{
  if (!show_settings_)
    return;
  ImGui::SetNextWindowSize({620, 0}, ImGuiCond_Appearing);
  ImGui::SetNextWindowPos(ImGui::GetMainViewport()->GetCenter(), ImGuiCond_Appearing, {0.5f, 0.5f});
  if (!ImGui::Begin("Settings", &show_settings_))
  {
    ImGui::End();
    return;
  }
  Settings& s = project_.settings();
  std::string rom = s.rom.string(), app = s.glinux_appimage.string(), user = s.glinux_user_dir.string();
  ImGui::TextDisabled("Workspace: %s", s.workspace.string().c_str());
  ImGui::SeparatorText("Game");
  if (ImGui::InputText("ROM (RVZ/ISO)", &rom))
    s.rom = rom;
  ImGui::Text("Disc: %s", project_.DiscReady() ? "extracted" : "missing");
  if (ImGui::Button("Extract disc from ROM"))
  {
    std::string msg;
    const bool ok = project_.ExtractDisc(msg);
    Status(ok ? "Extracting in the background, then use \"Reload course list\". " + msg : msg, !ok);
  }
  ImGui::SameLine();
  if (ImGui::Button("Reload course list"))
  {
    RefreshCourses();
    if (!course_ && !courses_.empty())
      OpenCourse(courses_.front().id);
  }
  ImGui::SeparatorText("Display");
  if (ImGui::Checkbox("Load textures", &s.textures))
    UploadModels();
  ImGui::SeparatorText("Testing");
  int mode = s.launcher == "dolphin" ? 1 : 0;
  ImGui::RadioButton("F-ZERO GLINUX", &mode, 0);
  ImGui::SameLine();
  ImGui::RadioButton("Dolphin (Flatpak)", &mode, 1);
  s.launcher = mode ? "dolphin" : "glinux";
  ImGui::BeginDisabled(mode != 0);
  ImGui::Checkbox("Start directly in the course (Practice, no menus)", &s.quickstart);
  ImGui::EndDisabled();
  if (ImGui::InputText("GLINUX AppImage", &app))
    s.glinux_appimage = app;
  if (ImGui::InputText("GLINUX user folder", &user))
    s.glinux_user_dir = user;
  ImGui::TextDisabled("Mod folder: %s", s.mod().string().c_str());
  ImGui::Spacing();
  if (ImGui::Button("Apply and save"))
  {
    s.Save();
    Status("Settings saved: " + s.file().string());
  }
  ImGui::End();
}

bool Editor::ApplyCups()
{
  std::string msg;
  const bool ok = project_.ApplyCups(cups_, msg);
  if (!ok)
    Status(msg, true);
  return ok;
}

unsigned Editor::CupIconTexture(int cup)
{
  if (!selgf_)
    selgf_ = project_.LoadDiscTpl("bmp_selgf");
  if (!selgf_)
    return 0;
  cup_icons_.resize(cups_.size(), 0);
  cup_icon_hue_.resize(cups_.size(), -1);
  if (cup_icons_[cup] == 0 || cup_icon_hue_[cup] != cups_[cup].hue)
  {
    renderer_.FreeTexture(cup_icons_[cup]);
    try
    {
      cup_icons_[cup] = renderer_.CreateTexture(CupIcon(*selgf_, cups_[cup].hue), 0);
    }
    catch (const FormatError&)
    {
      cup_icons_[cup] = 0;
    }
    cup_icon_hue_[cup] = cups_[cup].hue;
  }
  return cup_icons_[cup];
}

void Editor::DrawCups()
{
  if (!show_cups_)
    return;
  ImGui::SetNextWindowSize({640, 560}, ImGuiCond_Appearing);
  ImGui::SetNextWindowPos(ImGui::GetMainViewport()->GetCenter(), ImGuiCond_Appearing, {0.5f, 0.5f});
  if (!ImGui::Begin("Cups", &show_cups_))
  {
    ImGui::End();
    return;
  }
  auto course_label = [](int id) {
    char buf[64];
    std::snprintf(buf, sizeof buf, "%02d  %s", id, CourseName(id));
    return std::string(buf);
  };

  ImGui::SeparatorText("Game cups");
  if (ImGui::BeginTable("##orig", 2, ImGuiTableFlags_RowBg | ImGuiTableFlags_SizingFixedFit))
  {
    for (const auto& c : OriginalCups())
    {
      ImGui::TableNextRow();
      ImGui::TableNextColumn();
      ImGui::TextUnformatted(c.name);
      ImGui::TableNextColumn();
      std::string list;
      for (const int id : c.courses)
        list += (list.empty() ? "" : ", ") + std::string(CourseName(id));
      ImGui::TextDisabled("%s", list.c_str());
    }
    ImGui::EndTable();
  }

  ImGui::SeparatorText("Custom cups");
  ImGui::TextWrapped("Custom cups appear in every cup selection (Grand Prix, Time Attack, Practice, VS) "
                     "behind the AX Cup: an arrow leads to a second page with these cups. Up to %d cups with up to %d courses each.",
                     kMaxCustomCups, kMaxCupCourses);
  std::vector<int> race_courses;
  for (const auto& c : courses_)
    if (*CourseName(c.id))
      race_courses.push_back(c.id);

  int remove_cup = -1;
  for (int i = 0; i < int(cups_.size()); ++i)
  {
    CupDef& cup = cups_[i];
    ImGui::PushID(i);
    ImGui::Separator();
    if (const unsigned tex = CupIconTexture(i))
    {
      ImGui::Image(ImTextureID(std::intptr_t(tex)), {46, 58});
      ImGui::SameLine();
    }
    ImGui::BeginGroup();
    ImGui::SetNextItemWidth(240);
    if (ImGui::InputText("Name", &cup.name))
      cups_dirty_ = true;
    ImGui::SetNextItemWidth(240);
    if (ImGui::SliderFloat("Gem colour", &cup.hue, 0, 359, "%.0f°"))
      cups_dirty_ = true;
    ImGui::EndGroup();
    ImGui::SameLine(ImGui::GetContentRegionAvail().x - 90);
    if (ImGui::Button("Remove cup"))
      remove_cup = i;

    int remove_course = -1;
    for (int k = 0; k < int(cup.courses.size()); ++k)
    {
      ImGui::PushID(k);
      ImGui::Text("Race %d", k + 1);
      ImGui::SameLine(70);
      ImGui::SetNextItemWidth(260);
      if (ImGui::BeginCombo("##course", course_label(cup.courses[k]).c_str()))
      {
        for (const int id : race_courses)
          if (ImGui::Selectable(course_label(id).c_str(), id == cup.courses[k]))
          {
            cup.courses[k] = id;
            cups_dirty_ = true;
          }
        ImGui::EndCombo();
      }
      ImGui::SameLine();
      if (ImGui::SmallButton("Remove"))
        remove_course = k;
      ImGui::PopID();
    }
    if (remove_course >= 0)
    {
      cup.courses.erase(cup.courses.begin() + remove_course);
      cups_dirty_ = true;
    }
    ImGui::BeginDisabled(int(cup.courses.size()) >= kMaxCupCourses || race_courses.empty());
    if (ImGui::Button("Add course"))
    {
      cup.courses.push_back(course_id_ > 0 && *CourseName(course_id_) ? course_id_ : race_courses.front());
      cups_dirty_ = true;
    }
    ImGui::EndDisabled();
    ImGui::PopID();
  }
  if (remove_cup >= 0)
  {
    cups_.erase(cups_.begin() + remove_cup);
    if (remove_cup < int(cup_icons_.size()))
    {
      renderer_.FreeTexture(cup_icons_[remove_cup]);
      cup_icons_.erase(cup_icons_.begin() + remove_cup);
      cup_icon_hue_.erase(cup_icon_hue_.begin() + remove_cup);
    }
    cups_dirty_ = true;
  }
  ImGui::Separator();
  ImGui::BeginDisabled(int(cups_.size()) >= kMaxCustomCups || race_courses.empty());
  if (ImGui::Button("Add custom cup"))
  {
    CupDef c;
    c.name = "Custom Cup " + std::to_string(cups_.size() + 1);
    c.hue = cups_.empty() ? 30.0f : 200.0f;
    c.courses.push_back(course_id_ > 0 && *CourseName(course_id_) ? course_id_ : race_courses.front());
    cups_.push_back(c);
    cups_dirty_ = true;
  }
  ImGui::EndDisabled();

  const std::string problem = ValidateCups(cups_);
  if (!problem.empty())
    ImGui::TextColored({1, 0.4f, 0.4f, 1}, "%s", problem.c_str());
  ImGui::BeginDisabled(!problem.empty());
  if (ImGui::Button(cups_dirty_ ? "Save cups and write mod folder *" : "Save cups and write mod folder"))
  {
    std::string msg;
    if (!SaveCups(project_.settings().cups_file(), cups_))
      Status("Could not write " + project_.settings().cups_file().string(), true);
    else if (project_.ApplyCups(cups_, msg))
    {
      cups_dirty_ = false;
      Status(msg + " Start the game normally (not the direct start) to choose the cup.");
    }
    else
      Status(msg, true);
  }
  ImGui::EndDisabled();
  ImGui::TextDisabled("Written: sys/main.dol (patched) and files/bmp_e/bmp_selgf.tpl.lz (icons) in the mod folder.");
  ImGui::End();
}

void Editor::DrawPopups()
{
  if (pending_course_ >= 0)
    ImGui::OpenPopup("Unsaved changes");
  if (confirm_quit_)
    ImGui::OpenPopup("Quit?");
  if (ImGui::BeginPopupModal("Unsaved changes", nullptr, ImGuiWindowFlags_AlwaysAutoResize))
  {
    ImGui::Text("Course %02d has unsaved changes.", course_id_);
    if (ImGui::Button("Save"))
    {
      if (Save())
        OpenCourse(pending_course_);
      pending_course_ = -1;
      ImGui::CloseCurrentPopup();
    }
    ImGui::SameLine();
    if (ImGui::Button("Discard"))
    {
      OpenCourse(pending_course_);
      pending_course_ = -1;
      ImGui::CloseCurrentPopup();
    }
    ImGui::SameLine();
    if (ImGui::Button("Cancel"))
    {
      pending_course_ = -1;
      ImGui::CloseCurrentPopup();
    }
    ImGui::EndPopup();
  }
  if (ImGui::BeginPopupModal("Quit?", nullptr, ImGuiWindowFlags_AlwaysAutoResize))
  {
    ImGui::Text("There are unsaved changes.");
    if (ImGui::Button("Save and quit"))
    {
      quit_ = Save();
      confirm_quit_ = false;
      ImGui::CloseCurrentPopup();
    }
    ImGui::SameLine();
    if (ImGui::Button("Discard"))
      quit_ = true;
    ImGui::SameLine();
    if (ImGui::Button("Cancel"))
    {
      confirm_quit_ = false;
      ImGui::CloseCurrentPopup();
    }
    ImGui::EndPopup();
  }
}
}  // namespace fzgx
