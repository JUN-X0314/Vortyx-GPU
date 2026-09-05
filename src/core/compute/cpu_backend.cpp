#include "core/compute/cpu_backend.hpp"

#include "core/device/discovery.hpp"

#include <string>
#include <system_error>
#include <thread>
#include <vector>

namespace vortyx::compute {
namespace {

// Resolves the host storage of a dispatch buffer. The buffer must belong to
// the cpu backend (routing bugs are rejected, never dereferenced).
const std::int32_t* input_data(const vortyx::resource::IBufferImpl& buffer,
                               vortyx::compute::Status& status, std::string& error) {
    const vortyx::resource::CpuBuffer* cpu =
        dynamic_cast<const vortyx::resource::CpuBuffer*>(&buffer);
    if (cpu == nullptr) {
        status = Status::BackendError;
        error = "buffer does not belong to the cpu backend";
        return nullptr;
    }
    return static_cast<const std::int32_t*>(cpu->data());
}

std::int32_t* output_data(vortyx::resource::IBufferImpl& buffer,
                          vortyx::compute::Status& status, std::string& error) {
    vortyx::resource::CpuBuffer* cpu = dynamic_cast<vortyx::resource::CpuBuffer*>(&buffer);
    if (cpu == nullptr) {
        status = Status::BackendError;
        error = "buffer does not belong to the cpu backend";
        return nullptr;
    }
    return static_cast<std::int32_t*>(cpu->data());
}

// The sequential elementwise kernel over one index range. All ops are
// independent per element, so a range can never influence another range —
// this is what makes fork-join partitioning bit-exact identical to the
// sequential result.
//
// Integer semantics (defined, never UB):
//   - VectorAdd follows the Phase 3 policy: callers keep sums inside int32
//     range (documented at VectorAddTask); in range, CPU and GPU agree
//     bit-exactly.
//   - VectorMultiply / VectorScale are DEFINED modular: the multiplication
//     happens on uint32 (well-defined modulo 2^32) and converts back to the
//     low 32 bits — exactly the semantics the Vulkan kernels guarantee, so
//     overflow cases stay bit-exact across backends by construction. The
//     unsigned->signed conversion is two's complement on every supported
//     compiler (GCC / Clang / MSVC) and mandated by C++20.
void compute_range(ComputeOp op, const std::int32_t* a, const std::int32_t* b,
                   std::int32_t scalar, std::int32_t* out, std::size_t begin,
                   std::size_t end) {
    switch (op) {
        case ComputeOp::VectorAdd:
            for (std::size_t i = begin; i < end; ++i) out[i] = a[i] + b[i];
            break;
        case ComputeOp::VectorMultiply:
            for (std::size_t i = begin; i < end; ++i) {
                out[i] = static_cast<std::int32_t>(static_cast<std::uint32_t>(a[i]) *
                                                   static_cast<std::uint32_t>(b[i]));
            }
            break;
        case ComputeOp::VectorScale:
            for (std::size_t i = begin; i < end; ++i) {
                out[i] = static_cast<std::int32_t>(static_cast<std::uint32_t>(a[i]) *
                                                   static_cast<std::uint32_t>(scalar));
            }
            break;
    }
}

}  // namespace

vortyx::device::DeviceInfo CpuBackend::device_info() const {
    // Reuses the Phase 2 CPU discovery; the machine always has at least
    // one CPU entry. If discovery unexpectedly fails, an unknown device is
    // reported instead of fabricated data.
    const vortyx::device::DiscoveryResult cpus = vortyx::device::discover_cpus();
    if (cpus.ok && !cpus.devices.empty()) {
        return cpus.devices.front();
    }
    vortyx::device::DeviceInfo unknown;
    unknown.type = vortyx::device::DeviceType::Cpu;
    unknown.backend = "cpu";
    return unknown;
}

ComputeResult CpuBackend::execute(const vortyx::resource::IBufferImpl& a,
                                  const vortyx::resource::IBufferImpl& b,
                                  vortyx::resource::IBufferImpl& c) {
    // Phase 4 contract kept verbatim: this signature IS vector addition.
    ComputeDispatch dispatch;
    dispatch.op = ComputeOp::VectorAdd;
    dispatch.input_a = &a;
    dispatch.input_b = &b;
    dispatch.output = &c;
    return execute(dispatch);
}

ComputeResult CpuBackend::execute(const ComputeDispatch& dispatch) {
    // The dispatch must name a real operation and a structurally valid
    // buffer triple. Enforced here so direct backend users cannot bypass it
    // (the Runtime validates the same rules before dispatching).
    switch (dispatch.op) {
        case ComputeOp::VectorAdd:
        case ComputeOp::VectorMultiply:
        case ComputeOp::VectorScale:
            break;
        default:
            return ComputeResult{Status::InvalidInput,
                                 "compute dispatch names an unknown operation"};
    }
    if (dispatch.input_a == nullptr || dispatch.output == nullptr) {
        return ComputeResult{Status::InvalidInput,
                             "compute dispatch is missing its input/output buffers"};
    }

    std::string error;
    const Status validation = validate_compute_dispatch_buffers(
        dispatch.op, dispatch.input_a->desc(),
        dispatch.input_b != nullptr ? &dispatch.input_b->desc() : nullptr,
        dispatch.output->desc(), error);
    if (validation != Status::Ok) {
        return ComputeResult{validation, error};
    }

    // Resolve the host storage (rejects foreign buffers before any access).
    Status status = Status::Ok;
    const std::int32_t* pa = input_data(*dispatch.input_a, status, error);
    if (pa == nullptr) return ComputeResult{status, error};
    const std::int32_t* pb = nullptr;
    if (dispatch.input_b != nullptr) {
        pb = input_data(*dispatch.input_b, status, error);
        if (pb == nullptr) return ComputeResult{status, error};
    }
    std::int32_t* pc = output_data(*dispatch.output, status, error);
    if (pc == nullptr) return ComputeResult{status, error};

    const ComputeOp op = dispatch.op;
    const std::int32_t scalar = dispatch.scalar;
    const std::size_t count = dispatch.input_a->desc().element_count;

    // --- Worker policy (see cpu_backend.hpp for the full documentation) ---
    unsigned int hardware = std::thread::hardware_concurrency();  // 0 == unknown
    if (hardware == 0) hardware = 1;
    unsigned int workers = hardware < CpuBackend::kMaxCpuWorkers ? hardware
                                                                 : CpuBackend::kMaxCpuWorkers;
    if (workers < 1) workers = 1;

    if (workers <= 1 || count < CpuBackend::kParallelThreshold) {
        // Sequential path: small workloads (and single-CPU machines) run on
        // the calling thread — thread overhead would only make them slower.
        compute_range(op, pa, pb, scalar, pc, 0, count);
        return ComputeResult{Status::Ok, {}};
    }

    // Fork-join over disjoint index ranges. Each worker owns exactly one
    // equal chunk; the calling thread computes the final range, which also
    // absorbs the remainder (count % workers). Workers are joined before
    // execute() returns. If a worker spawn fails, its whole remaining range
    // falls back to the calling thread — identical result, only slower.
    const std::size_t chunk = count / workers;
    std::size_t begin = 0;
    std::vector<std::thread> pool;
    pool.reserve(workers - 1);
    for (unsigned int w = 1; w < workers; ++w) {
        const std::size_t end = begin + chunk;
        if (begin >= end) break;  // workload exhausted for this many workers
        try {
            pool.emplace_back([op, pa, pb, scalar, pc, begin, end] {
                compute_range(op, pa, pb, scalar, pc, begin, end);
            });
        } catch (const std::system_error&) {
            // A worker could not be created: leave its whole remaining range
            // to the calling thread below. Never an error, never a data loss.
            break;
        }
        begin = end;
    }
    if (begin < count) {
        compute_range(op, pa, pb, scalar, pc, begin, count);
    }
    for (std::thread& worker : pool) {
        worker.join();
    }
    return ComputeResult{Status::Ok, {}};
}

}  // namespace vortyx::compute
