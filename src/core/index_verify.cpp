#include "core/index_verify.h"

#include <algorithm>
#include <atomic>
#include <cstdio>
#include <cstring>
#include <limits>
#include <mutex>
#include <thread>

#include "mxf/essence_walker.h"
#include "mxf/h264_bitstream.h"
#include "mxf/klv.h"
#include "mxf/mpeg2_bitstream.h"

namespace mxfsrc {

namespace {

constexpr uint64_t kNoBound = std::numeric_limits<uint64_t>::max();
// Entries per work item: contiguous runs keep each worker's reads moving
// forward through the file.
constexpr size_t kEntriesPerChunk = 256;
constexpr size_t kLocalWorkers = 4;
constexpr size_t kHttpWorkers = 16;
// The repair walk ends this many consecutive agreeing entries after the last
// wrong one -- more than a long GOP, so a region shifted by whole edit units
// is not ended early by entries that pass by coincidence.
constexpr int kAgreeingEntriesToEndRepair = 32;

std::string HexBytes(const uint8_t* p, size_t n) {
  static const char kHex[] = "0123456789abcdef";
  std::string s;
  for (size_t i = 0; i < n; ++i) {
    s += kHex[p[i] >> 4];
    s += kHex[p[i] & 0x0f];
  }
  return s;
}

// Picture type from IndexTableEntry-style flags (SMPTE 377-1 bit4 backward,
// bit5 forward prediction; the same convention ParseMpeg2FrameHeader and
// ParseH264FrameHeader synthesize): I, P or B.
char PictureClass(uint8_t flags) {
  if ((flags & 0x30) == 0) return 'I';
  return (flags & 0x10) ? 'B' : 'P';
}

// Codecs whose picture type can be read from the first bytes of a frame.
bool HasPictureTypeParser(CodecFamily codec) {
  return codec == CodecFamily::kMpeg2Video || codec == CodecFamily::kH264LongGop;
}

// The picture type of the picture element at `elementOffset`, from its
// leading bytes (1 KiB, then 8 KiB if no picture header was found in that).
// nullopt: not recognized -- nothing to compare, never reported.
std::optional<char> StreamPictureClass(MxfReader& reader, uint64_t elementOffset, CodecFamily codec) {
  if (!reader.Seek(elementOffset)) return std::nullopt;
  KlvHeader klv{};
  if (!ReadKlvHeader(reader, klv)) return std::nullopt;
  std::vector<uint8_t> prefix;
  for (const uint64_t want : {uint64_t{1024}, uint64_t{8192}}) {
    const size_t n = static_cast<size_t>(std::min(want, klv.length));
    prefix.resize(n);
    if (!reader.Seek(klv.valueOffset) || (n > 0 && !reader.ReadBytes(prefix.data(), n))) return std::nullopt;
    if (codec == CodecFamily::kMpeg2Video) {
      const Mpeg2FrameInfo frame = ParseMpeg2FrameHeader(prefix.data(), prefix.size());
      if (frame.recognized) return PictureClass(frame.flags);
    } else {
      const H264FrameInfo frame = ParseH264FrameHeader(prefix.data(), prefix.size());
      if (frame.recognized) return PictureClass(frame.flags);
    }
    if (n >= klv.length) break;  // already read the whole frame
  }
  return std::nullopt;
}

// Checks one entry offset. On success returns nullopt and sets
// `videoElementOffset`; otherwise returns what is wrong. For codecs with a
// picture type parser, the entry's I/P/B type (`flags`) must also match the
// picture it leads to -- this catches an index shifted by whole edit units,
// whose offsets all land on valid KLVs.
std::optional<std::string> CheckEntryOffset(MxfReader& reader, uint64_t offset, uint64_t nextEntryOffset,
                                            const MxfStreamInfo& info, uint8_t flags, uint64_t& videoElementOffset,
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
  const uint64_t elementEnd = walker.Position();
  if (HasPictureTypeParser(info.videoCodec)) {
    if (const std::optional<char> found = StreamPictureClass(reader, element.filePosition, info.videoCodec)) {
      const char announced = PictureClass(flags);
      if (*found != announced) {
        const auto frame = [](char c) { return std::string(c == 'I' ? "an " : "a ") + c + "-frame"; };
        return "the index announces " + frame(announced) + ", the picture there (byte " +
               std::to_string(element.filePosition) + ") is " + frame(*found);
      }
    }
  }
  videoElementOffset = element.filePosition;
  if (videoElementEnd) *videoElementEnd = elementEnd;
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

bool IsVideoPictureKey(const Ul& key, const MxfStreamInfo& info) {
  return IsEssenceElementKey(key) && std::memcmp(&key[12], info.videoTrackNumber.data(), 4) == 0;
}

struct ChainProblem {
  std::optional<std::string> damage;   // essence damaged between the entries
  std::optional<std::string> skipped;  // the next entry skips edit unit(s): an index problem
};

// From the end of the picture element whose Value ends at `from`, the KLV
// headers must chain to exactly `nextEntryOffset`, passing no other picture
// element of the video track on the way (that would mean the next entry
// skips an edit unit, e.g. an index shifted by whole edit units).
ChainProblem CheckChainTo(MxfReader& reader, uint64_t from, uint64_t nextEntryOffset, const MxfStreamInfo& info) {
  ChainProblem problem;
  uint64_t pos = from;
  while (pos < nextEntryOffset) {
    if (!reader.Seek(pos)) {
      problem.damage = "cannot read at byte " + std::to_string(pos);
      return problem;
    }
    KlvHeader klv{};
    if (!ReadKlvHeader(reader, klv)) {
      problem.damage = "invalid KLV length field at byte " + std::to_string(pos);
      return problem;
    }
    if (!LooksLikeKlvKey(klv.key)) {
      problem.damage = "no KLV key at byte " + std::to_string(pos) + " (found " + HexBytes(klv.key.data(), 8) + "...)";
      return problem;
    }
    if (IsVideoPictureKey(klv.key, info)) {
      problem.skipped = "it skips an edit unit: the picture element at byte " + std::to_string(pos) +
                        " lies between the previous entry's picture and this entry";
      return problem;
    }
    pos = klv.valueOffset + klv.length;
  }
  if (pos != nextEntryOffset) {
    problem.damage = "a KLV ends at byte " + std::to_string(pos) + ", past the next entry's offset " +
                     std::to_string(nextEntryOffset);
  }
  return problem;
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
    problems[i] = CheckEntryOffset(reader, entries[i].byteOffset, next, info, entries[i].flags,
                                   result.videoElementOffsets[i], &videoEnds[i]);
  });
  if (result.error) return result;

