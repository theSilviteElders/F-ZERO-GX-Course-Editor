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
#include <cstdio>

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
  CloseCourse();
  renderer_.Shutdown();
}

bool Editor::Init()
{
  if (!renderer_.Init())
    return false;
  RefreshCourses();
  if (!project_.DiscReady())
  {
    show_settings_ = true;
    Status("Keine extrahierte Disc gefunden. Bitte in den Einstellungen die ROM wählen und extrahieren.", true);
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

void Editor::CloseCourse()
{
  for (auto& [name, mesh] : gpu_)
    renderer_.Free(mesh);
  gpu_.clear();
  renderer_.Free(collision_mesh_);
  course_.reset();
  models_.reset();
  original_.clear();
  original_polys_.clear();
  background_.clear();
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
    Status(std::string("Kurs konnte nicht geladen werden: ") + e.what(), true);
    return;
  }
  course_id_ = id;
  for (int i = 0; i < int(course_->instances().size()); ++i)
    original_.push_back(Get(i));
  for (const auto& p : course_->polys())
    original_polys_.push_back(p.v);
  // Upload every referenced model once.
  for (const auto& in : course_->instances())
  {
    if (gpu_.count(in.name))
      continue;
    const ModelMesh* m = models_->Find(in.name);
    gpu_[in.name] = (m && !m->positions.empty()) ? renderer_.Upload(*m) : GpuMesh{};
  }
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

  bool from_mod = false;
  for (const auto& c : courses_)
    from_mod |= c.id == id && c.modified;
  Status("Kurs " + std::to_string(id) + (venue_.empty() ? "" : " (" + venue_ + ")") + " geladen: " +
         std::to_string(course_->instances().size()) + " Objekte" +
         (from_mod ? ", bearbeitete Fassung aus dem Mod-Ordner." : "."));
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

std::vector<int> Editor::Linked(int i) const
{
  std::vector<int> out;
  if (!link_ || !course_ || i < 0)
    return out;
  const auto& list = course_->instances();
  auto prefix = [](const std::string& n) { return n.substr(0, n.find('_')); };
  const glm::vec3 p(list[i].matrix[3], list[i].matrix[7], list[i].matrix[11]);
  // Track pieces (roads, guardrails) all sit at the world origin: never link those.
  if (glm::length(p) < 1.0f || list[i].name.empty())
    return out;
  const std::string pre = prefix(list[i].name);
  for (int j = 0; j < int(list.size()); ++j)
  {
    if (j == i || IsBackground(j) || prefix(list[j].name) != pre)
      continue;
    const glm::vec3 q(list[j].matrix[3], list[j].matrix[7], list[j].matrix[11]);
    if (glm::length(q) >= 1.0f && glm::distance(p, q) <= link_radius_)
      out.push_back(j);
  }
  return out;
}

std::vector<int> Editor::AttachedPolys(const std::vector<int>& objects) const
{
  std::vector<int> out;
  if (!course_ || !move_collision_)
    return out;
  const auto& polys = course_->polys();
  std::vector<char> taken(polys.size(), 0);
  for (int i : objects)
  {
    const auto& in = course_->instances()[i];
    const glm::vec3 pos(in.matrix[3], in.matrix[7], in.matrix[11]);
    if (glm::length(pos) < 1.0f || IsBackground(i))
      continue;  // track pieces at the origin / huge scenery: never grab collision
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
      if (taken[p])
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

void Editor::ApplyPrimary(const Transform& t)
{
  if (!editing_)
    BeginEdit();
  if (edit_snapshot_.empty())
    return;
  const glm::mat4 before = ToGlm(edit_snapshot_[0].second.matrix);
  Set(edit_snapshot_[0].first, t);
  // Same world-space change for every linked object (relative to the start of the edit).
  const glm::mat4 delta = ToGlm(t.matrix) * glm::inverse(before);
  for (std::size_t k = 1; k < edit_snapshot_.size(); ++k)
    SetFromMatrix(edit_snapshot_[k].first, FromGlm(delta * ToGlm(edit_snapshot_[k].second.matrix)));
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
  if (!e.items.empty())
    selected_ = e.items.front().index;
  undo_.push_back(e);
}

bool Editor::Modified(int i) const { return !(Get(i) == original_[i]); }

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
  std::string msg;
  const bool quick = project_.settings().quickstart && project_.settings().launcher == "glinux";
  const bool ok = project_.Launch(course_id_, msg);
  Status(!ok      ? msg
         : quick ? "Spiel startet direkt auf Kurs " + std::to_string(course_id_) +
                       " (Practice). Beim ersten Mal navigiert es ca. 30 s automatisch durch die Menüs. " + msg
                 : "Spiel startet aus dem Mod-Ordner, Kurs im Spiel wählen. " + msg,
         !ok);
}

void Editor::DebugStart(int course, int select, bool collision)
{
  if (course >= 0)
    OpenCourse(course);
  if (course_ && select >= 0 && select < int(course_->instances().size()))
  {
    selected_ = select;
    scroll_to_selection_ = true;
    FocusSelection();
  }
  show_collision_ |= collision;
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

int Editor::Pick(const glm::vec3& o, const glm::vec3& d)
{
  int best = -1;
  float best_t = 1e30f;
  const auto& list = course_->instances();
  for (int i = 0; i < int(list.size()); ++i)
  {
    const auto& in = list[i];
    if (IsBackground(i))
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
  return best;
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
      if (ImGui::IsKeyPressed(ImGuiKey_Escape))
        selected_ = -1;
    }
  }

  ImGuizmo::BeginFrame();
  DrawMenu();
  DrawCoursePanel(0, menu_h, kLeftWidth, H - menu_h - kStatusHeight);
  DrawInspector(W - kRightWidth, menu_h, kRightWidth, H - menu_h - kStatusHeight);
  DrawStatus(0, H - kStatusHeight, W);
  DrawSettings();
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

  // Click to select (unless the gizmo takes the click).
  if (over && course_ && ImGui::IsMouseClicked(ImGuiMouseButton_Left) && !ImGuizmo::IsOver())
  {
    const float nx = (mp.x - vx) / vw * 2 - 1, ny = 1 - (mp.y - vy) / vh * 2;
    const glm::mat4 inv = glm::inverse(proj_ * view_);
    glm::vec4 a = inv * glm::vec4(nx, ny, -1, 1), b = inv * glm::vec4(nx, ny, 1, 1);
    const glm::vec3 o = glm::vec3(a) / a.w, e = glm::vec3(b) / b.w;
    selected_ = Pick(o, glm::normalize(e - o));
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
      for (std::size_t k = 0; k < cps.size(); ++k)
      {
        const auto& cp = cps[k];
        const glm::vec3 a(cp.start[0], cp.start[1], cp.start[2]), b(cp.end[0], cp.end[1], cp.end[2]);
        const glm::vec3 n(cp.start_normal[0], cp.start_normal[1], cp.start_normal[2]);
        lines.push_back({a, Rgba(255, 220, 40)});
        lines.push_back({b, Rgba(255, 220, 40)});
        // Width marker across the track at each checkpoint start.
        glm::vec3 side = glm::cross(n, glm::vec3(0, 1, 0));
        if (glm::length(side) < 1e-3f)
          side = glm::vec3(1, 0, 0);
        side = glm::normalize(side) * cp.width * 0.5f;
        const std::uint32_t col = k == 0 ? Rgba(80, 255, 120) : Rgba(255, 220, 40, 110);
        lines.push_back({a - side, col});
        lines.push_back({a + side, col});
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

  // Gizmo (drawn by ImGui on the background draw list, inside the viewport rect).
  if (course_ && selected_ >= 0)
  {
    ImGuizmo::SetDrawlist(ImGui::GetBackgroundDrawList());
    ImGuizmo::SetRect(vx, vy, vw, vh);
    glm::mat4 M = ToGlm(course_->instances()[selected_].matrix);
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
      Vec3 pos, rot, scl;
      if (DecomposeMatrix(FromGlm(M), pos, rot, scl))
      {
        Transform t;
        t.position = pos;
        t.rotation = {DegToAngle(rot[0]), DegToAngle(rot[1]), DegToAngle(rot[2])};
        t.scale = scl;
        t.matrix = ComposeMatrix(t.position, t.rotation, t.scale);
        ApplyPrimary(t);
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
  if (ImGui::BeginMenu("Datei"))
  {
    if (ImGui::MenuItem("Speichern", "Strg+S", false, course_.has_value()))
      Save();
    if (ImGui::MenuItem("Im Spiel testen", "F5", false, course_.has_value()))
      Launch();
    ImGui::Separator();
    if (ImGui::MenuItem("Kurs auf Original zurücksetzen", nullptr, false, course_.has_value()))
    {
      std::string msg;
      const bool ok = project_.RestoreCourse(course_id_, msg);
      Status(msg, !ok);
      RefreshCourses();
      OpenCourse(course_id_);
    }
    if (ImGui::MenuItem("Einstellungen…"))
      show_settings_ = true;
    ImGui::Separator();
    if (ImGui::MenuItem("Beenden"))
      RequestQuit();
    ImGui::EndMenu();
  }
  if (ImGui::BeginMenu("Bearbeiten"))
  {
    if (ImGui::MenuItem("Rückgängig", "Strg+Z", false, !undo_.empty()))
      Undo();
    if (ImGui::MenuItem("Wiederholen", "Strg+Y", false, !redo_.empty()))
      Redo();
    ImGui::EndMenu();
  }
  if (ImGui::BeginMenu("Ansicht"))
  {
    ImGui::MenuItem("Modelle", nullptr, &show_models_);
    ImGui::MenuItem("Hintergrund (Boden/Himmel)", nullptr, &show_background_);
    ImGui::MenuItem("Platzhalter (fehlende/animierte Modelle)", nullptr, &show_missing_);
    ImGui::MenuItem("Streckenverlauf (Checkpoints)", nullptr, &show_track_);
    ImGui::MenuItem("Kollision (statisch, gelb = Boost)", nullptr, &show_collision_);
    ImGui::Separator();
    if (ImGui::MenuItem("Ganzen Kurs zeigen"))
      FrameCourse();
    if (ImGui::MenuItem("Auswahl fokussieren", "F", false, selected_ >= 0))
      FocusSelection();
    ImGui::EndMenu();
  }
  if (ImGui::BeginMenu("Hilfe"))
  {
    ImGui::TextUnformatted(
        "Kamera:  rechte Maustaste halten + WASD/QE fliegen, Mausrad = Tempo\n"
        "         Mausrad = vor/zurück, mittlere Maustaste = schieben\n"
        "Auswahl: Linksklick im 3D-Fenster oder in der Objektliste\n"
        "Gizmo:   W verschieben, E drehen, R skalieren, F fokussieren\n"
        "Datei:   Strg+S speichern (in den Mod-Ordner), F5 im Spiel testen\n\n"
        "Hinweis: Straßenmodelle sind nur Optik. Fahrbahn und Physik kommen\n"
        "aus dem Track-Spline (gelbe Linie) und werden noch nicht bearbeitet.");
    ImGui::EndMenu();
  }
  ImGui::SameLine(ImGui::GetWindowWidth() - 360);
  if (course_)
    ImGui::TextDisabled("%s  |  %s", Project::CourseFile(course_id_).c_str(), dirty_ ? "ungespeichert *" : "gespeichert");
  ImGui::EndMainMenuBar();
}

void Editor::DrawCoursePanel(float x, float y, float w, float h)
{
  ImGui::SetNextWindowPos({x, y});
  ImGui::SetNextWindowSize({w, h});
  ImGui::Begin("##left", nullptr, ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove |
                                      ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoBringToFrontOnFocus);
  ImGui::SeparatorText("Kurse");
  if (ImGui::BeginListBox("##courses", {-1, std::min(220.0f, h * 0.35f)}))
  {
    for (const auto& c : courses_)
    {
      char label[64];
      std::snprintf(label, sizeof label, "%s Kurs %02d", c.modified ? "*" : " ", c.id);
      if (ImGui::Selectable(label, c.id == course_id_) && c.id != course_id_)
      {
        if (dirty_)
          pending_course_ = c.id;
        else
          OpenCourse(c.id);
      }
    }
    ImGui::EndListBox();
  }
  ImGui::TextDisabled("* = im Mod-Ordner geändert");

  ImGui::SeparatorText("Objekte");
  if (!course_)
  {
    ImGui::TextDisabled("Kein Kurs geladen.");
    ImGui::End();
    return;
  }
  ImGui::SetNextItemWidth(-1);
  ImGui::InputTextWithHint("##filter", "Filter (Name)…", &filter_);
  std::string needle = filter_;
  std::transform(needle.begin(), needle.end(), needle.begin(), ::toupper);
  std::vector<int> rows;
  const auto& list = course_->instances();
  for (int i = 0; i < int(list.size()); ++i)
    if (needle.empty() || list[i].name.find(needle) != std::string::npos)
      rows.push_back(i);
  ImGui::TextDisabled("%zu von %zu", rows.size(), list.size());
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
      std::snprintf(label, sizeof label, "%4d  %s%s", i, in.name.empty() ? "(ohne Name)" : in.name.c_str(),
                    Modified(i) ? "  *" : "");
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
  ImGui::End();
}

void Editor::DrawInspector(float x, float y, float w, float h)
{
  ImGui::SetNextWindowPos({x, y});
  ImGui::SetNextWindowSize({w, h});
  ImGui::Begin("##right", nullptr, ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove |
                                       ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoBringToFrontOnFocus);
  ImGui::SeparatorText("Werkzeug");
  ImGui::RadioButton("Verschieben (W)", &gizmo_op_, 0);
  ImGui::RadioButton("Drehen (E)", &gizmo_op_, 1);
  ImGui::RadioButton("Skalieren (R)", &gizmo_op_, 2);
  ImGui::Checkbox("Lokale Achsen", &gizmo_local_);
  ImGui::Checkbox("Raster", &snap_);
  if (snap_)
  {
    ImGui::SetNextItemWidth(100);
    ImGui::DragFloat("Schritt", gizmo_op_ == 0 ? &snap_move_ : gizmo_op_ == 1 ? &snap_rot_ : &snap_scale_, 0.1f, 0.01f, 1000);
  }

  ImGui::SeparatorText("Objekt");
  if (!course_ || selected_ < 0)
  {
    ImGui::TextDisabled("Nichts ausgewählt.\nLinksklick im 3D-Fenster\noder in der Objektliste.");
    ImGui::End();
    return;
  }
  auto& in = course_->instances()[selected_];
  ImGui::Text("#%d  %s", selected_, in.name.c_str());
  ImGui::TextDisabled("Offset 0x%05X  Flags 0x%08X", in.offset, in.flags);
  const ModelMesh* m = models_->Find(in.name);
  if (!m)
    ImGui::TextColored({1, 0.5f, 0.4f, 1}, "Modell nicht gefunden (Platzhalter)");
  else if (m->skinned)
    ImGui::TextColored({0.5f, 0.8f, 1, 1}, "Animiertes/Skin-Modell (nur Box)");
  else
    ImGui::TextDisabled("%zu Dreiecke", m->positions.size() / 3);
  if (!in.matrix_offset)
    ImGui::TextColored({1, 0.8f, 0.3f, 1}, "Keine Matrix: Spiel nutzt eigene Logik,\nÄnderungen evtl. ohne Wirkung.");

  Transform t = Get(selected_);
  Vec3 rot_deg = {AngleToDeg(t.rotation[0]), AngleToDeg(t.rotation[1]), AngleToDeg(t.rotation[2])};
  bool changed = false, activated = false, deactivated = false;
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
    if (ImGui::DragFloat("Skalierung", &s, 0.01f, 0.001f, 1000, "%.3f"))
    {
      const float f = t.scale[0] != 0 ? s / t.scale[0] : 1;
      for (auto& v : t.scale)
        v = v * f;
      changed = true;
    }
    activated |= ImGui::IsItemActivated();
    deactivated |= ImGui::IsItemDeactivatedAfterEdit();
  }
  else
    track(ImGui::DragFloat3("Skalierung", t.scale.data(), 0.01f, 0.001f, 1000, "%.3f"));
  ImGui::PopItemWidth();
  ImGui::Checkbox("Gleichmäßig skalieren", &uniform);

  if (activated)
    BeginEdit();
  if (changed)
  {
    t.rotation = {DegToAngle(rot_deg[0]), DegToAngle(rot_deg[1]), DegToAngle(rot_deg[2])};
    t.matrix = ComposeMatrix(t.position, t.rotation, t.scale);
    ApplyPrimary(t);
  }
  if (deactivated || (editing_ && !ImGui::IsAnyItemActive() && !ImGuizmo::IsUsing()))
    EndEdit();

  // Linked objects (e.g. dash plate + arrow marker + ring) move together.
  const std::vector<int> linked = Linked(selected_);
  ImGui::Checkbox("Verknüpfte Objekte mitbewegen", &link_);
  ImGui::Checkbox("Kollision/Boost-Flächen mitbewegen", &move_collision_);
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
      ImGui::TextColored({1, 0.85f, 0.3f, 1}, "%zu Kollisionsflächen hängen dran (%d Boost)", attached.size(), dash);
      if (outside)
        ImGui::TextColored({1, 0.45f, 0.4f, 1}, "Achtung: Fläche liegt außerhalb des\nKollisionsgitters, wirkt dort evtl. nicht.");
    }
  }
  if (link_)
  {
    ImGui::SetNextItemWidth(120);
    ImGui::DragFloat("Radius", &link_radius_, 1, 1, 500, "%.0f");
    if (linked.empty())
      ImGui::TextDisabled("Keine verknüpften Objekte.");
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

  ImGui::Spacing();
  if (ImGui::Button("Vor die Kamera holen"))
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
  if (ImGui::Button(linked.empty() ? "Objekt zurücksetzen" : "Objekt + Verknüpfte zurücksetzen"))
  {
    BeginEdit();
    for (const auto& [i, before] : edit_snapshot_)
      Set(i, original_[i]);
    for (const auto& [p, before] : poly_snapshot_)
      SetPoly(p, original_polys_[p]);
    EndEdit();
  }
  ImGui::EndDisabled();

  ImGui::SeparatorText("Kurs");
  ImGui::Text("Kurs %02d  %s", course_id_, venue_.c_str());
  ImGui::TextDisabled("%zu Objekte, %zu Checkpoints", course_->instances().size(), course_->checkpoints().size());
  int changed_count = 0;
  for (int i = 0; i < int(course_->instances().size()); ++i)
    changed_count += Modified(i);
  ImGui::Text("Geänderte Objekte: %d", changed_count);
  if (ImGui::Button("Speichern", {-1, 0}))
    Save();
  if (ImGui::Button("Im Spiel testen (F5)", {-1, 0}))
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
  if (!ImGui::Begin("Einstellungen", &show_settings_))
  {
    ImGui::End();
    return;
  }
  Settings& s = project_.settings();
  std::string rom = s.rom.string(), app = s.glinux_appimage.string(), user = s.glinux_user_dir.string();
  ImGui::TextDisabled("Arbeitsordner: %s", s.workspace.string().c_str());
  ImGui::SeparatorText("Spiel");
  if (ImGui::InputText("ROM (RVZ/ISO)", &rom))
    s.rom = rom;
  ImGui::Text("Disc: %s", project_.DiscReady() ? "extrahiert" : "fehlt");
  if (ImGui::Button("Disc aus ROM extrahieren"))
  {
    std::string msg;
    const bool ok = project_.ExtractDisc(msg);
    Status(ok ? "Extraktion läuft im Hintergrund, danach \"Kursliste neu laden\". " + msg : msg, !ok);
  }
  ImGui::SameLine();
  if (ImGui::Button("Kursliste neu laden"))
  {
    RefreshCourses();
    if (!course_ && !courses_.empty())
      OpenCourse(courses_.front().id);
  }
  ImGui::SeparatorText("Testen");
  int mode = s.launcher == "dolphin" ? 1 : 0;
  ImGui::RadioButton("F-ZERO GLINUX", &mode, 0);
  ImGui::SameLine();
  ImGui::RadioButton("Dolphin (Flatpak)", &mode, 1);
  s.launcher = mode ? "dolphin" : "glinux";
  ImGui::BeginDisabled(mode != 0);
  ImGui::Checkbox("Direkt in den Kurs starten (Practice, ohne Menüs)", &s.quickstart);
  ImGui::EndDisabled();
  if (ImGui::InputText("GLINUX AppImage", &app))
    s.glinux_appimage = app;
  if (ImGui::InputText("GLINUX Benutzerordner", &user))
    s.glinux_user_dir = user;
  ImGui::TextDisabled("Mod-Ordner: %s", s.mod().string().c_str());
  ImGui::Spacing();
  if (ImGui::Button("Übernehmen und speichern"))
  {
    s.Save();
    Status("Einstellungen gespeichert: " + s.file().string());
  }
  ImGui::End();
}

void Editor::DrawPopups()
{
  if (pending_course_ >= 0)
    ImGui::OpenPopup("Ungespeicherte Änderungen");
  if (confirm_quit_)
    ImGui::OpenPopup("Beenden?");
  if (ImGui::BeginPopupModal("Ungespeicherte Änderungen", nullptr, ImGuiWindowFlags_AlwaysAutoResize))
  {
    ImGui::Text("Kurs %02d hat ungespeicherte Änderungen.", course_id_);
    if (ImGui::Button("Speichern"))
    {
      if (Save())
        OpenCourse(pending_course_);
      pending_course_ = -1;
      ImGui::CloseCurrentPopup();
    }
    ImGui::SameLine();
    if (ImGui::Button("Verwerfen"))
    {
      OpenCourse(pending_course_);
      pending_course_ = -1;
      ImGui::CloseCurrentPopup();
    }
    ImGui::SameLine();
    if (ImGui::Button("Abbrechen"))
    {
      pending_course_ = -1;
      ImGui::CloseCurrentPopup();
    }
    ImGui::EndPopup();
  }
  if (ImGui::BeginPopupModal("Beenden?", nullptr, ImGuiWindowFlags_AlwaysAutoResize))
  {
    ImGui::Text("Es gibt ungespeicherte Änderungen.");
    if (ImGui::Button("Speichern und beenden"))
    {
      quit_ = Save();
      confirm_quit_ = false;
      ImGui::CloseCurrentPopup();
    }
    ImGui::SameLine();
    if (ImGui::Button("Verwerfen"))
      quit_ = true;
    ImGui::SameLine();
    if (ImGui::Button("Abbrechen"))
    {
      confirm_quit_ = false;
      ImGui::CloseCurrentPopup();
    }
    ImGui::EndPopup();
  }
}
}  // namespace fzgx
