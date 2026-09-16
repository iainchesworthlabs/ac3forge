#pragma once

#include <cstdint>
#include <string_view>

// Where a JSON document written token by token goes, for the writers two
// applications share: ac3cli's probe streams its document to stdout through
// its own writer (apps/cli/json.hpp), and Hearth's media information builds a
// string with ac3::sendspin's. probe_json.hpp's functions write to either
// through this.
//
// The calls are ac3cli's JsonWriter's, which came first. Commas, colons and
// layout are the implementation's business; the caller keeps keys and values
// in order and closes what it opens.

namespace ac3::apps {

class JsonSink {
public:
    JsonSink() = default;
    JsonSink(const JsonSink&) = delete;
    JsonSink& operator=(const JsonSink&) = delete;
    JsonSink(JsonSink&&) = delete;
    JsonSink& operator=(JsonSink&&) = delete;
    virtual ~JsonSink() = default;

    virtual void begin_object() = 0;
    virtual void end_object() = 0;
    virtual void begin_array() = 0;
    virtual void end_array() = 0;

    // A member name. The next value written belongs to it.
    virtual void key(std::string_view name) = 0;

    virtual void value(std::string_view text) = 0;
    // A string literal is a const char*, whose conversion to bool outranks the
    // one to std::string_view, so without this overload value("text") would
    // write true.
    virtual void value(const char* text) = 0;
    virtual void value(bool flag) = 0;
    virtual void value(std::int64_t number) = 0;
    virtual void value(std::uint64_t number) = 0;
    // `decimals` places after the point. A value that is not finite is
    // written as null, since JSON has no way to write one.
    virtual void value(double number, int decimals) = 0;
    virtual void value_null() = 0;

    void member(std::string_view name, std::string_view text) {
        key(name);
        value(text);
    }
    void member(std::string_view name, const char* text) {
        key(name);
        value(text);
    }
    void member(std::string_view name, bool flag) {
        key(name);
        value(flag);
    }
    void member(std::string_view name, std::int64_t number) {
        key(name);
        value(number);
    }
    void member(std::string_view name, std::uint64_t number) {
        key(name);
        value(number);
    }
    void member(std::string_view name, double number, int decimals) {
        key(name);
        value(number, decimals);
    }
    void member_null(std::string_view name) {
        key(name);
        value_null();
    }
};

}  // namespace ac3::apps
