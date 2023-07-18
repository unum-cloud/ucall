#pragma once
#include <charconv>
#include <variant>

#include <simdjson.h>

#include "helpers/shared.hpp"

namespace unum::ucall {

namespace sj = simdjson;
namespace sjd = sj::dom;

struct scratch_space_t {

    char json_pointer[json_pointer_capacity_k]{};
    char printed_int_id[max_integer_length_k]{};

    sjd::parser parser{};
    sjd::element tree{};
    bool is_batch{};
    bool is_async{};
    bool is_http{};

    sjd::parser* dynamic_parser{};
    std::string_view dynamic_packet{};
    std::string_view dynamic_id{};

    sj::simdjson_result<sjd::element> point_to_param(std::string_view name) noexcept {
        bool has_slash = name.size() && name.front() == '/';
        std::size_t final_size = name.size() + 8u - has_slash;
        if (final_size > json_pointer_capacity_k)
            return sj::INVALID_JSON_POINTER;
        std::memcpy((void*)json_pointer, "/params/", 8u);
        std::memcpy((void*)(json_pointer + 8u - has_slash), name.data(), name.size());
        return tree.at_pointer({json_pointer, final_size});
    }

    sj::simdjson_result<sjd::element> point_to_param(std::size_t position) noexcept {
        std::memcpy((void*)json_pointer, "/params/", 8u);
        std::to_chars_result res = std::to_chars(json_pointer + 8u, json_pointer + json_pointer_capacity_k, position);
        if (res.ec != std::errc(0))
            return sj::INVALID_JSON_POINTER;
        std::size_t final_size = res.ptr - json_pointer;
        return tree.at_pointer({json_pointer, final_size});
    }
};

struct default_error_t {
    int code{};
    std::string_view note;
};

struct named_callback_t {
    ucall_str_t name{};
    ucall_callback_t callback{};
    ucall_callback_tag_t callback_tag{};
};

/**
 * @brief Validates the contents of the JSON call DOM, and finds a matching callback.
 */
template <typename named_callbacks_at>
inline std::variant<named_callback_t, default_error_t> find_callback(named_callbacks_at const& callbacks,
                                                                     scratch_space_t& scratch) noexcept {
    sjd::element const& doc = scratch.tree;
    if (!doc.is_object())
        return default_error_t{-32600, "The JSON sent is not a valid request object."};

    // We don't support JSON-RPC before version 2.0.
    sj::simdjson_result<sjd::element> version = doc["jsonrpc"];
    if (!version.is_string() || version.get_string().value_unsafe() != "2.0")
        return default_error_t{-32600, "The request doesn't specify the 2.0 version."};

    // Check if the shape of the request is correct:
    sj::simdjson_result<sjd::element> id = doc["id"];
    // SIMDJSON will consider a field a `double` even if it is simply convertible to it.
    bool id_invalid = (id.is_double() && !id.is_int64() && !id.is_uint64()) || id.is_object() || id.is_array();
    if (id_invalid)
        return default_error_t{-32600, "The request must have integer or string id."};
    sj::simdjson_result<sjd::element> method = doc["method"];
    bool method_invalid = !method.is_string();
    if (method_invalid)
        return default_error_t{-32600, "The method must be a string."};
    sj::simdjson_result<sjd::element> params = doc["params"];
    bool params_present_and_invalid = !params.is_array() && !params.is_object() && params.error() == sj::SUCCESS;
    if (params_present_and_invalid)
        return default_error_t{-32600, "Parameters can only be passed in arrays or objects."};

    if (id.is_string()) {
        scratch.dynamic_id = id.get_string().value_unsafe();
    } else if (id.is_int64() || id.is_uint64()) {
        char* code = &scratch.printed_int_id[0];
        std::to_chars_result res = std::to_chars(code, code + max_integer_length_k, id.get_int64().value_unsafe());
        auto code_len = res.ptr - code;
        if (res.ec != std::errc(0))
            return default_error_t{-32600, "The request ID is invalid integer."};
        scratch.dynamic_id = std::string_view(code, code_len);
    } else
        scratch.dynamic_id = "";

    // Make sure we have such a method:
    auto method_name = method.get_string().value_unsafe();
    auto callbacks_end = callbacks.data() + callbacks.size();
    auto callback_it = std::find_if(callbacks.data(), callbacks_end, [=](named_callback_t const& callback) noexcept {
        return callback.name == method_name;
    });
    if (callback_it == callbacks_end)
        return default_error_t{-32601, "Method not found."};

    return *callback_it;
}

} // namespace unum::ucall
