#include "ac3/sendspin/json.hpp"

#include <algorithm>
#include <array>
#include <charconv>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <system_error>
#include <vector>

namespace ac3::sendspin::json {

namespace {

constexpr std::uint32_t kReplacementCharacter = 0xFFFD;

[[nodiscard]] constexpr bool is_space(char c) {
    return c == ' ' || c == '\t' || c == '\n' || c == '\r';
}

[[nodiscard]] constexpr bool is_digit(char c) { return c >= '0' && c <= '9'; }

[[nodiscard]] constexpr int hex_value(char c) {
    if (c >= '0' && c <= '9') {
        return c - '0';
    }
    if (c >= 'a' && c <= 'f') {
        return c - 'a' + 10;
    }
    if (c >= 'A' && c <= 'F') {
        return c - 'A' + 10;
    }
    return -1;
}

[[nodiscard]] constexpr unsigned char byte_at(std::string_view text, std::size_t i) {
    return static_cast<unsigned char>(text[i]);
}

// The length of the well-formed UTF-8 sequence starting at text[i], or 0. RFC
// 3629's table: no overlong forms, no encoded surrogates, nothing past U+10FFFF.
[[nodiscard]] std::size_t utf8_sequence(std::string_view text, std::size_t i) {
    const unsigned char lead = byte_at(text, i);
    if (lead < 0x80) {
        return 1;
    }
    const auto continuation = [&](std::size_t k) {
        return i + k < text.size() && (byte_at(text, i + k) & 0xC0U) == 0x80U;
    };
    if (lead >= 0xC2 && lead <= 0xDF) {
        return continuation(1) ? 2 : 0;
    }
    if (lead >= 0xE0 && lead <= 0xEF) {
        if (!continuation(1) || !continuation(2)) {
            return 0;
        }
        const unsigned char second = byte_at(text, i + 1);
        if ((lead == 0xE0 && second < 0xA0) || (lead == 0xED && second > 0x9F)) {
            return 0;
        }
        return 3;
    }
    if (lead >= 0xF0 && lead <= 0xF4) {
        if (!continuation(1) || !continuation(2) || !continuation(3)) {
            return 0;
        }
        const unsigned char second = byte_at(text, i + 1);
        if ((lead == 0xF0 && second < 0x90) || (lead == 0xF4 && second > 0x8F)) {
            return 0;
        }
        return 4;
    }
    return 0;
}

struct Scan {
    Error error = Error::kNone;
    // One past the token on success; where it failed otherwise.
    std::size_t end = 0;
    std::uint8_t flags = 0;
};

[[nodiscard]] Scan scan_string(std::string_view text, std::size_t pos) {
    std::size_t i = pos + 1;
    std::uint8_t flags = 0;
    while (i < text.size()) {
        const unsigned char c = byte_at(text, i);
        if (c == '"') {
            return {Error::kNone, i + 1, flags};
        }
        if (c == '\\') {
            flags = kFlagEscaped;
            if (i + 1 >= text.size()) {
                return {Error::kTruncated, text.size(), flags};
            }
            switch (text[i + 1]) {
                case '"':
                case '\\':
                case '/':
                case 'b':
                case 'f':
                case 'n':
                case 'r':
                case 't':
                    i += 2;
                    continue;
                case 'u':
                    for (std::size_t k = 2; k < 6; ++k) {
                        if (i + k >= text.size()) {
                            return {Error::kTruncated, text.size(), flags};
                        }
                        if (hex_value(text[i + k]) < 0) {
                            return {Error::kBadString, i + k, flags};
                        }
                    }
                    i += 6;
                    continue;
                default:
                    return {Error::kBadString, i + 1, flags};
            }
        }
        if (c < 0x20) {
            return {Error::kBadString, i, flags};
        }
        const std::size_t length = utf8_sequence(text, i);
        if (length == 0) {
            return {Error::kBadString, i, flags};
        }
        i += length;
    }
    return {Error::kTruncated, text.size(), flags};
}

[[nodiscard]] Scan scan_number(std::string_view text, std::size_t pos) {
    std::size_t i = pos;
    const auto digits = [&] {
        while (i < text.size() && is_digit(text[i])) {
            ++i;
        }
    };
    if (text[i] == '-') {
        ++i;
    }
    if (i >= text.size()) {
        return {Error::kTruncated, i, 0};
    }
    if (text[i] == '0') {
        ++i;
        if (i < text.size() && is_digit(text[i])) {
            return {Error::kBadNumber, i, 0};
        }
    } else if (is_digit(text[i])) {
        digits();
    } else {
        return {Error::kBadNumber, i, 0};
    }
    std::uint8_t flags = kFlagInteger;
    if (i < text.size() && text[i] == '.') {
        flags = 0;
        ++i;
        if (i >= text.size()) {
            return {Error::kTruncated, i, 0};
        }
        if (!is_digit(text[i])) {
            return {Error::kBadNumber, i, 0};
        }
        digits();
    }
    if (i < text.size() && (text[i] == 'e' || text[i] == 'E')) {
        flags = 0;
        ++i;
        if (i < text.size() && (text[i] == '+' || text[i] == '-')) {
            ++i;
        }
        if (i >= text.size()) {
            return {Error::kTruncated, i, 0};
        }
        if (!is_digit(text[i])) {
            return {Error::kBadNumber, i, 0};
        }
        digits();
    }
    return {Error::kNone, i, flags};
}

[[nodiscard]] Scan scan_literal(std::string_view text, std::size_t pos) {
    static constexpr std::array<std::string_view, 3> kWords{"true", "false", "null"};
    for (const std::string_view word : kWords) {
        if (word.front() != text[pos]) {
            continue;
        }
        const std::string_view rest = text.substr(pos);
        const std::size_t common = std::min(rest.size(), word.size());
        for (std::size_t k = 0; k < common; ++k) {
            if (rest[k] != word[k]) {
                return {Error::kSyntax, pos + k, 0};
            }
        }
        if (rest.size() < word.size()) {
            return {Error::kTruncated, text.size(), 0};
        }
        return {Error::kNone, pos + word.size(), word == "true" ? kFlagTrue : std::uint8_t{0}};
    }
    return {Error::kSyntax, pos, 0};
}

// A string token's decoded UTF-8, a byte at a time, from its already-validated
// body (the text between the quotes).
class DecodedBytes {
   public:
    explicit DecodedBytes(std::string_view body) : body_(body) {}

