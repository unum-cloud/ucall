#pragma once
#include <variant>

#include <simdjson.h>

#include "shared.hpp"

#include <picohttpparser.h>

namespace unum::ujrpc {

namespace sj = simdjson;
namespace sjd = sj::dom;

struct scratch_space_t {
    char json_pointer[json_pointer_capacity_k]{};
    sjd::parser parser{};
    sjd::element tree{};
    std::string_view id{};
    bool is_batch{};
    bool is_async{};

    sjd::parser* dynamic_parser{};
    std::string_view dynamic_packet{};
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
inline std::variant<ujrpc_callback_t, default_error_t> find_callback(named_callbacks_at const& callbacks,
                                                                     scratch_space_t& scratch) noexcept {
    sjd::element const& doc = scratch.tree;
    if (!doc.is_object())
        return default_error_t{-32600, "The JSON sent is not a valid request object."};

    // We don't support JSON-RPC before version 2.0.
    sj::simdjson_result<sjd::element> version = doc["jsonrpc"];
    if (!version.is_string() || version.get_string().value() != "2.0")
        return default_error_t{-32600, "The request doesn't specify the 2.0 version."};

    // Check if the shape of the requst is correct:
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

    // TODO: Patch SIMD-JSON to extract the token
    scratch.id = id.error() == sj::SUCCESS ? "null" : "";

    // Make sure we have such a method:
    auto method_name = method.get_string().value_unsafe();
    auto callbacks_end = callbacks.data() + callbacks.size();
    auto callback_it = std::find_if(callbacks.data(), callbacks_end, [=](named_callback_t const& callback) noexcept {
        return callback.name == method_name;
    });
    if (callback_it == callbacks_end)
        return default_error_t{-32601, "Method not found."};

    return callback_it->callback;
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
inline std::variant<parsed_request_t, default_error_t> strip_http_headers(std::string_view body) noexcept {
    // A typical HTTP-header may look like this
    // POST /myservice HTTP/1.1
    // Host: rpc.example.com
    // Content-Type: application/json
    // Content-Length: ...
    // Accept: application/json
    parsed_request_t req;

    const size_t header_cnt = 32;
    const char* method;
    size_t method_len;
    const char* path;
    size_t path_len;
    int minor_version;
    phr_header headers[header_cnt];
    size_t num_headers;

    int res = phr_parse_request(body.data(), body.size(), &method, &method_len, &path, &path_len, &minor_version,
                                headers, &num_headers, 0);

    if (res == -2)
        return default_error_t{-2, "Partial HTTP request"};

    if (res > 0) {
        req.type = std::string_view(method, method_len);
        for (std::size_t i = 0; i < header_cnt; ++i) {
            if (headers[i].name_len == 0)
                continue;
            if (headers[i].name == "Keep-Alive")
                req.keep_alive = std::string_view(headers[i].value, headers[i].value_len);
            else if (headers[i].name == "Keep-Alive")
                req.keep_alive = std::string_view(headers[i].value, headers[i].value_len);
            else if (headers[i].name == "Content-Type")
                req.content_type = std::string_view(headers[i].value, headers[i].value_len);
            else if (headers[i].name == "Content-Length")
                req.content_length = std::string_view(headers[i].value, headers[i].value_len);
        }
    }

    if (req.type.size() > 0 && req.type == "POST") {
        auto pos = body.find("\r\n\r\n");
        if (pos == std::string_view::npos)
            return default_error_t{-32700, "Invalid JSON was received by the server."};
        req.body = body.substr(pos);
    } else
        req.body = body;

    return req;
}

} // namespace unum::ujrpc
