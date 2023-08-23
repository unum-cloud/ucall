#pragma once

#include <atomic>
#include <optional>

#include "connection.hpp"
#include "containers.hpp"
#include "log.hpp"
#include "network.hpp"
#include "parse/protocol/protocol.hpp"
#include "shared.hpp"

namespace unum::ucall {

struct named_callback_t {
    ucall_str_t name{};
    ucall_callback_t callback{};
    request_type_t type{};
    ucall_callback_tag_t callback_tag{};
};

struct engine_t {

    /// @brief An array of function callbacks. Can be in dozens.
    array_gt<named_callback_t> callbacks{};

    bool find_and_call(std::string_view, request_type_t, ucall_call_t) const noexcept;
    // void raise_calls(sjd::parser&, scratch_space_t&, exchange_pipes_t&, protocol_t&, ucall_call_t) const noexcept;
    void raise_request(exchange_pipes_t&, protocol_t&, ucall_call_t) const noexcept;
};

void engine_t::raise_request(exchange_pipes_t& pipes, protocol_t& protocol, ucall_call_t call) const noexcept {

    if (auto error_ptr = protocol.parse_headers(pipes.input_span()); error_ptr)
        return ucall_call_reply_error(call, error_ptr->code, error_ptr->note.data(), error_ptr->note.size());

    if (auto error_ptr = protocol.parse_content(); error_ptr)
        return ucall_call_reply_error(call, error_ptr->code, error_ptr->note.data(), error_ptr->note.size());

    protocol.prepare_response(pipes);
    auto error_ptr =
        protocol.populate_response(pipes, [&](std::string_view method_name, request_type_t req_type) -> bool {
            return find_and_call(method_name, req_type, call);
        });
    if (error_ptr)
        return ucall_call_reply_error(call, error_ptr->code, error_ptr->note.data(), error_ptr->note.size());
    protocol.finalize_response(pipes);
}

bool engine_t::find_and_call(std::string_view method_name, request_type_t req_type, ucall_call_t call) const noexcept {
    auto callbacks_end = callbacks.data() + callbacks.size();
    auto callback_it = std::find_if(callbacks.data(), callbacks_end, [=](named_callback_t const& callback) noexcept {
        return callback.type == req_type && callback.name == method_name;
    });
    if (callback_it == callbacks_end)
        return false;

    named_callback_t named_callback = *callback_it;
    named_callback.callback(call, named_callback.callback_tag);
    return true;
};

} // namespace unum::ucall
