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
    explicit protocol_t(protocol_type_t = protocol_type_t::tcp_k) noexcept;

    void reset_protocol(protocol_type_t) noexcept;

    void reset() noexcept;

    std::optional<default_error_t> set_to(sjd::element const&) noexcept;
    std::string_view get_content() const noexcept;
    std::string_view get_id() const noexcept;
    std::string_view get_method_name() const noexcept;
    request_type_t get_request_type() const noexcept;
    any_param_t get_param(size_t) const noexcept;
    any_param_t get_param(std::string_view) const noexcept;

    void prepare_response(exchange_pipes_t&) noexcept;
    bool append_response(exchange_pipes_t&, std::string_view, std::string_view) noexcept;
    bool append_error(exchange_pipes_t&, std::string_view, std::string_view, std::string_view) noexcept;
    void finalize_response(exchange_pipes_t&) noexcept;

    bool is_input_complete(span_gt<char>) noexcept;

    std::optional<default_error_t> parse_headers(std::string_view) noexcept;
    std::optional<default_error_t> parse_content(scratch_space_t&) noexcept;
};

protocol_t::protocol_t(protocol_type_t p_type) noexcept : sp_proto({}), type(p_type) { reset_protocol(p_type); }

void protocol_t::reset_protocol(protocol_type_t p_type) noexcept {
    type = p_type;
    switch (type) {
    case protocol_type_t::tcp_k:
        sp_proto.emplace<tcp_protocol_t>();
        break;
    case protocol_type_t::http_k:
        sp_proto.emplace<http_protocol_t>();
        break;
    case protocol_type_t::jsonrpc_tcp_k:
        sp_proto.emplace<jsonrpc_protocol_t<tcp_protocol_t>>();
        break;
    case protocol_type_t::jsonrpc_http_k:
        sp_proto.emplace<jsonrpc_protocol_t<http_protocol_t>>();
        break;
    }
}

void protocol_t::reset() noexcept {
    switch (type) {
    case protocol_type_t::tcp_k:
        std::get<tcp_protocol_t>(sp_proto).reset();
        break;
    case protocol_type_t::http_k:
        std::get<http_protocol_t>(sp_proto).reset();
        break;
    case protocol_type_t::jsonrpc_tcp_k:
        std::get<jsonrpc_protocol_t<tcp_protocol_t>>(sp_proto).reset();
        break;
    case protocol_type_t::jsonrpc_http_k:
        std::get<jsonrpc_protocol_t<http_protocol_t>>(sp_proto).reset();
        break;
    }
}

inline std::optional<default_error_t> protocol_t::set_to(sjd::element const& elm) noexcept {
    switch (type) {
    case protocol_type_t::tcp_k:
        return std::get<tcp_protocol_t>(sp_proto).set_to(elm);
    case protocol_type_t::http_k:
        return std::get<http_protocol_t>(sp_proto).set_to(elm);
    case protocol_type_t::jsonrpc_tcp_k:
        return std::get<jsonrpc_protocol_t<tcp_protocol_t>>(sp_proto).set_to(elm);
    case protocol_type_t::jsonrpc_http_k:
        return std::get<jsonrpc_protocol_t<http_protocol_t>>(sp_proto).set_to(elm);
    }

    return std::nullopt;
}

inline std::string_view protocol_t::get_content() const noexcept {
    switch (type) {
    case protocol_type_t::tcp_k:
        return std::get<tcp_protocol_t>(sp_proto).get_content();
    case protocol_type_t::http_k:
        return std::get<http_protocol_t>(sp_proto).get_content();
    case protocol_type_t::jsonrpc_tcp_k:
        return std::get<jsonrpc_protocol_t<tcp_protocol_t>>(sp_proto).get_content();
    case protocol_type_t::jsonrpc_http_k:
        return std::get<jsonrpc_protocol_t<http_protocol_t>>(sp_proto).get_content();
    }

    return {};
}

inline std::string_view protocol_t::get_id() const noexcept {
    switch (type) {
    case protocol_type_t::tcp_k:
        return std::get<tcp_protocol_t>(sp_proto).get_id();
    case protocol_type_t::http_k:
        return std::get<http_protocol_t>(sp_proto).get_id();
    case protocol_type_t::jsonrpc_tcp_k:
        return std::get<jsonrpc_protocol_t<tcp_protocol_t>>(sp_proto).get_id();
    case protocol_type_t::jsonrpc_http_k:
        return std::get<jsonrpc_protocol_t<http_protocol_t>>(sp_proto).get_id();
    }

    return {};
}

