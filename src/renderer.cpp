#include "renderer.hpp"

#include <epoxy/gl.h>
#include <glm/gtc/type_ptr.hpp>

#include <cstdio>

namespace fzgx
{
namespace
{
struct Vertex
{
  float p[3];
  float n[3];
  std::uint8_t c[4];
  float uv[2];
};

const char* kMeshVs = R"(#version 330 core
layout(location=0) in vec3 aPos;
layout(location=1) in vec3 aNrm;
layout(location=2) in vec4 aCol;
layout(location=3) in vec2 aUV;
uniform mat4 uVP; uniform mat4 uModel;
out vec3 vNrm; out vec4 vCol; out vec3 vWorld; out vec2 vUV;
void main(){
  vec4 w = uModel * vec4(aPos,1.0);
  vWorld = w.xyz;
  vNrm = mat3(uModel) * aNrm;
  vCol = aCol;
  vUV = aUV;
  gl_Position = uVP * w;
})";

const char* kMeshFs = R"(#version 330 core
in vec3 vNrm; in vec4 vCol; in vec3 vWorld; in vec2 vUV;
uniform vec4 uTint; uniform vec4 uHighlight; uniform vec3 uEye;
uniform int uUseTex; uniform sampler2D uTex;
out vec4 frag;
void main(){
  vec4 tex = vec4(1.0);
  if (uUseTex == 1) {
    tex = texture(uTex, vUV);
    if (tex.a < 0.35) discard;            // cut-out textures (fences, signs, foliage)
  }
  vec3 n = vNrm;
  if (dot(n,n) < 1e-8) n = normalize(cross(dFdx(vWorld), dFdy(vWorld)));
  n = normalize(n);
  vec3 toEye = normalize(uEye - vWorld);
  if (dot(n, toEye) < 0.0) n = -n;          // two-sided lighting
  vec3 l = normalize(vec3(0.4, 0.9, 0.3));
  float diff = max(dot(n,l),0.0);
  float hemi = 0.5 + 0.5*n.y;
  vec3 base = vCol.rgb * uTint.rgb * tex.rgb;
  vec3 c = uUseTex == 1 ? base*(0.55 + 0.2*hemi + 0.4*diff) : base*(0.35 + 0.25*hemi + 0.55*diff);
  float dist = length(uEye - vWorld);
  c = mix(c, vec3(0.08,0.09,0.12), clamp((dist-4000.0)/12000.0, 0.0, 0.6));
  frag = vec4(c + uHighlight.rgb*uHighlight.a, vCol.a*uTint.a);
})";

const char* kLineVs = R"(#version 330 core
layout(location=0) in vec3 aPos;
layout(location=1) in vec4 aCol;
uniform mat4 uVP; out vec4 vCol;
void main(){ vCol = aCol; gl_Position = uVP*vec4(aPos,1.0); })";

const char* kLineFs = R"(#version 330 core
in vec4 vCol; out vec4 frag; void main(){ frag = vCol; })";

unsigned Compile(const char* vs, const char* fs)
{
  auto stage = [](GLenum type, const char* src) {
    const GLuint s = glCreateShader(type);
    glShaderSource(s, 1, &src, nullptr);
    glCompileShader(s);
    GLint ok = 0;
    glGetShaderiv(s, GL_COMPILE_STATUS, &ok);
    if (!ok)
    {
      char log[2048];
      glGetShaderInfoLog(s, sizeof log, nullptr, log);
      std::fprintf(stderr, "shader error: %s\n", log);
    }
    return s;
  };
  const GLuint p = glCreateProgram();
  const GLuint a = stage(GL_VERTEX_SHADER, vs), b = stage(GL_FRAGMENT_SHADER, fs);
  glAttachShader(p, a);
  glAttachShader(p, b);
  glLinkProgram(p);
  glDeleteShader(a);
  glDeleteShader(b);
  GLint ok = 0;
  glGetProgramiv(p, GL_LINK_STATUS, &ok);
  return ok ? p : 0;
}

