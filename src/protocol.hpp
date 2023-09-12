#pragma once
#include <variant>

#include "ucall/ucall.h"

#include "containers.hpp"
#include "protocol_http.hpp"
#include "protocol_jsonrpc.hpp"
#include "protocol_rest.hpp"
#include "protocol_tcp.hpp"
#include "shared.hpp"

namespace unum::ucall {

class protocol_t {
  private:
    using protocol_variants_t = std::variant<protocol_tcp_t, http_protocol_t, protocol_jsonrpc_t<protocol_tcp_t>,
                                             protocol_jsonrpc_t<http_protocol_t>, protocol_rest_t>;

    protocol_variants_t protocol_variant_;
    protocol_type_t protocol_type_;

  public:
    explicit protocol_t(protocol_type_t = protocol_type_t::tcp_k) noexcept;

    void reset_protocol(protocol_type_t) noexcept;

    void reset() noexcept;

    std::string_view get_content() const noexcept;
    request_type_t get_request_type() const noexcept;
    any_param_t get_param(size_t) const noexcept;
    any_param_t get_param(std::string_view) const noexcept;
    std::string_view get_header(std::string_view) const noexcept;

    void prepare_response(exchange_pipes_t&) noexcept;
    bool append_response(exchange_pipes_t&, std::string_view) noexcept;
    bool append_error(exchange_pipes_t&, std::string_view, std::string_view) noexcept;
    void finalize_response(exchange_pipes_t&) noexcept;

    bool is_input_complete(span_gt<char>) noexcept;

    std::optional<default_error_t> parse_headers(std::string_view) noexcept;
    std::optional<default_error_t> parse_content() noexcept;

