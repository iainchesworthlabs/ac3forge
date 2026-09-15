#pragma once

#include <cstddef>
#include <cstdint>
#include <iterator>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

// RFC 8259 JSON for Sendspin's messages, read and written in-tree.
//
// The tree's other JSON reader is private to ac3::oba (scene_json.cpp) and
// allocates a node per value. This one is shaped by where Sendspin's player half
// runs: on an ESP32 inside an HTTP server task with a 6,144-byte stack and a
// regioned heap. So the reader
//
//   - parses into caller-owned token storage (one 16-byte Token per value, key
//     or container) and allocates nothing itself; a Document that needs more
//     tokens than it was given fails with kTooManyTokens rather than growing;
//   - never recurses: nesting is tracked in a fixed array sized by the depth
//     limit, so a hostile "[[[[..." costs no stack;
//   - leaves strings in the text and decodes them on demand, comparing a key
//     against a name without copying it.
//
// It is strict where leniency would let two readers disagree about one message:
// invalid UTF-8, unescaped control characters, trailing commas, leading zeros,
// duplicate keys in one object and anything after the top-level value are all
// errors. An unpaired UTF-16 surrogate in a \u escape decodes to U+FFFD, since
// no UTF-8 can carry it and rejecting a whole message over a damaged track title
// would be worse.
//
// Numbers: integers read exactly into int64. A number with a fraction or
// exponent reads into double exactly when its significand fits in 53 bits and
// its decimal exponent is within 22 (every value Sendspin sends), and
// approximately otherwise: each step of 22 decimal places beyond that can cost
// one unit in the last place. No std::from_chars for double (absent from some
// libc++ builds this project targets) and no strtod (it follows the process
// locale, which Qt sets from the user's).
//
// The writer appends to a std::string, escaping as it goes, and writes doubles
// with a fixed number of decimals for the same locale reason.

namespace ac3::sendspin::json {

enum class Type : std::uint8_t {
    kNull,
    kBool,
    kNumber,
    kString,
    kArray,
    kObject,
};

enum class Error : std::uint8_t {
    kNone,
    kEmpty,          // no value in the text
    kSyntax,         // a character that cannot appear where it does
    kTruncated,      // the text ended inside a value
    kTrailing,       // something other than whitespace after the top-level value
    kDepth,          // arrays and objects nested deeper than the limit
    kTooManyTokens,  // more values than the token storage holds
    kTooManyMembers, // an object with more members than the limit
    kTooLarge,       // text of 4 GiB or more: token offsets are 32-bit
    kBadNumber,      // a number outside the JSON grammar
    kBadString,      // a control character, a bad escape or invalid UTF-8
    kDuplicateKey,   // one object naming the same key twice
};

[[nodiscard]] std::string_view describe(Error error);

struct Token {
    // Byte offset of the token's first character: the quote of a string, the
    // bracket of a container.
    std::uint32_t offset = 0;
    // Bytes the token spans, quotes and brackets included.
    std::uint32_t length = 0;
    // Index of the first token after this one and everything nested in it.
    std::uint32_t next = 0;
    Type type = Type::kNull;
    std::uint8_t flags = 0;
    std::uint16_t reserved = 0;
};

inline constexpr std::uint8_t kFlagTrue = 1;      // a kBool that is true
inline constexpr std::uint8_t kFlagEscaped = 2;   // a kString with at least one backslash
inline constexpr std::uint8_t kFlagInteger = 4;   // a kNumber with no fraction or exponent

struct Limits {
    // Arrays and objects open at once. Capped at kMaxDepth whatever is asked.
    std::size_t max_depth = 32;
    // Members of any one object; bounds the duplicate-key check, which is
    // quadratic in it.
    std::size_t max_members = 1024;
};

inline constexpr std::size_t kMaxDepth = 64;

struct ParseResult {
    Error error = Error::kNone;
    // Byte offset of the character that failed, or the text's length.
    std::size_t offset = 0;
    [[nodiscard]] explicit operator bool() const { return error == Error::kNone; }
};

class Document;
class Value;

struct Member;

class ElementIterator {
   public:
    using iterator_category = std::forward_iterator_tag;
    using value_type = Value;
    using difference_type = std::ptrdiff_t;

    ElementIterator() = default;
    ElementIterator(const Document* doc, std::uint32_t index) : doc_(doc), index_(index) {}

    [[nodiscard]] Value operator*() const;
    ElementIterator& operator++();
    ElementIterator operator++(int) {
        ElementIterator before = *this;
        ++*this;
        return before;
    }
    [[nodiscard]] bool operator==(const ElementIterator& other) const {
        return index_ == other.index_;
    }

   private:
    const Document* doc_ = nullptr;
    std::uint32_t index_ = 0;
};

class MemberIterator {
   public:
    using iterator_category = std::forward_iterator_tag;
    using value_type = Member;
    using difference_type = std::ptrdiff_t;

    MemberIterator() = default;
    MemberIterator(const Document* doc, std::uint32_t index) : doc_(doc), index_(index) {}

    [[nodiscard]] Member operator*() const;
    MemberIterator& operator++();
    MemberIterator operator++(int) {
        MemberIterator before = *this;
        ++*this;
        return before;
    }
    [[nodiscard]] bool operator==(const MemberIterator& other) const {
        return index_ == other.index_;
    }

