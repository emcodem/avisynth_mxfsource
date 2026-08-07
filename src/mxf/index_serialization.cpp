#include "mxf/index_serialization.h"

#include <cctype>
#include <cstdlib>
#include <vector>

namespace mxfsrc {

namespace {

std::string EmitEntry(const FlatIndexEntry& e) {
  std::string out = "{\"editUnit\": ";
  out += std::to_string(e.editUnit);
  out += ", \"byteOffset\": ";
  out += std::to_string(e.byteOffset);
  out += ", \"isKeyframe\": ";
  out += e.isKeyframe ? "true" : "false";
  out += ", \"flags\": ";
  out += std::to_string(static_cast<int>(e.flags));
  out += ", \"keyFrameOffset\": ";
  out += std::to_string(static_cast<int>(e.keyFrameOffset));
  out += "}";
  return out;
}

// Minimal JSON reader scoped exactly to the document shape
// SerializeEssenceIndex emits -- not a general-purpose JSON library. Still
// tolerant of arbitrary whitespace and of unrecognized extra fields/keys
// (skipped via SkipValue) so a hand-edited or future-versioned document
// doesn't choke the parser outright; the explicit version/bodySid checks in
// DeserializeEssenceIndex are what actually guards against an incompatible
// or mismatched document, not this parser's leniency.
class JsonReader {
 public:
  explicit JsonReader(const std::string& text) : text_(text) {}

  bool ParseTopLevel(int64_t& version, int64_t& bodySid, std::vector<FlatIndexEntry>& entries) {
    bool haveVersion = false;
    bool haveBodySid = false;
    bool haveEntries = false;

    SkipWs();
    if (!Consume('{')) return Fail("expected '{' at start of document");
    SkipWs();
    if (!Consume('}')) {
      for (;;) {
        SkipWs();
        std::string key;
        if (!ParseString(key)) return Fail("expected a quoted key");
        SkipWs();
        if (!Consume(':')) return Fail("expected ':' after key '" + key + "'");
        SkipWs();
        if (key == "version") {
          if (!ParseInteger(version)) return Fail("\"version\" must be an integer");
          haveVersion = true;
        } else if (key == "bodySid") {
          if (!ParseInteger(bodySid)) return Fail("\"bodySid\" must be an integer");
          haveBodySid = true;
        } else if (key == "entries") {
          if (!ParseEntriesArray(entries)) return false;
          haveEntries = true;
        } else {
          if (!SkipValue()) return Fail("failed to skip unrecognized field '" + key + "'");
        }
        SkipWs();
        if (Consume(',')) continue;
        if (Consume('}')) break;
        return Fail("expected ',' or '}'");
      }
    }

    if (!haveVersion) return Fail("missing \"version\" field");
    if (!haveBodySid) return Fail("missing \"bodySid\" field");
    if (!haveEntries) return Fail("missing \"entries\" field");
    return true;
  }

  const std::string& Error() const { return error_; }

 private:
  bool ParseEntriesArray(std::vector<FlatIndexEntry>& entries) {
    if (!Consume('[')) return Fail("\"entries\" must be an array");
    SkipWs();
    if (Consume(']')) return true;
    for (;;) {
      SkipWs();
      FlatIndexEntry e;
      if (!ParseEntryObject(e)) return false;
      entries.push_back(e);
      SkipWs();
      if (Consume(',')) continue;
      if (Consume(']')) break;
      return Fail("expected ',' or ']' in entries array");
    }
    return true;
  }

  bool ParseEntryObject(FlatIndexEntry& e) {
    if (!Consume('{')) return Fail("expected '{' for an index entry");
    bool haveEditUnit = false;
    bool haveByteOffset = false;
    SkipWs();
    if (!Consume('}')) {
      for (;;) {
        SkipWs();
        std::string key;
        if (!ParseString(key)) return Fail("expected a quoted key in an index entry");
        SkipWs();
        if (!Consume(':')) return Fail("expected ':' after key '" + key + "'");
        SkipWs();
        if (key == "editUnit") {
          int64_t v = 0;
          if (!ParseInteger(v)) return Fail("\"editUnit\" must be an integer");
          e.editUnit = v;
          haveEditUnit = true;
        } else if (key == "byteOffset") {
          int64_t v = 0;
          if (!ParseInteger(v)) return Fail("\"byteOffset\" must be an integer");
          e.byteOffset = static_cast<uint64_t>(v);
          haveByteOffset = true;
        } else if (key == "isKeyframe") {
          if (!ParseBool(e.isKeyframe)) return Fail("\"isKeyframe\" must be a boolean");
        } else if (key == "flags") {
          int64_t v = 0;
          if (!ParseInteger(v)) return Fail("\"flags\" must be an integer");
          e.flags = static_cast<uint8_t>(v);
        } else if (key == "keyFrameOffset") {
          int64_t v = 0;
          if (!ParseInteger(v)) return Fail("\"keyFrameOffset\" must be an integer");
          e.keyFrameOffset = static_cast<int8_t>(v);
        } else {
          if (!SkipValue()) return Fail("failed to skip unrecognized entry field '" + key + "'");
        }
        SkipWs();
        if (Consume(',')) continue;
        if (Consume('}')) break;
        return Fail("expected ',' or '}' in an index entry");
      }
    }
    if (!haveEditUnit) return Fail("index entry missing \"editUnit\"");
    if (!haveByteOffset) return Fail("index entry missing \"byteOffset\"");
    return true;
  }

