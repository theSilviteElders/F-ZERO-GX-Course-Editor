// Track spline: segments with animation curves, checkpoints, checkpoint grid.
// Format notes and verification status: docs/track-format.md.
#include "coli.hpp"

#include <algorithm>
#include <cmath>
#include <functional>
#include <map>

namespace fzgx
{
namespace
{
constexpr std::size_t kTrackNodes = 0x08;      // count, ptr (0x0C bytes: count, ptr checkpoints, ptr segment)
constexpr std::size_t kSurfaceAreas = 0x10;    // count, ptr (0x14 bytes: from, to, left, right, type)
constexpr std::size_t kTrackLength = 0x90;     // ptr to f32
constexpr std::size_t kCheckpointGrid = 0xBC;  // ptr to nx*nz list pointers
constexpr std::size_t kGridBounds = 0xC0;      // min x, min z, step x, step z, u32 nx, u32 nz
constexpr float kGridReach = 500;              // nodes within this (max of x/z distance) of a cell

bool Valid(const Bytes& d, std::uint32_t p, std::size_t size) { return p && p + size <= d.size(); }
Vec3 Read3(const Bytes& d, std::size_t o) { return {F32(d, o), F32(d, o + 4), F32(d, o + 8)}; }
void Put3(Bytes& d, std::size_t o, const Vec3& v)
{
  for (int k = 0; k < 3; ++k)
    PutF32(d, o + 4 * k, v[k]);
}
Vec3 Sub(const Vec3& a, const Vec3& b) { return {a[0] - b[0], a[1] - b[1], a[2] - b[2]}; }
float Dot(const Vec3& a, const Vec3& b) { return a[0] * b[0] + a[1] * b[1] + a[2] * b[2]; }
float Dist(const Vec3& a, const Vec3& b) { return std::sqrt(Dot(Sub(a, b), Sub(a, b))); }
Vec3 Normalize(const Vec3& v, const Vec3& fallback)
{
  const float l = std::sqrt(Dot(v, v));
  return l > 1e-9f ? Vec3{v[0] / l, v[1] / l, v[2] / l} : fallback;
}
Vec3 Column(const Mat34& m, int c) { return {m[c], m[4 + c], m[8 + c]}; }
Vec3 Origin(const Mat34& m) { return {m[3], m[7], m[11]}; }
Vec3 TransformVector(const Mat34& m, const Vec3& v)
{
  return {m[0] * v[0] + m[1] * v[1] + m[2] * v[2], m[4] * v[0] + m[5] * v[1] + m[6] * v[2],
          m[8] * v[0] + m[9] * v[1] + m[10] * v[2]};
}

Mat34 Identity() { return {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0}; }

// General inverse of an affine 3x4 matrix (identity if singular).
Mat34 Inverse(const Mat34& m)
{
  const float a = m[0], b = m[1], c = m[2], d = m[4], e = m[5], f = m[6], g = m[8], h = m[9], i = m[10];
  const float det = a * (e * i - f * h) - b * (d * i - f * g) + c * (d * h - e * g);
  if (std::fabs(det) < 1e-12f)
    return Identity();
  const float s = 1.0f / det;
  Mat34 r{};
  r[0] = (e * i - f * h) * s;
  r[1] = (c * h - b * i) * s;
  r[2] = (b * f - c * e) * s;
  r[4] = (f * g - d * i) * s;
  r[5] = (a * i - c * g) * s;
  r[6] = (c * d - a * f) * s;
  r[8] = (d * h - e * g) * s;
  r[9] = (b * g - a * h) * s;
  r[10] = (a * e - b * d) * s;
  const Vec3 t = TransformVector(r, Origin(m));
  r[3] = -t[0];
  r[7] = -t[1];
  r[11] = -t[2];
  return r;
}

// Position + normalised axes (scale removed): what checkpoints follow.
Mat34 Frame(const Mat34& m)
{
  static const Vec3 axes[3] = {{1, 0, 0}, {0, 1, 0}, {0, 0, 1}};
  Mat34 r = m;
  for (int c = 0; c < 3; ++c)
  {
    const Vec3 v = Normalize(Column(m, c), axes[c]);
    for (int k = 0; k < 3; ++k)
      r[k * 4 + c] = v[k];
  }
  return r;
}

void Align(Bytes& out, std::size_t a)
{
  while (out.size() % a)
    out.push_back(0);
}

// Max of x/z distance between the segment a-b and the cell rectangle.
float ReachXZ(const Vec3& a, const Vec3& b, float x0, float z0, float x1, float z1)
{
  float best = 1e30f;
  for (int s = 0; s <= 64; ++s)
  {
    const float f = s / 64.0f;
    const float x = a[0] + (b[0] - a[0]) * f, z = a[2] + (b[2] - a[2]) * f;
    const float dx = std::max({x0 - x, 0.0f, x - x1}), dz = std::max({z0 - z, 0.0f, z - z1});
    best = std::min(best, std::max(dx, dz));
  }
  return best;
}
}  // namespace

float EvalCurve(const std::vector<CurveKey>& keys, float t, float fallback)
{
  if (keys.empty())
    return fallback;
  if (t <= keys.front().time)
    return keys.front().value;
  if (t >= keys.back().time)
    return keys.back().value;
  for (std::size_t k = 0; k + 1 < keys.size(); ++k)
  {
    const CurveKey& a = keys[k];
    const CurveKey& b = keys[k + 1];
    if (t < a.time || t > b.time)
      continue;
    const float dt = b.time - a.time;
    const float s = dt > 0 ? (t - a.time) / dt : 0;
    if (a.mode == 0)
      return a.value;
    if (a.mode == 1)
      return a.value + (b.value - a.value) * s;
    const float s2 = s * s, s3 = s2 * s;
    return (2 * s3 - 3 * s2 + 1) * a.value + (s3 - 2 * s2 + s) * dt * a.tan_out + (-2 * s3 + 3 * s2) * b.value +
           (s3 - s2) * dt * b.tan_in;
  }
  return keys.back().value;
}

Mat34 Course::SegmentLocal(int s, float t) const
{
  const TrackSegment& seg = segments_[s];
  std::array<float, 9> v = seg.local;
  for (int k = 0; k < 9; ++k)
    v[k] = EvalCurve(seg.curves[k], t, v[k]);
  return ComposeMatrixDeg({v[6], v[7], v[8]}, {v[3], v[4], v[5]}, {v[0], v[1], v[2]});
}

Mat34 Course::SegmentWorld(int s, float t) const
{
  Mat34 m = SegmentLocal(s, t);
  for (int p = segments_[s].parent; p >= 0; p = segments_[p].parent)
    m = MulMatrix(SegmentLocal(p, t), m);
  return m;
}

void Course::ParseTrack()
{
  const Bytes& d = data_;
  const std::uint32_t node_count = U32(d, kTrackNodes);
  const std::uint32_t nodes = U32(d, kTrackNodes + 4);
  if (node_count == 0 || node_count > 100000 || !Valid(d, nodes, std::size_t(node_count) * 0x0C))
    return;

  std::map<std::uint32_t, int> index;
  std::function<int(std::uint32_t, int, int)> load = [&](std::uint32_t off, int parent, int depth) -> int {
    if (auto it = index.find(off); it != index.end())
      return it->second;
    Check(Valid(d, off, 0x50) && depth < 32, "coli: bad track segment");
    TrackSegment s;
    s.offset = off;
    s.flags = U32(d, off);
    s.curves_offset = U32(d, off + 0x04);
    s.parent = parent;
    s.depth = depth;
    for (int k = 0; k < 9; ++k)
      s.local[k] = F32(d, off + 0x14 + 4 * k);
    if (Valid(d, s.curves_offset, 0x48))
      for (int k = 0; k < 9; ++k)
      {
        const std::uint32_t n = U32(d, s.curves_offset + 4 * k);
        const std::uint32_t p = U32(d, s.curves_offset + 0x24 + 4 * k);
        if (n == 0 || n > 10000 || !Valid(d, p, std::size_t(n) * 0x14))
          continue;
        for (std::uint32_t i = 0; i < n; ++i)
        {
          const std::uint32_t o = p + i * 0x14;
          s.curves[k].push_back({U32(d, o), F32(d, o + 4), F32(d, o + 8), F32(d, o + 12), F32(d, o + 16)});
        }
      }
    s.curves_orig = s.curves;
    const int self = int(segments_.size());
    index[off] = self;
    segments_.push_back(std::move(s));
    const std::uint32_t child_count = U32(d, off + 0x0C), children = U32(d, off + 0x10);
    if (child_count < 64 && Valid(d, children, std::size_t(child_count) * 0x50))
      for (std::uint32_t k = 0; k < child_count; ++k)
      {
        const int c = load(children + k * 0x50, self, depth + 1);
        segments_[self].children.push_back(c);
      }
    return self;
  };

  for (std::uint32_t i = 0; i < node_count; ++i)
  {
    const std::uint32_t n = U32(d, nodes + i * 0x0C);
    const std::uint32_t cps = U32(d, nodes + i * 0x0C + 4);
    const std::uint32_t root = U32(d, nodes + i * 0x0C + 8);
    TrackNode node;
    if (Valid(d, root, 0x50))
      node.root = load(root, -1, 0);
    if (n <= 64 && Valid(d, cps, std::size_t(n) * 0x50))
      for (std::uint32_t k = 0; k < n; ++k)
      {
        const std::uint32_t o = cps + k * 0x50;
        Checkpoint cp;
        cp.offset = o;
        cp.node = int(i);
        cp.t0 = F32(d, o);
        cp.t1 = F32(d, o + 0x04);
        cp.start_normal = Read3(d, o + 0x0C);
        cp.start = Read3(d, o + 0x18);
        cp.end_normal = Read3(d, o + 0x28);
        cp.end = Read3(d, o + 0x34);
        cp.dist0 = F32(d, o + 0x40);
        cp.dist1 = F32(d, o + 0x44);
        cp.width = F32(d, o + 0x48);
        node.checkpoints.push_back(int(checkpoints_.size()));
        checkpoints_.push_back(cp);
      }
    track_nodes_.push_back(std::move(node));
  }
  const std::uint32_t len = U32(d, kTrackLength);
  track_length_ = Valid(d, len, 4) ? F32(d, len) : (checkpoints_.empty() ? 0 : checkpoints_.back().dist1);
  track_length_orig_ = track_length_;

  // Anchor every checkpoint to the segment (in its node's tree) that matches it best.
  for (auto& cp : checkpoints_)
  {
    const int root = track_nodes_[cp.node].root;
    if (root >= 0)
    {
      std::vector<int> stack{root};
      float best = 1e30f;
      while (!stack.empty())
      {
        const int s = stack.back();
        stack.pop_back();
        for (int c : segments_[s].children)
          stack.push_back(c);
        const float e = Dist(Origin(SegmentWorld(s, cp.t0)), cp.start) + Dist(Origin(SegmentWorld(s, cp.t1)), cp.end);
        if (e < best - 0.01f || (std::fabs(e - best) <= 0.01f && cp.anchor >= 0 && segments_[s].depth > segments_[cp.anchor].depth))
        {
          best = std::min(best, e);
          cp.anchor = s;
        }
      }
      cp.anchor_error = best * 0.5f;
      const Mat34 w0 = SegmentWorld(cp.anchor, cp.t0), w1 = SegmentWorld(cp.anchor, cp.t1);
      cp.frame0_orig = Frame(w0);
      cp.frame1_orig = Frame(w1);
      cp.side0 = Normalize(Column(w0, 0), {1, 0, 0});
      cp.side1 = Normalize(Column(w1, 0), {1, 0, 0});
    }
    cp.start_orig = cp.start;
    cp.end_orig = cp.end;
    cp.start_normal_orig = cp.start_normal;
    cp.end_normal_orig = cp.end_normal;
    cp.dist0_orig = cp.dist0;
    cp.dist1_orig = cp.dist1;
  }
}

std::vector<TrackHandle> Course::TrackHandles() const
{
  std::vector<TrackHandle> out;
  for (int s = 0; s < int(segments_.size()); ++s)
  {
    std::vector<float> times;
    for (int k = 6; k < 9; ++k)
      for (const auto& key : segments_[s].curves[k])
        times.push_back(key.time);
    std::sort(times.begin(), times.end());
    for (std::size_t i = 0; i < times.size(); ++i)
      if (i == 0 || times[i] - times[i - 1] > 1e-4f)
        out.push_back({s, times[i]});
  }
  return out;
}

Vec3 Course::HandlePosition(const TrackHandle& h) const { return Origin(SegmentWorld(h.segment, h.time)); }

void Course::SetCurves(int s, const Curves& c) { segments_[s].curves = c; }

void Course::MoveHandle(const TrackHandle& h, const Curves& base, const Vec3& world_delta)
{
  TrackSegment& seg = segments_[h.segment];
  seg.curves = base;
  const Mat34 parent = seg.parent >= 0 ? SegmentWorld(seg.parent, h.time) : Identity();
  Mat34 lin = parent;
  lin[3] = lin[7] = lin[11] = 0;
  const Vec3 delta = TransformVector(Inverse(lin), world_delta);

  // Neighbouring key times of this segment (bound the change of a channel without keys).
  std::vector<float> times;
  for (int k = 6; k < 9; ++k)
    for (const auto& key : base[k])
      times.push_back(key.time);
  float before = -1e30f, after = 1e30f;
  for (float t : times)
  {
    if (t < h.time - 1e-4f)
      before = std::max(before, t);
    if (t > h.time + 1e-4f)
      after = std::min(after, t);
  }

  for (int c = 0; c < 3; ++c)
  {
    if (std::fabs(delta[c]) < 1e-6f)
      continue;
    const int k = 6 + c;
    auto& keys = seg.curves[k];
    if (keys.empty())
    {
      const float v = seg.local[k];
      if (before > -1e29f)
        keys.push_back({2, before, v, 0, 0});
      keys.push_back({2, h.time, v + delta[c], 0, 0});
      if (after < 1e29f)
        keys.push_back({2, after, v, 0, 0});
      continue;
    }
    auto it = std::find_if(keys.begin(), keys.end(), [&](const CurveKey& key) { return std::fabs(key.time - h.time) <= 1e-4f; });
    if (it != keys.end())
    {
      it->value += delta[c];
      continue;
    }
    // Insert a key that keeps the current shape (value + slope), then move it.
    const float span = std::max(1e-3f, keys.back().time - keys.front().time);
    const float e = span * 1e-4f;
    const float v = EvalCurve(keys, h.time, seg.local[k]);
    const float slope = (EvalCurve(keys, h.time + e, v) - EvalCurve(keys, h.time - e, v)) / (2 * e);
    auto pos = std::find_if(keys.begin(), keys.end(), [&](const CurveKey& key) { return key.time > h.time; });
    const std::uint32_t mode = pos != keys.begin() ? std::prev(pos)->mode : pos->mode;
    keys.insert(pos, {mode, h.time, v + delta[c], slope, slope});
  }
}

void Course::UpdateTrack()
{
  track_modified_ = false;
  for (const auto& s : segments_)
    track_modified_ |= s.curves != s.curves_orig;
  for (auto& cp : checkpoints_)
  {
    if (cp.anchor < 0)
      continue;
    const Mat34 w0 = SegmentWorld(cp.anchor, cp.t0), w1 = SegmentWorld(cp.anchor, cp.t1);
    const Mat34 d0 = MulMatrix(Frame(w0), Inverse(cp.frame0_orig));
    const Mat34 d1 = MulMatrix(Frame(w1), Inverse(cp.frame1_orig));
    cp.start = TransformPoint(d0, cp.start_orig);
    cp.end = TransformPoint(d1, cp.end_orig);
    cp.start_normal = Normalize(TransformVector(d0, cp.start_normal_orig), cp.start_normal_orig);
    cp.end_normal = Normalize(TransformVector(d1, cp.end_normal_orig), cp.end_normal_orig);
    cp.side0 = Normalize(Column(w0, 0), {1, 0, 0});
    cp.side1 = Normalize(Column(w1, 0), {1, 0, 0});
  }
  // Track distance: cumulative checkpoint lengths along the nodes.
  float shift = 0;
  for (const auto& node : track_nodes_)
  {
    if (node.checkpoints.empty())
      continue;
    float next_shift = shift;
    for (std::size_t k = 0; k < node.checkpoints.size(); ++k)
    {
      Checkpoint& cp = checkpoints_[node.checkpoints[k]];
      const float old_len = Dist(cp.start_orig, cp.end_orig), new_len = Dist(cp.start, cp.end);
      cp.dist0 = cp.dist0_orig + shift;
      cp.dist1 = cp.dist0 + (cp.dist1_orig - cp.dist0_orig) * (old_len > 1e-4f ? new_len / old_len : 1.0f);
      if (k == 0)
        next_shift = cp.dist1 - cp.dist1_orig;
    }
    shift = next_shift;
  }
  track_length_ = track_length_orig_ + shift;
}

CheckpointGrid BuildCheckpointGrid(const Course& c, int nx, int nz)
{
  CheckpointGrid g;
  g.nx = nx;
  g.nz = nz;
  float x0 = 1e30f, x1 = -1e30f, z0 = 1e30f, z1 = -1e30f;
  for (const auto& cp : c.checkpoints())
    for (const Vec3& p : {cp.start, cp.end})
    {
      x0 = std::min(x0, p[0]);
      x1 = std::max(x1, p[0]);
      z0 = std::min(z0, p[2]);
      z1 = std::max(z1, p[2]);
    }
  if (x0 > x1 || nx <= 0 || nz <= 0)
    return g;
  g.min_x = x0;
  g.min_z = z0;
  g.step_x = std::max(1e-3f, (x1 - x0) / float(nx));
  g.step_z = std::max(1e-3f, (z1 - z0) / float(nz));
  g.cells.resize(std::size_t(nx) * nz);
  const auto& nodes = c.track_nodes();
  for (int z = 0; z < nz; ++z)
    for (int x = 0; x < nx; ++x)
    {
      const float ax = g.min_x + x * g.step_x, az = g.min_z + z * g.step_z;
      for (int n = 0; n < int(nodes.size()); ++n)
        for (int k : nodes[n].checkpoints)
        {
          const auto& cp = c.checkpoints()[k];
          // Small margin for the sampling: the result may only be a superset.
          const float margin = 1 + Dist(cp.start, cp.end) / 128;
          if (ReachXZ(cp.start, cp.end, ax, az, ax + g.step_x, az + g.step_z) <= kGridReach + margin)
          {
            g.cells[std::size_t(z) * nx + x].push_back(std::uint16_t(n));
            break;
          }
        }
    }
  return g;
}

void Course::WriteTrack(Bytes& out) const
{
  // Curves: changed channels get a new key array at the end of the file.
  for (const auto& s : segments_)
  {
    if (!Valid(out, s.curves_offset, 0x48))
      continue;
    for (int k = 0; k < 9; ++k)
    {
      if (s.curves[k] == s.curves_orig[k])
        continue;
      Align(out, 4);
      const std::uint32_t at = std::uint32_t(out.size());
      out.resize(out.size() + s.curves[k].size() * 0x14);
      for (std::size_t i = 0; i < s.curves[k].size(); ++i)
      {
        const CurveKey& key = s.curves[k][i];
        const std::uint32_t o = at + std::uint32_t(i) * 0x14;
        PutU32(out, o, key.mode);
        PutF32(out, o + 4, key.time);
        PutF32(out, o + 8, key.value);
        PutF32(out, o + 12, key.tan_in);
        PutF32(out, o + 16, key.tan_out);
      }
      PutU32(out, s.curves_offset + 4 * k, std::uint32_t(s.curves[k].size()));
      PutU32(out, s.curves_offset + 0x24 + 4 * k, s.curves[k].empty() ? 0 : at);
    }
  }
  // Checkpoints (fixed size, in place).
  for (const auto& cp : checkpoints_)
  {
    PutF32(out, cp.offset + 0x08, -Dot(cp.start_normal, cp.start));
    Put3(out, cp.offset + 0x0C, cp.start_normal);
    Put3(out, cp.offset + 0x18, cp.start);
    PutF32(out, cp.offset + 0x24, -Dot(cp.end_normal, cp.end));
    Put3(out, cp.offset + 0x28, cp.end_normal);
    Put3(out, cp.offset + 0x34, cp.end);
    PutF32(out, cp.offset + 0x40, cp.dist0);
    PutF32(out, cp.offset + 0x44, cp.dist1);
  }
  if (const std::uint32_t len = U32(out, kTrackLength); Valid(out, len, 4))
    PutF32(out, len, track_length_);

  // Surface areas are placed by track distance: map old -> new distance.
  std::vector<std::pair<float, float>> knots;
  for (const auto& node : track_nodes_)
    if (!node.checkpoints.empty())
    {
      const auto& cp = checkpoints_[node.checkpoints.front()];
      knots.push_back({cp.dist0_orig, cp.dist0});
    }
  knots.push_back({track_length_orig_, track_length_});
  auto remap = [&](float x) {
    for (std::size_t k = 0; k + 1 < knots.size(); ++k)
      if (x >= knots[k].first && x <= knots[k + 1].first)
      {
        const float span = knots[k + 1].first - knots[k].first;
        const float f = span > 1e-6f ? (x - knots[k].first) / span : 0;
        return knots[k].second + f * (knots[k + 1].second - knots[k].second);
      }
    return x <= knots.front().first ? x + knots.front().second - knots.front().first
                                    : x + knots.back().second - knots.back().first;
  };
  const std::uint32_t area_count = U32(out, kSurfaceAreas), areas = U32(out, kSurfaceAreas + 4);
  if (area_count < 10000 && Valid(out, areas, std::size_t(area_count) * 0x14))
    for (std::uint32_t i = 0; i < area_count; ++i)
      for (int k = 0; k < 2; ++k)
      {
        const std::size_t o = areas + i * 0x14 + 4 * k;
        PutF32(out, o, remap(F32(data_, o)));
      }

  // Checkpoint grid: new bounds and index lists.
  const std::uint32_t table = U32(out, kCheckpointGrid);
  const int nx = int(U32(out, kGridBounds + 0x10)), nz = int(U32(out, kGridBounds + 0x14));
  if (nx > 0 && nz > 0 && nx * nz <= 4096 && Valid(out, table, std::size_t(nx) * nz * 4))
  {
    const CheckpointGrid g = BuildCheckpointGrid(*this, nx, nz);
    if (!g.cells.empty())
    {
      PutF32(out, kGridBounds, g.min_x);
      PutF32(out, kGridBounds + 4, g.min_z);
      PutF32(out, kGridBounds + 8, g.step_x);
      PutF32(out, kGridBounds + 12, g.step_z);
      for (std::size_t cell = 0; cell < g.cells.size(); ++cell)
      {
        Align(out, 4);
        const std::uint32_t at = std::uint32_t(out.size());
        out.resize(out.size() + (g.cells[cell].size() + 1) * 2);
        for (std::size_t i = 0; i < g.cells[cell].size(); ++i)
          PutU16(out, at + 2 * i, g.cells[cell][i]);
        PutU16(out, at + 2 * g.cells[cell].size(), 0xFFFF);
        PutU32(out, table + 4 * std::uint32_t(cell), at);
      }
    }
  }
  Align(out, 0x20);
}
}  // namespace fzgx
