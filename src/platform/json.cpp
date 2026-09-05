// Minimal strict JSON implementation (Phase 11).
//
// See json.hpp for the full specification. This file is deliberately
// dependency-free and self-contained; it is part of the platform adapter
// boundary and must never be included by the compute core.

#include "platform/json.hpp"

#include <cmath>
#include <cstdio>

namespace vortyx::platform {

// ---------------------------------------------------------------------------
// Construction
// ---------------------------------------------------------------------------

JsonValue JsonValue::make_null() { return JsonValue{}; }

JsonValue JsonValue::make_bool(bool value) {
    JsonValue v;
    v.type_ = Type::Bool;
    v.bool_ = value;
    return v;
}

JsonValue JsonValue::make_number(double value) {
    JsonValue v;
    v.type_ = Type::Number;
    v.number_ = value;
    return v;
}

JsonValue JsonValue::make_string(std::string value) {
    JsonValue v;
    v.type_ = Type::String;
    v.string_ = std::move(value);
    return v;
}

JsonValue JsonValue::make_array() {
    JsonValue v;
    v.type_ = Type::Array;
    return v;
}

JsonValue JsonValue::make_object() {
    JsonValue v;
    v.type_ = Type::Object;
    return v;
}

void JsonValue::push(JsonValue value) {
    if (type_ != Type::Array) return;
    array_.push_back(std::move(value));
}

void JsonValue::add(std::string key, JsonValue value) {
    if (type_ != Type::Object) return;
    for (auto& member : members_) {
        if (member.first == key) {
            member.second = std::move(value);
            return;
        }
    }
    members_.emplace_back(std::move(key), std::move(value));
}

const JsonValue* JsonValue::find(const std::string& key) const {
    if (type_ != Type::Object) return nullptr;
    for (const auto& member : members_) {
        if (member.first == key) return &member.second;
    }
    return nullptr;
}

// ---------------------------------------------------------------------------
// Serialization
// ---------------------------------------------------------------------------

namespace {

const char* const kHexDigits = "0123456789abcdef";

void append_escaped(std::string& out, const std::string& text) {
    out.push_back('"');
    for (const char c : text) {
        const unsigned char uc = static_cast<unsigned char>(c);
        switch (c) {
            case '"': out += "\\\""; break;
            case '\\': out += "\\\\"; break;
            case '\n': out += "\\n"; break;
            case '\r': out += "\\r"; break;
            case '\t': out += "\\t"; break;
            case '\b': out += "\\b"; break;
            case '\f': out += "\\f"; break;
            default:
                if (uc < 0x20) {
                    // Control characters must be escaped (RFC 8259).
                    out += "\\u00";
                    out.push_back(kHexDigits[(uc >> 4) & 0xF]);
                    out.push_back(kHexDigits[uc & 0xF]);
                } else {
                    out.push_back(c);  // valid UTF-8 passes through unchanged
                }
        }
    }
    out.push_back('"');
}

void append_number(std::string& out, double value) {
    // Deterministic number formatting:
    //   integral values with |v| < 2^53 -> plain integer notation
    //   everything else                 -> %.17g (round-trip-safe, stable)
    if (std::isfinite(value) && value == std::floor(value) &&
        std::fabs(value) < 9007199254740992.0) {
        out += std::to_string(static_cast<long long>(value));
        return;
    }
    char buf[64];
    std::snprintf(buf, sizeof(buf), "%.17g", value);
    out += buf;
}

}  // namespace

void JsonValue::serialize_into(std::string& out) const {
    switch (type_) {
        case Type::Null:
            out += "null";
            break;
        case Type::Bool:
            out += bool_ ? "true" : "false";
            break;
        case Type::Number:
            append_number(out, number_);
            break;
        case Type::String:
            append_escaped(out, string_);
            break;
        case Type::Array: {
            out.push_back('[');
            bool first = true;
            for (const JsonValue& item : array_) {
                if (!first) out.push_back(',');
                first = false;
                item.serialize_into(out);
            }
            out.push_back(']');
            break;
        }
        case Type::Object: {
            out.push_back('{');
            bool first = true;
            for (const auto& [key, value] : members_) {
                if (!first) out.push_back(',');
                first = false;
                append_escaped(out, key);
                out.push_back(':');
                value.serialize_into(out);
            }
            out.push_back('}');
            break;
        }
    }
}

std::string JsonValue::serialize() const {
    std::string out;
    out.reserve(64);
    serialize_into(out);
    return out;
}

// ---------------------------------------------------------------------------
// Parsing (strict RFC 8259 subset)
// ---------------------------------------------------------------------------

namespace {

class Parser {
public:
    Parser(const std::string& text, std::string& error)
        : text_(text), error_(error) {}

    bool parse(JsonValue& out) {
        skip_ws();
        if (!parse_value(out, 0)) return false;
        skip_ws();
        if (pos_ != text_.size()) {
            fail("unexpected content after the JSON value");
            return false;
        }
        return true;
    }

private:
    void fail(const std::string& message) {
        if (error_.empty()) {
            error_ = message + " (at byte " + std::to_string(pos_) + ")";
        }
    }

