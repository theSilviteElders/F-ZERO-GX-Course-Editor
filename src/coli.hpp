// COLI_COURSExx scene data (decompressed). Only the parts the editor needs are
// interpreted; every other byte is kept as-is so saving is a byte-exact patch.
#pragma once
#include "binary.hpp"

#include <array>
#include <string>

namespace fzgx
{
using Vec3 = std::array<float, 3>;
// Row-major 3x4 matrix exactly as stored by the game (rotation*scale | translation).
using Mat34 = std::array<float, 12>;

// One placed instance ("dynamic scene object"), 0x40 bytes:
//  0x00 u32 flags  0x04 u32 unk  0x08 ptr SceneObject
//  0x0C float3 position  0x18 s16x3 rotation (65536 = 360 deg)  0x1E u16
//  0x20 float3 scale  0x2C..0x38 animation/texture/skeleton pointers
//  0x3C ptr 3x4 transform matrix (M = Rz*Ry*Rx*S, verified against all objects)
struct SceneInstance
{
  std::uint32_t offset = 0;
  std::uint32_t flags = 0;
  std::uint32_t scene_object = 0;
  std::string name;  // model name (first LOD)
  Vec3 position{};
  std::array<std::int16_t, 3> rotation{};
  Vec3 scale{1, 1, 1};
  std::uint32_t matrix_offset = 0;  // 0 if none
  Mat34 matrix{};
  bool animated = false;            // pointer at 0x2C set (meaning not verified yet)
  bool shared_matrix = false;       // matrix pointer is used by another instance too
  // Instances added in the editor: raw 0x40 bytes of the template they were
  // copied from, written into a relocated instance table on save.
  bool added = false;
  bool removed = false;             // added, then deleted again (kept for undo)
  bool own_matrix = false;          // added instance gets its own 3x4 matrix
  std::array<std::uint8_t, 0x40> raw{};
  bool HasMatrix() const { return matrix_offset != 0 || own_matrix; }
};

// Scene object (0x10 bytes, list at header 0x64 count / 0x68 ptr):
//  0x04 u32 LOD count  0x08 ptr LOD list (0x04 ptr name)
struct SceneObjectRef
{
  std::uint32_t offset = 0;
  std::string name;
};

// --- Track spline (see docs/track-format.md) ---------------------------------
// Animation key, 0x14 bytes: u32 mode (1 linear, 2/3 Hermite, 0 constant),
// f32 time, value, tangent in, tangent out (slopes per time unit).
struct CurveKey
{
  std::uint32_t mode = 2;
  float time = 0, value = 0, tan_in = 0, tan_out = 0;
  bool operator==(const CurveKey&) const = default;
};
// Channels: scale x/y/z, rotation x/y/z (degrees), position x/y/z.
using Curves = std::array<std::vector<CurveKey>, 9>;
float EvalCurve(const std::vector<CurveKey>& keys, float t, float fallback);

// Track segment, 0x50 bytes: 0x00 u32 flags, 0x04 ptr curves (9 counts, 9 ptrs),
// 0x0C/0x10 children (count, ptr to array of segments), 0x14 local scale,
// 0x20 local rotation (deg), 0x2C local position, 0x3C/0x40 rail heights, 0x4C branch.
// Transform = T * Rz * Ry * Rx * S, a channel with keys replaces the local value.
struct TrackSegment
{
  std::uint32_t offset = 0, flags = 0, curves_offset = 0;
  Curves curves, curves_orig;
  std::array<float, 9> local{};
  int parent = -1;
  std::vector<int> children;
  int depth = 0;
};

// Checkpoint (0x50 bytes) of a track node: precomputed samples of the spline
// with bounding planes, track distance and width.
struct Checkpoint
{
  std::uint32_t offset = 0;
  int node = 0;
  float t0 = 0, t1 = 0;
  Vec3 start{}, end{};
  Vec3 start_normal{}, end_normal{};  // start: forward tangent, end: backward
  float dist0 = 0, dist1 = 0;
  float width = 0;
  Vec3 side0{1, 0, 0}, side1{1, 0, 0};  // lateral direction (for drawing the surface)
  // Segment whose transform the checkpoint follows when curves are edited,
  // and the loaded state (edits are applied as a rigid delta of that frame).
  int anchor = -1;
  float anchor_error = 0;  // distance between the anchor and the stored position
  Vec3 start_orig{}, end_orig{}, start_normal_orig{}, end_normal_orig{};
  float dist0_orig = 0, dist1_orig = 0;
  Mat34 frame0_orig{}, frame1_orig{};
};

struct TrackNode
{
  int root = -1;  // segment
  std::vector<int> checkpoints;
};

// A movable point of the spline: a segment's origin at one key time.
struct TrackHandle
{
  int segment = -1;
  float time = 0;
};

struct CollisionPolys
{
  std::vector<Vec3> triangles;  // 3 vertices per triangle
  std::vector<int> surface;     // surface type per triangle (first grid that references it)
  std::vector<int> poly;        // index into Course::polys() per triangle
};

// Static collision polygon (triangle 0x58 bytes / quad 0x70 bytes):
//  0x00 float d = -n.v0   0x04 float3 n = normalize((v1-v0) x (v2-v0))
//  0x10 float3 v[3|4]     then float3 e[3|4], e_k = (n x (v[k+1]-v[k])) * |(v[k+1]-v[k]) x (v[k-1]-v[k])|
// Polygons are found through per-surface 2D grids (u16 index lists, 0xFFFF-terminated).
// Surface type 3 = dash plate (verified on course 01).
struct StaticPoly
{
  bool quad = false;
  int index = 0;            // index within the triangle or quad array
  std::uint32_t offset = 0;
  std::vector<Vec3> v;
  std::vector<int> surfaces;
  std::vector<int> cells;   // grid cells that reference this polygon
  bool modified = false;
  bool added = false;       // copy made in the editor, appended on save
  bool removed = false;     // added, then deleted again (kept for undo)
};

struct CollisionGrid
{
  float min_x = 0, min_z = 0, step_x = 1, step_z = 1;
  std::uint32_t nx = 0, nz = 0;
  std::uint32_t tri_tables = 0, quad_tables = 0;  // offsets of the 14 grid pointers
};

// Spatial index of the track nodes (header 0xBC): nx*nz cells over the
// checkpoint bounds, each listing the nodes within 500 units (max of x/z).
struct CheckpointGrid
{
  float min_x = 0, min_z = 0, step_x = 1, step_z = 1;
  int nx = 0, nz = 0;
  std::vector<std::vector<std::uint16_t>> cells;
};

class Course
{
public:
  static Course Parse(Bytes data);

