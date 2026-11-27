#pragma once

// Console rectangle lists without a geometry stage (MoltenVK and KosmicKrisp have none): the expansion moves into the
// vertex stage, as port/metal does it (rect_list.h, generate_rect_wrapper), here as a SPIR-V rewrite of the translated
// vertex shader.
//
// A rectangle list of N rectangles (3N vertices from firstVertex F, one instance) is drawn as N instances of 6
// vertices. The vertex shader's own code becomes a function that runs three times per invocation, once per corner,
// with VertexIndex = F + 3r + c (r = InstanceIndex - BaseInstance, the rectangle) and InstanceIndex = BaseInstance;
// its outputs go to Private copies. The new entry point finds the right angle among the three positions (the corner
// opposite the longest side, ties and degenerate triangles taking corner 1, rect_list.h right_angle), makes the fourth
// corner v3 = the other two minus it for every output, and writes the corner that vertex j = VertexIndex - BaseVertex
// of the two triangles is (both start at corner 0, with its winding, so flat inputs are corner 0's). The shader's vertex
// attributes are read per corner from three copies of each, at new locations, which the pipeline binds per instance:
// corner c of slot s at offset + (F + c) * stride with stride 3 * stride (VulkanReplay: rectangle_by_vertex).
//
// Refused, with a reason: SPIR-V before 1.4 (vector OpSelect, interface lists), an output that is not a 32-bit float
// scalar or vector (ClipDistance and other arrays), a variable used by anything but a load, store or access chain,
// more than 10 attribute locations.
//
// Plain word editing, standard library only.

#include <algorithm>
#include <cstdint>
#include <map>
#include <set>
#include <string>
#include <vector>

