#pragma once

#include <string_view>
#include <variant>

#include <picohttpparser.h>

#include "helpers/shared.hpp"

namespace unum::ucall {

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

bool set_http_content_length(char* headers, size_t content_len) {
    auto res = std::to_chars(headers + http_header_length_offset_k,
                             headers + http_header_length_offset_k + http_header_length_capacity_k, content_len);

    if (res.ec != std::errc())
        return false;
    return true;
}

} // namespace unum::ucall