inline std::string_view protocol_t::get_method_name() const noexcept {
    switch (type) {
    case protocol_type_t::tcp_k:
        return std::get<tcp_protocol_t>(sp_proto).get_method_name();
    case protocol_type_t::http_k:
        return std::get<http_protocol_t>(sp_proto).get_method_name();
    case protocol_type_t::jsonrpc_tcp_k:
        return std::get<jsonrpc_protocol_t<tcp_protocol_t>>(sp_proto).get_method_name();
    case protocol_type_t::jsonrpc_http_k:
        return std::get<jsonrpc_protocol_t<http_protocol_t>>(sp_proto).get_method_name();
    }

    return {};
}

inline request_type_t protocol_t::get_request_type() const noexcept {
    switch (type) {
    case protocol_type_t::tcp_k:
        return std::get<tcp_protocol_t>(sp_proto).get_request_type();
    case protocol_type_t::http_k:
        return std::get<http_protocol_t>(sp_proto).get_request_type();
    case protocol_type_t::jsonrpc_tcp_k:
        return std::get<jsonrpc_protocol_t<tcp_protocol_t>>(sp_proto).get_request_type();
    case protocol_type_t::jsonrpc_http_k:
        return std::get<jsonrpc_protocol_t<http_protocol_t>>(sp_proto).get_request_type();
    }

    return request_type_t::post_k;
}

inline any_param_t protocol_t::get_param(size_t param_idx) const noexcept {
    switch (type) {
    case protocol_type_t::tcp_k:
        return std::get<tcp_protocol_t>(sp_proto).get_param(param_idx);
    case protocol_type_t::http_k:
        return std::get<http_protocol_t>(sp_proto).get_param(param_idx);
    case protocol_type_t::jsonrpc_tcp_k:
        return std::get<jsonrpc_protocol_t<tcp_protocol_t>>(sp_proto).get_param(param_idx);
    case protocol_type_t::jsonrpc_http_k:
        return std::get<jsonrpc_protocol_t<http_protocol_t>>(sp_proto).get_param(param_idx);
    }

    return nullptr;
}

inline any_param_t protocol_t::get_param(std::string_view param_name) const noexcept {
    switch (type) {
    case protocol_type_t::tcp_k:
        return std::get<tcp_protocol_t>(sp_proto).get_param(param_name);
    case protocol_type_t::http_k:
        return std::get<http_protocol_t>(sp_proto).get_param(param_name);
    case protocol_type_t::jsonrpc_tcp_k:
        return std::get<jsonrpc_protocol_t<tcp_protocol_t>>(sp_proto).get_param(param_name);
    case protocol_type_t::jsonrpc_http_k:
        return std::get<jsonrpc_protocol_t<http_protocol_t>>(sp_proto).get_param(param_name);
    }

    return nullptr;
}

void protocol_t::prepare_response(exchange_pipes_t& pipes) noexcept {
    switch (type) {
    case protocol_type_t::tcp_k:
        std::get<tcp_protocol_t>(sp_proto).prepare_response(pipes);
        break;
    case protocol_type_t::http_k:
        std::get<http_protocol_t>(sp_proto).prepare_response(pipes);
        break;
    case protocol_type_t::jsonrpc_tcp_k:
        std::get<jsonrpc_protocol_t<tcp_protocol_t>>(sp_proto).prepare_response(pipes);
        break;
    case protocol_type_t::jsonrpc_http_k:
        std::get<jsonrpc_protocol_t<http_protocol_t>>(sp_proto).prepare_response(pipes);
        break;
    }
}

bool protocol_t::append_response(exchange_pipes_t& pipes, std::string_view request_id,
                                 std::string_view response) noexcept {
    switch (type) {
    case protocol_type_t::tcp_k:
        return std::get<tcp_protocol_t>(sp_proto).append_response(pipes, request_id, response);
    case protocol_type_t::http_k:
        return std::get<http_protocol_t>(sp_proto).append_response(pipes, request_id, response);
    case protocol_type_t::jsonrpc_tcp_k:
        return std::get<jsonrpc_protocol_t<tcp_protocol_t>>(sp_proto).append_response(pipes, request_id, response);
    case protocol_type_t::jsonrpc_http_k:
        return std::get<jsonrpc_protocol_t<http_protocol_t>>(sp_proto).append_response(pipes, request_id, response);
    }
    return false;
};