GpuMesh UploadVertices(const std::vector<Vertex>& v)
{
  GpuMesh m;
  if (v.empty())
    return m;
  glGenVertexArrays(1, &m.vao);
  glGenBuffers(1, &m.vbo);
  glBindVertexArray(m.vao);
  glBindBuffer(GL_ARRAY_BUFFER, m.vbo);
  glBufferData(GL_ARRAY_BUFFER, GLsizeiptr(v.size() * sizeof(Vertex)), v.data(), GL_STATIC_DRAW);
  glEnableVertexAttribArray(0);
  glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, sizeof(Vertex), (void*)offsetof(Vertex, p));
  glEnableVertexAttribArray(1);
  glVertexAttribPointer(1, 3, GL_FLOAT, GL_FALSE, sizeof(Vertex), (void*)offsetof(Vertex, n));
  glEnableVertexAttribArray(2);
  glVertexAttribPointer(2, 4, GL_UNSIGNED_BYTE, GL_TRUE, sizeof(Vertex), (void*)offsetof(Vertex, c));
  glEnableVertexAttribArray(3);
  glVertexAttribPointer(3, 2, GL_FLOAT, GL_FALSE, sizeof(Vertex), (void*)offsetof(Vertex, uv));
  glBindVertexArray(0);
  m.count = int(v.size());
  return m;
}
}  // namespace

bool Renderer::Init()
{
  program_ = Compile(kMeshVs, kMeshFs);
  line_program_ = Compile(kLineVs, kLineFs);
  if (!program_ || !line_program_)
    return false;
  u_vp_ = glGetUniformLocation(program_, "uVP");
  u_model_ = glGetUniformLocation(program_, "uModel");
  u_tint_ = glGetUniformLocation(program_, "uTint");
  u_highlight_ = glGetUniformLocation(program_, "uHighlight");
  u_eye_ = glGetUniformLocation(program_, "uEye");
  u_use_tex_ = glGetUniformLocation(program_, "uUseTex");
  u_tex_ = glGetUniformLocation(program_, "uTex");
  ul_vp_ = glGetUniformLocation(line_program_, "uVP");
  glGenVertexArrays(1, &line_vao_);
  glGenBuffers(1, &line_vbo_);
  glBindVertexArray(line_vao_);
  glBindBuffer(GL_ARRAY_BUFFER, line_vbo_);
  glEnableVertexAttribArray(0);
  glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, sizeof(LineVertex), (void*)offsetof(LineVertex, p));
  glEnableVertexAttribArray(1);
  glVertexAttribPointer(1, 4, GL_UNSIGNED_BYTE, GL_TRUE, sizeof(LineVertex), (void*)offsetof(LineVertex, rgba));
  glBindVertexArray(0);
  return true;
}

void Renderer::Shutdown()
{
  glDeleteProgram(program_);
  glDeleteProgram(line_program_);
  glDeleteBuffers(1, &line_vbo_);
  glDeleteVertexArrays(1, &line_vao_);
}

GpuMesh Renderer::Upload(const ModelMesh& mesh, bool textured)
{
  std::vector<Vertex> v(mesh.positions.size());
  for (std::size_t i = 0; i < v.size(); ++i)
  {
    for (int k = 0; k < 3; ++k)
    {
      v[i].p[k] = mesh.positions[i][k];
      v[i].n[k] = mesh.normals[i][k];
    }
    for (int k = 0; k < 4; ++k)
      v[i].c[k] = textured ? mesh.vcolors[i][k] : mesh.colors[i][k];
    v[i].uv[0] = mesh.uvs[i][0];
    v[i].uv[1] = mesh.uvs[i][1];
  }
  return UploadVertices(v);
}

unsigned Renderer::CreateTexture(const Texture& t, std::uint8_t wrap)
{
  GLuint id = 0;
  glGenTextures(1, &id);
  glBindTexture(GL_TEXTURE_2D, id);
  glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
  glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, t.width, t.height, 0, GL_RGBA, GL_UNSIGNED_BYTE, t.rgba.data());
  glGenerateMipmap(GL_TEXTURE_2D);
  auto mode = [](int bits) {
    return bits == 1 ? GL_REPEAT : bits == 2 ? GL_MIRRORED_REPEAT : GL_CLAMP_TO_EDGE;
  };
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, mode(wrap >> 2 & 3));
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, mode(wrap >> 4 & 3));
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR_MIPMAP_LINEAR);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
  if (epoxy_has_gl_extension("GL_EXT_texture_filter_anisotropic"))
    glTexParameterf(GL_TEXTURE_2D, GL_TEXTURE_MAX_ANISOTROPY_EXT, 8.0f);
  glBindTexture(GL_TEXTURE_2D, 0);
  return id;
}

void Renderer::FreeTexture(unsigned& tex)
{
  if (tex)
    glDeleteTextures(1, &tex);
  tex = 0;
}