    void skip_ws() {
        while (pos_ < text_.size()) {
            const char c = text_[pos_];
            if (c == ' ' || c == '\t' || c == '\n' || c == '\r') {
                ++pos_;
            } else {
                break;
            }
        }
    }

    bool parse_value(JsonValue& out, int depth) {
        if (depth > kMaxJsonDepth) {
            fail("JSON nesting deeper than the allowed maximum");
            return false;
        }
        if (pos_ >= text_.size()) {
            fail("unexpected end of input");
            return false;
        }
        const char c = text_[pos_];
        switch (c) {
            case '{': return parse_object(out, depth);
            case '[': return parse_array(out, depth);
            case '"': {
                std::string value;
                if (!parse_string(value)) return false;
                out = JsonValue::make_string(std::move(value));
                return true;
            }
            case 't':
                return parse_literal("true", JsonValue::make_bool(true), out);
            case 'f':
                return parse_literal("false", JsonValue::make_bool(false), out);
            case 'n':
                return parse_literal("null", JsonValue::make_null(), out);
            default:
                if (c == '-' || (c >= '0' && c <= '9')) return parse_number(out);
                fail("unexpected character");
                return false;
        }
    }

    bool parse_literal(const char* literal, JsonValue value, JsonValue& out) {
        const std::size_t length = std::char_traits<char>::length(literal);
        if (text_.compare(pos_, length, literal) != 0) {
            fail("invalid literal");
            return false;
        }
        pos_ += length;
        out = std::move(value);
        return true;
    }

    bool parse_number(JsonValue& out) {
        const std::size_t start = pos_;
        if (pos_ < text_.size() && text_[pos_] == '-') ++pos_;

        // Integer part: 0 | [1-9][0-9]* (leading zeros rejected).
        if (pos_ >= text_.size() || text_[pos_] < '0' || text_[pos_] > '9') {
            fail("invalid number");
            return false;
        }
        if (text_[pos_] == '0') {
            ++pos_;
        } else {
            while (pos_ < text_.size() && text_[pos_] >= '0' && text_[pos_] <= '9') ++pos_;
        }
        // Fraction (no trailing dot).
        if (pos_ < text_.size() && text_[pos_] == '.') {
            ++pos_;
            const std::size_t digits_start = pos_;
            while (pos_ < text_.size() && text_[pos_] >= '0' && text_[pos_] <= '9') ++pos_;
            if (pos_ == digits_start) {
                fail("invalid number (fraction needs at least one digit)");
                return false;
            }
        }
        // Exponent (no bare sign).
        if (pos_ < text_.size() && (text_[pos_] == 'e' || text_[pos_] == 'E')) {
            ++pos_;
            if (pos_ < text_.size() && (text_[pos_] == '+' || text_[pos_] == '-')) ++pos_;
            const std::size_t digits_start = pos_;
            while (pos_ < text_.size() && text_[pos_] >= '0' && text_[pos_] <= '9') ++pos_;
            if (pos_ == digits_start) {
                fail("invalid number (exponent needs at least one digit)");
                return false;
            }
        }

        const std::string token = text_.substr(start, pos_ - start);
        char* end = nullptr;
        const double value = std::strtod(token.c_str(), &end);
        if (end != token.c_str() + token.size() || !std::isfinite(value)) {
            fail("invalid number");
            return false;
        }
        out = JsonValue::make_number(value);
        return true;
    }

    bool parse_hex4(unsigned& value) {
        value = 0;
        for (int i = 0; i < 4; ++i) {
            if (pos_ >= text_.size()) {
                fail("unterminated \\u escape");
                return false;
            }
            const char c = text_[pos_++];
            value <<= 4;
            if (c >= '0' && c <= '9') {
                value |= static_cast<unsigned>(c - '0');
            } else if (c >= 'a' && c <= 'f') {
                value |= static_cast<unsigned>(c - 'a' + 10);
            } else if (c >= 'A' && c <= 'F') {
                value |= static_cast<unsigned>(c - 'A' + 10);
            } else {
                fail("invalid hex digit in \\u escape");
                return false;
            }
        }
        return true;
    }

    static void append_utf8(std::string& out, unsigned codepoint) {
        if (codepoint <= 0x7F) {
            out.push_back(static_cast<char>(codepoint));
        } else if (codepoint <= 0x7FF) {
            out.push_back(static_cast<char>(0xC0 | (codepoint >> 6)));
            out.push_back(static_cast<char>(0x80 | (codepoint & 0x3F)));
        } else if (codepoint <= 0xFFFF) {
            out.push_back(static_cast<char>(0xE0 | (codepoint >> 12)));
            out.push_back(static_cast<char>(0x80 | ((codepoint >> 6) & 0x3F)));
            out.push_back(static_cast<char>(0x80 | (codepoint & 0x3F)));
        } else {
            out.push_back(static_cast<char>(0xF0 | (codepoint >> 18)));
            out.push_back(static_cast<char>(0x80 | ((codepoint >> 12) & 0x3F)));
            out.push_back(static_cast<char>(0x80 | ((codepoint >> 6) & 0x3F)));
            out.push_back(static_cast<char>(0x80 | (codepoint & 0x3F)));
        }
    }

