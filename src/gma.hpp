// GMA model archives (GCMF models with GX display lists). Geometry only: the
// editor draws shaded meshes tinted with the material colour, no textures yet.
#pragma once
#include "binary.hpp"

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
  std::vector<std::array<std::uint8_t, 4>> colors;
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

private:
  struct Archive
  {
    std::filesystem::path path;
    Bytes data;
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
