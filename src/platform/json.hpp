#pragma once

// Minimal strict JSON for the platform layer (Phase 11).
//
// WHY THIS EXISTS: the control-plane contract is a JSON contract, but the
// project's dependency policy forbids pulling an HTTP/JSON/database stack
// into the C++ core. The platform layer therefore carries its own tiny,
// strictly-specified JSON reader/writer — standard library only. It is part
// of the ADAPTER boundary: the compute core (Runtime, VirtualGpu, Scheduler,
// TaskQueue, backends) has zero knowledge of it, and nothing outside
// src/platform includes it.
//
// SCOPE (deliberately small, fully specified):
//   - Value model: null / bool / number / string / array / object.
//   - Numbers are stored as IEEE-754 double. Every value the Phase 11
//     contract carries (element counts up to 2^31-1, epoch-millisecond
//     timestamps up to ~9e15, priorities) is exactly representable below
//     2^53 — this is a documented property of the contract, not an accident.
//   - Serialization: COMPACT, DETERMINISTIC. Object members are written in
//     insertion order (builders construct fields in the documented schema
//     order), so the same value always serializes to the same bytes —
//     pinned by tests. Strings are escaped per RFC 8259 (control chars as
//     \u00XX, plus \" \\ \n \r \t \b \f shorthand); valid UTF-8 passes
//     through unchanged.
//   - Parsing: strict RFC 8259 subset. REJECTED: trailing content, empty
//     input, leading zeros, bare +x / .5 / 1. / NaN / Infinity literals,
//     unescaped control characters in strings, invalid escapes, lone
//     surrogate halves, comments, trailing commas, nesting deeper than
//     kMaxJsonDepth (a stack-exhaustion guard — see docs/platform/security).
//
// NOT IMPLEMENTED (on purpose): streaming, number-preserving big ints,
// duplicate-key preservation (a duplicate object key REPLACES the earlier
// member — last one wins, like most JSON tools; the contract never relies
// on duplicate keys), pointer/query APIs, binary variants.

#include <cstdint>
#include <string>
#include <utility>
#include <vector>

namespace vortyx::platform {

class JsonValue {
public:
    enum class Type { Null, Bool, Number, String, Array, Object };

    // ---- construction ----------------------------------------------------
    JsonValue() = default;                       // null
    static JsonValue make_null();
    static JsonValue make_bool(bool value);
    static JsonValue make_number(double value);
    static JsonValue make_string(std::string value);
    static JsonValue make_array();
    static JsonValue make_object();

    // Appends to an array (no-op on non-arrays).
    void push(JsonValue value);
    // Sets 'key' on an object; a duplicate key REPLACES the earlier member
    // at its original position (last value wins). No-op on non-objects.
    void add(std::string key, JsonValue value);

    // ---- inspection ------------------------------------------------------
    Type type() const noexcept { return type_; }
    bool is_null() const noexcept { return type_ == Type::Null; }
    bool is_bool() const noexcept { return type_ == Type::Bool; }
    bool is_number() const noexcept { return type_ == Type::Number; }
    bool is_string() const noexcept { return type_ == Type::String; }
    bool is_array() const noexcept { return type_ == Type::Array; }
    bool is_object() const noexcept { return type_ == Type::Object; }

    // Typed accessors. Calling the wrong one returns the type's zero value
    // (callers validate with is_* first — the contract parsers do).
    bool as_bool() const noexcept { return bool_; }
    double as_number() const noexcept { return number_; }
    const std::string& as_string() const noexcept { return string_; }
    const std::vector<JsonValue>& items() const noexcept { return array_; }

    // Object member lookup; nullptr when absent or not an object.
    const JsonValue* find(const std::string& key) const;

    // Object members in insertion order (key + value pairs). Empty for
    // non-objects. Used by strict schema checks (unknown-field rejection).
    const std::vector<std::pair<std::string, JsonValue>>& members() const noexcept {
        return members_;
    }

    // ---- output ----------------------------------------------------------
    // Compact, deterministic serialization (see module documentation).
    std::string serialize() const;

private:
    // Compact serialization of THIS value into 'out' (serialize() delegates
    // here; keeps recursion allocation-free).
    void serialize_into(std::string& out) const;

    Type type_ = Type::Null;
    bool bool_ = false;
    double number_ = 0.0;
    std::string string_;
    std::vector<JsonValue> array_;
    // Object members in insertion order (key + value pairs).
    std::vector<std::pair<std::string, JsonValue>> members_;
};

// Maximum parser nesting depth (objects + arrays combined). Deep enough for
// any real contract payload; shallow enough that a hostile body cannot
// exhaust the stack. Part of the documented threat model.
inline constexpr int kMaxJsonDepth = 64;

// Parses 'text'. On success returns true and fills 'out'. On failure returns
// false and fills 'error' with a human-readable reason (including the byte
// offset where parsing stopped, when known).
bool parse_json(const std::string& text, JsonValue& out, std::string& error);

}  // namespace vortyx::platform
