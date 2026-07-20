#include "mxf/metadata_sets.h"

namespace mxfsrc {

namespace {
constexpr uint16_t kInstanceUidTag = 0x3C0A;
}

bool ReadLocalSet(MxfReader& reader, uint64_t length, LocalTagMap& out) {
  const uint64_t end = reader.Tell() + length;
  while (reader.Tell() + 4 <= end) {
    uint16_t tag = 0, itemLen = 0;
    if (!reader.ReadU16BE(tag)) return false;
    if (!reader.ReadU16BE(itemLen)) return false;
    if (reader.Tell() + itemLen > end) return false;
    std::vector<uint8_t> value(itemLen);
    if (itemLen > 0 && !reader.ReadBytes(value.data(), itemLen)) return false;
    out[tag] = std::move(value);
  }
  // KAG-aligned sets may have trailing pad bytes; land exactly on the
  // caller's declared end regardless of how the loop above terminated.
  return reader.Seek(end);
}

bool GetInstanceUid(const LocalTagMap& set, Ul& out) { return GetUl(set, kInstanceUidTag, out); }

bool GetUl(const LocalTagMap& set, uint16_t tag, Ul& out) {
  auto it = set.find(tag);
  if (it == set.end() || it->second.size() != 16) return false;
  std::copy(it->second.begin(), it->second.end(), out.begin());
  return true;
}

bool GetU32(const LocalTagMap& set, uint16_t tag, uint32_t& out) {
  auto it = set.find(tag);
  if (it == set.end() || it->second.size() != 4) return false;
  const auto& b = it->second;
  out = (static_cast<uint32_t>(b[0]) << 24) | (static_cast<uint32_t>(b[1]) << 16) |
        (static_cast<uint32_t>(b[2]) << 8) | static_cast<uint32_t>(b[3]);
  return true;
}

bool GetI64(const LocalTagMap& set, uint16_t tag, int64_t& out) {
  auto it = set.find(tag);
  if (it == set.end() || it->second.size() != 8) return false;
  const auto& b = it->second;
  uint64_t v = 0;
  for (int i = 0; i < 8; ++i) v = (v << 8) | b[i];
  out = static_cast<int64_t>(v);
  return true;
}

bool GetRational32(const LocalTagMap& set, uint16_t tag, Rational& out) {
  auto it = set.find(tag);
  if (it == set.end() || it->second.size() != 8) return false;
  const auto& b = it->second;
  uint32_t num = (static_cast<uint32_t>(b[0]) << 24) | (static_cast<uint32_t>(b[1]) << 16) |
                 (static_cast<uint32_t>(b[2]) << 8) | static_cast<uint32_t>(b[3]);
  uint32_t den = (static_cast<uint32_t>(b[4]) << 24) | (static_cast<uint32_t>(b[5]) << 16) |
                 (static_cast<uint32_t>(b[6]) << 8) | static_cast<uint32_t>(b[7]);
  out.num = static_cast<int32_t>(num);
  out.den = static_cast<int32_t>(den);
  return true;
}

std::vector<Ul> GetUlArray(const LocalTagMap& set, uint16_t tag) {
  std::vector<Ul> result;
  auto it = set.find(tag);
  if (it == set.end() || it->second.size() < 8) return result;
  const auto& b = it->second;
  uint32_t count = (static_cast<uint32_t>(b[0]) << 24) | (static_cast<uint32_t>(b[1]) << 16) |
                   (static_cast<uint32_t>(b[2]) << 8) | static_cast<uint32_t>(b[3]);
  uint32_t itemSize = (static_cast<uint32_t>(b[4]) << 24) | (static_cast<uint32_t>(b[5]) << 16) |
                      (static_cast<uint32_t>(b[6]) << 8) | static_cast<uint32_t>(b[7]);
  if (itemSize != 16) return result;
  if (b.size() < 8 + static_cast<size_t>(count) * 16) return result;
  result.reserve(count);
  for (uint32_t i = 0; i < count; ++i) {
    Ul ul{};
    std::copy(b.begin() + 8 + i * 16, b.begin() + 8 + (i + 1) * 16, ul.begin());
    result.push_back(ul);
  }
  return result;
}

}  // namespace mxfsrc
