// fzgx-cli: format self-test and small dump utilities.
//   fzgx-cli selftest <workspace>
//   fzgx-cli unlz <in.lz> <out.bin>
//   fzgx-cli lz <in.bin> <out.lz>
#include "lz.hpp"
#include "project.hpp"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <iostream>

using namespace fzgx;

static int SelfTest(const fs::path& workspace)
{
  Settings s;
  s.workspace = workspace;
  Project project(s);
  if (!project.DiscReady())
  {
    std::cerr << "no disc in " << s.disc() << "\n";
    return 1;
  }
  int failures = 0;
  std::size_t total_in = 0, total_out = 0;
  for (const auto& info : project.ListCourses())
  {
    const fs::path path = s.disc() / "files" / "stage" / Project::CourseFile(info.id);
    const Bytes raw = *ReadFile(path);
    const auto t0 = std::chrono::steady_clock::now();
    const Bytes data = LzDecompress(raw);
    const bool header_ok = U32LE(raw, 0) + 8 == raw.size();
    const Course course = Course::Parse(data);
    const bool roundtrip = course.Serialize() == data;
    const Bytes packed = LzCompress(data);
    const bool lz_ok = LzDecompress(packed) == data;
    const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - t0).count();
    total_in += raw.size();
    total_out += packed.size();

    float max_err = 0;
    int shared = 0, animated = 0, with_matrix = 0;
    for (const auto& in : course.instances())
    {
      shared += in.shared_matrix;
      animated += in.animated;
      if (!in.matrix_offset)
        continue;
      ++with_matrix;
      const Mat34 m = ComposeMatrix(in.position, in.rotation, in.scale);
      for (int k = 0; k < 12; ++k)
      {
        const float scale = k % 4 == 3 ? std::max(1.0f, std::fabs(in.matrix[k])) : 1.0f;
        max_err = std::max(max_err, std::fabs(m[k] - in.matrix[k]) / scale);
      }
    }
    std::string venue;
    ModelLibrary lib = project.BuildModels(info.id, course, &venue);
    int found = 0, empty = 0, skinned = 0;
    std::size_t tris = 0;
    for (const auto& in : course.instances())
    {
      const ModelMesh* m = lib.Find(in.name);
      if (!m)
        continue;
      ++found;
      skinned += m->skinned;
      empty += !m->skinned && m->positions.empty();
      tris += m->positions.size() / 3;
    }
    // Recompute every collision polygon (plane + edge vectors) and compare with the original.
    float poly_err = 0;
    {
      Course all = course;
      for (auto& p : all.polys())
        p.modified = true;
      const Bytes re = all.Serialize();
      for (const auto& p : all.polys())
      {
        // Compare as vectors (relative to their length): plane d, normal, vertices, edge vectors.
        const std::size_t nvec = p.quad ? 9 : 7;
        // d = -n.v0 scales with the coordinates, so compare it relative to them.
        const float coord = std::sqrt(p.v[0][0] * p.v[0][0] + p.v[0][1] * p.v[0][1] + p.v[0][2] * p.v[0][2]);
        poly_err = std::max(poly_err, std::fabs(F32(data, p.offset) - F32(re, p.offset)) / std::max(1.0f, coord));
        for (std::size_t v = 0; v < nvec; ++v)
        {
          const std::size_t o = p.offset + 4 + 12 * v;
          float diff = 0, len = 0;
          for (int k = 0; k < 3; ++k)
          {
            const float a = F32(data, o + 4 * k), b = F32(re, o + 4 * k);
            diff += (a - b) * (a - b);
            len += a * a;
          }
          poly_err = std::max(poly_err, std::sqrt(diff) / std::max(1.0f, std::sqrt(len)));
        }
      }
    }
    const bool ok = header_ok && roundtrip && lz_ok && poly_err < 2e-3f;
    failures += !ok;
    std::printf("%s %-20s %-14s inst=%3zu mtx=%3d anim=%3d shared=%d err=%.1e models=%3d/%3zu skinned=%d empty=%d tris=%7zu cps=%4zu coll=%5zu polyerr=%.1e lz %6zu->%6zu %lldms\n",
                ok ? "OK  " : "FAIL", Project::CourseFile(info.id).c_str(), venue.c_str(),
                course.instances().size(), with_matrix, animated, shared, max_err, found,
                course.instances().size(), skinned, empty, tris, course.checkpoints().size(),
                course.collision().triangles.size() / 3, poly_err, raw.size(), packed.size(), (long long)ms);
  }
  std::printf("lz total: original %zu, ours %zu bytes\n", total_in, total_out);
  return failures ? 1 : 0;
}