   private:
    const Document* doc_ = nullptr;
    std::uint32_t index_ = 0;
};

template <class Iterator>
struct Range {
    Iterator first;
    Iterator last;
    [[nodiscard]] Iterator begin() const { return first; }
    [[nodiscard]] Iterator end() const { return last; }
};

// A value inside a Document, or no value at all: a missing member, an index past
// the end, or a lookup on something that is not a container. Every accessor on
// an absent Value returns nothing, so a chain like doc.root()["a"]["b"] needs no
// check until the end.
class Value {
   public:
    Value() = default;

    [[nodiscard]] bool exists() const { return doc_ != nullptr; }
    [[nodiscard]] std::optional<Type> type() const;

    [[nodiscard]] bool is_null() const { return is(Type::kNull); }
    [[nodiscard]] bool is_bool() const { return is(Type::kBool); }
    [[nodiscard]] bool is_number() const { return is(Type::kNumber); }
    [[nodiscard]] bool is_string() const { return is(Type::kString); }
    [[nodiscard]] bool is_array() const { return is(Type::kArray); }
    [[nodiscard]] bool is_object() const { return is(Type::kObject); }

    [[nodiscard]] std::optional<bool> as_bool() const;
    // An integer written without fraction or exponent that fits in int64.
    [[nodiscard]] std::optional<std::int64_t> as_int() const;
    // Any number, as described in the header comment; nothing when the value
    // overflows a double.
    [[nodiscard]] std::optional<double> as_double() const;
    // The decoded string. Allocates; see equals() and raw() for the paths that
    // do not.
    [[nodiscard]] std::optional<std::string> as_string() const;
    // Whether this is a string whose decoded bytes are exactly `text`.
    [[nodiscard]] bool equals(std::string_view text) const;
    // The string's bytes as they sit in the text, when it has no escapes.
    [[nodiscard]] std::optional<std::string_view> raw() const;
    // Appends the decoded string to `out`; false when this is not a string.
    bool decode_to(std::string& out) const;

    // Elements of an array or members of an object; 0 for anything else.
    [[nodiscard]] std::size_t size() const;
    // The first member named `key` (the reader refuses duplicates, so the only).
    [[nodiscard]] Value operator[](std::string_view key) const;
    [[nodiscard]] Value at(std::size_t index) const;
    [[nodiscard]] Range<ElementIterator> elements() const;
    [[nodiscard]] Range<MemberIterator> members() const;

    // The value's JSON text exactly as it appeared.
    [[nodiscard]] std::string_view source() const;

   private:
    friend class Document;
    friend class ElementIterator;
    friend class MemberIterator;

    Value(const Document* doc, std::uint32_t index) : doc_(doc), index_(index) {}

    [[nodiscard]] bool is(Type t) const { return type() == t; }
    [[nodiscard]] const Token* token() const;

    const Document* doc_ = nullptr;
    std::uint32_t index_ = 0;
};

struct Member {
    Value key;
    Value value;
};

class Document {
   public:
    Document() = default;

    // Parses `text` into `tokens`. Both must outlive the Document and every
    // Value taken from it. On failure the Document holds nothing.
    ParseResult parse(std::string_view text, std::span<Token> tokens, const Limits& limits = {});

    // Sizes `storage` for `text` (a JSON text of n bytes holds at most n/2 + 1
    // values), capped at `max_tokens`, then parses into it.
    ParseResult parse(std::string_view text, std::vector<Token>& storage, std::size_t max_tokens,
                      const Limits& limits = {});

    [[nodiscard]] bool empty() const { return count_ == 0; }
    [[nodiscard]] Value root() const { return empty() ? Value{} : Value{this, 0}; }
    [[nodiscard]] std::size_t token_count() const { return count_; }

   private:
    friend class Value;
    friend class ElementIterator;
    friend class MemberIterator;

    std::string_view text_;
    const Token* tokens_ = nullptr;
    std::uint32_t count_ = 0;
};

// Appends one JSON text to a string. Commas and colons are placed for the
// caller; the caller keeps keys and values in order, which a debug check in each
// call verifies only as far as it cheaply can.
class Writer {
   public:
    explicit Writer(std::string& out) : out_(&out) {}

    Writer& begin_object();
    Writer& end_object();
    Writer& begin_array();
    Writer& end_array();

    Writer& key(std::string_view name);

    Writer& null();
    Writer& boolean(bool value);
    Writer& integer(std::int64_t value);
    Writer& unsigned_integer(std::uint64_t value);
    // `decimals` digits after the point (0 to 9), trailing zeros removed; a
    // value that is not finite, or too large to scale, is written as null.
    Writer& number(double value, int decimals = 3);
    // Escapes as JSON needs. Bytes that are not valid UTF-8 are written as
    // U+FFFD, so the output is always a valid JSON text.
    Writer& string(std::string_view value);

    // Convenience for the common member forms.
    Writer& member(std::string_view name, std::string_view value) { return key(name).string(value); }
    Writer& member(std::string_view name, const char* value) {
        return key(name).string(std::string_view{value});
    }
    Writer& member(std::string_view name, bool value) { return key(name).boolean(value); }
    Writer& member(std::string_view name, std::int64_t value) { return key(name).integer(value); }
    Writer& member(std::string_view name, int value) {
        return key(name).integer(static_cast<std::int64_t>(value));
    }

    // True once exactly one top-level value is complete.
    [[nodiscard]] bool complete() const { return depth_ == 0 && wrote_root_; }

   private:
    void before_value();
    void escape(std::string_view value);
    void open(char bracket, bool object);
    void close(char bracket);

    std::string* out_;
    // Bit d set: the container at depth d has not had its first element.
    std::uint64_t empty_bits_ = 0;
    std::uint8_t depth_ = 0;
    bool after_key_ = false;
    bool wrote_root_ = false;
};

}  // namespace ac3::sendspin::json
