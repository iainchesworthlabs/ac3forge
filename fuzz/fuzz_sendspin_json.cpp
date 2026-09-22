#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <optional>
#include <string>
#include <string_view>

#include "ac3/sendspin/json.hpp"

// ac3::sendspin::json::Document::parse (src/sendspin/src/json.cpp) - the first
// code a Sendspin peer's bytes reach: client/init and server/init before any
// authentication, and every decrypted JSON message after it, on the server and on
// a sink.
//
// Beyond not crashing, two properties are checked. Every accessor is called on
// every value the parse produced, so a token whose offsets or skip index are wrong
// is read. And a parsed document is written back out and parsed again: the
// writer promises valid JSON for any input, and writing the re-parsed document
// must give the same text, so reader and writer agree on every string and number.

namespace {

using ac3::sendspin::json::Document;
using ac3::sendspin::json::Error;
using ac3::sendspin::json::Limits;
using ac3::sendspin::json::Token;
using ac3::sendspin::json::Type;
using ac3::sendspin::json::Value;
using ac3::sendspin::json::Writer;

void write_value(Value value, Writer& out) {
    const std::optional<Type> type = value.type();
    if (!type) {
        std::abort();
    }
    switch (*type) {
        case Type::kNull:
            out.null();
            return;
        case Type::kBool:
            out.boolean(*value.as_bool());
            return;
        case Type::kNumber:
            if (const std::optional<std::int64_t> integer = value.as_int()) {
                out.integer(*integer);
            } else if (const std::optional<double> real = value.as_double();
                       real && std::fabs(*real) < 1e9) {
                // Below 1e9, six decimals scale to well under 2^53 units, so the
                // re-read value rounds back to the same text.
                out.number(*real, 6);
            } else {
                out.null();
            }
            return;
        case Type::kString: {
            const std::optional<std::string> text = value.as_string();
            if (!text || !value.equals(*text)) {
                std::abort();
            }
            out.string(*text);
            return;
        }
        case Type::kArray:
            out.begin_array();
            for (const Value element : value.elements()) {
                write_value(element, out);
            }
            out.end_array();
            return;
        case Type::kObject: {
            out.begin_object();
            std::size_t members = 0;
            for (const auto member : value.members()) {
                const std::optional<std::string> key = member.key.as_string();
                if (!key || !member.key.is_string()) {
                    std::abort();
                }
                // The reader refuses duplicates, so a lookup by name finds this member.
                if (value[*key].source().data() != member.value.source().data()) {
                    std::abort();
                }
                out.key(*key);
                write_value(member.value, out);
                ++members;
            }
            if (members != value.size()) {
                std::abort();
            }
            out.end_object();
            return;
        }
    }
}

std::optional<std::string> canonical(std::string_view text, std::array<Token, 1024>& tokens) {
    Document doc;
    if (!doc.parse(text, tokens, Limits{.max_depth = 32, .max_members = 256})) {
        return std::nullopt;
    }
    std::string out;
    Writer writer(out);
    write_value(doc.root(), writer);
    if (!writer.complete()) {
        std::abort();
    }
    return out;
}

}  // namespace

extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t* data, std::size_t size) {
    const std::string_view text{reinterpret_cast<const char*>(data), size};
    static std::array<Token, 1024> tokens{};

    const std::optional<std::string> first = canonical(text, tokens);
    if (!first) {
        return 0;
    }
    const std::optional<std::string> second = canonical(*first, tokens);
    // What the writer produced must parse. A text that needed more tokens than
    // the input did would be a writer bug too, since it adds no values.
    if (!second || *second != *first) {
        std::abort();
    }
    return 0;
}
