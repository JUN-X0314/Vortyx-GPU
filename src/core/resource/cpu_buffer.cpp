#include "core/resource/cpu_buffer.hpp"

#include <cstring>
#include <new>

namespace vortyx::resource {

CpuBuffer::CpuBuffer(const BufferDesc& desc) : IBufferImpl(desc) {}

bool CpuBuffer::allocate(std::string& error) {
    if (data_ != nullptr) {
        error = "cpu buffer is already allocated";
        return false;
    }
    if (byte_size() == 0) {
        error = "cannot allocate a zero-byte cpu buffer";
        return false;
    }
    // Non-throwing allocation: a failed (or absurd) host allocation becomes
    // an explicit error instead of an exception. Large-request sanity is
    // already enforced by validate_buffer_desc (kMaxBufferBytes), this is
    // the last line of defense for genuine out-of-memory.
    data_.reset(new (std::nothrow) std::uint8_t[byte_size()]);
    if (data_ == nullptr) {
        error = "host memory allocation of " + std::to_string(byte_size()) + " bytes failed";
        return false;
    }
    return true;
}

bool CpuBuffer::upload(const void* src, std::size_t bytes, std::string& error) {
    if (data_ == nullptr) {
        error = "cpu buffer has no allocation";
        return false;
    }
    if (src == nullptr) {
        error = "null source pointer";
        return false;
    }
    if (bytes == 0 || bytes > byte_size()) {
        error = "upload of " + std::to_string(bytes) + " bytes exceeds buffer bounds (" +
                std::to_string(byte_size()) + " bytes)";
        return false;
    }
    std::memcpy(data_.get(), src, bytes);
    return true;
}

bool CpuBuffer::download(void* dst, std::size_t bytes, std::string& error) {
    if (data_ == nullptr) {
        error = "cpu buffer has no allocation";
        return false;
    }
    if (dst == nullptr) {
        error = "null destination pointer";
        return false;
    }
    if (bytes == 0 || bytes > byte_size()) {
        error = "download of " + std::to_string(bytes) + " bytes exceeds buffer bounds (" +
                std::to_string(byte_size()) + " bytes)";
        return false;
    }
    std::memcpy(dst, data_.get(), bytes);
    return true;
}

std::unique_ptr<IBufferImpl> CpuBufferProvider::create_buffer(const BufferDesc& desc,
                                                              std::string& error) {
    auto buffer = std::make_unique<CpuBuffer>(desc);
    if (!buffer->allocate(error)) {
        return nullptr;
    }
    return buffer;
}

}  // namespace vortyx::resource
