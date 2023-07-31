#pragma once

#include <variant>

#include "containers.hpp"
#include "parse/protocol/http.hpp"
#include "parse/protocol/jsonrpc.hpp"
#include "parse/protocol/tcp.hpp"
#include "shared.hpp"
#include "ucall/ucall.h"

namespace unum::ucall {

class protocol_t {
  private:
    using protocol_variants_t = std::variant<tcp_protocol_t, http_protocol_t, jsonrpc_protocol_t<tcp_protocol_t>,
                                             jsonrpc_protocol_t<http_protocol_t>>;

    protocol_variants_t sp_proto;
    protocol_type_t type;

  public:
    explicit protocol_t(protocol_type_t = protocol_type_t::TCP) noexcept;

    void reset_protocol(protocol_type_t) noexcept;

    void reset() noexcept;

    void prepare_response(exchange_pipes_t&) noexcept;

    bool append_response(exchange_pipes_t&, std::string_view, std::string_view) noexcept;
    bool append_error(exchange_pipes_t&, std::string_view, std::string_view, std::string_view) noexcept;

    void finalize_response(exchange_pipes_t&) noexcept;

    bool is_input_complete(span_gt<char> const&) noexcept;

    std::variant<parsed_request_t, default_error_t> parse(std::string_view) const noexcept;
};

protocol_t::protocol_t(protocol_type_t p_type) noexcept : sp_proto({}), type(p_type) { reset_protocol(p_type); }

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
        sp_proto.emplace<jsonrpc_protocol_t<tcp_protocol_t>>();
        break;
    case protocol_type_t::JSONRPC_HTTP:
        sp_proto.emplace<jsonrpc_protocol_t<http_protocol_t>>();
        break;
    }
}

void protocol_t::reset() noexcept {
    switch (type) {
    case protocol_type_t::TCP:
        std::get<tcp_protocol_t>(sp_proto).reset();
        break;
    case protocol_type_t::HTTP:
        std::get<http_protocol_t>(sp_proto).reset();
        break;
    case protocol_type_t::JSONRPC_TCP:
        std::get<jsonrpc_protocol_t<tcp_protocol_t>>(sp_proto).reset();
        break;
    case protocol_type_t::JSONRPC_HTTP:
        std::get<jsonrpc_protocol_t<http_protocol_t>>(sp_proto).reset();
        break;
    }
}

void protocol_t::prepare_response(exchange_pipes_t& pipes) noexcept {
    switch (type) {
    case protocol_type_t::TCP:
        std::get<tcp_protocol_t>(sp_proto).prepare_response(pipes);
        break;
    case protocol_type_t::HTTP:
        std::get<http_protocol_t>(sp_proto).prepare_response(pipes);
        break;
    case protocol_type_t::JSONRPC_TCP:
        std::get<jsonrpc_protocol_t<tcp_protocol_t>>(sp_proto).prepare_response(pipes);
        break;
    case protocol_type_t::JSONRPC_HTTP:
        std::get<jsonrpc_protocol_t<http_protocol_t>>(sp_proto).prepare_response(pipes);
        break;
    }
}

bool protocol_t::append_response(exchange_pipes_t& pipes, std::string_view request_id,
                                 std::string_view response) noexcept {
    switch (type) {
    case protocol_type_t::TCP:
        return std::get<tcp_protocol_t>(sp_proto).append_response(pipes, request_id, response);
    case protocol_type_t::HTTP:
        return std::get<http_protocol_t>(sp_proto).append_response(pipes, request_id, response);
    case protocol_type_t::JSONRPC_TCP:
        return std::get<jsonrpc_protocol_t<tcp_protocol_t>>(sp_proto).append_response(pipes, request_id, response);
    case protocol_type_t::JSONRPC_HTTP:
        return std::get<jsonrpc_protocol_t<http_protocol_t>>(sp_proto).append_response(pipes, request_id, response);
    }
    return false;
};

bool protocol_t::append_error(exchange_pipes_t& pipes, std::string_view request_id, std::string_view error_code,
                              std::string_view response) noexcept {
    switch (type) {
    case protocol_type_t::TCP:
        return std::get<tcp_protocol_t>(sp_proto).append_error(pipes, request_id, error_code, response);
    case protocol_type_t::HTTP:
        return std::get<http_protocol_t>(sp_proto).append_error(pipes, request_id, error_code, response);
    case protocol_type_t::JSONRPC_TCP:
        return std::get<jsonrpc_protocol_t<tcp_protocol_t>>(sp_proto).append_error(pipes, request_id, error_code,
                                                                                   response);
    case protocol_type_t::JSONRPC_HTTP:
        return std::get<jsonrpc_protocol_t<http_protocol_t>>(sp_proto).append_error(pipes, request_id, error_code,
                                                                                    response);
    }
    return false;
};

void protocol_t::finalize_response(exchange_pipes_t& pipes) noexcept {
    switch (type) {
    case protocol_type_t::TCP:
        std::get<tcp_protocol_t>(sp_proto).finalize_response(pipes);
        break;
    case protocol_type_t::HTTP:
        std::get<http_protocol_t>(sp_proto).finalize_response(pipes);
        break;
    case protocol_type_t::JSONRPC_TCP:
        std::get<jsonrpc_protocol_t<tcp_protocol_t>>(sp_proto).finalize_response(pipes);
        break;
    case protocol_type_t::JSONRPC_HTTP:
        std::get<jsonrpc_protocol_t<http_protocol_t>>(sp_proto).finalize_response(pipes);
        break;
    }
};

bool protocol_t::is_input_complete(span_gt<char> const& input) noexcept {
    switch (type) {
    case protocol_type_t::TCP:
        return std::get<tcp_protocol_t>(sp_proto).is_input_complete(input);
    case protocol_type_t::HTTP:
        return std::get<http_protocol_t>(sp_proto).is_input_complete(input);
    case protocol_type_t::JSONRPC_TCP:
        return std::get<jsonrpc_protocol_t<tcp_protocol_t>>(sp_proto).is_input_complete(input);
    case protocol_type_t::JSONRPC_HTTP:
        return std::get<jsonrpc_protocol_t<http_protocol_t>>(sp_proto).is_input_complete(input);
    }
    return true;
};

std::variant<parsed_request_t, default_error_t> protocol_t::parse(std::string_view body) const noexcept {
    switch (type) {
    case protocol_type_t::TCP:
        return std::get<tcp_protocol_t>(sp_proto).parse(body);
    case protocol_type_t::HTTP:
        return std::get<http_protocol_t>(sp_proto).parse(body);
    case protocol_type_t::JSONRPC_TCP:
        return std::get<jsonrpc_protocol_t<tcp_protocol_t>>(sp_proto).parse(body);
    case protocol_type_t::JSONRPC_HTTP:
        return std::get<jsonrpc_protocol_t<http_protocol_t>>(sp_proto).parse(body);
    }

    return default_error_t{-1, "Unknown"};
}

} // namespace unum::ucall
