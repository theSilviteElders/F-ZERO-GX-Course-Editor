#pragma once
#include "project.hpp"
#include "renderer.hpp"

#include <glm/glm.hpp>

#include <map>
#include <memory>
#include <optional>
#include <string>

namespace fzgx
{
struct Transform
{
  Vec3 position{};
  std::array<std::int16_t, 3> rotation{};
  Vec3 scale{1, 1, 1};
  Mat34 matrix{};
  bool operator==(const Transform&) const = default;
};

struct UndoItem
{
  int index;
  Transform before, after;
};

// One user action; may cover the selected object plus its linked objects.
struct PolyUndoItem
{
  int poly;
  std::vector<Vec3> before, after;
};

// An object or collision polygon placed (alive = true) or deleted (false).
struct AliveItem
{
  int index;
  bool poly;
  bool alive;  // state after the action
};

struct CurveUndoItem
{
  int segment;
  Curves before, after;
};

struct UndoEntry
{
  std::vector<CurveUndoItem> curves;  // track spline edits
  std::vector<UndoItem> items;
  std::vector<PolyUndoItem> polys;  // collision polygons carried along (e.g. dash zones)
  std::vector<AliveItem> alive;     // objects/polygons placed or deleted
};

// A placeable structure: a group of objects found in the course (e.g. dash
// plate + arrow marker + ring) together with its collision polygons (boost
// zone). Placing it copies all of them.
struct Structure
{
  std::string label;
  int category = 0;           // see kStructureCategories in editor.cpp
  std::vector<int> members;   // template instances, members[0] = primary
  std::vector<int> polys;     // template collision polygons
  glm::vec3 anchor{0};        // point that lands on the clicked spot
  int uses = 0;               // how often the group occurs in the course
};

struct Camera
{
  glm::vec3 pos{0, 500, 1500};
  float yaw = -90, pitch = -20;  // degrees
  float speed = 600;             // units per second
  glm::vec3 Forward() const;
  glm::mat4 View() const;
};

class Editor
{
public:
  explicit Editor(Settings settings);
  ~Editor();
  bool Init();
  // Draws UI + viewport. fb_w/fb_h = framebuffer pixels, scale = pixels per UI unit.
  void Frame(int fb_w, int fb_h, float scale, float dt);
  bool WantsQuit() const { return quit_; }
  void RequestQuit();
  void DebugStart(int course, int select, bool collision, int place = -1, int handle = -1, float lift = 0);

private:
  // Course handling
  void RefreshCourses();
  void OpenCourse(int id);
  void CloseCourse();
  // (Re-)uploads all models of the open course, with or without textures.
  void UploadModels();
  void FreeModels();
  bool Save();
  void Launch();

  // Editing
  Transform Get(int i) const;
  void Set(int i, const Transform& t);
  void SetFromMatrix(int i, const Mat34& m);
  // Edit transactions: BeginEdit snapshots the selection (+ linked objects),
  // ApplyPrimary moves the selection and carries the linked objects along,
  // EndEdit records one undo step.
  std::vector<int> Linked(int i) const;
  // Static collision polygons lying entirely under one of the given objects.
  // force: ignore the "move collision along" setting.
  std::vector<int> AttachedPolys(const std::vector<int>& objects, bool force = false) const;
  void SetPoly(int p, const std::vector<Vec3>& v);
  void BeginEdit();
  void ApplyPrimary(const Transform& t);
  void EndEdit();
  void Undo();
  void Redo();
  bool Modified(int i) const;
  bool Alive(int i) const { return !course_->instances()[i].removed; }

  // Track spline editing
  void RefreshHandles();
  void SetTrackMode(bool on);
  void ApplyCurves(int segment, const Curves& c);
  void ResetTrack();

  // Magnetism: snapping to nearby vertices / track points (off while Alt is held).
  bool MagnetActive();
  // Offset that puts a border vertex of `sources` (instance matrices pre-multiplied
  // by `pre`) onto the nearest border vertex of another object within the range.
  bool SnapVertices(const std::vector<int>& sources, const std::vector<int>& exclude, const glm::mat4& pre,
                    glm::vec3& offset);
  // Border vertices of a model (cached): unique vertices on the four
  // horizontal sides of its bounding box, where pieces connect (road ends).
  const std::vector<glm::vec3>& BorderVertices(const std::string& name);

