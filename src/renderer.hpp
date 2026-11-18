#pragma once
#include "gma.hpp"

#include <glm/glm.hpp>

#include <cstdint>
#include <vector>

namespace fzgx
{
struct GpuMesh
{
  unsigned vao = 0, vbo = 0;
  int count = 0;
};

struct LineVertex
{
  glm::vec3 p;
  std::uint32_t rgba;  // 0xAABBGGRR
};

class Renderer
{
public:
  bool Init();
  void Shutdown();

  GpuMesh Upload(const ModelMesh& mesh);
  // Triangle soup with one colour per triangle (collision view).
  GpuMesh UploadColored(const std::vector<std::array<float, 3>>& tris,
                        const std::vector<std::uint32_t>& tri_colors);
  void Free(GpuMesh& mesh);

  void BeginFrame(int x, int y, int w, int h, const glm::mat4& view_proj, const glm::vec3& eye);
  // tint multiplies the vertex colour, highlight is added on top.
  void Draw(const GpuMesh& mesh, const glm::mat4& model, const glm::vec4& tint,
            const glm::vec4& highlight = {0, 0, 0, 0}, bool wire = false);
  void DrawLines(const std::vector<LineVertex>& lines, bool depth_test = true);

private:
  unsigned program_ = 0, line_program_ = 0;
  int u_vp_ = -1, u_model_ = -1, u_tint_ = -1, u_highlight_ = -1, u_eye_ = -1;
  int ul_vp_ = -1;
  unsigned line_vao_ = 0, line_vbo_ = 0;
  glm::mat4 view_proj_{1};
};

void AddBox(std::vector<LineVertex>& out, const glm::mat4& m, const glm::vec3& mn, const glm::vec3& mx,
            std::uint32_t rgba);
}  // namespace fzgx
