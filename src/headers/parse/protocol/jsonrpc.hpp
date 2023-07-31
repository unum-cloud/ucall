#pragma once

#include <charconv>
#include <optional>

#include "containers.hpp"
#include "shared.hpp"

namespace unum::ucall {

template <typename base_protocol_t> struct jsonrpc_protocol_t {
    base_protocol_t base_proto;

    inline void prepare_response(exchange_pipes_t& pipes) noexcept;

    bool append_response(exchange_pipes_t&, std::string_view, std::string_view) noexcept;
    bool append_error(exchange_pipes_t&, std::string_view, std::string_view, std::string_view) noexcept;

    inline void finalize_response(exchange_pipes_t& pipes) noexcept;

    bool is_input_complete(span_gt<char> const& input) noexcept;

    inline void reset() noexcept;

    inline std::variant<parsed_request_t, default_error_t> parse(std::string_view body) const noexcept;
};
template <typename base_protocol_t>
inline void jsonrpc_protocol_t<base_protocol_t>::prepare_response(exchange_pipes_t& pipes) noexcept {
    base_proto.prepare_response(pipes);
}

template <typename base_protocol_t>
inline bool jsonrpc_protocol_t<base_protocol_t>::append_response(exchange_pipes_t& pipes, std::string_view request_id,
                                                                 std::string_view response) noexcept {
    // Communication example would be:
    // --> {"jsonrpc": "2.0", "method": "subtract", "params": [42, 23], "id": 1}
    // <-- {"jsonrpc": "2.0", "id": 1, "result": 19}
    if (!pipes.append_outputs({R"({"jsonrpc":"2.0","id":)", 22}))
        return false;
    if (!pipes.append_outputs(request_id))
        return false;
    if (!pipes.append_outputs({R"(,"result":)", 10}))
        return false;
    if (!pipes.append_outputs(response))
        return false;
    if (!pipes.append_outputs({R"(},)", 2}))
        return false;
    return true;
};

template <typename base_protocol_t>
inline bool jsonrpc_protocol_t<base_protocol_t>::append_error(exchange_pipes_t& pipes, std::string_view request_id,
                                                              std::string_view error_code,
                                                              std::string_view message) noexcept {
    // Communication example would be:
    // --> {"jsonrpc": "2.0", "method": "foobar", "id": "1"}
    // <-- {"jsonrpc": "2.0", "id": "1", "error": {"code": -32601, "message": "Method not found"}}
    if (!pipes.append_outputs({R"({"jsonrpc":"2.0","id":)", 22}))
        return false;
    if (!pipes.append_outputs(request_id))
        return false;
    if (!pipes.append_outputs({R"(,"error":{"code":)", 17}))
        return false;
    if (!pipes.append_outputs(error_code))
        return false;
    if (!pipes.append_outputs({R"(,"message":")", 12}))
        return false;
    if (!pipes.append_outputs(message))
        return false;
    if (!pipes.append_outputs({R"("}})", 3}))
        return false;
    return true;
};

template <typename base_protocol_t>
inline void jsonrpc_protocol_t<base_protocol_t>::finalize_response(exchange_pipes_t& pipes) noexcept {
    base_proto.finalize_response(pipes);
}

template <typename base_protocol_t>
bool jsonrpc_protocol_t<base_protocol_t>::is_input_complete(span_gt<char> const& input) noexcept {
    return base_proto.is_input_complete(input);
}

template <typename base_protocol_t> void jsonrpc_protocol_t<base_protocol_t>::reset() noexcept { base_proto.reset(); }

template <typename base_protocol_t>
inline std::variant<parsed_request_t, default_error_t>
jsonrpc_protocol_t<base_protocol_t>::parse(std::string_view body) const noexcept {
    return base_proto.parse(body);
}

} // namespace unum::ucall