    // The next byte, or -1 after the last.
    [[nodiscard]] int next() {
        if (pending_at_ < pending_size_) {
            return pending_[pending_at_++];
        }
        if (at_ >= body_.size()) {
            return -1;
        }
        const char c = body_[at_];
        if (c != '\\') {
            ++at_;
            return static_cast<unsigned char>(c);
        }
        const char escape = body_[at_ + 1];
        at_ += 2;
        switch (escape) {
            case 'b':
                return '\b';
            case 'f':
                return '\f';
            case 'n':
                return '\n';
            case 'r':
                return '\r';
            case 't':
                return '\t';
            case 'u':
                break;
            default:
                return static_cast<unsigned char>(escape);
        }
        std::uint32_t code = hex4(at_);
        at_ += 4;
        if (code >= 0xD800 && code <= 0xDBFF) {
            const bool paired = at_ + 6 <= body_.size() && body_[at_] == '\\' &&
                                body_[at_ + 1] == 'u' && hex4(at_ + 2) >= 0xDC00 &&
                                hex4(at_ + 2) <= 0xDFFF;
            if (paired) {
                code = 0x10000U + ((code - 0xD800U) << 10U) + (hex4(at_ + 2) - 0xDC00U);
                at_ += 6;
            } else {
                code = kReplacementCharacter;
            }
        } else if (code >= 0xDC00 && code <= 0xDFFF) {
            code = kReplacementCharacter;
        }
        encode(code);
        return pending_[pending_at_++];
    }

   private:
    [[nodiscard]] std::uint32_t hex4(std::size_t at) const {
        std::uint32_t value = 0;
        for (std::size_t k = 0; k < 4; ++k) {
            value = (value << 4U) | static_cast<std::uint32_t>(hex_value(body_[at + k]));
        }
        return value;
    }

