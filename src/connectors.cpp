#include "connectors.hpp"

#include <glm/gtc/matrix_transform.hpp>

#include <algorithm>
#include <cmath>
#include <map>
#include <set>

namespace fzgx
{
namespace
{
constexpr float kMinWidth = 40;  // narrower loops are holes, decals, lamps ...

struct StripEnd
{
  glm::vec3 a, b, normal;
};

// Major axis of points projected into the plane with normal n.
glm::vec3 MajorAxis(const std::vector<glm::vec3>& pts, const glm::vec3& c, const glm::vec3& n)
{
  const glm::vec3 helper = std::fabs(n.y) < 0.9f ? glm::vec3(0, 1, 0) : glm::vec3(1, 0, 0);
  const glm::vec3 e1 = glm::normalize(glm::cross(helper, n)), e2 = glm::cross(n, e1);
  float xx = 0, xy = 0, yy = 0;
  for (const auto& p : pts)
  {
    const float x = glm::dot(p - c, e1), y = glm::dot(p - c, e2);
    xx += x * x;
    xy += x * y;
    yy += y * y;
  }
  const float angle = 0.5f * std::atan2(2 * xy, xx - yy);
  return std::cos(angle) * e1 + std::sin(angle) * e2;
}
}  // namespace

std::vector<Connector> FindConnectors(const ModelMesh& mesh)
{
  std::vector<Connector> out;
  // Weld the triangle soup (0.1 unit grid) and collect each edge's opposite vertices.
  std::map<std::array<long, 3>, int> ids;
  std::vector<glm::vec3> P;
  auto id = [&](const std::array<float, 3>& q) {
    const std::array<long, 3> k{std::lround(q[0] * 10), std::lround(q[1] * 10), std::lround(q[2] * 10)};
    auto it = ids.find(k);
    if (it != ids.end())
      return it->second;
    ids.emplace(k, int(P.size()));
    P.emplace_back(q[0], q[1], q[2]);
    return int(P.size()) - 1;
  };
  std::map<std::pair<int, int>, std::vector<int>> edges;
  for (std::size_t k = 0; k + 2 < mesh.positions.size(); k += 3)
  {
    const int t[3] = {id(mesh.positions[k]), id(mesh.positions[k + 1]), id(mesh.positions[k + 2])};
    if (t[0] == t[1] || t[1] == t[2] || t[0] == t[2])
      continue;
    for (int e = 0; e < 3; ++e)
    {
      const int x = t[e], y = t[(e + 1) % 3];
      edges[{std::min(x, y), std::max(x, y)}].push_back(t[(e + 2) % 3]);
    }
  }
  auto opposite = [&](int x, int y) -> const std::vector<int>& { return edges[{std::min(x, y), std::max(x, y)}]; };
  std::map<int, std::vector<int>> adj;  // boundary edges (used by one triangle)
  for (const auto& [e, o] : edges)
    if (o.size() == 1)
    {
      adj[e.first].push_back(e.second);
      adj[e.second].push_back(e.first);
    }

  std::vector<StripEnd> ends;
  std::set<int> seen;
  for (const auto& [v0, unused] : adj)
  {
    (void)unused;
    if (seen.count(v0))
      continue;
    // Walk the boundary loop starting at v0 (only simple loops are used).
    std::vector<int> loop{v0};
    bool ok = adj[v0].size() == 2;
    int prev = -1, cur = v0;
    while (ok)
    {
      int next = -1;
      for (int y : adj[cur])
        if (y != prev)
        {
          next = y;
          break;
        }
      if (next < 0 || adj[next].size() != 2)
        ok = false;
      else if (next == v0)
        break;
      else if (std::find(loop.begin(), loop.end(), next) != loop.end() || loop.size() > 4000)
        ok = false;
      else
      {
        loop.push_back(next);
        prev = cur;
        cur = next;
      }
    }
    // Mark the whole component as seen.
    for (std::vector<int> stack{v0}; !stack.empty();)
    {
      const int x = stack.back();
      stack.pop_back();
      seen.insert(x);
      for (int y : adj[x])
        if (!seen.count(y))
          stack.push_back(y);
    }
    if (!ok || loop.size() < 3)
      continue;
    const std::size_t L = loop.size();
    auto pt = [&](std::size_t i) { return P[loop[i % L]]; };

    // Cap loop: planar cross-section at an open end.
    glm::vec3 n(0), c(0);
    for (std::size_t i = 0; i < L; ++i)
    {
      n += glm::cross(pt(i), pt(i + 1));
      c += pt(i);
    }
    c /= float(L);
    if (glm::length(n) > 1e-6f)
    {
      n = glm::normalize(n);
      float width = 0, flat = 0;
      std::vector<glm::vec3> pts;
      for (std::size_t i = 0; i < L; ++i)
      {
        const glm::vec3 d = pt(i) - c;
        width = std::max(width, glm::length(d - n * glm::dot(d, n)));
        flat = std::max(flat, std::fabs(glm::dot(d, n)));
        pts.push_back(pt(i));
      }
      width *= 2;
      if (width >= kMinWidth && flat <= 0.02f * width + 0.5f)
      {
        // The adjacent triangles must leave the plane on one side (else: a hole in a surface).
        int pos = 0, neg = 0, in = 0;
        for (std::size_t i = 0; i < L; ++i)
          for (int o : opposite(loop[i], loop[(i + 1) % L]))
          {
            const float d = glm::dot(P[o] - c, n);
            (d > 1 ? pos : d < -1 ? neg : in)++;
          }
        if (std::max(pos, neg) >= 0.8f * float(pos + neg + in))
        {
          Connector k;
          k.centre = c;
          k.normal = pos > neg ? -n : n;
          k.across = MajorAxis(pts, c, k.normal);
          float lo = 1e30f, hi = -1e30f;
          for (const auto& p : pts)
          {
            lo = std::min(lo, glm::dot(p - c, k.across));
            hi = std::max(hi, glm::dot(p - c, k.across));
          }
          k.width = hi - lo;
          for (std::size_t i = 0; i < L; ++i)
          {
            k.lines.push_back(pt(i));
            k.lines.push_back(pt(i + 1));
          }
          out.push_back(std::move(k));
          continue;
        }
      }
    }

    // Strip outline: split at corners; short straight pieces are strip ends.
    if (L < 8)
      continue;
    auto dir = [&](std::size_t i) { return glm::normalize(pt(i + 1) - pt(i)); };
    std::vector<std::size_t> corners;
    for (std::size_t i = 0; i < L; ++i)
      if (glm::dot(dir(i + L - 1), dir(i)) < 0.6f)
        corners.push_back(i);
    if (corners.size() < 4)
      continue;
    auto length = [&](std::size_t a, std::size_t b) {
      float len = 0;
      for (std::size_t i = a; i != b; i = (i + 1) % L)
        len += glm::length(pt(i + 1) - pt(i));
      return len;
    };
    float longest = 0;
    for (std::size_t k = 0; k < corners.size(); ++k)
      longest = std::max(longest, length(corners[k], corners[(k + 1) % corners.size()]));
    for (std::size_t k = 0; k < corners.size(); ++k)
    {
      const std::size_t a = corners[k], b = corners[(k + 1) % corners.size()];
      const glm::vec3 A = pt(a), B = pt(b);
      const float chord = glm::length(B - A), len = length(a, b);
      if (chord < 4 || len > chord * 1.02f || len > 0.5f * longest)
        continue;
      // Outward: perpendicular to the end, away from the adjacent triangle.
      const auto& opp = opposite(loop[a], loop[(a + 1) % L]);
      if (opp.empty())
        continue;
      const glm::vec3 d = glm::normalize(B - A);
      glm::vec3 w = P[opp.front()] - A;
      w -= d * glm::dot(w, d);
      if (glm::length(w) < 1e-4f)
        continue;
      ends.push_back({A, B, -glm::normalize(w)});
    }
  }

  // Strip ends in one plane (the lanes/borders of one road end) form one connector.
  std::vector<char> used(ends.size(), 0);
  for (std::size_t i = 0; i < ends.size(); ++i)
  {
    if (used[i])
      continue;
    used[i] = 1;
    std::vector<std::size_t> group{i};
    const glm::vec3 mi = (ends[i].a + ends[i].b) * 0.5f;
    for (std::size_t j = i + 1; j < ends.size(); ++j)
    {
      const glm::vec3 mj = (ends[j].a + ends[j].b) * 0.5f;
      if (!used[j] && glm::dot(ends[i].normal, ends[j].normal) > 0.95f &&
          std::fabs(glm::dot(mj - mi, ends[i].normal)) < 2 && glm::distance(mi, mj) < 300)
      {
        used[j] = 1;
        group.push_back(j);
      }
    }
    Connector k;
    glm::vec3 n(0), across(0);
    std::vector<glm::vec3> pts;
    for (std::size_t g : group)
    {
      n += ends[g].normal;
      glm::vec3 d = ends[g].b - ends[g].a;
      across += glm::dot(d, across) < 0 ? -d : d;
      pts.push_back(ends[g].a);
      pts.push_back(ends[g].b);
      k.lines.push_back(ends[g].a);
      k.lines.push_back(ends[g].b);
    }
    k.normal = glm::normalize(n);
    across -= k.normal * glm::dot(across, k.normal);
    if (glm::length(across) < 1e-6f)
      continue;
    k.across = glm::normalize(across);
    float lo = 1e30f, hi = -1e30f;
    glm::vec3 c(0);
    for (const auto& p : pts)
      c += p;
    c /= float(pts.size());
    for (const auto& p : pts)
    {
      lo = std::min(lo, glm::dot(p - c, k.across));
      hi = std::max(hi, glm::dot(p - c, k.across));
    }
    k.width = hi - lo;
    k.centre = c + k.across * ((lo + hi) * 0.5f);
    if (k.width >= kMinWidth)
      out.push_back(std::move(k));
  }
  return out;
}

Connector TransformConnector(const Connector& c, const glm::mat4& m)
{
  Connector r;
  r.centre = glm::vec3(m * glm::vec4(c.centre, 1));
  // Normals: inverse transpose (handles non-uniform scale).
  const glm::mat3 nm = glm::transpose(glm::inverse(glm::mat3(m)));
  r.normal = glm::normalize(nm * c.normal);
  const glm::vec3 a = glm::mat3(m) * c.across;
  r.width = c.width * glm::length(a);
  r.across = glm::normalize(a - r.normal * glm::dot(a, r.normal));
  r.lines.reserve(c.lines.size());
  for (const auto& p : c.lines)
    r.lines.push_back(glm::vec3(m * glm::vec4(p, 1)));
  return r;
}

bool ConnectorsFit(const Connector& a, const Connector& b, float max_facing_dot, float range)
{
  const float ratio = a.width / std::max(1e-3f, b.width);
  return ratio > 0.8f && ratio < 1.25f && glm::dot(a.normal, b.normal) <= max_facing_dot &&
         glm::distance(a.centre, b.centre) <= range;
}

glm::mat4 ConnectTransform(const Connector& from, const Connector& to)
{
  auto frame = [](const glm::vec3& n, const glm::vec3& a) { return glm::mat3(n, a, glm::cross(n, a)); };
  const glm::mat3 src = frame(from.normal, from.across);
  glm::mat3 best(1);
  float best_trace = -1e30f;
  for (const float sign : {1.0f, -1.0f})
  {
    const glm::mat3 r = frame(-to.normal, to.across * sign) * glm::transpose(src);
    const float trace = r[0][0] + r[1][1] + r[2][2];
    if (trace > best_trace)
      best_trace = trace, best = r;
  }
  glm::mat4 R(best);
  return glm::translate(glm::mat4(1), to.centre) * R * glm::translate(glm::mat4(1), -from.centre);
}
}  // namespace fzgx
