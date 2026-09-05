# Tensor Layer — Local Development (Phase 13)

## Build

The tensor layer builds with the project (default ON):

```bash
cmake -B build -S . -DCMAKE_BUILD_TYPE=Release
cmake --build build -j
ctest --test-dir build -C Release --output-on-failure
```

Options:

| Option | Effect |
|--------|--------|
| `VORTYX_ENABLE_TENSOR=OFF` | builds exactly like Phase 12 (no tensor library, no tensor tests) |
| `VORTYX_ENABLE_DISTRIBUTED=OFF` | disables the tensor layer automatically (it builds on the distributed layer) — builds exactly like Phase 11 |
| `VORTYX_ENABLE_PLATFORM=OFF` | disables the whole upper stack — builds exactly like Phase 10 |

## Minimal single-op execution

```cpp
#include "tensor/tensor.hpp"

auto manager = std::make_shared<vortyx::resource::ResourceManager>();  // Phase 4 memory system
vortyx::resource::CpuBufferProvider cpu;
manager->register_provider(&cpu);

vortyx::tensor::TensorExecutor::Deps deps;
deps.resources = manager.get();
std::unique_ptr<vortyx::tensor::TensorExecutor> executor;
std::string error;
vortyx::tensor::TensorExecutor::create(deps, executor, error);

const float a[6] = {1, 2, 3, 4, 5, 6};
const float b[6] = {7, 8, 9, 10, 11, 12};
vortyx::tensor::Tensor ta, tb;
vortyx::tensor::Tensor::from_host(*manager, vortyx::tensor::TensorShape::make({2, 3}),
                                  vortyx::tensor::DataType::FP32, a, sizeof(a), ta, error);
vortyx::tensor::Tensor::from_host(*manager, vortyx::tensor::TensorShape::make({3, 2}),
                                  vortyx::tensor::DataType::FP32, b, sizeof(b), tb, error);

vortyx::tensor::Tensor out;
const vortyx::tensor::TensorStatus status = executor->execute_op(
    vortyx::tensor::TensorOp::MatMul, vortyx::tensor::TensorOpParams{}, {ta, tb}, out, error);
// out = [[58, 64], [139, 154]] on the reference path.
```

With `deps.runtime = &myInitializedRuntime` the int32 elementwise ops route
through the REAL compute engine (CPU, and the Vulkan GPU when one exists).

## Graph execution

```cpp
vortyx::tensor::TensorGraph graph;
vortyx::tensor::GraphInputDesc x;
x.name = "x";
x.shape = vortyx::tensor::TensorShape::make({2, 2});
x.dtype = vortyx::tensor::DataType::FP32;
graph.add_input(x, error);

vortyx::tensor::NodeId mm, relu;
graph.add_node(vortyx::tensor::TensorOp::MatMul, {}, mm, error);
graph.add_node(vortyx::tensor::TensorOp::Relu, {}, relu, error);

vortyx::tensor::GraphNodeInput fx{vortyx::tensor::GraphNodeInput::Source::GraphInput, 0};
vortyx::tensor::GraphNodeInput fmm{vortyx::tensor::GraphNodeInput::Source::NodeOutput,
                                   static_cast<std::int32_t>(mm)};
graph.bind_input(mm, 0, fx, error);
graph.bind_input(mm, 1, fx, error);
graph.bind_input(relu, 0, fmm, error);
graph.set_outputs({relu}, error);

vortyx::tensor::GraphExecutor graph_executor(*executor);
auto result = graph_executor.execute(graph, {x_tensor});
// result.outputs[0], result.trace[i].elapsed_ns (real measurements)
```

## Tests (no GPU required)

| Test | Focus |
|------|-------|
| `TensorCoreTest` | dtype/shape/layout/broadcast/placement/storage/views/serialization |
| `TensorOpsTest` | every op against hand-verified values and FP32 baselines |
| `TensorGraphTest` | validation, deterministic plans, memory reuse, end-to-end graphs |
| `TensorDispatchTest` | capability dispatch, runtime adapter, error codes |
| `TensorDistributedTest` | placement over Phase 12 snapshots, failover, transfer honesty |