  // Placing structures
  void BuildStructures();
  // Copies objects + polygons, moved by `offset`; one undo step. Returns the new objects.
  std::vector<int> Spawn(const std::vector<int>& members, const std::vector<int>& polys, const glm::vec3& offset);
  void PlaceStructure(int s, const glm::vec3& offset);
  void DuplicateSelection();
  void DeleteSelection();
  void SetAlive(const AliveItem& item, bool alive);

  // UI pieces
  void DrawMenu();
  void DrawCoursePanel(float x, float y, float w, float h);
  void DrawObjectList();
  void DrawStructurePanel();
  void DrawInspector(float x, float y, float w, float h);
  void DrawSettings();
  void DrawPopups();
  void DrawStatus(float x, float y, float w);
  void HandleViewportInput(float vx, float vy, float vw, float vh, float dt);
  void RenderViewport(int fb_w, int fb_h, float scale, float vx, float vy, float vw, float vh);
  // Closest object under the ray; *t = hit distance. background: include ground/sky models.
  int Pick(const glm::vec3& origin, const glm::vec3& dir, float* t = nullptr, bool background = false);
  // Point where a ray hits a model or collision polygon (false: nothing hit).
  bool RayHit(const glm::vec3& origin, const glm::vec3& dir, glm::vec3& hit);
  void FocusSelection();
  void FrameCourse();
  void Status(std::string msg, bool error = false);

  Project project_;
  Renderer renderer_;
  std::vector<CourseInfo> courses_;

  // Current course
  int course_id_ = -1;
  std::optional<Course> course_;
  std::vector<Transform> original_;  // as loaded (for "modified" markers / reset)
  std::unique_ptr<ModelLibrary> models_;
  std::string venue_;
  std::map<std::string, GpuMesh> gpu_;
  std::map<std::uint64_t, unsigned> textures_;  // (archive, tpl index, wrap) -> GL texture
  GpuMesh collision_mesh_;
  bool dirty_ = false;

  std::vector<UndoEntry> undo_, redo_;
  int selected_ = -1;
  std::string filter_;
  bool scroll_to_selection_ = false;

  // Gizmo
  int gizmo_op_ = 0;  // 0 move, 1 rotate, 2 scale
  bool gizmo_local_ = false;
  bool snap_ = false;
  float snap_move_ = 10, snap_rot_ = 15, snap_scale_ = 0.1f;
  bool gizmo_was_using_ = false;
  bool link_ = true;
  float link_radius_ = 40;
  bool editing_ = false;
  std::vector<std::pair<int, Transform>> edit_snapshot_;
  std::vector<std::pair<int, std::vector<Vec3>>> poly_snapshot_;
  std::vector<std::vector<Vec3>> original_polys_;
  bool collision_dirty_ = false;
  bool move_collision_ = true;

  // View
  Camera cam_;
  glm::mat4 view_{1}, proj_{1};
  bool show_models_ = true, show_collision_ = false, show_track_ = true, show_missing_ = true;
  bool show_background_ = true;
  std::vector<char> background_;  // per instance: huge ground/sky model (> 8000 units)
  bool IsBackground(int i) const { return i < int(background_.size()) && background_[i]; }
  // Track pieces (roads, guardrails): world-space geometry far away from the
  // object's own origin, usually placed at (0,0,0).
  bool IsWorldPiece(int i) const;
  bool look_active_ = false;

  // Track spline
  bool track_mode_ = false;
  std::vector<TrackHandle> handles_;
  int handle_ = -1;
  Curves handle_base_;
  Vec3 handle_start_{};

  // Structures
  std::vector<Structure> structures_;
  std::string structure_filter_;
  int armed_ = -1;  // structure that the next click in the 3D view places
  bool open_structures_tab_ = false;
  bool hover_valid_ = false;
  glm::vec3 hover_{0};
  glm::vec3 place_offset_{0};  // offset of the structure preview (after snapping)

  // Magnetism feedback: snapped pair of points (drawn one frame later).
  struct SnapLine
  {
    bool valid = false;
    glm::vec3 from{0}, to{0};
  };
  SnapLine snap_line_, snap_line_draw_;
  std::map<std::string, std::vector<glm::vec3>> vertex_cache_;
  // Placed object -> the objects and polygons placed together with it.
  std::map<int, std::pair<std::vector<int>, std::vector<int>>> spawn_groups_;

  // Popups / status
  int pending_course_ = -1;
  bool confirm_quit_ = false;
  bool show_settings_ = false;
  bool quit_ = false;
  std::string status_;
  bool status_error_ = false;
  double status_time_ = 0;
};
}  // namespace fzgx