  bool ParseString(std::string& out) {
    if (!Consume('"')) return false;
    out.clear();
    while (pos_ < text_.size() && text_[pos_] != '"') {
      char c = text_[pos_++];
      if (c == '\\' && pos_ < text_.size()) {
        char esc = text_[pos_++];
        switch (esc) {
          case '"':
            out += '"';
            break;
          case '\\':
            out += '\\';
            break;
          case '/':
            out += '/';
            break;
          default:
            out += esc;
            break;
        }
      } else {
        out += c;
      }
    }
    if (pos_ >= text_.size()) return false;  // unterminated string
    ++pos_;                                  // closing quote
    return true;
  }

  bool ParseInteger(int64_t& out) {
    const size_t start = pos_;
    if (pos_ < text_.size() && text_[pos_] == '-') ++pos_;
    const size_t digitsStart = pos_;
    while (pos_ < text_.size() && std::isdigit(static_cast<unsigned char>(text_[pos_]))) ++pos_;
    if (pos_ == digitsStart) {
      pos_ = start;
      return false;
    }
    // Reject a decimal point/exponent immediately following -- our own
    // format never emits fractional numbers, and silently truncating one
    // would be worse than failing loud.
    if (pos_ < text_.size() && (text_[pos_] == '.' || text_[pos_] == 'e' || text_[pos_] == 'E')) return false;
    out = std::strtoll(text_.substr(start, pos_ - start).c_str(), nullptr, 10);
    return true;
  }

  bool ParseBool(bool& out) {
    if (text_.compare(pos_, 4, "true") == 0) {
      out = true;
      pos_ += 4;
      return true;
    }
    if (text_.compare(pos_, 5, "false") == 0) {
      out = false;
      pos_ += 5;
      return true;
    }
    return false;
  }

  // Skips one JSON value of any kind (string/number/bool/null/object/array)
  // without interpreting it -- used for unrecognized fields.
  bool SkipValue() {
    SkipWs();
    if (pos_ >= text_.size()) return false;
    const char c = text_[pos_];
    if (c == '"') {
      std::string dummy;
      return ParseString(dummy);
    }
    if (c == '{') {
      ++pos_;
      SkipWs();
      if (Consume('}')) return true;
      for (;;) {
        SkipWs();
        std::string key;
        if (!ParseString(key)) return false;
        SkipWs();
        if (!Consume(':')) return false;
        if (!SkipValue()) return false;
        SkipWs();
        if (Consume(',')) continue;
        if (Consume('}')) return true;
        return false;
      }
    }
    if (c == '[') {
      ++pos_;
      SkipWs();
      if (Consume(']')) return true;
      for (;;) {
        if (!SkipValue()) return false;
        SkipWs();
        if (Consume(',')) continue;
        if (Consume(']')) return true;
        return false;
      }
    }
    if (c == 't' && text_.compare(pos_, 4, "true") == 0) {
      pos_ += 4;
      return true;
    }
    if (c == 'f' && text_.compare(pos_, 5, "false") == 0) {
      pos_ += 5;
      return true;
    }
    if (c == 'n' && text_.compare(pos_, 4, "null") == 0) {
      pos_ += 4;
      return true;
    }
    int64_t dummy = 0;
    return ParseInteger(dummy);
  }

  void SkipWs() {
    while (pos_ < text_.size() && std::isspace(static_cast<unsigned char>(text_[pos_]))) ++pos_;
  }

  bool Consume(char c) {
    if (pos_ < text_.size() && text_[pos_] == c) {
      ++pos_;
      return true;
    }
    return false;
  }

  bool Fail(const std::string& msg) {
    error_ = "index JSON: " + msg + " (at offset " + std::to_string(pos_) + ")";
    return false;
  }

  const std::string& text_;
  size_t pos_ = 0;
  std::string error_;
};

}  // namespace

std::string SerializeEssenceIndex(const EssenceIndex& index, uint32_t bodySid) {
  const std::vector<FlatIndexEntry>& entries = index.GetEntries();
  std::string out;
  out += "{\n";
  out += "  \"version\": " + std::to_string(kIndexBlobVersion) + ",\n";
  out += "  \"bodySid\": " + std::to_string(bodySid) + ",\n";
  out += "  \"entries\": [\n";
  for (size_t i = 0; i < entries.size(); ++i) {
    out += "    " + EmitEntry(entries[i]);
    if (i + 1 < entries.size()) out += ",";
    out += "\n";
  }
  out += "  ]\n";
  out += "}\n";
  return out;
}

std::optional<EssenceIndex> DeserializeEssenceIndex(const std::string& json, uint32_t expectedBodySid,
                                                     std::string* errorOut) {
  JsonReader reader(json);
  int64_t version = 0;
  int64_t bodySid = 0;
  std::vector<FlatIndexEntry> entries;
  if (!reader.ParseTopLevel(version, bodySid, entries)) {
    if (errorOut) *errorOut = reader.Error();
    return std::nullopt;
  }
  if (version != kIndexBlobVersion) {
    if (errorOut) {
      *errorOut = "index JSON: unsupported version " + std::to_string(version) + " (this build expects version " +
                  std::to_string(kIndexBlobVersion) + ")";
    }
    return std::nullopt;
  }
  if (bodySid != static_cast<int64_t>(expectedBodySid)) {
    if (errorOut) {
      *errorOut = "index JSON: bodySid mismatch (document was built for bodySid " + std::to_string(bodySid) +
                  ", this file's video bodySid is " + std::to_string(expectedBodySid) + ")";
    }
    return std::nullopt;
  }
  return EssenceIndex(std::move(entries));
}

}  // namespace mxfsrc
