// Platform identity implementation (Phase 11).

#include "platform/identity.hpp"

#include <cstdio>
#include <random>

namespace vortyx::platform {

namespace {

// Charset of a syntactically valid id: ASCII letters, digits, dot,
// underscore, hyphen. Nothing else — ids appear in URL paths, JSON bodies
// and logs, and must never carry shell/SQL/HTML significance by themselves.
bool id_char(char c) {
    const bool letter = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z');
    const bool digit = (c >= '0' && c <= '9');
    return letter || digit || c == '.' || c == '_' || c == '-';
}

std::string generate_uuid_v4() {
    // 64 bits of entropy per draw; enough structure to make accidental
    // collisions practically impossible for the scale of this project while
    // staying in the standard library (no external dependency).
    static thread_local std::mt19937_64 rng(std::random_device{}());
    std::uint64_t hi = rng();
    std::uint64_t lo = rng();

    // Set the UUID v4 version (high 4 bits of the 3rd group = 0100) and the
    // RFC 4122 variant (top 2 bits of the 4th group = 10) so generated ids
    // are genuine v4 UUIDs, not just random-looking strings.
    hi = (hi & 0xFFFFFFFFFFFF0FFFULL) | 0x0000000000004000ULL;
    lo = (lo & 0x3FFFFFFFFFFFFFFFULL) | 0x8000000000000000ULL;

    char buf[40];
    std::snprintf(buf, sizeof(buf),
                  "%08x-%04x-%04x-%04x-%012llx",
                  static_cast<unsigned>((hi >> 32) & 0xFFFFFFFFULL),
                  static_cast<unsigned>((hi >> 16) & 0xFFFFULL),
                  static_cast<unsigned>(hi & 0xFFFFULL),
                  static_cast<unsigned>((lo >> 48) & 0xFFFFULL),
                  static_cast<unsigned long long>(lo & 0xFFFFFFFFFFFFULL));
    return std::string(buf);
}

}  // namespace

bool is_valid_id(const std::string& value) {
    if (value.empty() || value.size() > kMaxIdLength) return false;
    for (const char c : value) {
        if (!id_char(c)) return false;
    }
    return true;
}

Status validate_id(const std::string& label, const std::string& value, std::string& error) {
    if (value.empty()) {
        error = label + " must not be empty";
        return Status::InvalidInput;
    }
    if (value.size() > kMaxIdLength) {
        error = label + " exceeds the maximum length of " + std::to_string(kMaxIdLength) + " characters";
        return Status::InvalidInput;
    }
    for (const char c : value) {
        if (!id_char(c)) {
            error = label + " contains an invalid character (allowed: A-Z a-z 0-9 . _ -)";
            return Status::InvalidInput;
        }
    }
    error.clear();
    return Status::Ok;
}

DeviceId generate_device_id() {
    return generate_uuid_v4();
}

JobId generate_job_id() {
    return generate_uuid_v4();
}

}  // namespace vortyx::platform