  const Bytes& data() const { return data_; }
  std::vector<SceneInstance>& instances() { return instances_; }
  const std::vector<SceneInstance>& instances() const { return instances_; }
  const std::vector<Checkpoint>& checkpoints() const { return checkpoints_; }
  const std::vector<TrackSegment>& segments() const { return segments_; }
  const std::vector<TrackNode>& track_nodes() const { return track_nodes_; }
  float track_length() const { return track_length_; }
  bool track_modified() const { return track_modified_; }

  // Track spline evaluation (world transform of a segment at time t).
  Mat34 SegmentLocal(int s, float t) const;
  Mat34 SegmentWorld(int s, float t) const;
  // Points that can be moved: every key time of a segment's position curves.
  std::vector<TrackHandle> TrackHandles() const;
  Vec3 HandlePosition(const TrackHandle& h) const;
  // Sets the segment's curves to `base` with the handle moved by a world delta
  // (keys are inserted where a channel has none at that time).
  void MoveHandle(const TrackHandle& h, const Curves& base, const Vec3& world_delta);
  void SetCurves(int s, const Curves& c);
  // Re-derives checkpoints, distances and track length after curve edits.
  void UpdateTrack();
  const CollisionPolys& collision() const { return collision_; }
  std::vector<StaticPoly>& polys() { return polys_; }
  const std::vector<StaticPoly>& polys() const { return polys_; }
  const CollisionGrid& grid() const { return grid_; }
  const std::vector<SceneObjectRef>& scene_objects() const { return scene_objects_; }
  void RebuildCollision();

  // Appends a copy of instance `source` (same scene object, flags and
  // pointers) and returns its index. It is written into a relocated instance
  // table on save (no pointers into the table exist besides header 0x54).
  int AddInstance(int source);
  // Appends a copy of collision polygon `source` (same surfaces); it gets a
  // new index in a relocated triangle/quad array on save.
  int AddPoly(int source);

  // Writes the editable fields of all instances back into the raw buffer.
  Bytes Serialize() const;

private:
  Bytes data_;
  std::vector<SceneInstance> instances_;
  std::vector<Checkpoint> checkpoints_;
  std::vector<TrackSegment> segments_;
  std::vector<TrackNode> track_nodes_;
  float track_length_ = 0, track_length_orig_ = 0;
  bool track_modified_ = false;
  void ParseTrack();
  void WriteTrack(Bytes& out) const;
  CollisionPolys collision_;
  std::vector<StaticPoly> polys_;
  std::vector<SceneObjectRef> scene_objects_;
  CollisionGrid grid_;
  std::uint32_t static_collider_ = 0;
};

Mat34 ComposeMatrix(const Vec3& pos, const std::array<std::int16_t, 3>& rot, const Vec3& scale);
CheckpointGrid BuildCheckpointGrid(const Course& c, int nx, int nz);
Mat34 ComposeMatrixDeg(const Vec3& pos, const Vec3& rot_deg, const Vec3& scale);
Mat34 MulMatrix(const Mat34& a, const Mat34& b);
Vec3 TransformPoint(const Mat34& m, const Vec3& p);
// Inverse of ComposeMatrix (rotation in degrees). Returns false for a degenerate matrix.
bool DecomposeMatrix(const Mat34& m, Vec3& pos, Vec3& rot_deg, Vec3& scale);
std::int16_t DegToAngle(float deg);
// Grid cells a polygon's XZ bounds overlap; *outside = parts lie outside the grid.
std::vector<int> CellsCovered(const CollisionGrid& g, const StaticPoly& p, bool* outside);
float AngleToDeg(std::int16_t a);
}  // namespace fzgx
