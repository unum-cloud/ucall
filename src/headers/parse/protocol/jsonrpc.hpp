#pragma once

#include <charconv>
#include <optional>

#include "contain/pipe.hpp"
#include "contain/span.hpp"
#include "parse/protocol/protocol_concept.hpp"
#include "shared.hpp"

namespace unum::ucall {

struct jsonrpc_protocol_t {
    protocol_t base_proto;

    explicit jsonrpc_protocol_t(protocol_type_t type) noexcept : base_proto(type){};

    inline void prepare_response(exchange_pipes_t& pipes) noexcept;

    inline void finalize_response(exchange_pipes_t& pipes) noexcept;

    bool is_input_complete(span_gt<char> const& input) noexcept;

    inline void reset() noexcept;

    inline std::variant<parsed_request_t, default_error_t> parse(std::string_view body) const noexcept;
};

inline void jsonrpc_protocol_t::prepare_response(exchange_pipes_t& pipes) noexcept {
    base_proto.prepare_response(pipes);
}

inline void jsonrpc_protocol_t::finalize_response(exchange_pipes_t& pipes) noexcept {
    base_proto.finalize_response(pipes);
}

bool jsonrpc_protocol_t::is_input_complete(span_gt<char> const& input) noexcept {
    return base_proto.is_input_complete(input);
}

void jsonrpc_protocol_t::reset() noexcept { base_proto.reset(); }

inline std::variant<parsed_request_t, default_error_t> jsonrpc_protocol_t::parse(std::string_view body) const noexcept {
    return base_proto.parse(body);
}

} // namespace unum::ucall
