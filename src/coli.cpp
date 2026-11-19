#include "coli.hpp"

#include <algorithm>
#include <cmath>
#include <map>

namespace fzgx
{
namespace
{
constexpr float kPi = 3.14159265358979f;

// Header offsets (GX, big-endian).
constexpr std::size_t kTrackNodes = 0x08;        // count, ptr  (0x0C bytes each)
constexpr std::size_t kStaticCollider = 0x1C;    // ptr
constexpr std::size_t kInstanceCount = 0x48;     // count
constexpr std::size_t kInstancePtr = 0x54;       // ptr (0x40 bytes each)
constexpr std::size_t kSceneObjects = 0x64;      // count, ptr (0x10 bytes each)
constexpr int kSurfaceTypes = 14;

Vec3 ReadVec3(const Bytes& d, std::size_t o) { return {F32(d, o), F32(d, o + 4), F32(d, o + 8)}; }
bool ValidPtr(const Bytes& d, std::uint32_t p, std::size_t size) { return p && p + size <= d.size(); }
}  // namespace

std::int16_t DegToAngle(float deg)
{
  long v = std::lround(deg / 360.0f * 65536.0f);
  v = ((v % 65536) + 65536) % 65536;
  return std::int16_t(std::uint16_t(v));
}
float AngleToDeg(std::int16_t a) { return float(a) * 360.0f / 65536.0f; }

Mat34 ComposeMatrix(const Vec3& p, const std::array<std::int16_t, 3>& rot, const Vec3& s)
{
  const float x = AngleToDeg(rot[0]) * kPi / 180, y = AngleToDeg(rot[1]) * kPi / 180,
              z = AngleToDeg(rot[2]) * kPi / 180;
  const float cx = std::cos(x), sx = std::sin(x), cy = std::cos(y), sy = std::sin(y),
              cz = std::cos(z), sz = std::sin(z);
  // R = Rz * Ry * Rx
  const float r[3][3] = {
      {cz * cy, cz * sy * sx - sz * cx, cz * sy * cx + sz * sx},
      {sz * cy, sz * sy * sx + cz * cx, sz * sy * cx - cz * sx},
      {-sy, cy * sx, cy * cx},
  };
  Mat34 m{};
  for (int i = 0; i < 3; ++i)
  {
    for (int j = 0; j < 3; ++j)
      m[i * 4 + j] = r[i][j] * s[j];
    m[i * 4 + 3] = p[i];
  }
  return m;
}

bool DecomposeMatrix(const Mat34& m, Vec3& pos, Vec3& rot, Vec3& scale)
{
  float r[3][3];
  for (int j = 0; j < 3; ++j)
  {
    const float len = std::sqrt(m[j] * m[j] + m[4 + j] * m[4 + j] + m[8 + j] * m[8 + j]);
    if (len < 1e-8f)
      return false;
    scale[j] = len;
    for (int i = 0; i < 3; ++i)
      r[i][j] = m[i * 4 + j] / len;
  }
  pos = {m[3], m[7], m[11]};
  const float sy = std::clamp(-r[2][0], -1.0f, 1.0f);
  const float y = std::asin(sy);
  float x, z;
  if (std::fabs(sy) < 0.99999f)
  {
    x = std::atan2(r[2][1], r[2][2]);
    z = std::atan2(r[1][0], r[0][0]);
  }
  else  // gimbal lock (y = +-90 deg): put the remaining rotation into x
  {
    z = 0;
    x = std::atan2(sy * r[0][1], r[1][1]);
  }
  rot = {x * 180 / kPi, y * 180 / kPi, z * 180 / kPi};
  return true;
}

Course Course::Parse(Bytes data)
{
  Course c;
  c.data_ = std::move(data);
  const Bytes& d = c.data_;
  Check(d.size() > 0x100, "coli: file too small");

  // --- Placed instances ---------------------------------------------------
  const std::uint32_t count = U32(d, kInstanceCount);
  const std::uint32_t base = U32(d, kInstancePtr);
  Check(count < 10000 && ValidPtr(d, base, std::size_t(count) * 0x40), "coli: bad instance table");
  std::map<std::uint32_t, int> matrix_users;
  for (std::uint32_t i = 0; i < count; ++i)
  {
    SceneInstance in;
    const std::uint32_t o = base + i * 0x40;
    in.offset = o;
    in.flags = U32(d, o);
    in.scene_object = U32(d, o + 0x08);
    in.position = ReadVec3(d, o + 0x0C);
    in.rotation = {S16(d, o + 0x18), S16(d, o + 0x1A), S16(d, o + 0x1C)};
    in.scale = ReadVec3(d, o + 0x20);
    in.animated = U32(d, o + 0x2C) != 0;
    in.matrix_offset = U32(d, o + 0x3C);
    if (ValidPtr(d, in.scene_object, 0x10))
    {
      const std::uint32_t lod_count = U32(d, in.scene_object + 0x04);
      const std::uint32_t lods = U32(d, in.scene_object + 0x08);
      if (lod_count > 0 && ValidPtr(d, lods, 0x10))
      {
        const std::uint32_t name = U32(d, lods + 0x04);
        if (ValidPtr(d, name, 1))
          in.name = CString(d, name);
      }
    }
    if (ValidPtr(d, in.matrix_offset, 0x30))
    {
      for (int k = 0; k < 12; ++k)
        in.matrix[k] = F32(d, in.matrix_offset + 4 * k);
      ++matrix_users[in.matrix_offset];
    }
    else
    {
      in.matrix_offset = 0;
      in.matrix = ComposeMatrix(in.position, in.rotation, in.scale);
    }
    c.instances_.push_back(std::move(in));
  }
  for (auto& in : c.instances_)
    in.shared_matrix = in.matrix_offset && matrix_users[in.matrix_offset] > 1;

  // --- Scene objects (model references) -------------------------------------
  const std::uint32_t so_count = U32(d, kSceneObjects);
  const std::uint32_t so_base = U32(d, kSceneObjects + 4);
  if (so_count < 10000 && ValidPtr(d, so_base, std::size_t(so_count) * 0x10))
    for (std::uint32_t i = 0; i < so_count; ++i)
    {
      SceneObjectRef so;
      so.offset = so_base + i * 0x10;
      const std::uint32_t lod_count = U32(d, so.offset + 0x04);
      const std::uint32_t lods = U32(d, so.offset + 0x08);
      if (lod_count > 0 && ValidPtr(d, lods, 0x10))
        if (const std::uint32_t name = U32(d, lods + 0x04); ValidPtr(d, name, 1))
          so.name = CString(d, name);
      c.scene_objects_.push_back(std::move(so));
    }

  // --- Track checkpoints (centre line + width) ------------------------------
  const std::uint32_t node_count = U32(d, kTrackNodes);
  const std::uint32_t nodes = U32(d, kTrackNodes + 4);
  if (node_count < 100000 && ValidPtr(d, nodes, std::size_t(node_count) * 0x0C))
  {
    for (std::uint32_t i = 0; i < node_count; ++i)
    {
      const std::uint32_t n = U32(d, nodes + i * 0x0C);
      const std::uint32_t cps = U32(d, nodes + i * 0x0C + 4);
      if (n > 64 || !ValidPtr(d, cps, std::size_t(n) * 0x50))
        continue;
      for (std::uint32_t k = 0; k < n; ++k)
      {
        const std::uint32_t o = cps + k * 0x50;
        Checkpoint cp;
        cp.start_normal = ReadVec3(d, o + 0x0C);
        cp.start = ReadVec3(d, o + 0x18);
        cp.end_normal = ReadVec3(d, o + 0x28);
        cp.end = ReadVec3(d, o + 0x34);
        cp.width = F32(d, o + 0x48);
        c.checkpoints_.push_back(cp);
      }
    }
  }

  // --- Static collision (walls, special surfaces) ---------------------------
  const std::uint32_t sc = U32(d, kStaticCollider);
  if (ValidPtr(d, sc, 0xB4))
  {
    c.static_collider_ = sc;
    CollisionGrid& g = c.grid_;
    g.min_x = F32(d, sc + 0x60);
    g.min_z = F32(d, sc + 0x64);
    g.step_x = F32(d, sc + 0x68);
    g.step_z = F32(d, sc + 0x6C);
    g.nx = U32(d, sc + 0x70);
    g.nz = U32(d, sc + 0x74);
    g.tri_tables = sc + 0x28;
    g.quad_tables = sc + 0x7C;
    auto scan = [&](std::size_t grid_table, std::uint32_t poly_base, bool quad, std::size_t stride) {
      if (g.nx == 0 || g.nz == 0 || g.nx * g.nz > 4096 || !poly_base)
        return;
      std::map<int, std::pair<std::vector<int>, std::vector<int>>> refs;  // idx -> surfaces, cells
      for (int s = 0; s < kSurfaceTypes; ++s)
      {
        const std::uint32_t grid = U32(d, grid_table + 4 * s);
        if (!ValidPtr(d, grid, std::size_t(g.nx) * g.nz * 4))
          continue;
        for (std::uint32_t cell = 0; cell < g.nx * g.nz; ++cell)
        {
          std::uint32_t list = U32(d, grid + 4 * cell);
          if (!list)
            continue;
          for (int guard = 0; guard < 65536 && list + 2 <= d.size(); ++guard, list += 2)
          {
            const std::uint16_t idx = U16(d, list);
            if (idx == 0xFFFF)
              break;
            auto& [surfs, cells] = refs[idx];
            if (std::find(surfs.begin(), surfs.end(), s) == surfs.end())
              surfs.push_back(s);
            if (std::find(cells.begin(), cells.end(), int(cell)) == cells.end())
              cells.push_back(int(cell));
          }
        }
      }
      const int verts = quad ? 4 : 3;
      for (auto& [idx, sc_pair] : refs)
      {
        const std::size_t o = poly_base + std::size_t(idx) * stride;
        if (!ValidPtr(d, std::uint32_t(o), stride))
          continue;
        StaticPoly p;
        p.quad = quad;
        p.index = idx;
        p.offset = std::uint32_t(o);
        for (int k = 0; k < verts; ++k)
          p.v.push_back(ReadVec3(d, o + 0x10 + 12 * k));
        p.surfaces = sc_pair.first;
        p.cells = sc_pair.second;
        c.polys_.push_back(std::move(p));
      }
    };
    scan(sc + 0x28, U32(d, sc + 0x24), false, 0x58);
    scan(sc + 0x7C, U32(d, sc + 0x78), true, 0x70);
  }
  c.RebuildCollision();
  return c;
}

void Course::RebuildCollision()
{
  collision_ = {};
  for (int i = 0; i < int(polys_.size()); ++i)
  {
    const auto& p = polys_[i];
    if (p.removed)
      continue;
    const int surf = p.surfaces.empty() ? 0 : p.surfaces.front();
    collision_.triangles.insert(collision_.triangles.end(), {p.v[0], p.v[1], p.v[2]});
    collision_.surface.push_back(surf);
    collision_.poly.push_back(i);
    if (p.quad)
    {
      collision_.triangles.insert(collision_.triangles.end(), {p.v[0], p.v[2], p.v[3]});
      collision_.surface.push_back(surf);
      collision_.poly.push_back(i);
    }
  }
}

int Course::AddInstance(int source)
{
  const SceneInstance& src = instances_.at(std::size_t(source));
  SceneInstance in = src;
  if (src.added)
    in.raw = src.raw;
  else
    std::copy_n(data_.begin() + src.offset, 0x40, in.raw.begin());
  in.added = true;
  in.removed = false;
  in.own_matrix = src.HasMatrix();
  in.offset = 0;
  in.matrix_offset = 0;
  in.shared_matrix = false;
  instances_.push_back(std::move(in));
  return int(instances_.size()) - 1;
}

int Course::AddPoly(int source)
{
  StaticPoly p = polys_.at(std::size_t(source));
  p.added = true;
  p.removed = false;
  p.modified = false;
  p.cells.clear();
  p.offset = 0;
  p.index = -1;
  polys_.push_back(std::move(p));
  return int(polys_.size()) - 1;
}

namespace
{
void Align(Bytes& out, std::size_t a)
{
  while (out.size() % a)
    out.push_back(0);
}

Vec3 Sub(const Vec3& a, const Vec3& b) { return {a[0] - b[0], a[1] - b[1], a[2] - b[2]}; }
Vec3 Cross(const Vec3& a, const Vec3& b)
{
  return {a[1] * b[2] - a[2] * b[1], a[2] * b[0] - a[0] * b[2], a[0] * b[1] - a[1] * b[0]};
}
float Dot(const Vec3& a, const Vec3& b) { return a[0] * b[0] + a[1] * b[1] + a[2] * b[2]; }

void WritePoly(Bytes& out, const StaticPoly& p)
{
  const Vec3 c = Cross(Sub(p.v[1], p.v[0]), Sub(p.v[2], p.v[0]));
  const float area2 = std::sqrt(Dot(c, c));
  Vec3 n = area2 > 0 ? Vec3{c[0] / area2, c[1] / area2, c[2] / area2} : Vec3{0, 1, 0};
  PutF32(out, p.offset, -Dot(n, p.v[0]));
  for (int k = 0; k < 3; ++k)
    PutF32(out, p.offset + 4 + 4 * k, n[k]);
  const int nv = int(p.v.size());
  for (int i = 0; i < nv; ++i)
    for (int k = 0; k < 3; ++k)
      PutF32(out, p.offset + 0x10 + 12 * i + 4 * k, p.v[i][k]);
  for (int i = 0; i < nv; ++i)
  {
    // Edge vector scaled by the parallelogram area at corner i (edge i and the previous vertex).
    const Vec3 edge = Sub(p.v[(i + 1) % nv], p.v[i]);
    const Vec3 corner = Cross(edge, Sub(p.v[(i + nv - 1) % nv], p.v[i]));
    const float scale = std::sqrt(Dot(corner, corner));
    const Vec3 e = Cross(n, edge);
    for (int k = 0; k < 3; ++k)
      PutF32(out, p.offset + 0x10 + 12 * nv + 12 * i + 4 * k, e[k] * scale);
  }
}
}  // namespace

std::vector<int> CellsCovered(const CollisionGrid& g, const StaticPoly& p, bool* outside)
{
  float x0 = 1e30f, x1 = -1e30f, z0 = 1e30f, z1 = -1e30f;
  for (const auto& v : p.v)
  {
    x0 = std::min(x0, v[0]);
    x1 = std::max(x1, v[0]);
    z0 = std::min(z0, v[2]);
    z1 = std::max(z1, v[2]);
  }
  auto cx = [&](float x) { return int(std::floor((x - g.min_x) / g.step_x)); };
  auto cz = [&](float z) { return int(std::floor((z - g.min_z) / g.step_z)); };
  const int a = cx(x0), b = cx(x1), e = cz(z0), f = cz(z1);
  if (outside)
    *outside = a < 0 || e < 0 || b >= int(g.nx) || f >= int(g.nz);
  std::vector<int> cells;
  for (int z = std::max(e, 0); z <= std::min(f, int(g.nz) - 1); ++z)
    for (int x = std::max(a, 0); x <= std::min(b, int(g.nx) - 1); ++x)
      cells.push_back(z * int(g.nx) + x);
  return cells;
}

Bytes Course::Serialize() const
{
  Bytes out = data_;
  std::vector<StaticPoly> polys = polys_;
  // Added collision polygons: copy the whole triangle/quad array to the end of
  // the file and append the new ones (polygons are only reached through the
  // grid index lists, the arrays have no count).
  for (const bool quad : {false, true})
  {
    int max_index = -1, n_added = 0;
    for (const auto& p : polys)
    {
      if (p.quad != quad || p.removed)
        continue;
      if (p.added)
        ++n_added;
      else
        max_index = std::max(max_index, p.index);
    }
    if (!n_added || !static_collider_)
      continue;
    const std::size_t stride = quad ? 0x70 : 0x58;
    const std::uint32_t slot = static_collider_ + (quad ? 0x78 : 0x24);
    const std::uint32_t old_base = U32(out, slot);
    Align(out, 0x20);
    const std::uint32_t base = std::uint32_t(out.size());
    out.resize(out.size() + std::size_t(max_index + 1 + n_added) * stride);
    if (old_base && max_index >= 0)
      std::copy_n(out.begin() + old_base, std::size_t(max_index + 1) * stride, out.begin() + base);
    PutU32(out, slot, base);
    int next = max_index + 1;
    for (auto& p : polys)
    {
      if (p.quad != quad || p.removed)
        continue;
      if (p.added)
        p.index = next++;
      p.offset = base + std::uint32_t(p.index) * std::uint32_t(stride);
    }
    Check(next < 0xFFFF, "coli: too many collision polygons");
  }

  // Polygons that were moved or added: new geometry + grid references for
  // every cell they now cover. New index lists are appended to the file.
  std::map<std::uint32_t, std::vector<std::uint16_t>> additions;  // cell pointer slot -> indices
  for (const auto& p : polys)
  {
    if (p.removed || !(p.modified || p.added))
      continue;
    WritePoly(out, p);
    for (int cell : CellsCovered(grid_, p, nullptr))
    {
      if (std::find(p.cells.begin(), p.cells.end(), cell) != p.cells.end())
        continue;
      for (int s : p.surfaces)
      {
        const std::uint32_t table = U32(data_, (p.quad ? grid_.quad_tables : grid_.tri_tables) + 4 * s);
        if (!table)
          continue;
        additions[table + 4 * std::uint32_t(cell)].push_back(std::uint16_t(p.index));
      }
    }
  }
  for (auto& [slot, extra] : additions)
  {
    std::vector<std::uint16_t> list;
    std::uint32_t old = U32(out, slot);
    for (int guard = 0; old && guard < 65536 && old + 2 <= out.size(); ++guard, old += 2)
    {
      const std::uint16_t idx = U16(out, old);
      if (idx == 0xFFFF)
        break;
      list.push_back(idx);
    }
    for (auto i : extra)
      if (std::find(list.begin(), list.end(), i) == list.end())
        list.push_back(i);
    list.push_back(0xFFFF);
    Align(out, 4);
    const std::uint32_t at = std::uint32_t(out.size());
    out.resize(out.size() + list.size() * 2);
    for (std::size_t k = 0; k < list.size(); ++k)
      PutU16(out, at + 2 * k, list[k]);
    PutU32(out, slot, at);
  }
  if (!additions.empty())
    Align(out, 4);

  auto write_fields = [&](std::uint32_t o, const SceneInstance& in) {
    for (int k = 0; k < 3; ++k)
    {
      PutF32(out, o + 0x0C + 4 * k, in.position[k]);
      PutU16(out, o + 0x18 + 2 * k, std::uint16_t(in.rotation[k]));
      PutF32(out, o + 0x20 + 4 * k, in.scale[k]);
    }
  };
  std::vector<const SceneInstance*> added;
  for (const auto& in : instances_)
  {
    if (in.added)
    {
      if (!in.removed)
        added.push_back(&in);
      continue;
    }
    write_fields(in.offset, in);
    if (in.matrix_offset)
      for (int k = 0; k < 12; ++k)
        PutF32(out, in.matrix_offset + 4 * k, in.matrix[k]);
  }
  if (!added.empty())
  {
    // Relocated instance table: the original entries followed by the new ones.
    const std::uint32_t count = U32(data_, kInstanceCount);
    const std::uint32_t old_base = U32(data_, kInstancePtr);
    Align(out, 0x20);
    const std::uint32_t base = std::uint32_t(out.size());
    out.resize(out.size() + std::size_t(count + added.size()) * 0x40);
    std::copy_n(out.begin() + old_base, std::size_t(count) * 0x40, out.begin() + base);
    for (std::size_t k = 0; k < added.size(); ++k)
    {
      const std::uint32_t o = base + std::uint32_t(count + k) * 0x40;
      std::copy(added[k]->raw.begin(), added[k]->raw.end(), out.begin() + o);
      write_fields(o, *added[k]);
      PutU32(out, o + 0x3C, 0);
    }
    for (std::size_t k = 0; k < added.size(); ++k)
    {
      if (!added[k]->own_matrix)
        continue;
      const std::uint32_t m = std::uint32_t(out.size());
      out.resize(out.size() + 0x30);
      for (int j = 0; j < 12; ++j)
        PutF32(out, m + 4 * j, added[k]->matrix[j]);
      PutU32(out, base + std::uint32_t(count + k) * 0x40 + 0x3C, m);
    }
    Align(out, 0x20);
    PutU32(out, kInstanceCount, count + std::uint32_t(added.size()));
    PutU32(out, kInstancePtr, base);
  }
  return out;
}
}  // namespace fzgx
