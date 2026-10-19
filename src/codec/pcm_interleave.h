#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "mxf/essence_walker.h"
#include "mxf/header_parser.h"

namespace mxfsrc {

// One edit unit's worth of decoded audio, channel-interleaved across every
// summed audio track (see InterleaveAudioEditUnit), at the file's native bit
// depth -- no format conversion, matching this plugin's PCM-passthrough
// design (see plan "Real audio output: multi-track PCM passthrough").
struct AudioBlock {
  std::vector<uint8_t> data;  // interleaved: [track0 ch0, track0 ch1, ..., track1 ch0, ...] per sample frame
  int64_t sampleCount = 0;    // samples-per-channel in this edit unit
};

// Byte-repacks one edit unit's independently-read-per-track essence elements
// into a single interleaved AudioBlock. `perTrackElements` must have exactly
// one entry per `tracks` entry, in the same order (i.e. perTrackElements[i]
// is tracks[i]'s element for this edit unit) -- SourceEngine's decode loop
// is responsible for that pairing (via EssenceElement::audioTrackIndex).
//
// This is a pure repack, like codec/raw_unpack.cpp's RawUyvyDecoder: no
// resampling or requantization, just a byte-exact rearrangement into
// interleaved order. Validates each track's element byte length is an exact
// multiple of channelCount*bytesPerSample and that every track agrees on
// the resulting sample count for this edit unit -- both should always hold
// for a synchronized recording; a mismatch is treated as malformed input
// (returns false with a descriptive `*errorOut`) rather than silently
// padding/truncating, consistent with this codebase's "fail loud on
// malformed input" convention elsewhere (see e.g. SourceEngine's decode
// error handling).
// Tracks whose essence is the D-10/IMX 8-Ch AES3 Element
// (AudioTrackInfo::eightChAes3Element) are unpacked to the same plain
// little-endian PCM first: the 4-byte element header is skipped, and of the 8 32-bit channel slots per sample frame the first
// channelCount are kept, as (word >> 4) & 0xFFFFFF for 24 bit or
// (word >> 12) & 0xFFFF for 16 bit -- the same conversion as FFmpeg's
// mxf_get_d10_aes3_packet.
bool InterleaveAudioEditUnit(const std::vector<EssenceElement>& perTrackElements,
                              const std::vector<AudioTrackInfo>& tracks, AudioBlock& out, std::string* errorOut);

}  // namespace mxfsrc
