#pragma once

#include <atomic>
#include <optional>

#include "connection.hpp"
#include "contain/array.hpp"
#include "log.hpp"
#include "network.hpp"
#include "parse/json.hpp"
#include "parse/protocol.hpp"
#include "shared.hpp"

namespace unum::ucall {

struct named_callback_t {
    ucall_str_t name{};
    ucall_callback_t callback{};
    ucall_callback_tag_t callback_tag{};
};

struct engine_t {

    /// @brief An array of function callbacks. Can be in dozens.
    array_gt<named_callback_t> callbacks{};

    std::variant<named_callback_t, default_error_t> find_callback(scratch_space_t&) const noexcept;

    bool find_and_call(scratch_space_t&, ucall_call_t) const noexcept;
    void raise_calls(scratch_space_t&, exchange_pipes_t&, protocol_t&, ucall_call_t) const noexcept;
    void raise_request(scratch_space_t&, exchange_pipes_t&, protocol_t&, ucall_call_t) const noexcept;
};

void engine_t::raise_request(scratch_space_t& scratch, exchange_pipes_t& pipes, protocol_t& protocol,
                             ucall_call_t call) const noexcept {
    auto parsed_request_or_error = protocol.parse(pipes.input_span());
    if (auto error_ptr = std::get_if<default_error_t>(&parsed_request_or_error); error_ptr)
        return ucall_call_reply_error(call, error_ptr->code, error_ptr->note.data(), error_ptr->note.size());

    auto parsed_request = std::get<parsed_request_t>(parsed_request_or_error);

    scratch.dynamic_packet = parsed_request.body;
    if (scratch.dynamic_packet.size() > ram_page_size_k) {
        sjd::parser parser;
        if (parser.allocate(scratch.dynamic_packet.size(), scratch.dynamic_packet.size() / 2) != sj::SUCCESS)
            return ucall_call_reply_error_out_of_memory(call);
        scratch.dynamic_parser = &parser;
    } else {
        scratch.dynamic_parser = &scratch.parser;
    }

    return raise_calls(scratch, pipes, protocol, call);
}

void engine_t::raise_calls(scratch_space_t& scratch, exchange_pipes_t& pipes, protocol_t& protocol,
                           ucall_call_t call) const noexcept {
    sjd::parser& parser = *scratch.dynamic_parser;
    std::string_view json_body = scratch.dynamic_packet;
    parser.set_max_capacity(json_body.size());
    auto one_or_many = parser.parse(json_body.data(), json_body.size(), false);

    if (one_or_many.error() == sj::CAPACITY)
        return ucall_call_reply_error_out_of_memory(call);

    if (one_or_many.error() != sj::SUCCESS)
        return ucall_call_reply_error(call, -32700, "Invalid JSON was received by the server.", 40);

    protocol.prepare_response(pipes);

    // Check if we hve received a batch request.
    if (one_or_many.is_array()) {
        sjd::array many = one_or_many.get_array().value_unsafe();
        scratch.is_batch = true;

        // Start a JSON array. Originally it must fit into `embedded` part.
        pipes.push_back_reserved('[');

        for (sjd::element const one : many) {
            scratch.tree = one;
            if (!find_and_call(scratch, call))
                return;
        }

        // Replace the last comma with the closing bracket.
        pipes.output_pop_back();
        pipes.push_back_reserved(']');
    }
    // This is a single request
    else {
        scratch.is_batch = false;
        scratch.tree = one_or_many.value_unsafe();
        auto callback_or_error = find_callback(scratch);
        if (!find_and_call(scratch, call))
            return;

        if (scratch.dynamic_id.empty()) {
            pipes.push_back_reserved('{');
            pipes.push_back_reserved('}');
        } else if (pipes.has_outputs()) // Drop the last comma, if present.
            pipes.output_pop_back();
    }

    protocol.finalize_response(pipes);
}

bool engine_t::find_and_call(scratch_space_t& scratch, ucall_call_t call) const noexcept {
    auto callback_or_error = find_callback(scratch);
    if (auto error_ptr = std::get_if<default_error_t>(&callback_or_error); error_ptr) {
        ucall_call_reply_error(call, error_ptr->code, error_ptr->note.data(), error_ptr->note.size());
        return false;
    }

    named_callback_t named_callback = std::get<named_callback_t>(callback_or_error);
    named_callback.callback(call, named_callback.callback_tag);
    return true;
};

/**
 * @brief Validates the contents of the JSON call DOM, and finds a matching callback.
 */
std::variant<named_callback_t, default_error_t> engine_t::find_callback(scratch_space_t& scratch) const noexcept {
    auto res = get_method_name(scratch);
    if (auto error_ptr = std::get_if<default_error_t>(&res); error_ptr)
        return *error_ptr;

    // Make sure we have such a method:
    std::string_view method_name = std::get<std::string_view>(res);
    auto callbacks_end = callbacks.data() + callbacks.size();
    auto callback_it = std::find_if(callbacks.data(), callbacks_end, [=](named_callback_t const& callback) noexcept {
        return callback.name == method_name;
    });
    if (callback_it == callbacks_end)
        return default_error_t{-32601, "Method not found."};

    return *callback_it;
}

} // namespace unum::ucall
