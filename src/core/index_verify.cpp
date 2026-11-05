#include "core/index_verify.h"

#include <algorithm>
#include <atomic>
#include <cstdio>
#include <limits>
#include <mutex>
#include <thread>

#include "mxf/essence_walker.h"
#include "mxf/klv.h"

namespace mxfsrc {

namespace {

constexpr uint64_t kNoBound = std::numeric_limits<uint64_t>::max();
// Entries per work item: contiguous runs keep each worker's reads moving
// forward through the file.
constexpr size_t kEntriesPerChunk = 256;
constexpr size_t kLocalWorkers = 4;
constexpr size_t kHttpWorkers = 16;

std::string HexBytes(const uint8_t* p, size_t n) {
  static const char kHex[] = "0123456789abcdef";
  std::string s;
  for (size_t i = 0; i < n; ++i) {
    s += kHex[p[i] >> 4];
    s += kHex[p[i] & 0x0f];
  }
  return s;
}

// Checks one entry offset. On success returns nullopt and sets
// `videoElementOffset`; otherwise returns what is wrong.
std::optional<std::string> CheckEntryOffset(MxfReader& reader, uint64_t offset, uint64_t nextEntryOffset,
                                            const MxfStreamInfo& info, uint64_t& videoElementOffset,
                                            uint64_t* videoElementEnd = nullptr) {
  const uint64_t size = reader.Size();
  if (offset + 17 > size) {
    return "offset is beyond the end of the file (" + std::to_string(size) + " bytes)";
  }
  if (!reader.Seek(offset)) return "cannot read there" + std::string(reader.LastError().empty() ? "" : " (" + reader.LastError() + ")");
  KlvHeader klv{};
  if (!ReadKlvHeader(reader, klv)) return "no readable KLV header there (invalid length field)";
  if (!LooksLikeKlvKey(klv.key)) return "no KLV key there (found " + HexBytes(klv.key.data(), 8) + "...)";

  std::string damage;
  EssenceWalkerOptions options;
  options.maxResyncBytes = 0;
  options.onDamage = [&damage](const WalkDamage& d) {
    damage = d.badLength ? "invalid KLV length field at byte " + std::to_string(d.offset)
                         : "no KLV key at byte " + std::to_string(d.offset);
  };
  EssenceWalker walker(reader, offset, info.videoTrackNumber, /*hasVideo=*/true, {}, std::move(options));
  EssenceElement element;
  if (!walker.Next(element, /*wantData=*/false)) {
    return damage.empty() ? "no picture element of the video track follows (end of file)"
                          : "no picture element of the video track follows (" + damage + ")";
  }
  if (element.filePosition >= nextEntryOffset) {
    return "the next picture element (byte " + std::to_string(element.filePosition) +
           ") lies beyond the next entry's offset " + std::to_string(nextEntryOffset);
  }
  videoElementOffset = element.filePosition;
  if (videoElementEnd) *videoElementEnd = walker.Position();
  return std::nullopt;
}

// Runs `check(reader, i)` for i in [0, n) on several threads, each with its
// own reader, in contiguous chunks. Returns an error if a reader can't open.
template <typename Check>
std::optional<std::string> ParallelForEntries(const std::string& path, const std::string& httpHeaders, size_t n,
                                              Check check) {
  if (n == 0) return std::nullopt;
  const size_t chunks = (n + kEntriesPerChunk - 1) / kEntriesPerChunk;
  const size_t workerCount = std::min(IsHttpUrl(path) ? kHttpWorkers : kLocalWorkers, chunks);
  std::atomic<size_t> nextChunk{0};
  std::mutex errorMutex;
  std::optional<std::string> error;
  auto work = [&] {
    MxfReader reader(path, httpHeaders);
    if (!reader.IsOpen()) {
      std::lock_guard<std::mutex> lock(errorMutex);
      if (!error) error = "verify: failed to open file: " + path;
      return;
    }
    for (;;) {
      const size_t chunk = nextChunk.fetch_add(1);
      if (chunk >= chunks) break;
      const size_t end = std::min(n, (chunk + 1) * kEntriesPerChunk);
      for (size_t i = chunk * kEntriesPerChunk; i < end; ++i) check(reader, i);
    }
  };
  std::vector<std::thread> workers;
  workers.reserve(workerCount);
  for (size_t w = 0; w < workerCount; ++w) workers.emplace_back(work);
  for (auto& t : workers) t.join();
  return error;
}

// From the end of the picture element whose Value ends at `from`, the KLV
// headers must chain to exactly `nextEntryOffset`. Returns what is wrong.
std::optional<std::string> CheckChainTo(MxfReader& reader, uint64_t from, uint64_t nextEntryOffset) {
  uint64_t pos = from;
  while (pos < nextEntryOffset) {
    if (!reader.Seek(pos)) return "cannot read at byte " + std::to_string(pos);
    KlvHeader klv{};
    if (!ReadKlvHeader(reader, klv)) return "invalid KLV length field at byte " + std::to_string(pos);
    if (!LooksLikeKlvKey(klv.key)) {
      return "no KLV key at byte " + std::to_string(pos) + " (found " + HexBytes(klv.key.data(), 8) + "...)";
    }
    pos = klv.valueOffset + klv.length;
  }
  if (pos != nextEntryOffset) {
    return "a KLV ends at byte " + std::to_string(pos) + ", past the next entry's offset " +
           std::to_string(nextEntryOffset);
  }
  return std::nullopt;
}

}  // namespace

IndexVerifyResult VerifyEssenceIndex(const std::string& path, const std::string& httpHeaders,
                                     const EssenceIndex& index, const MxfStreamInfo& info) {
  IndexVerifyResult result;
  const std::vector<FlatIndexEntry>& entries = index.GetEntries();
  const size_t n = entries.size();
  result.checked = static_cast<int64_t>(n);
  result.videoElementOffsets.assign(n, 0);
  if (n == 0) return result;

  // Pass 1: each entry leads to its own picture element.
  std::vector<std::optional<std::string>> problems(n);
  std::vector<uint64_t> videoEnds(n, 0);
  result.error = ParallelForEntries(path, httpHeaders, n, [&](MxfReader& reader, size_t i) {
    if (i > 0 && entries[i].byteOffset <= entries[i - 1].byteOffset) {
      problems[i] =
          "offset does not increase (previous entry is at byte " + std::to_string(entries[i - 1].byteOffset) + ")";
      return;
    }
    const uint64_t next = i + 1 < n ? entries[i + 1].byteOffset : kNoBound;
    problems[i] =
        CheckEntryOffset(reader, entries[i].byteOffset, next, info, result.videoElementOffsets[i], &videoEnds[i]);
  });
  if (result.error) return result;

  // Pass 2: between two good entries the essence chains without damage.
  std::vector<std::optional<std::string>> damage(n);
  result.error = ParallelForEntries(path, httpHeaders, n - 1, [&](MxfReader& reader, size_t i) {
    if (!problems[i] && !problems[i + 1]) damage[i] = CheckChainTo(reader, videoEnds[i], entries[i + 1].byteOffset);
  });
  if (result.error) return result;

  for (size_t i = 0; i < n; ++i) {
    if (problems[i]) {
      result.videoElementOffsets[i] = 0;
      result.issues.push_back({entries[i].editUnit, entries[i].byteOffset, *problems[i], i});
    }
    if (damage[i]) {
      result.essenceDamage.push_back({entries[i].editUnit, entries[i].byteOffset,
                                      "essence damaged after its picture element: " + *damage[i], i});
    }
  }
  return result;
}

IndexRepairResult RepairEssenceIndex(const std::string& path, const std::string& httpHeaders,
                                     const EssenceIndex& index, const MxfStreamInfo& info,
                                     const IndexVerifyResult& verified) {
  std::vector<FlatIndexEntry> entries = index.GetEntries();
  IndexRepairResult out;
  const auto finish = [&] {
    out.index = EssenceIndex(std::move(entries));
    out.index.SetEssenceStartResyncs(index.EssenceStartResyncs());
    return std::move(out);
  };
  if (verified.issues.empty()) return finish();

  MxfReader reader(path, httpHeaders);
  if (!reader.IsOpen()) {
    for (const IndexVerifyIssue& issue : verified.issues) {
      out.unrepaired.push_back({issue.editUnit, issue.byteOffset, "file could not be opened for the repair",
                                issue.entryIndex});
    }
    return finish();
  }

  std::vector<bool> bad(entries.size(), false);
  for (const IndexVerifyIssue& issue : verified.issues) bad[issue.entryIndex] = true;

  size_t issuePos = 0;
  while (issuePos < verified.issues.size()) {
    // One run of consecutive bad entries: [first, last].
    const size_t first = verified.issues[issuePos].entryIndex;
    size_t last = first;
    size_t runEnd = issuePos + 1;
    while (runEnd < verified.issues.size() && verified.issues[runEnd].entryIndex == last + 1) {
      last = verified.issues[runEnd].entryIndex;
      ++runEnd;
    }
    const auto giveUp = [&](size_t from, const std::string& why) {
      for (size_t k = issuePos; k < runEnd; ++k) {
        if (verified.issues[k].entryIndex >= from) {
          const IndexVerifyIssue& issue = verified.issues[k];
          out.unrepaired.push_back({issue.editUnit, issue.byteOffset, issue.what + "; not repaired: " + why,
                                    issue.entryIndex});
        }
      }
    };

    if (first == 0) {
      giveUp(first, "no valid entry before it to walk from");
      issuePos = runEnd;
      continue;
    }
    const size_t anchor = first - 1;  // good: the run starts right after it
    const uint64_t anchorVideo = verified.videoElementOffsets[anchor];
    const uint64_t layoutDelta = anchorVideo - entries[anchor].byteOffset;

    std::string damage;
    EssenceWalkerOptions options;
    options.maxResyncBytes = 0;
    options.onDamage = [&damage](const WalkDamage& d) {
      damage = (d.badLength ? "invalid KLV length field at byte " : "damaged essence at byte ") +
               std::to_string(d.offset);
    };
    EssenceWalker walker(reader, anchorVideo, info.videoTrackNumber, /*hasVideo=*/true, {}, std::move(options));
    EssenceElement element;
    if (!walker.Next(element, false) || element.filePosition != anchorVideo) {
      giveUp(first, "could not walk from edit unit " + std::to_string(entries[anchor].editUnit));
      issuePos = runEnd;
      continue;
    }

    for (size_t k = first; k <= last; ++k) {
      bool walked = true;
      for (int64_t step = entries[k - 1].editUnit; step < entries[k].editUnit; ++step) {
        if (!walker.Next(element, false)) {
          walked = false;
          break;
        }
      }
      if (!walked) {
        giveUp(k, damage.empty() ? "essence ends before this edit unit" : damage + " before this edit unit");
        break;
      }
      const uint64_t video = element.filePosition;
      const uint64_t walkerResume = walker.Position();
      const uint64_t next = k + 1 < entries.size() && !bad[k + 1] ? entries[k + 1].byteOffset : kNoBound;

      // Same layout as the anchor entry first, else straight at the picture
      // element (always a valid start for a walk to this edit unit).
      bool fixed = false;
      for (uint64_t candidate : {video >= layoutDelta ? video - layoutDelta : video, video}) {
        if (candidate <= entries[k - 1].byteOffset) continue;
        uint64_t found = 0;
        if (!CheckEntryOffset(reader, candidate, next, info, found) && found == video) {
          entries[k].byteOffset = candidate;
          fixed = true;
          break;
        }
      }
      reader.Seek(walkerResume);  // CheckEntryOffset moved the shared reader
      if (!fixed) {
        giveUp(k, "no offset before the picture element at byte " + std::to_string(video) + " checks out");
        break;
      }
      ++out.repaired;
    }
    issuePos = runEnd;
  }
  return finish();
}

std::string FormatVerifyIssues(const std::vector<IndexVerifyIssue>& issues, size_t maxListed) {
  std::string s;
  const size_t listed = std::min(maxListed, issues.size());
  for (size_t i = 0; i < listed; ++i) {
    if (i != 0) s += "; ";
    s += "edit unit " + std::to_string(issues[i].editUnit) + " at byte " + std::to_string(issues[i].byteOffset) +
         ": " + issues[i].what;
  }
  if (issues.size() > listed) s += "; and " + std::to_string(issues.size() - listed) + " more";
  return s;
}

}  // namespace mxfsrc
