#pragma once

#include "parse/http.hpp"
#include "parse/tcp.hpp"
#include "shared.hpp"
#include "ucall/ucall.h"

namespace unum::ucall {
struct protocol_t {
    union sp_proto {
        tcp_protocol_t tcp;
        http_protocol_t http;
    } sp_proto;
    protocol_type_t type;

    explicit protocol_t(protocol_type_t p_type = protocol_type_t::TCP) noexcept : sp_proto({}), type(p_type) {
        reset();
    }

    void reset_protocol(protocol_type_t p_type) noexcept {
        type = p_type;
        reset();
    }

    void reset() noexcept {
        switch (type) {
        case protocol_type_t::TCP:
            sp_proto.tcp = tcp_protocol_t();
            break;
        case protocol_type_t::HTTP:
            sp_proto.http = http_protocol_t();
            break;
        }
    }

    inline void prepare_response(exchange_pipes_t& pipes) noexcept {
        switch (type) {
        case protocol_type_t::TCP:
            return sp_proto.tcp.prepare_response(pipes);
        case protocol_type_t::HTTP:
            return sp_proto.http.prepare_response(pipes);
        }
    }

    inline void finalize_response(exchange_pipes_t& pipes) noexcept {
        switch (type) {
        case protocol_type_t::TCP:
            return sp_proto.tcp.finalize_response(pipes);
        case protocol_type_t::HTTP:
            return sp_proto.http.finalize_response(pipes);
        }
    };

    inline bool is_input_complete(span_gt<char> input) noexcept {
        switch (type) {
        case protocol_type_t::TCP:
            return sp_proto.tcp.is_input_complete(input);
        case protocol_type_t::HTTP:
            return sp_proto.http.is_input_complete(input);
        default:
            return true;
        }
    };

    inline std::variant<parsed_request_t, default_error_t> parse(std::string_view body) const noexcept {
        switch (type) {
        case protocol_type_t::TCP:
            return sp_proto.tcp.parse(body);
        case protocol_type_t::HTTP:
            return sp_proto.http.parse(body);
        default:
            return default_error_t{-1, "Invalid Protocol Type"};
        }
    }
};
} // namespace unum::ucall