    void encode(std::uint32_t code) {
        pending_at_ = 0;
        if (code < 0x80) {
            pending_[0] = static_cast<std::uint8_t>(code);
            pending_size_ = 1;
        } else if (code < 0x800) {
            pending_[0] = static_cast<std::uint8_t>(0xC0U | (code >> 6U));
            pending_[1] = static_cast<std::uint8_t>(0x80U | (code & 0x3FU));
            pending_size_ = 2;
        } else if (code < 0x10000) {
            pending_[0] = static_cast<std::uint8_t>(0xE0U | (code >> 12U));
            pending_[1] = static_cast<std::uint8_t>(0x80U | ((code >> 6U) & 0x3FU));
            pending_[2] = static_cast<std::uint8_t>(0x80U | (code & 0x3FU));
            pending_size_ = 3;
        } else {
            pending_[0] = static_cast<std::uint8_t>(0xF0U | (code >> 18U));
            pending_[1] = static_cast<std::uint8_t>(0x80U | ((code >> 12U) & 0x3FU));
            pending_[2] = static_cast<std::uint8_t>(0x80U | ((code >> 6U) & 0x3FU));
            pending_[3] = static_cast<std::uint8_t>(0x80U | (code & 0x3FU));
            pending_size_ = 4;
        }
    }

