#pragma once
#include <variant>

#include <picohttpparser.h>
#include <simdjson.h>

#include "shared.hpp"

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

/**
 * @brief Rounds integer to the next multiple of a given number. Is needed for aligned memory allocations.
 */
template <std::size_t step_ak> constexpr std::size_t round_up_to(std::size_t n) noexcept {
    return ((n + step_ak - 1) / step_ak) * step_ak;
}

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

struct parsed_request_t {
    std::string_view type{};
    std::string_view keep_alive{};
    std::string_view content_type{};
    std::string_view content_length{};
    std::string_view body{};
};

/**
 * @brief Analyzes the contents of the packet, bifurcating pure JSON-RPC from HTTP1-based.
 * @warning This doesn't check the headers for validity or additional metadata.
 */
inline std::variant<parsed_request_t, default_error_t> split_body_headers(std::string_view body) noexcept {
    // A typical HTTP-header may look like this
    // POST /endpoint HTTP/1.1
    // Host: rpc.example.com
    // Content-Type: application/json
    // Content-Length: ...
    // Accept: application/json
    constexpr size_t const max_headers_k = 32;

    parsed_request_t req{};
    char const* method{};
    size_t method_len{};
    char const* path{};
    size_t path_len{};
    int minor_version{};
    phr_header headers[max_headers_k]{};
    size_t count_headers{max_headers_k};

    int res = phr_parse_request(body.data(), body.size(), &method, &method_len, &path, &path_len, &minor_version,
                                headers, &count_headers, 0);

    if (res == -2)
        return default_error_t{-2, "Partial HTTP request"};

    if (res > 0) {
        req.type = std::string_view(method, method_len);
        for (std::size_t i = 0; i < count_headers; ++i) {
            if (headers[i].name_len == 0)
                continue;
            if (std::string_view(headers[i].name, headers[i].name_len) == std::string_view("Keep-Alive"))
                req.keep_alive = std::string_view(headers[i].value, headers[i].value_len);
            else if (std::string_view(headers[i].name, headers[i].name_len) == std::string_view("Content-Type"))
                req.content_type = std::string_view(headers[i].value, headers[i].value_len);
            else if (std::string_view(headers[i].name, headers[i].name_len) == std::string_view("Content-Length"))
                req.content_length = std::string_view(headers[i].value, headers[i].value_len);
        }
    }

    if (req.type.size() > 0 && req.type == "POST") {
        auto pos = body.find("\r\n\r\n");
        if (pos == std::string_view::npos)
            return default_error_t{-32700, "Invalid JSON was received by the server."};
        req.body = body.substr(pos + 4);
    } else
        req.body = body;

    return req;
}

} // namespace unum::ucall
