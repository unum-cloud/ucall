#pragma once

#include "parse/protocol/http.hpp"
#include "parse/protocol/jsonrpc.hpp"
#include "parse/protocol/protocol_concept.hpp"
#include "parse/protocol/tcp.hpp"
#include "shared.hpp"
#include "ucall/ucall.h"

namespace unum::ucall {

protocol_t::protocol_t(protocol_type_t p_type) noexcept : sp_proto(nullptr), type(p_type) { reset_protocol(p_type); }

void protocol_t::reset_protocol(protocol_type_t p_type) noexcept {
    type = p_type;
    switch (type) {
    case protocol_type_t::TCP:
        sp_proto.emplace<tcp_protocol_t>();
        break;
    case protocol_type_t::HTTP:
        sp_proto.emplace<http_protocol_t>();
        break;
    case protocol_type_t::JSONRPC_TCP:
        sp_proto.emplace<jsonrpc_protocol_t>(protocol_type_t::TCP);
        break;
    case protocol_type_t::JSONRPC_HTTP:
        sp_proto.emplace<jsonrpc_protocol_t>(protocol_type_t::HTTP);
        break;
    }
}

void protocol_t::reset() noexcept {
    switch (type) {
    case protocol_type_t::TCP:
        std::any_cast<tcp_protocol_t&>(sp_proto).reset();
        break;
    case protocol_type_t::HTTP:
        std::any_cast<http_protocol_t&>(sp_proto).reset();
        break;
    case protocol_type_t::JSONRPC_TCP:
    case protocol_type_t::JSONRPC_HTTP:
        std::any_cast<jsonrpc_protocol_t&>(sp_proto).reset();
        break;
    }
}

void protocol_t::prepare_response(exchange_pipes_t& pipes) noexcept {
    switch (type) {
    case protocol_type_t::TCP:
        std::any_cast<tcp_protocol_t&>(sp_proto).prepare_response(pipes);
        break;
    case protocol_type_t::HTTP:
        std::any_cast<http_protocol_t&>(sp_proto).prepare_response(pipes);
        break;
    case protocol_type_t::JSONRPC_TCP:
    case protocol_type_t::JSONRPC_HTTP:
        std::any_cast<jsonrpc_protocol_t&>(sp_proto).prepare_response(pipes);
        break;
    }
}

void protocol_t::finalize_response(exchange_pipes_t& pipes) noexcept {
    switch (type) {
    case protocol_type_t::TCP:
        std::any_cast<tcp_protocol_t&>(sp_proto).finalize_response(pipes);
        break;
    case protocol_type_t::HTTP:
        std::any_cast<http_protocol_t&>(sp_proto).finalize_response(pipes);
        break;
    case protocol_type_t::JSONRPC_TCP:
    case protocol_type_t::JSONRPC_HTTP:
        std::any_cast<jsonrpc_protocol_t&>(sp_proto).finalize_response(pipes);
        break;
    }
};

bool protocol_t::is_input_complete(span_gt<char> const& input) noexcept {
    switch (type) {
    case protocol_type_t::TCP:
        return std::any_cast<tcp_protocol_t&>(sp_proto).is_input_complete(input);
    case protocol_type_t::HTTP:
        return std::any_cast<http_protocol_t&>(sp_proto).is_input_complete(input);
    case protocol_type_t::JSONRPC_TCP:
    case protocol_type_t::JSONRPC_HTTP:
        return std::any_cast<jsonrpc_protocol_t&>(sp_proto).is_input_complete(input);
    }
};

std::variant<parsed_request_t, default_error_t> protocol_t::parse(std::string_view body) const noexcept {
    switch (type) {
    case protocol_type_t::TCP:
        return std::any_cast<tcp_protocol_t const&>(sp_proto).parse(body);
    case protocol_type_t::HTTP:
        return std::any_cast<http_protocol_t const&>(sp_proto).parse(body);
    case protocol_type_t::JSONRPC_TCP:
    case protocol_type_t::JSONRPC_HTTP:
        return std::any_cast<jsonrpc_protocol_t const&>(sp_proto).parse(body);
    }
}

} // namespace unum::ucall