    std::string_view body_;
    std::size_t at_ = 0;
    std::array<std::uint8_t, 4> pending_{};
    std::uint8_t pending_size_ = 0;
    std::uint8_t pending_at_ = 0;
};

[[nodiscard]] std::string_view string_body(std::string_view text, const Token& token) {
    return text.substr(token.offset + 1, token.length - 2);
}

[[nodiscard]] bool same_string(std::string_view text, const Token& a, const Token& b) {
    const std::string_view body_a = string_body(text, a);
    const std::string_view body_b = string_body(text, b);
    if (((a.flags | b.flags) & kFlagEscaped) == 0) {
        return body_a == body_b;
    }
    DecodedBytes left(body_a);
    DecodedBytes right(body_b);
    while (true) {
        const int l = left.next();
        if (l != right.next()) {
            return false;
        }
        if (l < 0) {
            return true;
        }
    }
}

[[nodiscard]] bool has_duplicate_keys(std::string_view text, std::span<const Token> tokens,
                                      std::uint32_t object) {
    const std::uint32_t end = tokens[object].next;
    for (std::uint32_t key = object + 1; key < end; key = tokens[key + 1].next) {
        for (std::uint32_t earlier = object + 1; earlier < key;
             earlier = tokens[earlier + 1].next) {
            if (same_string(text, tokens[earlier], tokens[key])) {
                return true;
            }
        }
    }
    return false;
}

constexpr std::array<double, 23> kPowersOfTen{
    1e0,  1e1,  1e2,  1e3,  1e4,  1e5,  1e6,  1e7,  1e8,  1e9,  1e10, 1e11,
    1e12, 1e13, 1e14, 1e15, 1e16, 1e17, 1e18, 1e19, 1e20, 1e21, 1e22,
};

// A validated JSON number to double. Exact on Clinger's fast path (a significand
// of at most 2^53 and a decimal exponent within 22), approximate outside it.
[[nodiscard]] std::optional<double> to_double(std::string_view number) {
    std::size_t i = 0;
    const bool negative = number[i] == '-';
    if (negative) {
        ++i;
    }
    std::uint64_t significand = 0;
    int kept = 0;
    long exponent = 0;
    const auto take = [&](int digit, bool fraction) {
        if (significand == 0 && digit == 0) {
            if (fraction) {
                --exponent;
            }
            return;
        }
        if (kept < 19) {
            significand = significand * 10U + static_cast<std::uint64_t>(digit);
            ++kept;
            if (fraction) {
                --exponent;
            }
        } else if (!fraction) {
            ++exponent;
        }
    };
    for (; i < number.size() && is_digit(number[i]); ++i) {
        take(number[i] - '0', false);
    }
    if (i < number.size() && number[i] == '.') {
        for (++i; i < number.size() && is_digit(number[i]); ++i) {
            take(number[i] - '0', true);
        }
    }
    if (i < number.size() && (number[i] == 'e' || number[i] == 'E')) {
        ++i;
        const bool exponent_negative = number[i] == '-';
        if (number[i] == '+' || number[i] == '-') {
            ++i;
        }
        long written = 0;
        for (; i < number.size() && is_digit(number[i]); ++i) {
            if (written < 100000) {
                written = written * 10 + (number[i] - '0');
            }
        }
        exponent += exponent_negative ? -written : written;
    }
    if (significand == 0) {
        return negative ? -0.0 : 0.0;
    }
    auto value = static_cast<double>(significand);
    if (significand <= (std::uint64_t{1} << 53U) && exponent >= -22 && exponent <= 22) {
        value = exponent >= 0 ? value * kPowersOfTen[static_cast<std::size_t>(exponent)]
                              : value / kPowersOfTen[static_cast<std::size_t>(-exponent)];
    } else {
        while (exponent > 22 && std::isfinite(value)) {
            value *= kPowersOfTen[22];
            exponent -= 22;
        }
        while (exponent < -22 && value != 0.0) {
            value /= kPowersOfTen[22];
            exponent += 22;
        }
        if (std::isfinite(value) && value != 0.0) {
            value = exponent >= 0 ? value * kPowersOfTen[static_cast<std::size_t>(exponent)]
                                  : value / kPowersOfTen[static_cast<std::size_t>(-exponent)];
        }
    }
    if (!std::isfinite(value)) {
        return std::nullopt;
    }
    return negative ? -value : value;
}

}  // namespace

std::string_view describe(Error error) {
    switch (error) {
        case Error::kNone:
            return "no error";
        case Error::kEmpty:
            return "no JSON value in the text";
        case Error::kSyntax:
            return "unexpected character";
        case Error::kTruncated:
            return "the text ends inside a value";
        case Error::kTrailing:
            return "text after the JSON value";
        case Error::kDepth:
            return "arrays and objects nested too deeply";
        case Error::kTooManyTokens:
            return "more values than the token storage holds";
        case Error::kTooManyMembers:
            return "an object with too many members";
        case Error::kTooLarge:
            return "the text is 4 GiB or more";
        case Error::kBadNumber:
            return "a malformed number";
        case Error::kBadString:
            return "a malformed string";
        case Error::kDuplicateKey:
            return "an object names the same key twice";
    }
    return "unknown error";
}

ParseResult Document::parse(std::string_view text, std::span<Token> tokens, const Limits& limits) {
    text_ = {};
    tokens_ = nullptr;
    count_ = 0;
    if (text.size() >= std::numeric_limits<std::uint32_t>::max()) {
        return {Error::kTooLarge, 0};
    }
    const std::size_t max_depth = std::min(limits.max_depth, kMaxDepth);

    enum class Expect : std::uint8_t {
        kValue,
        kValueOrClose,
        kKeyOrClose,
        kKey,
        kColon,
        kCommaOrClose,
        kEnd,
    };

    std::array<std::uint32_t, kMaxDepth> open{};
    std::array<std::size_t, kMaxDepth> members{};
    std::size_t depth = 0;
    std::uint32_t count = 0;
    std::size_t pos = 0;
    Expect expect = Expect::kValue;

    const auto push = [&](Type type, std::size_t offset, std::size_t end, std::uint8_t flags) {
        if (count >= tokens.size()) {
            return false;
        }
        tokens[count] = Token{.offset = static_cast<std::uint32_t>(offset),
                              .length = static_cast<std::uint32_t>(end - offset),
                              .next = count + 1,
                              .type = type,
                              .flags = flags,
                              .reserved = 0};
        ++count;
        return true;
    };
    const auto after_value = [&] { expect = depth == 0 ? Expect::kEnd : Expect::kCommaOrClose; };

    while (true) {
        while (pos < text.size() && is_space(text[pos])) {
            ++pos;
        }
        if (pos >= text.size()) {
            if (expect == Expect::kEnd) {
                break;
            }
            return {count == 0 ? Error::kEmpty : Error::kTruncated, text.size()};
        }
        const char c = text[pos];
        bool closing = false;
        switch (expect) {
            case Expect::kEnd:
                return {Error::kTrailing, pos};
            case Expect::kColon:
                if (c != ':') {
                    return {Error::kSyntax, pos};
                }
                ++pos;
                expect = Expect::kValue;
                continue;
            case Expect::kCommaOrClose: {
                const Type container = tokens[open[depth - 1]].type;
                if (c == ',') {
                    ++pos;
                    expect = container == Type::kObject ? Expect::kKey : Expect::kValue;
                    continue;
                }
                const bool matches = (c == ']' && container == Type::kArray) ||
                                     (c == '}' && container == Type::kObject);
                if (!matches) {
                    return {Error::kSyntax, pos};
                }
                closing = true;
                break;
            }
            case Expect::kKeyOrClose:
            case Expect::kKey: {
                if (c == '}' && expect == Expect::kKeyOrClose) {
                    closing = true;
                    break;
                }
                if (c != '"') {
                    return {Error::kSyntax, pos};
                }
                if (members[depth - 1] >= limits.max_members) {
                    return {Error::kTooManyMembers, pos};
                }
                ++members[depth - 1];
                const Scan scan = scan_string(text, pos);
                if (scan.error != Error::kNone) {
                    return {scan.error, scan.end};
                }
                if (!push(Type::kString, pos, scan.end, scan.flags)) {
                    return {Error::kTooManyTokens, pos};
                }
                pos = scan.end;
                expect = Expect::kColon;
                continue;
            }
            case Expect::kValue:
            case Expect::kValueOrClose: {
                if (c == ']' && expect == Expect::kValueOrClose) {
                    closing = true;
                    break;
                }
                if (c == '{' || c == '[') {
                    if (depth >= max_depth) {
                        return {Error::kDepth, pos};
                    }
                    const Type type = c == '{' ? Type::kObject : Type::kArray;
                    if (!push(type, pos, pos + 1, 0)) {
                        return {Error::kTooManyTokens, pos};
                    }
                    open[depth] = count - 1;
                    members[depth] = 0;
                    ++depth;
                    ++pos;
                    expect = type == Type::kObject ? Expect::kKeyOrClose : Expect::kValueOrClose;
                    continue;
                }
                Scan scan;
                Type type = Type::kNull;
                if (c == '"') {
                    scan = scan_string(text, pos);
                    type = Type::kString;
                } else if (c == '-' || is_digit(c)) {
                    scan = scan_number(text, pos);
                    type = Type::kNumber;
                } else if (c == 't' || c == 'f' || c == 'n') {
                    scan = scan_literal(text, pos);
                    type = c == 'n' ? Type::kNull : Type::kBool;
                } else {
                    return {Error::kSyntax, pos};
                }
                if (scan.error != Error::kNone) {
                    return {scan.error, scan.end};
                }
                if (!push(type, pos, scan.end, scan.flags)) {
                    return {Error::kTooManyTokens, pos};
                }
                pos = scan.end;
                after_value();
                continue;
            }
        }
        if (!closing) {
            return {Error::kSyntax, pos};
        }
        const std::uint32_t index = open[depth - 1];
        Token& container = tokens[index];
        container.length = static_cast<std::uint32_t>(pos + 1 - container.offset);
        container.next = count;
        if (container.type == Type::kObject &&
            has_duplicate_keys(text, std::span<const Token>(tokens.data(), count), index)) {
            return {Error::kDuplicateKey, pos};
        }
        --depth;
        ++pos;
        after_value();
    }

    text_ = text;
    tokens_ = tokens.data();
    count_ = count;
    return {Error::kNone, text.size()};
}

ParseResult Document::parse(std::string_view text, std::vector<Token>& storage,
                            std::size_t max_tokens, const Limits& limits) {
    storage.resize(std::min(max_tokens, (text.size() / 2) + 1));
    return parse(text, std::span<Token>(storage), limits);
}

const Token* Value::token() const { return doc_ == nullptr ? nullptr : &doc_->tokens_[index_]; }

std::optional<Type> Value::type() const {
    const Token* t = token();
    if (t == nullptr) {
        return std::nullopt;
    }
    return t->type;
}

std::optional<bool> Value::as_bool() const {
    const Token* t = token();
    if (t == nullptr || t->type != Type::kBool) {
        return std::nullopt;
    }
    return (t->flags & kFlagTrue) != 0;
}

std::optional<std::int64_t> Value::as_int() const {
    const Token* t = token();
    if (t == nullptr || t->type != Type::kNumber || (t->flags & kFlagInteger) == 0) {
        return std::nullopt;
    }
    std::string_view digits = source();
    const bool negative = digits.front() == '-';
    if (negative) {
        digits.remove_prefix(1);
    }
    std::uint64_t magnitude = 0;
    const auto [end, error] =
        std::from_chars(digits.data(), digits.data() + digits.size(), magnitude);
    if (error != std::errc{} || end != digits.data() + digits.size()) {
        return std::nullopt;
    }
    const auto limit = static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max());
    if (negative) {
        if (magnitude > limit + 1U) {
            return std::nullopt;
        }
        return magnitude == limit + 1U ? std::numeric_limits<std::int64_t>::min()
                                       : -static_cast<std::int64_t>(magnitude);
    }
    if (magnitude > limit) {
        return std::nullopt;
    }
    return static_cast<std::int64_t>(magnitude);
}