int main(int argc, char** argv)
{
  try
  {
    const std::string cmd = argc > 1 ? argv[1] : "";
    if (cmd == "dump" && argc == 4)
    {
      Settings st;
      st.workspace = argv[2];
      Project pr(st);
      const int id = std::atoi(argv[3]);
      const Course c = pr.LoadCourse(id);
      ModelLibrary lib = pr.BuildModels(id, c, nullptr);
      int i = 0;
      for (const auto& in : c.instances())
      {
        const ModelMesh* m = lib.Find(in.name);
        float ext = 0;
        if (m)
          for (int k = 0; k < 3; ++k)
            ext = std::max(ext, (m->bounds_max[k] - m->bounds_min[k]) * std::fabs(in.scale[k]));
        std::printf("%4d %-28s flags=%08X pos=(%.0f %.0f %.0f) scale=(%.2f %.2f %.2f) %s extent=%.0f tris=%zu\n", i++,
                    in.name.c_str(), in.flags, in.position[0], in.position[1], in.position[2], in.scale[0], in.scale[1],
                    in.scale[2], !m ? "MISSING" : m->skinned ? "SKINNED" : "", ext, m ? m->positions.size() / 3 : 0);
      }
      return 0;
    }
    if (cmd == "move" && argc == 8)  // move <workspace> <course> <index> dx dy dz
    {
      Settings st;
      st.workspace = argv[2];
      Project pr(st);
      const int id = std::atoi(argv[3]), idx = std::atoi(argv[4]);
      Course c = pr.LoadCourse(id);
      auto& in = c.instances().at(std::size_t(idx));
      for (int k = 0; k < 3; ++k)
        in.position[k] += float(std::atof(argv[5 + k]));
      in.matrix = ComposeMatrix(in.position, in.rotation, in.scale);
      std::string msg;
      const bool ok = pr.SaveCourse(id, c, msg);
      std::printf("%s\n", msg.c_str());
      return ok ? 0 : 1;
    }
    if (cmd == "dashscale" && argc == 6)  // dashscale <workspace> <course> <factor> <out.lz>: test only
    {
      Settings st;
      st.workspace = argv[2];
      Project pr(st);
      Course c = pr.LoadCourse(std::atoi(argv[3]));
      const float f = float(std::atof(argv[4]));
      for (auto& p : c.polys())
      {
        if (std::find(p.surfaces.begin(), p.surfaces.end(), 3) == p.surfaces.end())
          continue;
        Vec3 cen{};
        for (const auto& v : p.v)
          for (int k = 0; k < 3; ++k)
            cen[k] += v[k] / float(p.v.size());
        for (auto& v : p.v)
          for (int k = 0; k < 3; k += 2)
            v[k] = cen[k] + (v[k] - cen[k]) * f;
        p.modified = true;
        std::printf("%s %d cells before:", p.quad ? "quad" : "tri", p.index);
        for (int cell : p.cells)
          std::printf(" %d", cell);
        std::printf("  covered now:");
        for (int cell : CellsCovered(c.grid(), p, nullptr))
          std::printf(" %d", cell);
        std::printf("\n");
      }
      const Bytes out = c.Serialize();
      std::printf("size %zu -> %zu\n", c.data().size(), out.size());
      const Course re = Course::Parse(out);
      for (const auto& p : re.polys())
        if (std::find(p.surfaces.begin(), p.surfaces.end(), 3) != p.surfaces.end())
        {
          std::printf("reparsed %s %d cells:", p.quad ? "quad" : "tri", p.index);
          for (int cell : p.cells)
            std::printf(" %d", cell);
          std::printf("  v0=(%.1f %.1f %.1f)\n", p.v[0][0], p.v[0][1], p.v[0][2]);
        }
      return WriteFileAtomic(argv[5], LzCompress(out)) ? 0 : 1;
    }
    if (cmd == "selftest" && argc == 3)
      return SelfTest(argv[2]);
    if ((cmd == "unlz" || cmd == "lz") && argc == 4)
    {
      auto in = ReadFile(argv[2]);
      if (!in)
        return 1;
      return WriteFileAtomic(argv[3], cmd == "lz" ? LzCompress(*in) : LzDecompress(*in)) ? 0 : 1;
    }
    std::cerr << "usage: fzgx-cli selftest <workspace> | unlz <in> <out> | lz <in> <out>\n";
    return 2;
  }
  catch (const std::exception& e)
  {
    std::cerr << "error: " << e.what() << "\n";
    return 1;
  }
}
