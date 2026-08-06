#pragma once

#include <optional>
#include <string_view>

namespace mxfsrc {

// Playback policy for what to do when a file's audio is present but not in
// a state this plugin can output (tracks with mismatched bit depth/sample
// rate, or a non-PCM codec -- see MxfStreamInfo::audioConfigSupported).
// Never affects a file with no audio at all: that's always fine under every
// mode. Exposed to scripts as MxfSource(..., use_audio="always"/"never"/
// "supported").
enum class UseAudioMode {
  // Hard-fail SourceEngine::Open() if audio is present but unsupported.
  // Never fails just because a file has no audio at all.
  kAlways,
  // Always ignore audio, output video only, regardless of whether the
  // audio configuration would otherwise be supported.
  kNever,
  // Degrade to video-only, no error, whenever audio is present but
  // unsupported -- exactly like this plugin's original silence-only
  // behavior. The default.
  kSupported,
};

inline std::optional<UseAudioMode> ParseUseAudioMode(std::string_view s) {
  if (s == "always") return UseAudioMode::kAlways;
  if (s == "never") return UseAudioMode::kNever;
  if (s == "supported") return UseAudioMode::kSupported;
  return std::nullopt;
}

}  // namespace mxfsrc