bool protocol_t::append_error(exchange_pipes_t& pipes, std::string_view request_id, std::string_view error_code,
                              std::string_view response) noexcept {
    switch (type) {
    case protocol_type_t::tcp_k:
        return std::get<tcp_protocol_t>(sp_proto).append_error(pipes, request_id, error_code, response);
    case protocol_type_t::http_k:
        return std::get<http_protocol_t>(sp_proto).append_error(pipes, request_id, error_code, response);
    case protocol_type_t::jsonrpc_tcp_k:
        return std::get<jsonrpc_protocol_t<tcp_protocol_t>>(sp_proto).append_error(pipes, request_id, error_code,
                                                                                   response);
    case protocol_type_t::jsonrpc_http_k:
        return std::get<jsonrpc_protocol_t<http_protocol_t>>(sp_proto).append_error(pipes, request_id, error_code,
                                                                                    response);
    }
    return false;
};

void protocol_t::finalize_response(exchange_pipes_t& pipes) noexcept {
    switch (type) {
    case protocol_type_t::tcp_k:
        std::get<tcp_protocol_t>(sp_proto).finalize_response(pipes);
        break;
    case protocol_type_t::http_k:
        std::get<http_protocol_t>(sp_proto).finalize_response(pipes);
        break;
    case protocol_type_t::jsonrpc_tcp_k:
        std::get<jsonrpc_protocol_t<tcp_protocol_t>>(sp_proto).finalize_response(pipes);
        break;
    case protocol_type_t::jsonrpc_http_k:
        std::get<jsonrpc_protocol_t<http_protocol_t>>(sp_proto).finalize_response(pipes);
        break;
    }
};

bool protocol_t::is_input_complete(span_gt<char> input) noexcept {
    switch (type) {
    case protocol_type_t::tcp_k:
        return std::get<tcp_protocol_t>(sp_proto).is_input_complete(input);
    case protocol_type_t::http_k:
        return std::get<http_protocol_t>(sp_proto).is_input_complete(input);
    case protocol_type_t::jsonrpc_tcp_k:
        return std::get<jsonrpc_protocol_t<tcp_protocol_t>>(sp_proto).is_input_complete(input);
    case protocol_type_t::jsonrpc_http_k:
        return std::get<jsonrpc_protocol_t<http_protocol_t>>(sp_proto).is_input_complete(input);
    }
    return true;
};

std::optional<default_error_t> protocol_t::parse_headers(std::string_view body) noexcept {
    switch (type) {
    case protocol_type_t::tcp_k:
        return std::get<tcp_protocol_t>(sp_proto).parse_headers(body);
    case protocol_type_t::http_k:
        return std::get<http_protocol_t>(sp_proto).parse_headers(body);
    case protocol_type_t::jsonrpc_tcp_k:
        return std::get<jsonrpc_protocol_t<tcp_protocol_t>>(sp_proto).parse_headers(body);
    case protocol_type_t::jsonrpc_http_k:
        return std::get<jsonrpc_protocol_t<http_protocol_t>>(sp_proto).parse_headers(body);
    }

    return default_error_t{-1, "Unknown"};
}

std::optional<default_error_t> protocol_t::parse_content(scratch_space_t& scratch) noexcept {
    switch (type) {
    case protocol_type_t::tcp_k:
        return std::get<tcp_protocol_t>(sp_proto).parse_content(scratch);
    case protocol_type_t::http_k:
        return std::get<http_protocol_t>(sp_proto).parse_content(scratch);
    case protocol_type_t::jsonrpc_tcp_k:
        return std::get<jsonrpc_protocol_t<tcp_protocol_t>>(sp_proto).parse_content(scratch);
    case protocol_type_t::jsonrpc_http_k:
        return std::get<jsonrpc_protocol_t<http_protocol_t>>(sp_proto).parse_content(scratch);
    }

    return default_error_t{-1, "Unknown"};
}

} // namespace unum::ucall
