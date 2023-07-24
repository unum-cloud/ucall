#pragma once

#include <charconv>
#include <optional>

#include "contain/pipe.hpp"
#include "contain/span.hpp"
#include "parse/protocol.hpp"
#include "shared.hpp"

namespace unum::ucall {

struct tcp_protocol_t {
    /// @brief Expected reception length
    std::optional<std::size_t> content_length{};

    inline void prepare_response(exchange_pipes_t& pipes) noexcept;

    inline void finalize_response(exchange_pipes_t& pipes) noexcept;

    bool is_input_complete(span_gt<char> const& input) noexcept;

    inline std::variant<parsed_request_t, default_error_t> parse(std::string_view body) const noexcept;
};

inline void tcp_protocol_t::prepare_response(exchange_pipes_t& pipes) noexcept {}

inline void tcp_protocol_t::finalize_response(exchange_pipes_t& pipes) noexcept {}

bool tcp_protocol_t::is_input_complete(span_gt<char> const& input) noexcept {
    if (!content_length) {
        content_length = input.size();
        return false;
    }
    return content_length == input.size();
}

inline std::variant<parsed_request_t, default_error_t> tcp_protocol_t::parse(std::string_view body) const noexcept {
    parsed_request_t req{};
    req.body = body;
    return req;
}

} // namespace unum::ucall
