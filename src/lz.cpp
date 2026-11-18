#include "lz.hpp"

#include <array>
#include <fstream>

namespace fzgx
{
namespace
{
constexpr int kRing = 4096;
constexpr int kMaxLen = 18;
constexpr int kMinLen = 3;
constexpr int kRingStart = kRing - kMaxLen;  // 0xFEE
}  // namespace

std::optional<Bytes> ReadFile(const std::filesystem::path& path)
{
  std::ifstream in(path, std::ios::binary);
  if (!in)
    return std::nullopt;
  return Bytes(std::istreambuf_iterator<char>(in), {});
}

bool WriteFileAtomic(const std::filesystem::path& path, const Bytes& data)
{
  std::error_code ec;
  std::filesystem::create_directories(path.parent_path(), ec);
  const auto tmp = path.string() + ".tmp";
  {
    std::ofstream out(tmp, std::ios::binary | std::ios::trunc);
    if (!out)
      return false;
    out.write(reinterpret_cast<const char*>(data.data()), std::streamsize(data.size()));
    if (!out)
      return false;
  }
  // Replacing (not overwriting in place) also breaks btrfs reflinks cleanly.
  std::filesystem::rename(tmp, path, ec);
  return !ec;
}

Bytes LzDecompress(const Bytes& file)
{
  Check(file.size() >= 8, "lz: file too small");
  const std::uint32_t packed = U32LE(file, 0);
  const std::uint32_t size = U32LE(file, 4);
  Check(packed + 8 <= file.size() + 8 && size < (256u << 20), "lz: bad header");

  Bytes out;
  out.reserve(size);
  std::array<std::uint8_t, kRing> ring{};
  int r = kRingStart;
  std::size_t i = 8;
  unsigned flags = 0;
  while (out.size() < size)
  {
    flags >>= 1;
    if (!(flags & 0x100))
    {
      Check(i < file.size(), "lz: truncated");
      flags = file[i++] | 0xFF00u;
    }
    if (flags & 1)
    {
      Check(i < file.size(), "lz: truncated");
      const std::uint8_t c = file[i++];
      out.push_back(c);
      ring[r] = c;
      r = (r + 1) & (kRing - 1);
    }
    else
    {
      Check(i + 1 < file.size(), "lz: truncated");
      const int a = file[i], b = file[i + 1];
      i += 2;
      const int pos = a | (b & 0xF0) << 4;
      const int len = (b & 0x0F) + kMinLen;
      for (int k = 0; k < len && out.size() < size; ++k)
      {
        const std::uint8_t c = ring[(pos + k) & (kRing - 1)];
        out.push_back(c);
        ring[r] = c;
        r = (r + 1) & (kRing - 1);
      }
    }
  }
  return out;
}

Bytes LzCompress(const Bytes& data)
{
  // Hash-chain matcher over the last 4095 bytes. Only matches inside the real
  // data are used (never the zero-filled ring prefix), so the decoder's ring
  // always holds exactly the bytes we reference.
  constexpr int kHashSize = 1 << 14;
  constexpr int kWindow = kRing - 1;
  constexpr int kMaxChain = 256;
  const int n = int(data.size());
  std::vector<int> head(kHashSize, -1), prev(std::size_t(n), -1);
  auto hash = [&](int p) {
    return ((data[p] << 6) ^ (data[p + 1] << 3) ^ data[p + 2]) & (kHashSize - 1);
  };
  auto insert = [&](int p) {
    if (p + 2 >= n)
      return;
    const int h = hash(p);
    prev[p] = head[h];
    head[h] = p;
  };
  auto find = [&](int p, int& best_pos) {
    int best = 0;
    if (p + kMinLen > n)
      return 0;
    const int limit = std::min(kMaxLen, n - p);
    int chain = 0;
    for (int c = head[hash(p)]; c >= 0 && p - c <= kWindow && chain < kMaxChain; c = prev[c], ++chain)
    {
      int l = 0;
      while (l < limit && data[c + l] == data[p + l])
        ++l;
      if (l > best)
      {
        best = l;
        best_pos = c;
        if (l == limit)
          break;
      }
    }
    return best >= kMinLen ? best : 0;
  };

  Bytes out(8, 0);
  int p = 0;
  while (p < n)
  {
    const std::size_t flag_at = out.size();
    out.push_back(0);
    std::uint8_t flags = 0;
    for (int bit = 0; bit < 8 && p < n; ++bit)
    {
      int mpos = 0;
      int len = find(p, mpos);
      if (len)
      {
        // One step of lazy matching: prefer a literal if the next byte starts a longer match.
        int npos = 0;
        insert(p);
        const int next = (p + 1 < n) ? find(p + 1, npos) : 0;
        if (next > len)
        {
          flags |= std::uint8_t(1 << bit);
          out.push_back(data[p]);
          ++p;
          continue;
        }
        const int ring_pos = (mpos + kRingStart) & (kRing - 1);
        out.push_back(std::uint8_t(ring_pos & 0xFF));
        out.push_back(std::uint8_t((ring_pos >> 4 & 0xF0) | (len - kMinLen)));
        for (int k = 1; k < len; ++k)
          insert(p + k);
        p += len;
      }
      else
      {
        flags |= std::uint8_t(1 << bit);
        out.push_back(data[p]);
        insert(p);
        ++p;
      }
    }
    out[flag_at] = flags;
  }
  const std::uint32_t packed = std::uint32_t(out.size() - 8);
  const std::uint32_t size = std::uint32_t(n);
  for (int k = 0; k < 4; ++k)
  {
    out[k] = std::uint8_t(packed >> (8 * k));
    out[4 + k] = std::uint8_t(size >> (8 * k));
  }
  return out;
}
}  // namespace fzgx
