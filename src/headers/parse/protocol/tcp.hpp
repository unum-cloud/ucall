#pragma once

#include <charconv>
#include <optional>

#include "contain/pipe.hpp"
#include "contain/span.hpp"
#include "shared.hpp"

namespace unum::ucall {

struct tcp_protocol_t {
    static constexpr char tcp_termination_symbol = '\0';

    inline void prepare_response(exchange_pipes_t& pipes) noexcept;

    inline void finalize_response(exchange_pipes_t& pipes) noexcept;

    bool is_input_complete(span_gt<char> const& input) noexcept;

    inline void reset() noexcept;

    inline std::variant<parsed_request_t, default_error_t> parse(std::string_view body) const noexcept;
};

inline void tcp_protocol_t::prepare_response(exchange_pipes_t& pipes) noexcept {}

inline void tcp_protocol_t::finalize_response(exchange_pipes_t& pipes) noexcept {
    pipes.push_back_reserved(tcp_termination_symbol);
}

bool tcp_protocol_t::is_input_complete(span_gt<char> const& input) noexcept {
    return input[input.size() - 1] == tcp_termination_symbol;
}

void tcp_protocol_t::reset() noexcept {}

inline std::variant<parsed_request_t, default_error_t> tcp_protocol_t::parse(std::string_view body) const noexcept {
    parsed_request_t req{};
    req.body = {body.begin(), body.size() - 1};
    return req;
}

} // namespace unum::ucall
