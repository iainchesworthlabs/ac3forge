#include "json.hpp"

#include <cmath>
#include <cstdint>
#include <cstdio>
#include <fmt/base.h>
#include <fmt/format.h>
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
//
// Returns the finished token rather than writing as it goes: one fmt::print
// per value beats one per character, and the caller has nowhere to put a
// partially-written string if formatting were to throw part-way.
std::string quoted(std::string_view text) {
    std::string out;
    out.reserve(text.size() + 2);
    out.push_back('"');
    for (const char raw : text) {
        const auto byte = static_cast<unsigned char>(raw);
        switch (raw) {
            case '"': out += "\\\""; continue;
            case '\\': out += "\\\\"; continue;
            case '\b': out += "\\b"; continue;
            case '\f': out += "\\f"; continue;
            case '\n': out += "\\n"; continue;
            case '\r': out += "\\r"; continue;
            case '\t': out += "\\t"; continue;
            default: break;
        }
        if (byte < 0x20) {
            out += fmt::format("\\u{:04x}", byte);
            continue;
        }
        out.push_back(raw);
    }
    out.push_back('"');
    return out;
}

}  // namespace

// fmt::print rather than std::fputs throughout: this project's CI runs
// clang-tidy's cert-err33-c, which (rightly) refuses a discarded stdio return
// value, and every other status line in this CLI already goes out through
// fmt::print/fmt::println (see CHANGELOG.md's "std::format/std::print/
// std::printf replaced with {fmt}" entry - NDK r26's bundled libc++ has no
// <format> at all). main.cpp's top-level handler catches the fmt::format_error
// that can escape it.
void JsonWriter::indent() {
    fmt::print(out_, "\n");
    for (std::size_t level = 0; level < empty_.size(); ++level) {
        fmt::print(out_, "  ");
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
        fmt::print(out_, ",");
    }
    empty_.back() = false;
    indent();
}

void JsonWriter::begin_object() {
    separate();
    fmt::print(out_, "{{");
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
    fmt::print(out_, "}}");
}

void JsonWriter::begin_array() {
    separate();
    fmt::print(out_, "[");
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
    fmt::print(out_, "]");
}

void JsonWriter::key(std::string_view name) {
    separate();
    fmt::print(out_, "{}: ", quoted(name));
    after_key_ = true;
}

void JsonWriter::value(std::string_view text) {
    separate();
    fmt::print(out_, "{}", quoted(text));
}

void JsonWriter::value(const char* text) { value(std::string_view{text}); }

void JsonWriter::value(bool flag) {
    separate();
    fmt::print(out_, "{}", flag ? "true" : "false");
}

void JsonWriter::value(std::int64_t number) {
    separate();
    fmt::print(out_, "{}", number);
}

void JsonWriter::value(std::uint64_t number) {
    separate();
    fmt::print(out_, "{}", number);
}

void JsonWriter::value(double number, int decimals) {
    if (!std::isfinite(number)) {
        value_null();
        return;
    }
    separate();
    fmt::print(out_, "{:.{}f}", number, decimals);
}

void JsonWriter::value_null() {
    separate();
    fmt::print(out_, "null");
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

void JsonWriter::finish() { fmt::print(out_, "\n"); }

}  // namespace ac3cli