namespace leonida {

struct RectVertexShader {
    std::vector<std::uint32_t> words;
    std::vector<std::uint32_t> attribute_locations;  // the shader's attribute locations L_i, ascending
    // Attribute L_i of corner c is at location c * attribute_locations.size() + i.
};

// The word after OpEntryPoint's name (a nul-terminated string from word 3).
inline std::size_t name_end(const std::uint32_t* w, std::size_t n) {
    std::size_t k = 3;
    while (k < n) {
        const std::uint32_t word = w[k++];
        if (!(word & 0xffu) || !(word & 0xff00u) || !(word & 0xff0000u) || !(word & 0xff000000u)) break;
    }
    return k;
}

inline RectVertexShader rect_list_vertex(const std::vector<std::uint32_t>& in, std::string& why) {
    enum : std::uint32_t {
        op_name = 5, op_member_name = 6, op_memory_model = 14, op_entry_point = 15, op_execution_mode = 16,
        op_capability = 17, op_type_void = 19, op_type_bool = 20, op_type_int = 21, op_type_float = 22,
        op_type_vector = 23, op_type_array = 28, op_type_pointer = 32, op_type_function = 33,
        op_constant = 43, op_constant_composite = 44, op_function = 54, op_function_end = 56, op_function_call = 57,
        op_variable = 59, op_load = 61, op_store = 62, op_copy_memory = 63, op_copy_memory_sized = 64,
        op_access_chain = 65, op_in_bounds_access_chain = 66, op_ptr_access_chain = 67, op_decorate = 71,
        op_composite_extract = 81, op_iadd = 128, op_fadd = 129, op_isub = 130, op_fsub = 131, op_imul = 132,
        op_fmul = 133, op_select = 169, op_iequal = 170, op_logical_and = 167, op_ford_greater_than = 186,
        op_label = 248, op_return = 253,
        deco_builtin = 11, deco_location = 30,
        builtin_vertex_index = 42, builtin_instance_index = 43, builtin_base_vertex = 4424, builtin_base_instance = 4425,
        builtin_position = 0, storage_input = 1, storage_output = 3, storage_private = 6, storage_function = 7,
        capability_draw_parameters = 4427
    };
    RectVertexShader out;
    why.clear();
    if (in.size() < 6 || in[0] != 0x07230203u) {
        why = "the vertex shader is not a SPIR-V module";
        return out;
    }
    const std::uint32_t version = in[1];
    if (version < 0x00010400u) {
        why = "the vertex shader is SPIR-V before 1.4";
        return out;
    }
    std::uint32_t bound = in[3];
    const auto fresh = [&] { return bound++; };

    struct Type { std::uint32_t op = 0, a = 0, b = 0; };  // vector: a component, b count; pointer: a storage, b type
    std::map<std::uint32_t, Type> types;
    std::map<std::uint32_t, std::uint32_t> var_ptr, var_storage, builtin, location;
    std::map<std::uint32_t, std::pair<std::uint32_t, std::uint32_t>> int_constants;  // id -> (type, value)
    std::uint32_t entry = 0, void_type = 0, void_fn = 0, bool_type = 0, int_type = 0, float_type = 0;
    std::vector<std::uint32_t> interface_ids;
    std::size_t entry_at = 0, first_function = 0, first_type = 0, last_annotation = 0;
    bool draw_parameters = false;
    for (std::size_t i = 5; i < in.size();) {
        const std::uint32_t op = in[i] & 0xffffu, n = in[i] >> 16;
        if (!n || i + n > in.size()) {
            why = "the vertex shader's SPIR-V is cut short";
            return out;
        }
        const std::uint32_t* w = &in[i];
        switch (op) {
        case op_capability:
            if (n >= 2 && w[1] == capability_draw_parameters) draw_parameters = true;
            break;
        case op_entry_point:
            if (n >= 3 && w[1] == 0) {
                if (entry) {
                    why = "the module has more than one vertex entry point";
                    return out;
                }
                entry = w[2];
                entry_at = i;
                for (std::size_t k = name_end(w, n); k < n; ++k) interface_ids.push_back(w[k]);
            }
            break;
        case op_decorate:
            if (n >= 4 && w[2] == deco_builtin) builtin[w[1]] = w[3];
            if (n >= 4 && w[2] == deco_location) location[w[1]] = w[3];
            last_annotation = i + n;
            break;
        case op_type_void: void_type = w[1]; break;
        case op_type_bool: bool_type = w[1]; break;
        case op_type_int:
            types[w[1]] = {op, w[2], w[3]};
            if (w[2] == 32 && w[3] == 1) int_type = w[1];
            break;
        case op_type_float:
            types[w[1]] = {op, w[2], 0};
            if (w[2] == 32) float_type = w[1];
            break;
        case op_type_vector: types[w[1]] = {op, w[2], w[3]}; break;
        case op_type_pointer: types[w[1]] = {op, w[2], w[3]}; break;
        case op_type_function:
            if (n == 3 && w[2] == void_type) void_fn = w[1];
            types[w[1]] = {op, 0, 0};
            break;
        case op_type_array: types[w[1]] = {op, w[2], w[3]}; break;
        case op_constant:
            if (n == 4) int_constants[w[2]] = {w[1], w[3]};
            break;
        case op_variable:
            if (!first_function) {
                var_ptr[w[2]] = w[1];
                var_storage[w[2]] = w[3];
            }
            break;
        case op_function:
            if (!first_function) first_function = i;
            break;
        default: break;
        }
        if (!first_type && op >= op_type_void && op <= 39) first_type = i;
        i += n;
    }
    if (!entry || !first_function || !void_type || !void_fn || !float_type) {
        why = "the vertex shader lacks its entry point or the basic types";
        return out;
    }
    if (!last_annotation) last_annotation = first_type;
    const auto pointee = [&](std::uint32_t var) { return types[var_ptr[var]].b; };
    const auto is_float_value = [&](std::uint32_t type) {
        const Type& t = types[type];
        if (t.op == op_type_float) return t.a == 32;
        return t.op == op_type_vector && types[t.a].op == op_type_float && types[t.a].a == 32;
    };

    std::uint32_t vertex_index = 0, instance_index = 0, base_vertex = 0, base_instance = 0, position = 0;
    std::vector<std::uint32_t> outputs, attributes;
    for (const std::uint32_t id : interface_ids) {
        const auto storage = var_storage.find(id);
        if (storage == var_storage.end()) continue;
        const auto b = builtin.find(id);
        if (storage->second == storage_input) {
            if (b != builtin.end()) {
                if (b->second == builtin_vertex_index) vertex_index = id;
                else if (b->second == builtin_instance_index) instance_index = id;
                else if (b->second == builtin_base_vertex) base_vertex = id;
                else if (b->second == builtin_base_instance) base_instance = id;
            } else if (location.count(id)) {
                attributes.push_back(id);
            }
        } else if (storage->second == storage_output) {
            if (!is_float_value(pointee(id))) {
                why = "a vertex output is not a 32-bit float scalar or vector";
                return out;
            }
            if (b != builtin.end() && b->second == builtin_position) position = id;
            outputs.push_back(id);
        }
    }
    if (!position) {
        why = "the vertex shader writes no Position";
        return out;
    }
    if ((vertex_index && pointee(vertex_index) != int_type) || (instance_index && pointee(instance_index) != int_type) ||
        (base_vertex && pointee(base_vertex) != int_type) || (base_instance && pointee(base_instance) != int_type)) {
        why = "a vertex or instance index builtin is not a signed 32-bit integer";
        return out;
    }
    std::sort(attributes.begin(), attributes.end(),
              [&](std::uint32_t a, std::uint32_t b) { return location[a] < location[b]; });
    if (attributes.size() > 10) {
        why = "more than 10 vertex attribute locations";
        return out;
    }

    // Declarations made here, before the first function, and decorations, after the last one.
    std::vector<std::uint32_t> globals, annotations;
    const auto emit = [](std::vector<std::uint32_t>& to, std::uint32_t op, std::initializer_list<std::uint32_t> operands) {
        to.push_back((std::uint32_t(operands.size() + 1) << 16) | op);
        to.insert(to.end(), operands.begin(), operands.end());
    };
    if (!int_type) {
        int_type = fresh();
        emit(globals, op_type_int, {int_type, 32, 1});
    }
    if (!bool_type) {
        bool_type = fresh();
        emit(globals, op_type_bool, {bool_type});
    }
    std::map<std::pair<std::uint32_t, std::uint32_t>, std::uint32_t> pointers;  // (storage, type) -> id
    for (const auto& [id, t] : types)
        if (t.op == op_type_pointer) pointers.emplace(std::make_pair(t.a, t.b), id);
    const auto pointer = [&](std::uint32_t storage, std::uint32_t type) {
        auto it = pointers.find({storage, type});
        if (it != pointers.end()) return it->second;
        const std::uint32_t id = fresh();
        emit(globals, op_type_pointer, {id, storage, type});
        pointers.emplace(std::make_pair(storage, type), id);
        return id;
    };
    std::map<std::uint32_t, std::uint32_t> int_value;  // value -> constant id
    for (const auto& [id, tv] : int_constants)
        if (tv.first == int_type) int_value.emplace(tv.second, id);
    const auto constant = [&](std::uint32_t value) {
        auto it = int_value.find(value);
        if (it != int_value.end()) return it->second;
        const std::uint32_t id = fresh();
        emit(globals, op_constant, {int_type, id, value});
        int_value.emplace(value, id);
        return id;
    };
    const auto builtin_input = [&](std::uint32_t& var, std::uint32_t which) {
        if (var) return;
        var = fresh();
        emit(globals, op_variable, {pointer(storage_input, int_type), var, storage_input});
        emit(annotations, op_decorate, {var, deco_builtin, which});
        interface_ids.push_back(var);
    };
    builtin_input(vertex_index, builtin_vertex_index);
    builtin_input(instance_index, builtin_instance_index);
    builtin_input(base_vertex, builtin_base_vertex);
    builtin_input(base_instance, builtin_base_instance);

    // The body's variables: Private copies of the outputs, the attributes and the two indices.
    std::map<std::uint32_t, std::uint32_t> replace;
    std::map<std::uint32_t, std::uint32_t> private_of;
    std::vector<std::uint32_t> new_interface;
    const auto make_private = [&](std::uint32_t original, std::uint32_t type) {
        const std::uint32_t id = fresh();
        emit(globals, op_variable, {pointer(storage_private, type), id, storage_private});
        replace[original] = id;
        private_of[original] = id;
        new_interface.push_back(id);
        return id;
    };
    for (const std::uint32_t o : outputs) make_private(o, pointee(o));
    for (const std::uint32_t a : attributes) make_private(a, pointee(a));
    const std::uint32_t private_vertex = make_private(vertex_index, int_type);
    const std::uint32_t private_instance = make_private(instance_index, int_type);
    // Corner c's copy of attribute i, at location c * A + i.
    const std::size_t A = attributes.size();
    std::vector<std::vector<std::uint32_t>> corner_inputs(3, std::vector<std::uint32_t>(A));
    for (std::uint32_t c = 0; c < 3; ++c)
        for (std::size_t i = 0; i < A; ++i) {
            const std::uint32_t id = fresh();
            emit(globals, op_variable, {var_ptr[attributes[i]], id, storage_input});
            emit(annotations, op_decorate, {id, deco_location, std::uint32_t(c * A + i)});
            corner_inputs[c][i] = id;
            new_interface.push_back(id);
        }
    for (const std::uint32_t a : attributes) out.attribute_locations.push_back(location[a]);

    // The corner table: for right angle k (0..2), the six corners of the two triangles (3: the made corner).
    static constexpr std::uint32_t kCorners[18] = {0, 1, 3, 0, 3, 2,  0, 1, 2, 0, 2, 3,  0, 1, 2, 0, 3, 1};
    const std::uint32_t table_type = fresh();
    emit(globals, op_type_array, {table_type, int_type, constant(18)});
    std::vector<std::uint32_t> table_operands{table_type, 0};
    for (const std::uint32_t v : kCorners) table_operands.push_back(constant(v));
    const std::uint32_t table_value = fresh();
    table_operands[1] = table_value;
    globals.push_back((std::uint32_t(table_operands.size() + 1) << 16) | op_constant_composite);
    globals.insert(globals.end(), table_operands.begin(), table_operands.end());
    const std::uint32_t table_pointer = pointer(storage_function, table_type);
    const std::uint32_t int_function_pointer = pointer(storage_function, int_type);
    for (std::uint32_t v = 0; v <= 6; ++v) constant(v);

    // The new entry point.
    std::vector<std::uint32_t> body;
    const std::uint32_t main_id = fresh();
    emit(body, op_function, {void_type, main_id, 0, void_fn});
    emit(body, op_label, {fresh()});
    const std::uint32_t table = fresh();
    emit(body, op_variable, {table_pointer, table, storage_function, table_value});
    // port/: each corner's outputs are stored into Function variables of their own right after its call. Loading the
    // Private copies once per corner is the same in SPIR-V, but SPIRV-Cross (MoltenVK) forwards those loads to their
    // use after the third call, so every corner read corner 2's outputs and each rectangle was degenerate: nothing drawn.
    std::vector<std::vector<std::uint32_t>> corner_outputs(3, std::vector<std::uint32_t>(outputs.size()));
    for (std::uint32_t c = 0; c < 3; ++c)
        for (std::size_t k = 0; k < outputs.size(); ++k) {
            corner_outputs[c][k] = fresh();
            emit(body, op_variable, {pointer(storage_function, pointee(outputs[k])), corner_outputs[c][k], storage_function});
        }
    const auto value = [&](std::uint32_t op, std::uint32_t type, std::initializer_list<std::uint32_t> operands) {
        const std::uint32_t id = fresh();
        body.push_back((std::uint32_t(operands.size() + 3) << 16) | op);
        body.push_back(type);
        body.push_back(id);
        body.insert(body.end(), operands.begin(), operands.end());
        return id;
    };
    const std::uint32_t vi = value(op_load, int_type, {vertex_index});
    const std::uint32_t bv = value(op_load, int_type, {base_vertex});
    const std::uint32_t ii = value(op_load, int_type, {instance_index});
    const std::uint32_t bi = value(op_load, int_type, {base_instance});
    const std::uint32_t j = value(op_isub, int_type, {vi, bv});
    const std::uint32_t r = value(op_isub, int_type, {ii, bi});
    const std::uint32_t first = value(op_iadd, int_type, {bv, value(op_imul, int_type, {r, constant(3)})});
    std::vector<std::vector<std::uint32_t>> results(3, std::vector<std::uint32_t>(outputs.size()));
    for (std::uint32_t c = 0; c < 3; ++c) {
        emit(body, op_store, {private_vertex, value(op_iadd, int_type, {first, constant(c)})});
        emit(body, op_store, {private_instance, bi});
        for (std::size_t i = 0; i < A; ++i)
            emit(body, op_store, {private_of[attributes[i]], value(op_load, pointee(attributes[i]), {corner_inputs[c][i]})});
        value(op_function_call, void_type, {entry});
        for (std::size_t k = 0; k < outputs.size(); ++k)
            emit(body, op_store,
                 {corner_outputs[c][k], value(op_load, pointee(outputs[k]), {private_of[outputs[k]]})});
    }
    for (std::uint32_t c = 0; c < 3; ++c)
        for (std::size_t k = 0; k < outputs.size(); ++k)
            results[c][k] = value(op_load, pointee(outputs[k]), {corner_outputs[c][k]});
    const std::size_t pk = std::size_t(std::find(outputs.begin(), outputs.end(), position) - outputs.begin());
    std::uint32_t x[3], y[3];
    for (std::uint32_t c = 0; c < 3; ++c) {
        x[c] = value(op_composite_extract, float_type, {results[c][pk], 0});
        y[c] = value(op_composite_extract, float_type, {results[c][pk], 1});
    }
    const auto side = [&](int a, int b) {
        const std::uint32_t dx = value(op_fsub, float_type, {x[a], x[b]}), dy = value(op_fsub, float_type, {y[a], y[b]});
        return value(op_fadd, float_type, {value(op_fmul, float_type, {dx, dx}), value(op_fmul, float_type, {dy, dy})});
    };
    const std::uint32_t d12 = side(1, 2), d20 = side(2, 0), d01 = side(0, 1);
    const auto greater = [&](std::uint32_t a, std::uint32_t b) { return value(op_ford_greater_than, bool_type, {a, b}); };
    const std::uint32_t is0 = value(op_logical_and, bool_type, {greater(d12, d20), greater(d12, d01)});
    const std::uint32_t is2 = value(op_logical_and, bool_type, {greater(d01, d12), greater(d01, d20)});
    const std::uint32_t corner =
        value(op_select, int_type, {is0, constant(0), value(op_select, int_type, {is2, constant(2), constant(1)})});
    const std::uint32_t slot = value(op_iadd, int_type, {value(op_imul, int_type, {corner, constant(6)}), j});
    const std::uint32_t which = value(op_load, int_type, {value(op_access_chain, int_function_pointer, {table, slot})});
    const std::uint32_t is_v0 = value(op_iequal, bool_type, {which, constant(0)});
    const std::uint32_t is_v1 = value(op_iequal, bool_type, {which, constant(1)});
    const std::uint32_t is_v2 = value(op_iequal, bool_type, {which, constant(2)});
    for (std::size_t k = 0; k < outputs.size(); ++k) {
        const std::uint32_t t = pointee(outputs[k]);
        const std::uint32_t o0 = results[0][k], o1 = results[1][k], o2 = results[2][k];
        const std::uint32_t right = value(op_select, t, {is0, o0, value(op_select, t, {is2, o2, o1})});
        const std::uint32_t a = value(op_select, t, {is0, o1, o0});
        const std::uint32_t b = value(op_select, t, {is0, o2, value(op_select, t, {is2, o1, o2})});
        const std::uint32_t made = value(op_fsub, t, {value(op_fadd, t, {a, b}), right});
        const std::uint32_t chosen = value(
            op_select, t, {is_v0, o0, value(op_select, t, {is_v1, o1, value(op_select, t, {is_v2, o2, made})})});
        emit(body, op_store, {outputs[k], chosen});
    }
    emit(body, op_return, {});
    emit(body, op_function_end, {});

    // The rewrite of the original code: its globals become the Private copies.
    std::vector<std::uint32_t> words(in.begin(), in.begin() + 5);
    for (std::size_t i = 5; i < in.size();) {
        const std::uint32_t op = in[i] & 0xffffu, n = in[i] >> 16;
        std::vector<std::uint32_t> w(in.begin() + std::ptrdiff_t(i), in.begin() + std::ptrdiff_t(i + n));
        if (i == entry_at) {
            std::vector<std::uint32_t> ids;
            for (const std::uint32_t id : interface_ids)
                if (std::find(attributes.begin(), attributes.end(), id) == attributes.end()) ids.push_back(id);
            ids.insert(ids.end(), new_interface.begin(), new_interface.end());
            w.resize(name_end(w.data(), n));
            w[2] = main_id;
            w.insert(w.end(), ids.begin(), ids.end());
            w[0] = (std::uint32_t(w.size()) << 16) | op;
        } else if (op == op_execution_mode && n >= 2 && w[1] == entry) {
            w[1] = main_id;
        } else if (i >= first_function) {
            const auto swap = [&](std::size_t k) {
                if (k < w.size()) {
                    const auto it = replace.find(w[k]);
                    if (it != replace.end()) w[k] = it->second;
                }
            };
            if (op == op_load || op == op_access_chain || op == op_in_bounds_access_chain || op == op_ptr_access_chain)
                swap(3);
            else if (op == op_store) swap(1);
            else if (op == op_copy_memory || op == op_copy_memory_sized) swap(1), swap(2);
        }
        if (i == first_function) words.insert(words.end(), globals.begin(), globals.end());
        if (i == last_annotation && !annotations.empty()) {
            words.insert(words.end(), annotations.begin(), annotations.end());
            annotations.clear();
        }
        words.insert(words.end(), w.begin(), w.end());
        i += n;
    }
    if (!annotations.empty()) {
        why = "no place for the new decorations";
        return {};
    }
    words.insert(words.end(), body.begin(), body.end());
    if (!draw_parameters) {
        // BaseVertex and BaseInstance need DrawParameters (core in SPIR-V 1.3): the capability goes first.
        words.insert(words.begin() + 5, {(2u << 16) | op_capability, capability_draw_parameters});
    }
    words[3] = bound;
    out.words = std::move(words);
    return out;
}

}  // namespace leonida