std::optional<double> Value::as_double() const {
    const Token* t = token();
    if (t == nullptr || t->type != Type::kNumber) {
        return std::nullopt;
    }
    return to_double(source());
}

std::optional<std::string> Value::as_string() const {
    std::string out;
    if (!decode_to(out)) {
        return std::nullopt;
    }
    return out;
}

bool Value::equals(std::string_view text) const {
    const Token* t = token();
    if (t == nullptr || t->type != Type::kString) {
        return false;
    }
    const std::string_view body = string_body(doc_->text_, *t);
    if ((t->flags & kFlagEscaped) == 0) {
        return body == text;
    }
    DecodedBytes decoded(body);
    for (const char expected : text) {
        if (decoded.next() != static_cast<unsigned char>(expected)) {
            return false;
        }
    }
    return decoded.next() < 0;
}

std::optional<std::string_view> Value::raw() const {
    const Token* t = token();
    if (t == nullptr || t->type != Type::kString || (t->flags & kFlagEscaped) != 0) {
        return std::nullopt;
    }
    return string_body(doc_->text_, *t);
}

bool Value::decode_to(std::string& out) const {
    const Token* t = token();
    if (t == nullptr || t->type != Type::kString) {
        return false;
    }
    const std::string_view body = string_body(doc_->text_, *t);
    if ((t->flags & kFlagEscaped) == 0) {
        out.append(body);
        return true;
    }
    DecodedBytes decoded(body);
    for (int byte = decoded.next(); byte >= 0; byte = decoded.next()) {
        out.push_back(static_cast<char>(byte));
    }
    return true;
}

