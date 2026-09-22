#pragma once

#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <string_view>
#include <vector>

// A streaming JSON writer, for `ac3cli probe`'s machine-readable form.
//
// Deliberately not a document model. probe's whole posture is that a stream of
// any length costs the same to inspect (see ac3/io/probe.hpp), and a writer
// that built a tree of values before printing it would put that back: a
// per-frame dump of an hour of audio is millions of numbers, and none of them
// needs to be in memory at the same time as any other. So this writes each
// token as it is given one and keeps exactly one thing: the stack of
// still-open containers, which is what decides indentation and where the
// separating commas go.
//
// It is also not a general-purpose library. There is no reader, no value type
// and no schema validation, because the only consumer is one command emitting
// one documented shape - see docs/cli/commands.md for that shape, which is the
// contract, rather than anything here.

namespace ac3cli {

class JsonWriter {
   public:
    explicit JsonWriter(std::FILE* out) : out_(out) {}

    // Containers. Every begin_ must be matched by its own end_; the
    // destructor does not close them, because a writer abandoned part-way
    // through has produced invalid output either way and silently completing
    // the braces would hide that from whatever is parsing it.
    void begin_object();
    void end_object();
    void begin_array();
    void end_array();

    // A member name. The next value written belongs to it.
    void key(std::string_view name);

    void value(std::string_view text);
    // A string LITERAL is a const char*, and C++ ranks its conversion to bool
    // (a standard pointer-to-bool conversion) above the user-defined one to
    // std::string_view - so without this overload `value("text")` silently
    // writes `true`. It did, once, and the document still parsed.
    void value(const char* text);
    void value(bool flag);
    void value(std::int64_t number);
    void value(std::uint64_t number);
    // Written with `decimals` places, fixed - never scientific notation and
    // never a bare integer, so a consumer reading the field as a float always
    // sees one. A non-finite value is written as null: JSON has no
    // representation for infinity or NaN, and emitting the literal text would
    // produce a document nothing can parse.
    void value(double number, int decimals);
    void value_null();

    // The common shapes, so a caller writing thirty members is not writing
    // sixty calls.
    void member(std::string_view name, std::string_view text);
    void member(std::string_view name, const char* text);  // see value() above
    void member(std::string_view name, bool flag);
    void member(std::string_view name, std::int64_t number);
    void member(std::string_view name, std::uint64_t number);
    void member(std::string_view name, double number, int decimals);
    void member_null(std::string_view name);

    // Finish the document: a trailing newline, so the output is a well-formed
    // line for anything reading it a line at a time.
    void finish();

   private:
    void separate();
    void indent();

    std::FILE* out_;
    // One entry per open container: true while it is still empty, which is
    // what decides whether the next value needs a comma before it.
    std::vector<bool> empty_;
    bool after_key_ = false;
};

}  // namespace ac3cli