  // Pass 2: between two good entries the essence chains without damage, and
  // the next entry doesn't skip an edit unit. Iteration i writes only
  // damage[i] and skipped[i + 1].
  std::vector<std::optional<std::string>> damage(n);
  std::vector<std::optional<std::string>> skipped(n);
  result.error = ParallelForEntries(path, httpHeaders, n - 1, [&](MxfReader& reader, size_t i) {
    if (problems[i] || problems[i + 1]) return;
    ChainProblem chain = CheckChainTo(reader, videoEnds[i], entries[i + 1].byteOffset, info);
    damage[i] = std::move(chain.damage);
    skipped[i + 1] = std::move(chain.skipped);
  });
  if (result.error) return result;

  for (size_t i = 0; i < n; ++i) {
    if (!problems[i] && skipped[i]) problems[i] = std::move(skipped[i]);
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

  const size_t n = entries.size();
  std::vector<const IndexVerifyIssue*> issueOf(n, nullptr);
  for (const IndexVerifyIssue& issue : verified.issues) issueOf[issue.entryIndex] = &issue;
  // Picture element each entry leads to, once known to be right: from the
  // check for good entries, from the walk for repaired ones (0: not trusted).
  std::vector<uint64_t> trustedVideo = verified.videoElementOffsets;

  const auto unrepaired = [&](size_t k, const std::string& why) {
    const std::string what = issueOf[k] ? issueOf[k]->what : "passed the checks but leads to another edit unit";
    out.unrepaired.push_back({entries[k].editUnit, index.GetEntries()[k].byteOffset, what + "; not repaired: " + why, k});
    trustedVideo[k] = 0;
  };

  size_t i = 0;
  while (i < n) {
    if (!issueOf[i]) {
      ++i;
      continue;
    }
    // A problem region starts at i. Walk the essence from the trusted entry
    // before it and compare every entry with the walk -- also the ones that
    // passed the checks: inside an index shifted by whole edit units some
    // entries pass by coincidence (a B-frame where a B-frame was announced).
    // The region ends after kAgreeingEntriesToEndRepair entries in a row agree.
    if (i == 0 || trustedVideo[i - 1] == 0) {
      unrepaired(i, i == 0 ? "no valid entry before it to walk from" : "the entry before it is not valid either");
      ++i;
      continue;
    }
    const size_t anchor = i - 1;
    const uint64_t anchorVideo = trustedVideo[anchor];
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
      unrepaired(i, "could not walk from edit unit " + std::to_string(entries[anchor].editUnit));
      ++i;
      continue;
    }

    int agreeing = 0;
    size_t k = i;
    for (; k < n && agreeing < kAgreeingEntriesToEndRepair; ++k) {
      bool walked = true;
      for (int64_t step = entries[k - 1].editUnit; step < entries[k].editUnit; ++step) {
        if (!walker.Next(element, false)) {
          walked = false;
          break;
        }
      }
      if (!walked) {
        // Everything from here to the end of this region stays as it is.
        const std::string why =
            damage.empty() ? "essence ends before this edit unit" : damage + " before this edit unit";
        for (; k < n && issueOf[k]; ++k) unrepaired(k, why);
        break;
      }
      const uint64_t video = element.filePosition;
      if (!issueOf[k] && trustedVideo[k] == video) {
        ++agreeing;
        continue;
      }
      agreeing = 0;

      // Same layout as the anchor entry first, else straight at the picture
      // element (always a valid start for a walk to this edit unit).
      const uint64_t walkerResume = walker.Position();
      bool fixed = false;
      for (uint64_t candidate : {video >= layoutDelta ? video - layoutDelta : video, video}) {
        if (candidate <= entries[k - 1].byteOffset) continue;
        uint64_t found = 0;
        if (!CheckEntryOffset(reader, candidate, kNoBound, info, entries[k].flags, found) && found == video) {
          entries[k].byteOffset = candidate;
          fixed = true;
          break;
        }
      }
      reader.Seek(walkerResume);  // CheckEntryOffset moved the shared reader
      if (fixed) {
        trustedVideo[k] = video;
        ++out.repaired;
        if (!issueOf[k]) ++out.repairedUndetected;
      } else {
        unrepaired(k, "no offset before the picture element at byte " + std::to_string(video) + " checks out");
      }
    }
    i = k;
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