std::size_t Value::size() const {
    const Token* t = token();
    if (t == nullptr) {
        return 0;
    }
    std::size_t n = 0;
    if (t->type == Type::kArray) {
        for ([[maybe_unused]] const Value element : elements()) {
            ++n;
        }
    } else if (t->type == Type::kObject) {
        for ([[maybe_unused]] const Member member : members()) {
            ++n;
        }
    }
    return n;
}

Value Value::operator[](std::string_view key) const {
    for (const Member member : members()) {
        if (member.key.equals(key)) {
            return member.value;
        }
    }
    return {};
}

Value Value::at(std::size_t index) const {
    std::size_t i = 0;
    for (const Value element : elements()) {
        if (i == index) {
            return element;
        }
        ++i;
    }
    return {};
}

Range<ElementIterator> Value::elements() const {
    const Token* t = token();
    if (t == nullptr || t->type != Type::kArray) {
        return {};
    }
    return {ElementIterator(doc_, index_ + 1), ElementIterator(doc_, t->next)};
}

Range<MemberIterator> Value::members() const {
    const Token* t = token();
    if (t == nullptr || t->type != Type::kObject) {
        return {};
    }
    return {MemberIterator(doc_, index_ + 1), MemberIterator(doc_, t->next)};
}

std::string_view Value::source() const {
    const Token* t = token();
    if (t == nullptr) {
        return {};
    }
    return doc_->text_.substr(t->offset, t->length);
}

Value ElementIterator::operator*() const { return Value(doc_, index_); }

ElementIterator& ElementIterator::operator++() {
    index_ = doc_->tokens_[index_].next;
    return *this;
}

Member MemberIterator::operator*() const {
    return Member{.key = Value(doc_, index_), .value = Value(doc_, index_ + 1)};
}

MemberIterator& MemberIterator::operator++() {
    index_ = doc_->tokens_[index_ + 1].next;
    return *this;
}

// --- Writer -----------------------------------------------------------------

void Writer::before_value() {
    if (after_key_) {
        after_key_ = false;
        return;
    }
    if (depth_ == 0) {
        wrote_root_ = true;
        return;
    }
    const std::uint64_t bit = std::uint64_t{1} << (depth_ - 1U);
    if ((empty_bits_ & bit) != 0) {
        empty_bits_ &= ~bit;
    } else {
        out_->push_back(',');
    }
}

void Writer::open(char bracket, [[maybe_unused]] bool object) {
    before_value();
    out_->push_back(bracket);
    if (depth_ < kMaxDepth) {
        empty_bits_ |= std::uint64_t{1} << depth_;
        ++depth_;
    }
}