GpuMesh Renderer::UploadColored(const std::vector<std::array<float, 3>>& tris,
                                const std::vector<std::uint32_t>& colors)
{
  std::vector<Vertex> v(tris.size());
  for (std::size_t i = 0; i < v.size(); ++i)
  {
    const std::uint32_t c = colors[i / 3];
    for (int k = 0; k < 3; ++k)
    {
      v[i].p[k] = tris[i][k];
      v[i].n[k] = 0;  // flat shading from screen-space derivatives
    }
    for (int k = 0; k < 4; ++k)
      v[i].c[k] = std::uint8_t(c >> (8 * k));
  }
  return UploadVertices(v);
}

void Renderer::Free(GpuMesh& m)
{
  if (m.vbo)
    glDeleteBuffers(1, &m.vbo);
  if (m.vao)
    glDeleteVertexArrays(1, &m.vao);
  m = {};
}

void Renderer::BeginFrame(int x, int y, int w, int h, const glm::mat4& vp, const glm::vec3& eye)
{
  view_proj_ = vp;
  glViewport(x, y, w, h);
  glEnable(GL_SCISSOR_TEST);
  glScissor(x, y, w, h);
  glClearColor(0.08f, 0.09f, 0.12f, 1);
  glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
  glDisable(GL_SCISSOR_TEST);
  glEnable(GL_DEPTH_TEST);
  glDisable(GL_CULL_FACE);
  glEnable(GL_BLEND);
  glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
  glUseProgram(program_);
  glUniformMatrix4fv(u_vp_, 1, GL_FALSE, glm::value_ptr(vp));
  glUniform3fv(u_eye_, 1, glm::value_ptr(eye));
}

void Renderer::Draw(const GpuMesh& m, const glm::mat4& model, const glm::vec4& tint,
                    const glm::vec4& highlight, bool wire)
{
  if (!m.count)
    return;
  glUseProgram(program_);
  glUniformMatrix4fv(u_model_, 1, GL_FALSE, glm::value_ptr(model));
  glUniform4fv(u_tint_, 1, glm::value_ptr(tint));
  glUniform4fv(u_highlight_, 1, glm::value_ptr(highlight));
  if (wire)
    glPolygonMode(GL_FRONT_AND_BACK, GL_LINE);
  glBindVertexArray(m.vao);
  if (m.parts.empty())
  {
    glUniform1i(u_use_tex_, 0);
    glDrawArrays(GL_TRIANGLES, 0, m.count);
  }
  else
  {
    glActiveTexture(GL_TEXTURE0);
    glUniform1i(u_tex_, 0);
    for (const auto& p : m.parts)
    {
      glUniform1i(u_use_tex_, p.texture ? 1 : 0);
      if (p.texture)
        glBindTexture(GL_TEXTURE_2D, p.texture);
      glDrawArrays(GL_TRIANGLES, p.first, p.count);
    }
  }
  if (wire)
    glPolygonMode(GL_FRONT_AND_BACK, GL_FILL);
}

void Renderer::DrawLines(const std::vector<LineVertex>& lines, bool depth_test)
{
  if (lines.empty())
    return;
  if (!depth_test)
    glDisable(GL_DEPTH_TEST);
  glUseProgram(line_program_);
  glUniformMatrix4fv(ul_vp_, 1, GL_FALSE, glm::value_ptr(view_proj_));
  glBindVertexArray(line_vao_);
  glBindBuffer(GL_ARRAY_BUFFER, line_vbo_);
  glBufferData(GL_ARRAY_BUFFER, GLsizeiptr(lines.size() * sizeof(LineVertex)), lines.data(), GL_STREAM_DRAW);
  glDrawArrays(GL_LINES, 0, GLsizei(lines.size()));
  glEnable(GL_DEPTH_TEST);
}

void AddBox(std::vector<LineVertex>& out, const glm::mat4& m, const glm::vec3& mn, const glm::vec3& mx,
            std::uint32_t c)
{
  glm::vec3 p[8];
  for (int i = 0; i < 8; ++i)
    p[i] = glm::vec3(m * glm::vec4(i & 1 ? mx.x : mn.x, i & 2 ? mx.y : mn.y, i & 4 ? mx.z : mn.z, 1));
  const int e[12][2] = {{0, 1}, {2, 3}, {4, 5}, {6, 7}, {0, 2}, {1, 3}, {4, 6}, {5, 7}, {0, 4}, {1, 5}, {2, 6}, {3, 7}};
  for (auto& [a, b] : e)
  {
    out.push_back({p[a], c});
    out.push_back({p[b], c});
  }
}
}  // namespace fzgx
