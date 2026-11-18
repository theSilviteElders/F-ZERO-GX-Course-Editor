#include "gma.hpp"

#include "lz.hpp"

#include <algorithm>
#include <cmath>

namespace fzgx
{
namespace
{
// GX vertex attribute bits used in the GCMF vertex flags.
constexpr std::uint32_t kPnMtxIdx = 1u << 0;
constexpr std::uint32_t kPos = 1u << 9;
constexpr std::uint32_t kNrm = 1u << 10;
constexpr std::uint32_t kClr0 = 1u << 11;
constexpr std::uint32_t kClr1 = 1u << 12;
constexpr std::uint32_t kNbt = 1u << 25;

struct VertexLayout
{
  int size = 0;
  int pos = -1, nrm = -1, clr = -1;
};

VertexLayout Layout(std::uint32_t vf)
{
  VertexLayout l;
  int s = 0;
  if (vf & kPnMtxIdx)
    s += 1;
  for (int k = 0; k < 8; ++k)
    if (vf & (2u << k))
      s += 1;
  l.pos = s;
  s += 12;
  if (vf & kNrm)
  {
    l.nrm = s;
    s += 12;
  }
  if (vf & kNbt)
  {
    l.nrm = s;
    s += 36;
  }
  if (vf & kClr0)
  {
    l.clr = s;
    s += 4;
  }
  if (vf & kClr1)
    s += 4;
  for (int k = 0; k < 8; ++k)
    if (vf & (0x2000u << k))
      s += 8;
  l.size = s;
  return l;
}

void DecodeDisplayList(const Bytes& d, std::size_t o, std::size_t size, std::uint32_t vf,
                       std::array<std::uint8_t, 4> tint, ModelMesh& mesh)
{
  const VertexLayout l = Layout(vf);
  const std::size_t end = std::min(o + size, d.size());
  struct V
  {
    std::array<float, 3> p, n;
    std::array<std::uint8_t, 4> c;
    bool has_n;
  };
  std::vector<V> prim;
  while (o < end)
  {
    const std::uint8_t op = d[o];
    if (op == 0)  // GX_NOP padding
    {
      ++o;
      continue;
    }
    const std::uint8_t cmd = op & 0xF8;
    if (cmd != 0x90 && cmd != 0x98 && cmd != 0xA0)
      return;  // unknown command: stop decoding this list
    const int count = U16(d, o + 1);
    o += 3;
    if (o + std::size_t(count) * l.size > end)
      return;
    prim.clear();
    for (int i = 0; i < count; ++i, o += l.size)
    {
      V v;
      for (int k = 0; k < 3; ++k)
        v.p[k] = F32(d, o + l.pos + 4 * k);
      v.has_n = l.nrm >= 0;
      if (v.has_n)
        for (int k = 0; k < 3; ++k)
          v.n[k] = F32(d, o + l.nrm + 4 * k);
      v.c = tint;
      if (l.clr >= 0)
        for (int k = 0; k < 4; ++k)
          v.c[k] = std::uint8_t(d[o + l.clr + k] * tint[k] / 255);
      prim.push_back(v);
    }
    auto emit = [&](const V& a, const V& b, const V& c) {
      std::array<float, 3> fn{};
      if (!a.has_n)
      {
        const float u[3] = {b.p[0] - a.p[0], b.p[1] - a.p[1], b.p[2] - a.p[2]};
        const float w[3] = {c.p[0] - a.p[0], c.p[1] - a.p[1], c.p[2] - a.p[2]};
        fn = {u[1] * w[2] - u[2] * w[1], u[2] * w[0] - u[0] * w[2], u[0] * w[1] - u[1] * w[0]};
        const float len = std::sqrt(fn[0] * fn[0] + fn[1] * fn[1] + fn[2] * fn[2]);
        if (len > 0)
          for (auto& x : fn)
            x /= len;
      }
      for (const V* v : {&a, &b, &c})
      {
        mesh.positions.push_back(v->p);
        mesh.normals.push_back(v->has_n ? v->n : fn);
        mesh.colors.push_back(v->c);
      }
    };
    if (cmd == 0x90)
      for (int i = 0; i + 2 < count; i += 3)
        emit(prim[i], prim[i + 1], prim[i + 2]);
    else if (cmd == 0x98)
      for (int i = 0; i + 2 < count; ++i)
        (i & 1) ? emit(prim[i + 1], prim[i], prim[i + 2]) : emit(prim[i], prim[i + 1], prim[i + 2]);
    else
      for (int i = 1; i + 1 < count; ++i)
        emit(prim[0], prim[i], prim[i + 1]);
  }
}
}  // namespace

std::vector<std::pair<std::string, std::size_t>> ListGma(const Bytes& d)
{
  std::vector<std::pair<std::string, std::size_t>> out;
  const std::uint32_t n = U32(d, 0), base = U32(d, 4);
  Check(n < 100000 && 8 + std::size_t(n) * 8 <= d.size(), "gma: bad header");
  const std::size_t names = 8 + std::size_t(n) * 8;
  for (std::uint32_t i = 0; i < n; ++i)
  {
    const std::uint32_t mo = U32(d, 8 + i * 8), no = U32(d, 12 + i * 8);
    if (mo == 0xFFFFFFFF || base + std::size_t(mo) + 4 > d.size())
      continue;
    out.emplace_back(CString(d, names + no), base + std::size_t(mo));
  }
  return out;
}

ModelMesh DecodeGcmf(const Bytes& d, std::size_t o)
{
  ModelMesh mesh;
  Check(o + 0x40 <= d.size() && std::equal(d.begin() + o, d.begin() + o + 4, "GCMF"), "gcmf: bad magic");
  const std::uint32_t flags = U32(d, o + 0x04);
  const float cx = F32(d, o + 0x08), cy = F32(d, o + 0x0C), cz = F32(d, o + 0x10), r = F32(d, o + 0x14);
  mesh.bounds_min = {cx - r, cy - r, cz - r};
  mesh.bounds_max = {cx + r, cy + r, cz + r};
  // 0x01 = 16-bit vertices, 0x08 = skinned, 0x10 = effective model: different vertex storage.
  if (flags & 0x19)
  {
    mesh.skinned = true;
    return mesh;
  }
  const int meshes = U16(d, o + 0x1A) + U16(d, o + 0x1C);
  std::size_t p = o + U32(d, o + 0x20);
  for (int m = 0; m < meshes && p + 0x60 <= d.size(); ++m)
  {
    std::array<std::uint8_t, 4> tint = {d[p + 4], d[p + 5], d[p + 6], 255};
    // Some materials are nearly black (lit purely by textures); keep them visible.
    if (tint[0] + tint[1] + tint[2] < 120)
      tint = {170, 170, 170, 255};
    const std::uint8_t dl_flags = d[p + 0x13];
    const std::uint32_t vf = U32(d, p + 0x1C);
    const std::uint32_t s0 = U32(d, p + 0x28), s1 = U32(d, p + 0x2C);
    std::size_t q = p + 0x60;
    if (dl_flags & 1)
    {
      DecodeDisplayList(d, q, s0, vf, tint, mesh);
      q += s0;
    }
    if (dl_flags & 2)
    {
      DecodeDisplayList(d, q, s1, vf, tint, mesh);
      q += s1;
    }
    if (dl_flags & 0x0C)
    {
      const std::uint32_t e0 = U32(d, q + 0x08), e1 = U32(d, q + 0x0C);
      q += 0x20;
      if (dl_flags & 4)
      {
        DecodeDisplayList(d, q, e0, vf, tint, mesh);
        q += e0;
      }
      if (dl_flags & 8)
      {
        DecodeDisplayList(d, q, e1, vf, tint, mesh);
        q += e1;
      }
    }
    p = q;
  }
  if (!mesh.positions.empty())
  {
    mesh.bounds_min = mesh.bounds_max = mesh.positions[0];
    for (const auto& v : mesh.positions)
      for (int k = 0; k < 3; ++k)
      {
        mesh.bounds_min[k] = std::min(mesh.bounds_min[k], v[k]);
        mesh.bounds_max[k] = std::max(mesh.bounds_max[k], v[k]);
      }
  }
  return mesh;
}

void ModelLibrary::AddArchive(const std::filesystem::path& path)
{
  auto raw = ReadFile(path);
  if (!raw)
    return;
  Archive a{path, path.extension() == ".lz" ? LzDecompress(*raw) : std::move(*raw)};
  const int id = int(archives_.size());
  for (const auto& [name, offset] : ListGma(a.data))
    index_.try_emplace(name, Entry{id, offset});
  archives_.push_back(std::move(a));
}

const ModelMesh* ModelLibrary::Find(const std::string& name)
{
  if (auto it = cache_.find(name); it != cache_.end())
    return it->second.get();
  auto it = index_.find(name);
  std::unique_ptr<ModelMesh> mesh;
  if (it != index_.end())
  {
    try
    {
      mesh = std::make_unique<ModelMesh>(DecodeGcmf(archives_[it->second.archive].data, it->second.offset));
    }
    catch (const FormatError&)
    {
      mesh.reset();
    }
  }
  const ModelMesh* result = mesh.get();
  cache_[name] = std::move(mesh);
  return result;
}

std::vector<std::string> ModelLibrary::archives() const
{
  std::vector<std::string> out;
  for (const auto& a : archives_)
    out.push_back(a.path.filename().string());
  return out;
}
}  // namespace fzgx
