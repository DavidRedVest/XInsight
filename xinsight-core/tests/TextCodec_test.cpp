#include <doctest/doctest.h>

#include "xinsight/core/encoding/TextCodec.h"

using namespace xinsight::core::encoding;

namespace {
std::vector<uint8_t> bytesOf(std::string_view s) {
    return std::vector<uint8_t>(s.begin(), s.end());
}
} // namespace

TEST_CASE("decode: plain ASCII is detected as UTF-8 with high confidence") {
    auto raw = bytesOf("int main(void) { return 0; }\n");
    auto result = decode(raw);
    CHECK(result.encoding == TextEncoding::Utf8);
    CHECK(result.confident);
    CHECK_FALSE(result.hadBom);
    CHECK(result.utf8Text == "int main(void) { return 0; }\n");
}

TEST_CASE("decode: valid UTF-8 with multibyte Chinese text is detected as UTF-8") {
    std::string text = "// \xE4\xBD\xA0\xE5\xA5\xBD (hello)\nint x;\n"; // "你好" in UTF-8
    auto raw = bytesOf(text);
    auto result = decode(raw);
    CHECK(result.encoding == TextEncoding::Utf8);
    CHECK(result.confident);
    CHECK(result.utf8Text == text);
}

TEST_CASE("decode: UTF-8 BOM is detected and stripped, encoding still UTF-8") {
    std::vector<uint8_t> raw = {0xEF, 0xBB, 0xBF};
    auto body = bytesOf("int x;\n");
    raw.insert(raw.end(), body.begin(), body.end());

    auto result = decode(raw);
    CHECK(result.encoding == TextEncoding::Utf8);
    CHECK(result.hadBom);
    CHECK(result.utf8Text == "int x;\n");
}

TEST_CASE("decode: ground-truth GB18030 bytes for a known Chinese string decode correctly") {
    // "你好，世界\n" encoded to GB18030 via the system `iconv` CLI --
    // independent ground truth, not produced by our own encode().
    std::vector<uint8_t> raw = {0xc4, 0xe3, 0xba, 0xc3, 0xa3, 0xac, 0xca, 0xc0, 0xbd, 0xe7, 0x0a};

    auto result = decode(raw);
    CHECK(result.encoding == TextEncoding::Gb18030);
    CHECK(result.confident);
    CHECK(result.utf8Text == "\xE4\xBD\xA0\xE5\xA5\xBD\xEF\xBC\x8C\xE4\xB8\x96\xE7\x95\x8C\n");
}

TEST_CASE("decode: bytes that are neither valid UTF-8 nor valid GB18030 fall back to Latin-1, unconfident") {
    // 0xFF is not a valid UTF-8 leading byte, and not a valid GB18030
    // leading byte either (GB18030's single-byte range is 0x00-0x7F; its
    // multi-byte leading range is 0x81-0xFE, so 0xFF itself is invalid).
    std::vector<uint8_t> raw = {0xFF, 0x41, 0x42};
    auto result = decode(raw);
    CHECK(result.encoding == TextEncoding::Latin1);
    CHECK_FALSE(result.confident);
}

TEST_CASE("decode: CRLF is detected and normalized to LF; LF-only files are detected as LF") {
    auto crlf = decode(bytesOf("a\r\nb\r\n"));
    CHECK(crlf.lineEnding == LineEnding::CrLf);
    CHECK(crlf.utf8Text == "a\nb\n");

    auto lf = decode(bytesOf("a\nb\n"));
    CHECK(lf.lineEnding == LineEnding::Lf);
    CHECK(lf.utf8Text == "a\nb\n");
}

TEST_CASE("encode/decode round-trip: UTF-8, GB18030, and Latin-1 all reproduce the original bytes") {
    struct Case {
        std::vector<uint8_t> raw;
        TextEncoding expectedEncoding;
    };

    std::vector<Case> cases = {
        {bytesOf("int add(int a, int b) { return a + b; }\n"), TextEncoding::Utf8},
        {{0xc4, 0xe3, 0xba, 0xc3, 0xa3, 0xac, 0xca, 0xc0, 0xbd, 0xe7, 0x0a}, TextEncoding::Gb18030},
        {{0xE9, 0x20, 0x41}, TextEncoding::Latin1}, // 0xE9 alone: not a valid UTF-8/GB18030 lead in this context
    };

    for (const auto &c : cases) {
        auto decoded = decode(c.raw);
        CHECK(decoded.encoding == c.expectedEncoding);

        auto reencoded = encode(decoded.utf8Text, decoded.encoding, decoded.lineEnding, decoded.hadBom);
        CHECK(reencoded == c.raw);
    }
}

TEST_CASE("encode/decode round-trip: CRLF is restored on save") {
    auto raw = bytesOf("line1\r\nline2\r\n");
    auto decoded = decode(raw);
    auto reencoded = encode(decoded.utf8Text, decoded.encoding, decoded.lineEnding, decoded.hadBom);
    CHECK(reencoded == raw);
}

TEST_CASE("encode/decode round-trip: UTF-8 BOM is restored on save") {
    std::vector<uint8_t> raw = {0xEF, 0xBB, 0xBF, 'a', 'b', 'c', '\n'};
    auto decoded = decode(raw);
    auto reencoded = encode(decoded.utf8Text, decoded.encoding, decoded.lineEnding, decoded.hadBom);
    CHECK(reencoded == raw);
}

TEST_CASE("encodingName returns the expected labels") {
    CHECK(encodingName(TextEncoding::Utf8) == "UTF-8");
    CHECK(encodingName(TextEncoding::Gb18030) == "GB18030");
    CHECK(encodingName(TextEncoding::Latin1) == "Latin-1");
}
