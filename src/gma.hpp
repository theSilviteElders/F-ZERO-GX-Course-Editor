// GMA model archives (GCMF models with GX display lists): geometry, first UV
// set, material colour and the first texture layer of every mesh.
#pragma once
#include "binary.hpp"
#include "tpl.hpp"

#include <array>
#include <filesystem>
#include <memory>
#include <string>
#include <unordered_map>

namespace fzgx
{
struct ModelMesh
{
  // Non-indexed triangle list.
  std::vector<std::array<float, 3>> positions;
  std::vector<std::array<float, 3>> normals;
  std::vector<std::array<std::uint8_t, 4>> colors;   // material tint (untextured view)
  std::vector<std::array<std::uint8_t, 4>> vcolors;  // vertex colour or white (textured view)
  std::vector<std::array<float, 2>> uvs;             // first texture coordinate set
  // One part per GCMF mesh: a vertex range and its first texture layer.
  struct Part
  {
    int first = 0, count = 0;
    int tpl = -1;            // texture index in the paired .tpl, -1 = none
    std::uint8_t wrap = 0;   // GCMF wrap flags: bits 2-3 S, bits 4-5 T (0 clamp, 1 repeat, 2 mirror)
  };
  std::vector<Part> parts;
  int archive = -1;          // ModelLibrary archive the model came from
  std::array<float, 3> bounds_min{}, bounds_max{};
  bool skinned = false;  // skinned/effective models are not decoded
};

// Decodes a single GCMF model starting at `offset`.
ModelMesh DecodeGcmf(const Bytes& gma, std::size_t offset);

// Name -> decoded model, loaded lazily from a set of GMA files.
class ModelLibrary
{
public:
  // Files added first win when names collide.
  void AddArchive(const std::filesystem::path& gma_lz);
  const ModelMesh* Find(const std::string& name);
  bool Contains(const std::string& name) const { return index_.count(name) != 0; }
  std::vector<std::string> archives() const;
  // Textures of an archive (the .tpl next to the .gma), loaded on first use.
  const TplFile* Textures(int archive);

private:
  struct Archive
  {
    std::filesystem::path path;
    Bytes data;
    std::unique_ptr<TplFile> tpl;
    bool tpl_loaded = false;
  };
  struct Entry
  {
    int archive;
    std::size_t offset;
  };
  std::vector<Archive> archives_;
  std::unordered_map<std::string, Entry> index_;
  std::unordered_map<std::string, std::unique_ptr<ModelMesh>> cache_;
};

// Lists model names in a decompressed GMA archive.
std::vector<std::pair<std::string, std::size_t>> ListGma(const Bytes& gma);
}  // namespace fzgx
