// fzgx-cli: format self-test and small dump utilities.
//   fzgx-cli selftest <workspace>
//   fzgx-cli unlz <in.lz> <out.bin>
//   fzgx-cli lz <in.bin> <out.lz>
#include "lz.hpp"
#include "project.hpp"
#include "tpl.hpp"

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
    // Add a copied object and a copied collision polygon, save, re-parse.
    bool add_ok = true;
    if (!course.instances().empty())
    {
      Course c = course;
      const int src = int(c.instances().size()) / 2;
      const int a = c.AddInstance(src);
      auto& in = c.instances()[a];
      in.position = {in.position[0] + 50, in.position[1] + 5, in.position[2] - 30};
      in.matrix = ComposeMatrix(in.position, in.rotation, in.scale);
      const int gone = c.AddInstance(src);
      c.instances()[gone].removed = true;
      int pa = -1;
      // First polygon that still lies inside the collision grid after the shift.
      for (int k = 0; k < int(c.polys().size()) && pa < 0; ++k)
      {
        StaticPoly shifted = c.polys()[k];
        for (auto& v : shifted.v)
          v[0] += 20;
        bool outside = false;
        if (CellsCovered(c.grid(), shifted, &outside).empty() || outside || shifted.surfaces.empty())
          continue;
        pa = c.AddPoly(k);
        c.polys()[pa].v = shifted.v;
      }
      const Course re = Course::Parse(c.Serialize());
      add_ok = re.instances().size() == course.instances().size() + 1;
      if (add_ok)
      {
        const auto& r = re.instances().back();
        add_ok = r.name == in.name && r.position == in.position && r.rotation == in.rotation &&
                 r.HasMatrix() == in.HasMatrix() && (!r.matrix_offset || r.matrix == in.matrix) &&
                 r.flags == course.instances()[src].flags;
        for (std::size_t k = 0; k < course.instances().size() && add_ok; ++k)
          add_ok = re.instances()[k].position == course.instances()[k].position &&
                   re.instances()[k].name == course.instances()[k].name;
      }
      if (add_ok && pa >= 0)
      {
        const auto& want = c.polys()[pa];
        bool found = false;
        for (const auto& p : re.polys())
          found |= p.quad == want.quad && p.v == want.v && p.surfaces == want.surfaces;
        add_ok = found && re.polys().size() == course.polys().size() + 1;
      }
    }
    // Track spline: anchors, checkpoint grid rule, editing round trip.
    float anchor_err = 0;
    for (const auto& cp : course.checkpoints())
      anchor_err = std::max(anchor_err, cp.anchor_error);
    int grid_missing = 0, grid_extra = 0;
    {
      const std::uint32_t table = U32(data, 0xBC);
      const int nx = int(U32(data, 0xD0)), nz = int(U32(data, 0xD4));
      const CheckpointGrid g = BuildCheckpointGrid(course, nx, nz);
      for (int cell = 0; table && cell < nx * nz; ++cell)
      {
        std::vector<std::uint16_t> orig;
        for (std::uint32_t l = U32(data, table + 4 * cell); l && U16(data, l) != 0xFFFF; l += 2)
          orig.push_back(U16(data, l));
        const auto& mine = g.cells.at(std::size_t(cell));
        for (auto n : orig)
          grid_missing += std::find(mine.begin(), mine.end(), n) == mine.end();
        grid_extra += int(mine.size()) - int(orig.size());
      }
    }
    bool track_ok = true;
    {
      Course c = course;
      const auto handles = c.TrackHandles();
      if (!handles.empty())
      {
        const TrackHandle h = handles[handles.size() / 2];
        const Vec3 before = c.HandlePosition(h);
        c.MoveHandle(h, c.segments()[h.segment].curves, {0, 40, 25});
        c.UpdateTrack();
        const Vec3 after = c.HandlePosition(h);
        track_ok = c.track_modified() && std::fabs(after[1] - before[1] - 40) < 0.05f && std::fabs(after[2] - before[2] - 25) < 0.05f;
        const Course re = Course::Parse(c.Serialize());
        track_ok &= re.instances().size() == c.instances().size() && re.segments().size() == c.segments().size() &&
                    re.checkpoints().size() == c.checkpoints().size() && std::fabs(re.track_length() - c.track_length()) < 1e-3f;
        for (std::size_t k = 0; track_ok && k < re.segments().size(); ++k)
          track_ok = re.segments()[k].curves == c.segments()[k].curves;
        for (std::size_t k = 0; track_ok && k < re.checkpoints().size(); ++k)
        {
          const auto& a = re.checkpoints()[k];
          const auto& b = c.checkpoints()[k];
          track_ok = a.start == b.start && a.end == b.end && a.dist0 == b.dist0 && a.dist1 == b.dist1;
        }
        if (!track_ok)
          std::printf("  track edit failed: handle seg %d t=%.3f\n", h.segment, h.time);
      }
    }
    const bool ok = header_ok && roundtrip && lz_ok && poly_err < 2e-3f && add_ok && grid_missing == 0 && track_ok;
    failures += !ok;
    std::printf("%s %-20s %-14s inst=%3zu mtx=%3d anim=%3d shared=%d err=%.1e models=%3d/%3zu skinned=%d empty=%d tris=%7zu cps=%4zu coll=%5zu polyerr=%.1e add=%s track: segs=%zu anchor=%.1f grid-%d/+%d edit=%s lz %6zu->%6zu %lldms\n",
                ok ? "OK  " : "FAIL", Project::CourseFile(info.id).c_str(), venue.c_str(),
                course.instances().size(), with_matrix, animated, shared, max_err, found,
                course.instances().size(), skinned, empty, tris, course.checkpoints().size(),
                course.collision().triangles.size() / 3, poly_err, add_ok ? "ok" : "FAIL", course.segments().size(), anchor_err, grid_missing, grid_extra, track_ok ? "ok" : "FAIL", raw.size(), packed.size(), (long long)ms);
  }
  std::printf("lz total: original %zu, ours %zu bytes\n", total_in, total_out);

  // Custom cups: main.dol patch and icon textures (nothing is written).
  {
    bool ok = false;
    std::string what;
    try
    {
      const auto dol = ReadFile(s.disc() / "sys" / "main.dol");
      const Bytes selgf_raw = LzDecompress(*ReadFile(s.disc() / "files" / "bmp_e" / "bmp_selgf.tpl.lz"));
      const TplFile selgf(selgf_raw);
      const auto sel = project.LoadDiscTpl("bmp_sel");
      const std::vector<CupDef> cups = {{"Self Test", {7, 13, 21, 25, 27}, 200}, {"Two", {1}, 60}};
      const std::vector<CourseDef> courses = {{2, "Self Test Road", 16}, {23, "Second", 32}};
      const Bytes patched = PatchDol(*dol, cups, selgf.count(), courses);
      // The custom-course entries must sit inside the patch table, before its terminator.
      const Bytes plain = PatchDol(*dol, cups, selgf.count());
      const bool courses_ok = patched.size() >= plain.size() && ValidateCourses(courses).empty() &&
                              ValidateCups({{"With new", {2, 1}, 0}}, courses).empty() &&
                              !ValidateCups({{"With new", {2, 1}, 0}}).empty();
      const Texture icon = CupIcon(selgf, 200), arrow = LeftArrow(*sel);
      const TplFile out(AppendTextures(selgf_raw, {arrow, icon}));
      bool same = out.count() == selgf.count() + 2;
      for (int i = 0; same && i < selgf.count(); ++i)
      {
        const auto a = selgf.Decode(i), b = out.Decode(i);
        same = a.has_value() == b.has_value() && (!a || a->rgba == b->rgba);
      }
      const auto back = out.Decode(selgf.count() + 1);
      // RGB5A3 keeps 5 bits per channel for opaque pixels.
      bool icon_ok = back && back->width == icon.width && back->height == icon.height;
      for (std::size_t i = 0; icon_ok && i < icon.rgba.size(); ++i)
        icon_ok = std::abs(int(back->rgba[i]) - int(icon.rgba[i])) <= 36;
      ok = patched.size() > dol->size() && same && icon_ok && courses_ok;
      what = "dol " + std::to_string(dol->size()) + "->" + std::to_string(patched.size()) +
             " textures " + std::to_string(selgf.count()) + "->" + std::to_string(out.count()) +
             (same ? "" : " ORIGINAL-TEXTURES-CHANGED") + (icon_ok ? "" : " ICON-MISMATCH") +
             (courses_ok ? "" : " NEW-COURSES-FAILED");
    }
    catch (const std::exception& e)
    {
      what = e.what();
    }
    std::printf("%s cups: %s\n", ok ? "OK  " : "FAIL", what.c_str());
    failures += !ok;
  }
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
    if (cmd == "tplsheet" && argc == 4)  // tplsheet <in.tpl.lz> <out.ppm>: all textures, 128 px cells
    {
      const TplFile tpl(LzDecompress(*ReadFile(argv[2])));
      const int cell = 128, cols = 8, n = tpl.count(), rows = (n + cols - 1) / cols;
      std::vector<std::uint8_t> img(std::size_t(cols * cell) * rows * cell * 3, 40);
      int decoded = 0;
      for (int i = 0; i < n; ++i)
      {
        const auto t = tpl.Decode(i);
        if (!t)
          continue;
        ++decoded;
        for (int y = 0; y < cell; ++y)
          for (int x = 0; x < cell; ++x)
          {
            const int sx = x * t->width / cell, sy = y * t->height / cell;
            const std::uint8_t* p = &t->rgba[(std::size_t(sy) * t->width + sx) * 4];
            // checkerboard behind transparent pixels
            const int bg = ((x / 8 + y / 8) & 1) ? 90 : 60;
            std::uint8_t* q = &img[((std::size_t((i / cols) * cell + y)) * cols * cell + (i % cols) * cell + x) * 3];
            for (int k = 0; k < 3; ++k)
              q[k] = std::uint8_t((p[k] * p[3] + bg * (255 - p[3])) / 255);
          }
      }
      FILE* f = std::fopen(argv[3], "wb");
      std::fprintf(f, "P6 %d %d 255\n", cols * cell, rows * cell);
      std::fwrite(img.data(), 1, img.size(), f);
      std::fclose(f);
      std::printf("%d of %d textures decoded\n", decoded, n);
      return 0;
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
    if (cmd == "cups" && argc == 3)
    {
      Settings st;
      st.workspace = argv[2];
      Project pr(st);
      std::string msg;
      const bool ok = pr.ApplyCups(LoadCups(st.cups_file()), msg, LoadCourses(st.courses_file()));
      std::cout << msg << "\n";
      return ok ? 0 : 1;
    }
    if (cmd == "newcourse" && argc == 6)
    {
      // newcourse <workspace> <slot> <template> <name>: like File -> New course in the editor.
      Settings st;
      st.workspace = argv[2];
      Project pr(st);
      std::vector<CourseDef> courses = LoadCourses(st.courses_file());
      courses.push_back({std::atoi(argv[3]), argv[5], std::atoi(argv[4])});
      std::string msg;
      if (const std::string problem = ValidateCourses(courses); !problem.empty())
        msg = problem;
      else if (pr.CreateCourse(courses.back(), msg) && SaveCourses(st.courses_file(), courses))
      {
        std::cout << msg << "\n";
        const bool ok = pr.ApplyCups(LoadCups(st.cups_file()), msg, courses);
        std::cout << msg << "\n";
        return ok ? 0 : 1;
      }
      std::cout << msg << "\n";
      return 1;
    }
    std::cerr << "usage: fzgx-cli selftest <workspace> | cups <workspace> | newcourse <workspace> <slot> <template> <name> | unlz <in> <out> | lz <in> <out>\n";
    return 2;
  }
  catch (const std::exception& e)
  {
    std::cerr << "error: " << e.what() << "\n";
    return 1;
  }
}
