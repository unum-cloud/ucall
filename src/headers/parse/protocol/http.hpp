#pragma once

#include <charconv>
#include <optional>
#include <picohttpparser.h>
#include <string_view>
#include <variant>

#include "containers.hpp"
#include "shared.hpp"

namespace unum::ucall {

static constexpr char const* http_header_k =
    "HTTP/1.1 200 OK\r\nContent-Length:          \r\nContent-Type: application/json\r\n\r\n";
static constexpr std::size_t http_header_size_k = 78;
static constexpr std::size_t http_header_length_offset_k = 33;
static constexpr std::size_t http_header_length_capacity_k = 9;

struct http_protocol_t {
    size_t body_size;
    /// @brief Expected reception length extracted from HTTP headers.
    std::optional<std::size_t> content_length{};
    // /// @brief Absolute time extracted from HTTP headers, for the requested lifetime of this channel.
    // std::optional<ssize_t> keep_alive{};
    // /// @brief Expected MIME type of payload extracted from HTTP headers. Generally "application/json".
    // std::optional<std::string_view> content_type{};

    inline void prepare_response(exchange_pipes_t& pipes) noexcept;

    inline bool append_response(exchange_pipes_t&, std::string_view, std::string_view) noexcept;
    inline bool append_error(exchange_pipes_t&, std::string_view, std::string_view, std::string_view) noexcept;

    inline void finalize_response(exchange_pipes_t& pipes) noexcept;

    inline void reset() noexcept;

    bool is_input_complete(span_gt<char> input) noexcept;

    /**
     * @brief Analyzes the contents of the packet, bifurcating pure JSON-RPC from HTTP1-based.
     * @warning This doesn't check the headers for validity or additional metadata.
     */
    inline std::variant<parsed_request_t, default_error_t> parse(std::string_view body) const noexcept;
};

inline void http_protocol_t::prepare_response(exchange_pipes_t& pipes) noexcept {
    pipes.append_reserved(http_header_k, http_header_size_k);
    body_size = pipes.output_span().size();
}

inline bool http_protocol_t::append_response(exchange_pipes_t& pipes, std::string_view,
                                             std::string_view response) noexcept {
    return pipes.append_outputs(response);
};

inline bool http_protocol_t::append_error(exchange_pipes_t& pipes, std::string_view error_code, std::string_view,
                                          std::string_view message) noexcept {
    return pipes.append_outputs(error_code);
};

inline void http_protocol_t::finalize_response(exchange_pipes_t& pipes) noexcept {
    auto output = pipes.output_span();
    body_size = output.size() - body_size;
    auto res = std::to_chars(output.data() + http_header_length_offset_k,
                             output.data() + http_header_length_offset_k + http_header_length_capacity_k, body_size);

    if (res.ec != std::errc()) {
        // TODO Return error
    }
}

void http_protocol_t::reset() noexcept { content_length.reset(); }

bool http_protocol_t::is_input_complete(span_gt<char> input) noexcept {

    if (!content_length) {
        size_t bytes_expected = 0;

        auto json_or_error = parse(std::string_view(input.data(), input.size()));
        if (auto error_ptr = std::get_if<default_error_t>(&json_or_error); error_ptr)
            return false;
        parsed_request_t request = std::get<parsed_request_t>(json_or_error);

        auto res = std::from_chars(request.content_length.data(),
                                   request.content_length.data() + request.content_length.size(), bytes_expected);
        bytes_expected += (request.body.data() - input.data());

        content_length = bytes_expected;
    }

    return input.size() >= content_length;
}

/**
 * @brief Analyzes the contents of the packet, bifurcating pure JSON-RPC from HTTP1-based.
 * @warning This doesn't check the headers for validity or additional metadata.
 */
inline std::variant<parsed_request_t, default_error_t> http_protocol_t::parse(std::string_view body) const noexcept {
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
    } else if (req.type.size() == 0)
        return default_error_t{-2, "Partial HTTP request"};

    return req;
}

} // namespace unum::ucall
