#pragma once

#include <charconv>
#include <optional>

#include "containers.hpp"
#include "shared.hpp"

namespace unum::ucall {

struct tcp_protocol_t {
    static constexpr char tcp_termination_symbol = '\0';

    inline void prepare_response(exchange_pipes_t& pipes) noexcept;

    inline bool append_response(exchange_pipes_t&, std::string_view, std::string_view) noexcept;
    inline bool append_error(exchange_pipes_t&, std::string_view, std::string_view, std::string_view) noexcept;

    inline void finalize_response(exchange_pipes_t& pipes) noexcept;

    bool is_input_complete(span_gt<char> const& input) noexcept;

    inline void reset() noexcept;

    inline std::variant<parsed_request_t, default_error_t> parse(std::string_view body) const noexcept;
};

inline void tcp_protocol_t::prepare_response(exchange_pipes_t& pipes) noexcept {}

inline bool tcp_protocol_t::append_response(exchange_pipes_t& pipes, std::string_view,
                                            std::string_view response) noexcept {
    return pipes.append_outputs(response);
};

inline bool tcp_protocol_t::append_error(exchange_pipes_t& pipes, std::string_view error_code, std::string_view,
                                         std::string_view message) noexcept {
    return pipes.append_outputs(error_code);
};

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