    bool parse_string(std::string& out) {
        if (text_[pos_] != '"') {
            fail("expected a string");
            return false;
        }
        ++pos_;
        out.clear();
        while (true) {
            if (pos_ >= text_.size()) {
                fail("unterminated string");
                return false;
            }
            const char c = text_[pos_++];
            if (c == '"') return true;
            if (c == '\\') {
                if (pos_ >= text_.size()) {
                    fail("unterminated escape sequence");
                    return false;
                }
                const char esc = text_[pos_++];
                switch (esc) {
                    case '"': out.push_back('"'); break;
                    case '\\': out.push_back('\\'); break;
                    case '/': out.push_back('/'); break;
                    case 'n': out.push_back('\n'); break;
                    case 'r': out.push_back('\r'); break;
                    case 't': out.push_back('\t'); break;
                    case 'b': out.push_back('\b'); break;
                    case 'f': out.push_back('\f'); break;
                    case 'u': {
                        unsigned first = 0;
                        if (!parse_hex4(first)) return false;
                        if (first >= 0xD800 && first <= 0xDBFF) {
                            // High surrogate: a low surrogate MUST follow.
                            if (pos_ + 1 < text_.size() && text_[pos_] == '\\' &&
                                text_[pos_ + 1] == 'u') {
                                pos_ += 2;
                                unsigned second = 0;
                                if (!parse_hex4(second)) return false;
                                if (second >= 0xDC00 && second <= 0xDFFF) {
                                    const unsigned codepoint =
                                        0x10000 + ((first - 0xD800) << 10) + (second - 0xDC00);
                                    append_utf8(out, codepoint);
                                } else {
                                    fail("invalid surrogate pair");
                                    return false;
                                }
                            } else {
                                fail("lone high surrogate");
                                return false;
                            }
                        } else if (first >= 0xDC00 && first <= 0xDFFF) {
                            fail("lone low surrogate");
                            return false;
                        } else {
                            append_utf8(out, first);
                        }
                        break;
                    }
                    default:
                        fail("invalid escape sequence");
                        return false;
                }
                continue;
            }
            const unsigned char uc = static_cast<unsigned char>(c);
            if (uc < 0x20) {
                fail("unescaped control character in string");
                return false;
            }
            out.push_back(c);  // UTF-8 bytes pass through (validated downstream)
        }
    }

    bool parse_array(JsonValue& out, int depth) {
        ++pos_;  // consume '['
        JsonValue array = JsonValue::make_array();
        skip_ws();
        if (pos_ < text_.size() && text_[pos_] == ']') {
            ++pos_;
            out = std::move(array);
            return true;
        }
        while (true) {
            skip_ws();
            JsonValue item;
            if (!parse_value(item, depth + 1)) return false;
            array.push(std::move(item));
            skip_ws();
            if (pos_ >= text_.size()) {
                fail("unterminated array");
                return false;
            }
            if (text_[pos_] == ',') {
                ++pos_;
                continue;  // a trailing comma is rejected by the value check below
            }
            if (text_[pos_] == ']') {
                ++pos_;
                out = std::move(array);
                return true;
            }
            fail("expected ',' or ']' in array");
            return false;
        }
    }

    bool parse_object(JsonValue& out, int depth) {
        ++pos_;  // consume '{'
        JsonValue object = JsonValue::make_object();
        skip_ws();
        if (pos_ < text_.size() && text_[pos_] == '}') {
            ++pos_;
            out = std::move(object);
            return true;
        }
        while (true) {
            skip_ws();
            std::string key;
            if (pos_ >= text_.size() || text_[pos_] != '"') {
                fail("expected a string key in object");
                return false;
            }
            if (!parse_string(key)) return false;
            skip_ws();
            if (pos_ >= text_.size() || text_[pos_] != ':') {
                fail("expected ':' after object key");
                return false;
            }
            ++pos_;
            skip_ws();
            JsonValue value;
            if (!parse_value(value, depth + 1)) return false;
            object.add(std::move(key), std::move(value));
            skip_ws();
            if (pos_ >= text_.size()) {
                fail("unterminated object");
                return false;
            }
            if (text_[pos_] == ',') {
                ++pos_;
                continue;
            }
            if (text_[pos_] == '}') {
                ++pos_;
                out = std::move(object);
                return true;
            }
            fail("expected ',' or '}' in object");
            return false;
        }
    }

    const std::string& text_;
    std::string& error_;
    std::size_t pos_ = 0;
};

}  // namespace

bool parse_json(const std::string& text, JsonValue& out, std::string& error) {
    error.clear();
    Parser parser(text, error);
    if (!parser.parse(out)) {
        if (error.empty()) error = "JSON parse failure";
        return false;
    }
    return true;
}

}  // namespace vortyx::platform
