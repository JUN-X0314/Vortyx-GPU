#pragma once

// CPU host-memory buffer (Phase 4).
//
// The CPU backend's implementation of the shared Buffer abstraction: plain
// host memory. This is deliberately simple and honest:
//   - memory_location() is Host (it really is ordinary CPU memory),
//   - upload/download are plain copies,
//   - allocation uses a non-throwing operator new so a failed or absurd
//     allocation becomes an explicit error, never an exception and never a
//     crash (the project avoids exceptions by design).

#include <cstddef>
#include <memory>
#include <string>

#include "core/resource/backend_buffer.hpp"

namespace vortyx::resource {

class CpuBuffer final : public IBufferImpl {
public:
    explicit CpuBuffer(const BufferDesc& desc);

    const char* backend_name() const override { return "cpu"; }
    MemoryLocation memory_location() const override { return MemoryLocation::Host; }

    // Allocates the storage (new (std::nothrow)). Must be called once before
    // first use; returns false and fills 'error' on failure.
    bool allocate(std::string& error);

    bool upload(const void* src, std::size_t bytes, std::string& error) override;
    bool download(void* dst, std::size_t bytes, std::string& error) override;

    // Raw host storage for the CPU backend's compute loops. The pointers are
    // valid for the buffer's whole lifetime once allocate() succeeded.
    void* data() { return data_.get(); }
    const void* data() const { return data_.get(); }

private:
    std::unique_ptr<std::uint8_t[]> data_;
};

// Always-available provider for host-memory buffers.
class CpuBufferProvider final : public IBufferProvider {
public:
    const char* name() const override { return "cpu"; }
    bool available() const override { return true; }
    std::string unavailable_reason() const override { return {}; }

    std::unique_ptr<IBufferImpl> create_buffer(const BufferDesc& desc,
                                               std::string& error) override;
};

}  // namespace vortyx::resource