void Writer::close(char bracket) {
    out_->push_back(bracket);
    if (depth_ > 0) {
        --depth_;
    }
}

Writer& Writer::begin_object() {
    open('{', true);
    return *this;
}

Writer& Writer::end_object() {
    close('}');
    return *this;
}

Writer& Writer::begin_array() {
    open('[', false);
    return *this;
}

Writer& Writer::end_array() {
    close(']');
    return *this;
}

Writer& Writer::key(std::string_view name) {
    before_value();
    escape(name);
    out_->push_back(':');
    after_key_ = true;
    return *this;
}

Writer& Writer::null() {
    before_value();
    out_->append("null");
    return *this;
}

Writer& Writer::boolean(bool value) {
    before_value();
    out_->append(value ? "true" : "false");
    return *this;
}

Writer& Writer::integer(std::int64_t value) {
    before_value();
    std::array<char, 24> buffer{};
    const auto result = std::to_chars(buffer.data(), buffer.data() + buffer.size(), value);
    out_->append(buffer.data(), result.ptr);
    return *this;
}

Writer& Writer::unsigned_integer(std::uint64_t value) {
    before_value();
    std::array<char, 24> buffer{};
    const auto result = std::to_chars(buffer.data(), buffer.data() + buffer.size(), value);
    out_->append(buffer.data(), result.ptr);
    return *this;
}

Writer& Writer::number(double value, int decimals) {
    static constexpr std::array<std::int64_t, 10> kScale{
        1, 10, 100, 1'000, 10'000, 100'000, 1'000'000, 10'000'000, 100'000'000, 1'000'000'000};
    const auto places = static_cast<std::size_t>(std::clamp(decimals, 0, 9));
    const double scaled = value * static_cast<double>(kScale[places]);
    // 2^53: past it llround's result is no longer the nearest integer.
    if (!std::isfinite(scaled) || std::fabs(scaled) >= 9007199254740992.0) {
        return null();
    }
    std::int64_t units = std::llround(scaled);
    before_value();
    if (units < 0) {
        out_->push_back('-');
        units = -units;
    }
    std::array<char, 24> buffer{};
    const auto whole = std::to_chars(buffer.data(), buffer.data() + buffer.size(),
                                     units / kScale[places]);
    out_->append(buffer.data(), whole.ptr);
    std::int64_t fraction = units % kScale[places];
    if (fraction == 0) {
        return *this;
    }
    std::size_t width = places;
    while (fraction % 10 == 0) {
        fraction /= 10;
        --width;
    }
    out_->push_back('.');
    const auto digits = std::to_chars(buffer.data(), buffer.data() + buffer.size(), fraction);
    const auto written = static_cast<std::size_t>(digits.ptr - buffer.data());
    out_->append(width - written, '0');
    out_->append(buffer.data(), digits.ptr);
    return *this;
}

Writer& Writer::string(std::string_view value) {
    before_value();
    escape(value);
    return *this;
}

void Writer::escape(std::string_view value) {
    static constexpr std::string_view kHex = "0123456789abcdef";
    out_->push_back('"');
    std::size_t i = 0;
    while (i < value.size()) {
        const unsigned char c = byte_at(value, i);
        if (c >= 0x80) {
            const std::size_t length = utf8_sequence(value, i);
            if (length == 0) {
                out_->append("\xEF\xBF\xBD");
                ++i;
            } else {
                out_->append(value.substr(i, length));
                i += length;
            }
            continue;
        }
        switch (c) {
            case '"':
                out_->append("\\\"");
                break;
            case '\\':
                out_->append("\\\\");
                break;
            case '\b':
                out_->append("\\b");
                break;
            case '\f':
                out_->append("\\f");
                break;
            case '\n':
                out_->append("\\n");
                break;
            case '\r':
                out_->append("\\r");
                break;
            case '\t':
                out_->append("\\t");
                break;
            default:
                if (c < 0x20) {
                    out_->append("\\u00");
                    out_->push_back(kHex[c >> 4U]);
                    out_->push_back(kHex[c & 0x0FU]);
                } else {
                    out_->push_back(static_cast<char>(c));
                }
        }
        ++i;
    }
    out_->push_back('"');
}

}  // namespace ac3::sendspin::json
