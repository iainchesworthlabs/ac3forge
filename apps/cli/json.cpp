#include "json.hpp"

#include <cmath>
#include <cstdint>
#include <cstdio>
#include <format>
#include <string>
#include <string_view>

namespace ac3cli {

namespace {

// RFC 8259 §7. The escapes below are the whole of what a JSON string may not
// carry literally: the quote, the backslash, and every code point under 0x20.
// Everything above that - including UTF-8's own multi-byte sequences, which a
// file path can perfectly well contain - passes through untouched, because
// those bytes are already valid JSON string content and re-encoding them would
// only risk mangling them.
void write_escaped(std::FILE* out, std::string_view text) {
    std::fputc('"', out);
    for (const char raw : text) {
        const auto byte = static_cast<unsigned char>(raw);
        switch (raw) {
            case '"': std::fputs("\\\"", out); continue;
            case '\\': std::fputs("\\\\", out); continue;
            case '\b': std::fputs("\\b", out); continue;
            case '\f': std::fputs("\\f", out); continue;
            case '\n': std::fputs("\\n", out); continue;
            case '\r': std::fputs("\\r", out); continue;
            case '\t': std::fputs("\\t", out); continue;
            default: break;
        }
        if (byte < 0x20) {
            std::fputs(std::format("\\u{:04x}", byte).c_str(), out);
            continue;
        }
        std::fputc(raw, out);
    }
    std::fputc('"', out);
}

}  // namespace

void JsonWriter::indent() {
    std::fputc('\n', out_);
    for (std::size_t level = 0; level < empty_.size(); ++level) {
        std::fputs("  ", out_);
    }
}

// Before any value or key: close off whatever came before it. A value that
// directly follows its own key stays on that line; anything else starts a new
// one, indented to its container's depth.
void JsonWriter::separate() {
    if (after_key_) {
        after_key_ = false;
        return;
    }
    if (empty_.empty()) {
        return;
    }
    if (!empty_.back()) {
        std::fputc(',', out_);
    }
    empty_.back() = false;
    indent();
}

void JsonWriter::begin_object() {
    separate();
    std::fputc('{', out_);
    empty_.push_back(true);
}

void JsonWriter::end_object() {
    const bool was_empty = empty_.empty() || empty_.back();
    if (!empty_.empty()) {
        empty_.pop_back();
    }
    // An empty object stays on one line as {}: the alternative is a lone
    // brace on a line of its own with nothing above it, which reads as
    // damage.
    if (!was_empty) {
        indent();
    }
    std::fputc('}', out_);
}

void JsonWriter::begin_array() {
    separate();
    std::fputc('[', out_);
    empty_.push_back(true);
}

void JsonWriter::end_array() {
    const bool was_empty = empty_.empty() || empty_.back();
    if (!empty_.empty()) {
        empty_.pop_back();
    }
    if (!was_empty) {
        indent();
    }
    std::fputc(']', out_);
}

void JsonWriter::key(std::string_view name) {
    separate();
    write_escaped(out_, name);
    std::fputs(": ", out_);
    after_key_ = true;
}

void JsonWriter::value(std::string_view text) {
    separate();
    write_escaped(out_, text);
}

void JsonWriter::value(const char* text) { value(std::string_view{text}); }

void JsonWriter::value(bool flag) {
    separate();
    std::fputs(flag ? "true" : "false", out_);
}

void JsonWriter::value(std::int64_t number) {
    separate();
    std::fputs(std::format("{}", number).c_str(), out_);
}

void JsonWriter::value(std::uint64_t number) {
    separate();
    std::fputs(std::format("{}", number).c_str(), out_);
}

void JsonWriter::value(double number, int decimals) {
    if (!std::isfinite(number)) {
        value_null();
        return;
    }
    separate();
    std::fputs(std::format("{:.{}f}", number, decimals).c_str(), out_);
}

void JsonWriter::value_null() {
    separate();
    std::fputs("null", out_);
}

void JsonWriter::member(std::string_view name, std::string_view text) {
    key(name);
    value(text);
}

void JsonWriter::member(std::string_view name, const char* text) {
    key(name);
    value(text);
}

void JsonWriter::member(std::string_view name, bool flag) {
    key(name);
    value(flag);
}

void JsonWriter::member(std::string_view name, std::int64_t number) {
    key(name);
    value(number);
}

void JsonWriter::member(std::string_view name, std::uint64_t number) {
    key(name);
    value(number);
}

void JsonWriter::member(std::string_view name, double number, int decimals) {
    key(name);
    value(number, decimals);
}

void JsonWriter::member_null(std::string_view name) {
    key(name);
    value_null();
}

void JsonWriter::finish() { std::fputc('\n', out_); }

}  // namespace ac3cli
