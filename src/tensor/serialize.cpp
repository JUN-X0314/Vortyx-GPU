// Tensor metadata serialization (Phase 13) — implementation.

#include "tensor/serialize.hpp"

#include "tensor/shape.hpp"

namespace vortyx::tensor {

namespace {

vortyx::platform::JsonValue serialize_layout(const TensorLayout& layout) {
    vortyx::platform::JsonValue object = vortyx::platform::JsonValue::make_object();
    object.add("kind", vortyx::platform::JsonValue::make_string(to_string(layout.kind)));
    vortyx::platform::JsonValue strides = vortyx::platform::JsonValue::make_array();
    for (const std::int64_t stride : layout.strides) {
        strides.push(vortyx::platform::JsonValue::make_number(
            static_cast<double>(stride)));
    }
    object.add("strides", std::move(strides));
    return object;
}

vortyx::platform::JsonValue serialize_placement(const TensorPlacement& placement) {
    vortyx::platform::JsonValue object = vortyx::platform::JsonValue::make_object();
    object.add("location", vortyx::platform::JsonValue::make_string(
                               to_string(placement.location)));
    if (placement.location == PlacementLocation::Device) {
        object.add("device_id",
                   vortyx::platform::JsonValue::make_string(placement.device_id));
    }
    if (!placement.backend.empty()) {
        object.add("backend", vortyx::platform::JsonValue::make_string(placement.backend));
    }
    return object;
}

bool parse_int64_array(const vortyx::platform::JsonValue& value,
                       std::vector<std::int64_t>& out, std::string& error) {
    if (!value.is_array()) {
        error = "expected an array of integers";
        return false;
    }
    out.clear();
    for (const vortyx::platform::JsonValue& item : value.items()) {
        if (!item.is_number()) {
            error = "array items must be numbers";
            return false;
        }
        const double number = item.as_number();
        // Range-check BEFORE the int64 cast: casting an out-of-range double
        // to int64 is undefined behavior (float-cast-overflow). The previous
        // integrality check only noticed the corruption afterwards — the UB
        // happened first. NaN fails the comparison and is refused too.
        // (Phase 13 audit hardening; this is the metadata contract boundary.)
        if (!(number >= -9223372036854775808.0 && number < 9223372036854775808.0)) {
            error = "array item is out of the signed 64-bit range";
            return false;
        }
        if (number != static_cast<double>(static_cast<std::int64_t>(number))) {
            error = "array items must be exact integers";
            return false;
        }
        out.push_back(static_cast<std::int64_t>(number));
    }
    return true;
}

}  // namespace

std::string serialize_tensor_meta(const TensorMeta& meta) {
    vortyx::platform::JsonValue object = vortyx::platform::JsonValue::make_object();
    vortyx::platform::JsonValue shape = vortyx::platform::JsonValue::make_array();
    for (const TensorDim dim : meta.shape.dims) {
        shape.push(vortyx::platform::JsonValue::make_number(static_cast<double>(dim)));
    }
    object.add("shape", std::move(shape));
    object.add("dtype", vortyx::platform::JsonValue::make_string(to_string(meta.dtype)));
    object.add("layout", serialize_layout(meta.layout));
    object.add("placement", serialize_placement(meta.placement));
    return object.serialize();
}

std::string serialize_tensor_meta(const Tensor& tensor) {
    TensorMeta meta;
    meta.shape = tensor.shape();
    meta.dtype = tensor.dtype();
    meta.layout = tensor.layout();
    meta.placement = tensor.placement();
    return serialize_tensor_meta(meta);
}

TensorStatus parse_tensor_meta(const std::string& text, TensorMeta& out, std::string& error) {
    vortyx::platform::JsonValue value;
    if (!vortyx::platform::parse_json(text, value, error)) {
        return TensorStatus::InvalidInput;
    }
    if (!value.is_object()) {
        error = "tensor meta must be a JSON object";
        return TensorStatus::InvalidInput;
    }

    // Unknown-field rejection (the project contract rule).
    for (const auto& member : value.members()) {
        const std::string& key = member.first;
        if (key != "shape" && key != "dtype" && key != "layout" && key != "placement") {
            error = "unknown field in tensor meta: '" + key + "'";
            return TensorStatus::InvalidInput;
        }
    }

    const vortyx::platform::JsonValue* shape = value.find("shape");
    const vortyx::platform::JsonValue* dtype = value.find("dtype");
    const vortyx::platform::JsonValue* layout = value.find("layout");
    const vortyx::platform::JsonValue* placement = value.find("placement");
    if (shape == nullptr || dtype == nullptr || layout == nullptr || placement == nullptr) {
        error = "tensor meta requires shape, dtype, layout and placement";
        return TensorStatus::InvalidInput;
    }

    std::vector<std::int64_t> dims;
    if (!parse_int64_array(*shape, dims, error)) return TensorStatus::InvalidShape;
    out.shape = TensorShape(std::move(dims));
    const TensorStatus shape_status = out.shape.validate(error);
    if (shape_status != TensorStatus::Ok) return shape_status;

    if (!dtype->is_string() ||
        !data_type_from_string(dtype->as_string(), out.dtype)) {
        error = "unknown dtype name";
        return TensorStatus::UnsupportedDtype;
    }

    if (!layout->is_object()) {
        error = "layout must be an object";
        return TensorStatus::InvalidInput;
    }
    for (const auto& member : layout->members()) {
        if (member.first != "kind" && member.first != "strides") {
            error = "unknown field in layout: '" + member.first + "'";
            return TensorStatus::InvalidInput;
        }
    }
    const vortyx::platform::JsonValue* kind = layout->find("kind");
    const vortyx::platform::JsonValue* strides = layout->find("strides");
    if (kind == nullptr || strides == nullptr) {
        error = "layout requires kind and strides";
        return TensorStatus::InvalidInput;
    }
    if (!kind->is_string()) {
        error = "layout kind must be a string";
        return TensorStatus::InvalidInput;
    }
    const std::string kind_name = kind->as_string();
    if (kind_name == "row_major_contiguous") {
        out.layout.kind = LayoutKind::RowMajorContiguous;
    } else if (kind_name == "strided") {
        out.layout.kind = LayoutKind::Strided;
    } else {
        error = "unknown layout kind '" + kind_name + "'";
        return TensorStatus::UnsupportedLayout;
    }
    std::vector<std::int64_t> stride_values;
    if (!parse_int64_array(*strides, stride_values, error)) {
        return TensorStatus::InvalidStride;
    }
    out.layout.strides = std::move(stride_values);

    if (!placement->is_object()) {
        error = "placement must be an object";
        return TensorStatus::InvalidInput;
    }
    for (const auto& member : placement->members()) {
        if (member.first != "location" && member.first != "device_id" &&
            member.first != "backend") {
            error = "unknown field in placement: '" + member.first + "'";
            return TensorStatus::InvalidInput;
        }
    }
    const vortyx::platform::JsonValue* location = placement->find("location");
    if (location == nullptr || !location->is_string()) {
        error = "placement requires a location string";
        return TensorStatus::InvalidPlacement;
    }
    const std::string location_name = location->as_string();
    if (location_name == "host") {
        out.placement = TensorPlacement::host();
    } else if (location_name == "device") {
        const vortyx::platform::JsonValue* device_id = placement->find("device_id");
        if (device_id == nullptr || !device_id->is_string()) {
            error = "device placement requires device_id";
            return TensorStatus::InvalidPlacement;
        }
        const vortyx::platform::JsonValue* backend = placement->find("backend");
        out.placement =
            TensorPlacement::on_device(device_id->as_string(),
                                       backend != nullptr && backend->is_string()
                                           ? backend->as_string()
                                           : std::string());
    } else {
        error = "unknown placement location '" + location_name + "'";
        return TensorStatus::InvalidPlacement;
    }

    // Final validation against the real rules (stride sanity included; the
    // storage span is unknown at meta level, so the stride check uses the
    // shape's own element span as the bound — a meta cannot claim more).
    std::int64_t elements = 0;
    if (!out.shape.total_elements(elements)) {
        error = "tensor meta element count overflows";
        return TensorStatus::ResourceLimitExceeded;
    }
    if (elements > 0) {
        const TensorStatus layout_status =
            out.layout.validate(out.shape, elements, error);
        if (layout_status != TensorStatus::Ok) return layout_status;
    }
    const TensorStatus placement_status = out.placement.validate(error);
    if (placement_status != TensorStatus::Ok) return placement_status;

    return TensorStatus::Ok;
}

}  // namespace vortyx::tensor
