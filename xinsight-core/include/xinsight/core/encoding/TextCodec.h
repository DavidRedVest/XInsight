#pragma once

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace xinsight::core::encoding {

// PRD 2.3: v1 must support these four on open/edit/save, preserving the
// original encoding on save rather than silently normalizing to UTF-8.
// GB18030 is a strict superset of GBK's mapping for the code points GBK
// covers, so a single GB18030 codec correctly round-trips genuine GBK
// content too -- we don't need to distinguish "was it GBK or GB18030".
enum class TextEncoding { Utf8, Gb18030, Latin1 };

enum class LineEnding { Lf, CrLf };

struct DecodeResult {
    std::string utf8Text; // always normalized to LF internally; original line ending recorded separately
    TextEncoding encoding = TextEncoding::Utf8;
    LineEnding lineEnding = LineEnding::Lf;
    bool hadBom = false;
    // False only when detection fell all the way through to the Latin-1
    // catch-all (which always "succeeds" since every byte is valid
    // Latin-1) -- PRD 2.3: "检测不确定时...不静默猜错致乱码保存". Callers
    // should surface this to the user rather than silently trusting it.
    bool confident = true;
};

std::string_view encodingName(TextEncoding encoding);

// Detects encoding (BOM check, then strict UTF-8 validation, then GB18030
// validation, then Latin-1 fallback) and decodes to UTF-8. Also detects
// and strips the file's line-ending style, normalizing to bare '\n' in
// utf8Text (LineEnding records what to restore on save).
DecodeResult decode(const std::vector<uint8_t> &raw);

// Inverse of decode(): re-encodes UTF-8 text (with '\n' line endings) back
// to `encoding`, restoring `lineEnding` and prepending a BOM iff
// `includeBom` (only meaningful for Utf8).
std::vector<uint8_t> encode(std::string_view utf8Text, TextEncoding encoding, LineEnding lineEnding, bool includeBom);

} // namespace xinsight::core::encoding
