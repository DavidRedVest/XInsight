#include "xinsight/core/encoding/TextCodec.h"

#include <algorithm>
#include <cerrno>
#include <optional>

#include <iconv.h>

namespace xinsight::core::encoding {

namespace {

bool isValidUtf8(const uint8_t *data, size_t len) {
    size_t i = 0;
    while (i < len) {
        uint8_t b0 = data[i];
        if (b0 <= 0x7F) {
            ++i;
            continue;
        }

        int extra;
        uint32_t codepoint;
        uint32_t minCodepoint;
        if ((b0 & 0xE0) == 0xC0) {
            extra = 1;
            codepoint = b0 & 0x1Fu;
            minCodepoint = 0x80;
        } else if ((b0 & 0xF0) == 0xE0) {
            extra = 2;
            codepoint = b0 & 0x0Fu;
            minCodepoint = 0x800;
        } else if ((b0 & 0xF8) == 0xF0) {
            extra = 3;
            codepoint = b0 & 0x07u;
            minCodepoint = 0x10000;
        } else {
            return false; // stray continuation byte or invalid leading byte
        }

        if (i + 1 + static_cast<size_t>(extra) > len) return false;

        for (int k = 1; k <= extra; ++k) {
            uint8_t b = data[i + static_cast<size_t>(k)];
            if ((b & 0xC0) != 0x80) return false;
            codepoint = (codepoint << 6) | (b & 0x3Fu);
        }

        if (codepoint < minCodepoint) return false; // overlong encoding
        if (codepoint > 0x10FFFF) return false;
        if (codepoint >= 0xD800 && codepoint <= 0xDFFF) return false; // surrogate

        i += 1 + static_cast<size_t>(extra);
    }
    return true;
}

std::optional<std::string> iconvConvert(const char *fromCode, const char *toCode, std::string_view input) {
    iconv_t cd = iconv_open(toCode, fromCode);
    if (cd == reinterpret_cast<iconv_t>(-1)) return std::nullopt;

    std::string result;
    result.resize(input.size() * 4 + 64);

    // iconv's signature takes non-const `char **inbuf` on macOS/BSD libiconv.
    char *inBuf = const_cast<char *>(input.data());
    size_t inBytesLeft = input.size();
    char *outBuf = result.data();
    size_t outBytesLeft = result.size();

    size_t rc = iconv(cd, &inBuf, &inBytesLeft, &outBuf, &outBytesLeft);
    iconv_close(cd);

    if (rc == static_cast<size_t>(-1) || inBytesLeft != 0) return std::nullopt;

    result.resize(result.size() - outBytesLeft);
    return result;
}

std::string latin1ToUtf8(const uint8_t *data, size_t len) {
    std::string out;
    out.reserve(len * 2);
    for (size_t i = 0; i < len; ++i) {
        uint8_t b = data[i];
        if (b <= 0x7F) {
            out.push_back(static_cast<char>(b));
        } else {
            out.push_back(static_cast<char>(0xC0 | (b >> 6)));
            out.push_back(static_cast<char>(0x80 | (b & 0x3F)));
        }
    }
    return out;
}

std::vector<uint8_t> utf8ToLatin1(std::string_view utf8) {
    std::vector<uint8_t> out;
    out.reserve(utf8.size());
    size_t i = 0;
    while (i < utf8.size()) {
        uint8_t b0 = static_cast<uint8_t>(utf8[i]);
        if (b0 <= 0x7F) {
            out.push_back(b0);
            ++i;
        } else if ((b0 & 0xE0) == 0xC0 && i + 1 < utf8.size()) {
            uint32_t cp = ((b0 & 0x1Fu) << 6) | (static_cast<uint8_t>(utf8[i + 1]) & 0x3Fu);
            out.push_back(cp <= 0xFF ? static_cast<uint8_t>(cp) : uint8_t{'?'});
            i += 2;
        } else {
            // 3/4-byte sequence: definitionally > 0xFF, can't be represented in Latin-1.
            out.push_back('?');
            // Skip the whole sequence so we don't emit one '?' per continuation byte.
            size_t skip = ((b0 & 0xF0) == 0xE0) ? 3 : ((b0 & 0xF8) == 0xF0 ? 4 : 1);
            i += skip;
        }
    }
    return out;
}

struct LineEndingSplit {
    std::string normalized; // '\n' only
    LineEnding ending;
};

LineEndingSplit normalizeLineEndings(std::string_view text) {
    bool sawCrLf = text.find("\r\n") != std::string_view::npos;

    std::string normalized;
    normalized.reserve(text.size());
    for (size_t i = 0; i < text.size(); ++i) {
        if (text[i] == '\r' && i + 1 < text.size() && text[i + 1] == '\n') continue; // drop the \r, keep the \n
        normalized.push_back(text[i]);
    }

    return LineEndingSplit{std::move(normalized), sawCrLf ? LineEnding::CrLf : LineEnding::Lf};
}

std::string expandLineEndings(std::string_view normalized, LineEnding ending) {
    if (ending == LineEnding::Lf) return std::string(normalized);

    std::string out;
    out.reserve(normalized.size() + normalized.size() / 20);
    for (char c : normalized) {
        if (c == '\n') out.push_back('\r');
        out.push_back(c);
    }
    return out;
}

constexpr uint8_t kUtf8Bom[3] = {0xEF, 0xBB, 0xBF};

} // namespace

std::string_view encodingName(TextEncoding encoding) {
    switch (encoding) {
    case TextEncoding::Utf8: return "UTF-8";
    case TextEncoding::Gb18030: return "GB18030";
    case TextEncoding::Latin1: return "Latin-1";
    }
    return "UTF-8";
}

DecodeResult decode(const std::vector<uint8_t> &raw) {
    DecodeResult result;

    const uint8_t *data = raw.data();
    size_t len = raw.size();

    bool hadBom = len >= 3 && data[0] == kUtf8Bom[0] && data[1] == kUtf8Bom[1] && data[2] == kUtf8Bom[2];
    if (hadBom) {
        data += 3;
        len -= 3;
    }

    std::string decoded;
    TextEncoding encoding;
    bool confident = true;

    if (hadBom || isValidUtf8(data, len)) {
        decoded.assign(reinterpret_cast<const char *>(data), len);
        encoding = TextEncoding::Utf8;
    } else if (auto gb = iconvConvert("GB18030", "UTF-8", std::string_view(reinterpret_cast<const char *>(data), len))) {
        decoded = std::move(*gb);
        encoding = TextEncoding::Gb18030;
    } else {
        // Latin-1 always "succeeds" (every byte 0-255 is a valid Latin-1
        // code point) -- last-resort fallback per PRD 2.3, flagged via
        // `confident = false` rather than silently trusted.
        decoded = latin1ToUtf8(data, len);
        encoding = TextEncoding::Latin1;
        confident = false;
    }

    auto [normalized, lineEnding] = normalizeLineEndings(decoded);

    result.utf8Text = std::move(normalized);
    result.encoding = encoding;
    result.lineEnding = lineEnding;
    result.hadBom = hadBom;
    result.confident = confident;
    return result;
}

std::vector<uint8_t> encode(std::string_view utf8Text, TextEncoding encoding, LineEnding lineEnding, bool includeBom) {
    std::string withLineEndings = expandLineEndings(utf8Text, lineEnding);

    std::vector<uint8_t> out;

    if (encoding == TextEncoding::Utf8) {
        if (includeBom) out.insert(out.end(), kUtf8Bom, kUtf8Bom + 3);
        out.insert(out.end(), withLineEndings.begin(), withLineEndings.end());
        return out;
    }

    if (encoding == TextEncoding::Gb18030) {
        if (auto gb = iconvConvert("UTF-8", "GB18030", withLineEndings)) {
            out.assign(gb->begin(), gb->end());
            return out;
        }
        // GB18030 is a total mapping over Unicode, so this shouldn't happen
        // for well-formed UTF-8 input; fall back to UTF-8 bytes rather than
        // silently dropping content.
        out.insert(out.end(), withLineEndings.begin(), withLineEndings.end());
        return out;
    }

    return utf8ToLatin1(withLineEndings);
}

} // namespace xinsight::core::encoding