    template <typename caller_at>
    std::optional<default_error_t> populate_response(exchange_pipes_t&, caller_at) noexcept;
};

protocol_t::protocol_t(protocol_type_t p_type) noexcept : protocol_variant_({}), protocol_type_(p_type) {
    reset_protocol(p_type);
}

void protocol_t::reset_protocol(protocol_type_t p_type) noexcept {
    protocol_type_ = p_type;
    switch (protocol_type_) {
    case protocol_type_t::tcp_k:
        protocol_variant_.emplace<protocol_tcp_t>();
        break;
    case protocol_type_t::http_k:
        protocol_variant_.emplace<http_protocol_t>();
        break;
    case protocol_type_t::jsonrpc_tcp_k:
        protocol_variant_.emplace<protocol_jsonrpc_t<protocol_tcp_t>>();
        break;
    case protocol_type_t::jsonrpc_http_k:
        protocol_variant_.emplace<protocol_jsonrpc_t<http_protocol_t>>();
        break;
    case protocol_type_t::rest_k:
        protocol_variant_.emplace<protocol_rest_t>();
        break;
    }
}

void protocol_t::reset() noexcept {
    switch (protocol_type_) {
    case protocol_type_t::tcp_k:
        return std::get<protocol_tcp_t>(protocol_variant_).reset();
    case protocol_type_t::http_k:
        return std::get<http_protocol_t>(protocol_variant_).reset();
    case protocol_type_t::jsonrpc_tcp_k:
        return std::get<protocol_jsonrpc_t<protocol_tcp_t>>(protocol_variant_).reset();
    case protocol_type_t::jsonrpc_http_k:
        return std::get<protocol_jsonrpc_t<http_protocol_t>>(protocol_variant_).reset();
    case protocol_type_t::rest_k:
        return std::get<protocol_rest_t>(protocol_variant_).reset();
    }
}

inline std::string_view protocol_t::get_content() const noexcept {
    switch (protocol_type_) {
    case protocol_type_t::tcp_k:
        return std::get<protocol_tcp_t>(protocol_variant_).get_content();
    case protocol_type_t::http_k:
        return std::get<http_protocol_t>(protocol_variant_).get_content();
    case protocol_type_t::jsonrpc_tcp_k:
        return std::get<protocol_jsonrpc_t<protocol_tcp_t>>(protocol_variant_).get_content();
    case protocol_type_t::jsonrpc_http_k:
        return std::get<protocol_jsonrpc_t<http_protocol_t>>(protocol_variant_).get_content();
    case protocol_type_t::rest_k:
        return std::get<protocol_rest_t>(protocol_variant_).get_content();
    }

    return {};
}

inline request_type_t protocol_t::get_request_type() const noexcept {
    switch (protocol_type_) {
    case protocol_type_t::tcp_k:
        return std::get<protocol_tcp_t>(protocol_variant_).get_request_type();
    case protocol_type_t::http_k:
        return std::get<http_protocol_t>(protocol_variant_).get_request_type();
    case protocol_type_t::jsonrpc_tcp_k:
        return std::get<protocol_jsonrpc_t<protocol_tcp_t>>(protocol_variant_).get_request_type();
    case protocol_type_t::jsonrpc_http_k:
        return std::get<protocol_jsonrpc_t<http_protocol_t>>(protocol_variant_).get_request_type();
    case protocol_type_t::rest_k:
        return std::get<protocol_rest_t>(protocol_variant_).get_request_type();
    }

    return request_type_t::post_k;
}

inline any_param_t protocol_t::get_param(size_t param_idx) const noexcept {
    switch (protocol_type_) {
    case protocol_type_t::tcp_k:
        return std::get<protocol_tcp_t>(protocol_variant_).get_param(param_idx);
    case protocol_type_t::http_k:
        return std::get<http_protocol_t>(protocol_variant_).get_param(param_idx);
    case protocol_type_t::jsonrpc_tcp_k:
        return std::get<protocol_jsonrpc_t<protocol_tcp_t>>(protocol_variant_).get_param(param_idx);
    case protocol_type_t::jsonrpc_http_k:
        return std::get<protocol_jsonrpc_t<http_protocol_t>>(protocol_variant_).get_param(param_idx);
    case protocol_type_t::rest_k:
        return std::get<protocol_rest_t>(protocol_variant_).get_param(param_idx);
    }

    return nullptr;
}

inline any_param_t protocol_t::get_param(std::string_view param_name) const noexcept {
    switch (protocol_type_) {
    case protocol_type_t::tcp_k:
        return std::get<protocol_tcp_t>(protocol_variant_).get_param(param_name);
    case protocol_type_t::http_k:
        return std::get<http_protocol_t>(protocol_variant_).get_param(param_name);
    case protocol_type_t::jsonrpc_tcp_k:
        return std::get<protocol_jsonrpc_t<protocol_tcp_t>>(protocol_variant_).get_param(param_name);
    case protocol_type_t::jsonrpc_http_k:
        return std::get<protocol_jsonrpc_t<http_protocol_t>>(protocol_variant_).get_param(param_name);
    case protocol_type_t::rest_k:
        return std::get<protocol_rest_t>(protocol_variant_).get_param(param_name);
    }

    return nullptr;
}

inline std::string_view protocol_t::get_header(std::string_view header_name) const noexcept {
    switch (protocol_type_) {
    case protocol_type_t::tcp_k:
        return std::get<protocol_tcp_t>(protocol_variant_).get_header(header_name);
    case protocol_type_t::http_k:
        return std::get<http_protocol_t>(protocol_variant_).get_header(header_name);
    case protocol_type_t::jsonrpc_tcp_k:
        return std::get<protocol_jsonrpc_t<protocol_tcp_t>>(protocol_variant_).get_header(header_name);
    case protocol_type_t::jsonrpc_http_k:
        return std::get<protocol_jsonrpc_t<http_protocol_t>>(protocol_variant_).get_header(header_name);
    case protocol_type_t::rest_k:
        return std::get<protocol_rest_t>(protocol_variant_).get_header(header_name);
    }

    return std::string_view();
}

void protocol_t::prepare_response(exchange_pipes_t& pipes) noexcept {
    switch (protocol_type_) {
    case protocol_type_t::tcp_k:
        return std::get<protocol_tcp_t>(protocol_variant_).prepare_response(pipes);
    case protocol_type_t::http_k:
        return std::get<http_protocol_t>(protocol_variant_).prepare_response(pipes);
    case protocol_type_t::jsonrpc_tcp_k:
        return std::get<protocol_jsonrpc_t<protocol_tcp_t>>(protocol_variant_).prepare_response(pipes);
    case protocol_type_t::jsonrpc_http_k:
        return std::get<protocol_jsonrpc_t<http_protocol_t>>(protocol_variant_).prepare_response(pipes);
    case protocol_type_t::rest_k:
        return std::get<protocol_rest_t>(protocol_variant_).prepare_response(pipes);
    }
}

bool protocol_t::append_response(exchange_pipes_t& pipes, std::string_view response) noexcept {
    switch (protocol_type_) {
    case protocol_type_t::tcp_k:
        return std::get<protocol_tcp_t>(protocol_variant_).append_response(pipes, response);
    case protocol_type_t::http_k:
        return std::get<http_protocol_t>(protocol_variant_).append_response(pipes, response);
    case protocol_type_t::jsonrpc_tcp_k:
        return std::get<protocol_jsonrpc_t<protocol_tcp_t>>(protocol_variant_).append_response(pipes, response);
    case protocol_type_t::jsonrpc_http_k:
        return std::get<protocol_jsonrpc_t<http_protocol_t>>(protocol_variant_).append_response(pipes, response);
    case protocol_type_t::rest_k:
        return std::get<protocol_rest_t>(protocol_variant_).append_response(pipes, response);
    }
    return false;
}

bool protocol_t::append_error(exchange_pipes_t& pipes, std::string_view error_code,
                              std::string_view response) noexcept {
    switch (protocol_type_) {
    case protocol_type_t::tcp_k:
        return std::get<protocol_tcp_t>(protocol_variant_).append_error(pipes, error_code, response);
    case protocol_type_t::http_k:
        return std::get<http_protocol_t>(protocol_variant_).append_error(pipes, error_code, response);
    case protocol_type_t::jsonrpc_tcp_k:
        return std::get<protocol_jsonrpc_t<protocol_tcp_t>>(protocol_variant_)
            .append_error(pipes, error_code, response);
    case protocol_type_t::jsonrpc_http_k:
        return std::get<protocol_jsonrpc_t<http_protocol_t>>(protocol_variant_)
            .append_error(pipes, error_code, response);
    case protocol_type_t::rest_k:
        return std::get<protocol_rest_t>(protocol_variant_).append_error(pipes, error_code, response);
    }
    return false;
}

void protocol_t::finalize_response(exchange_pipes_t& pipes) noexcept {
    switch (protocol_type_) {
    case protocol_type_t::tcp_k:
        return std::get<protocol_tcp_t>(protocol_variant_).finalize_response(pipes);
    case protocol_type_t::http_k:
        return std::get<http_protocol_t>(protocol_variant_).finalize_response(pipes);
    case protocol_type_t::jsonrpc_tcp_k:
        return std::get<protocol_jsonrpc_t<protocol_tcp_t>>(protocol_variant_).finalize_response(pipes);
    case protocol_type_t::jsonrpc_http_k:
        return std::get<protocol_jsonrpc_t<http_protocol_t>>(protocol_variant_).finalize_response(pipes);
    case protocol_type_t::rest_k:
        return std::get<protocol_rest_t>(protocol_variant_).finalize_response(pipes);
    }
}

bool protocol_t::is_input_complete(span_gt<char> input) noexcept {
    switch (protocol_type_) {
    case protocol_type_t::tcp_k:
        return std::get<protocol_tcp_t>(protocol_variant_).is_input_complete(input);
    case protocol_type_t::http_k:
        return std::get<http_protocol_t>(protocol_variant_).is_input_complete(input);
    case protocol_type_t::jsonrpc_tcp_k:
        return std::get<protocol_jsonrpc_t<protocol_tcp_t>>(protocol_variant_).is_input_complete(input);
    case protocol_type_t::jsonrpc_http_k:
        return std::get<protocol_jsonrpc_t<http_protocol_t>>(protocol_variant_).is_input_complete(input);
    case protocol_type_t::rest_k:
        return std::get<protocol_rest_t>(protocol_variant_).is_input_complete(input);
    }
    return true;
}

std::optional<default_error_t> protocol_t::parse_headers(std::string_view body) noexcept {
    switch (protocol_type_) {
    case protocol_type_t::tcp_k:
        return std::get<protocol_tcp_t>(protocol_variant_).parse_headers(body);
    case protocol_type_t::http_k:
        return std::get<http_protocol_t>(protocol_variant_).parse_headers(body);
    case protocol_type_t::jsonrpc_tcp_k:
        return std::get<protocol_jsonrpc_t<protocol_tcp_t>>(protocol_variant_).parse_headers(body);
    case protocol_type_t::jsonrpc_http_k:
        return std::get<protocol_jsonrpc_t<http_protocol_t>>(protocol_variant_).parse_headers(body);
    case protocol_type_t::rest_k:
        return std::get<protocol_rest_t>(protocol_variant_).parse_headers(body);
    }

    return default_error_t{-1, "Unknown"};
}

std::optional<default_error_t> protocol_t::parse_content() noexcept {
    switch (protocol_type_) {
    case protocol_type_t::tcp_k:
        return std::get<protocol_tcp_t>(protocol_variant_).parse_content();
    case protocol_type_t::http_k:
        return std::get<http_protocol_t>(protocol_variant_).parse_content();
    case protocol_type_t::jsonrpc_tcp_k:
        return std::get<protocol_jsonrpc_t<protocol_tcp_t>>(protocol_variant_).parse_content();
    case protocol_type_t::jsonrpc_http_k:
        return std::get<protocol_jsonrpc_t<http_protocol_t>>(protocol_variant_).parse_content();
    case protocol_type_t::rest_k:
        return std::get<protocol_rest_t>(protocol_variant_).parse_content();
    }

    return default_error_t{-1, "Unknown"};
}

template <typename caller_at>
inline std::optional<default_error_t> protocol_t::populate_response(exchange_pipes_t& pipes,
                                                                    caller_at caller) noexcept {
    switch (protocol_type_) {
    case protocol_type_t::tcp_k:
        return std::get<protocol_tcp_t>(protocol_variant_).populate_response(pipes, caller);
    case protocol_type_t::http_k:
        return std::get<http_protocol_t>(protocol_variant_).populate_response(pipes, caller);
    case protocol_type_t::jsonrpc_tcp_k:
        return std::get<protocol_jsonrpc_t<protocol_tcp_t>>(protocol_variant_).populate_response(pipes, caller);
    case protocol_type_t::jsonrpc_http_k:
        return std::get<protocol_jsonrpc_t<http_protocol_t>>(protocol_variant_).populate_response(pipes, caller);
    case protocol_type_t::rest_k:
        return std::get<protocol_rest_t>(protocol_variant_).populate_response(pipes, caller);
    }

    return default_error_t{-1, "Unknown"};
}

} // namespace unum::ucall
