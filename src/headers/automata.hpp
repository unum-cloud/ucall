#pragma once

#include "connection.hpp"
#include "parse/json.hpp"
#include "reply.hpp"
#include "server.hpp"
#include "shared.hpp"

namespace unum::ucall {

struct automata_t {
    server_t& server;
    scratch_space_t& scratch;
    connection_t& connection;
    int completed_result{};

    void operator()() noexcept;

    // Computed properties:
    bool should_release() const noexcept;
    bool is_corrupted() const noexcept;

    void send_next() noexcept;
    void receive_next() noexcept;
    void close_gracefully() noexcept;
};

bool automata_t::should_release() const noexcept { return connection.expired() || connection.empty_transmits > 100; }

bool automata_t::is_corrupted() const noexcept { return completed_result == -EPIPE || completed_result == -EBADF; }

void automata_t::close_gracefully() noexcept {
    connection.stage = stage_t::waiting_to_close_k;
    server.network_engine.close_connection_gracefully(connection);
}

void automata_t::send_next() noexcept {
    exchange_pipes_t& pipes = connection.pipes;
    connection.stage = stage_t::responding_in_progress_k;
    connection.protocol.reset();
    pipes.release_inputs();

    server.network_engine.send_packet(connection, (void*)pipes.next_output_address(), pipes.next_output_length(),
                                      server.connections.offset_of(connection) * 2u + 1u);
}

void automata_t::receive_next() noexcept {
    exchange_pipes_t& pipes = connection.pipes;
    connection.stage = stage_t::expecting_reception_k;
    pipes.release_outputs();

    server.network_engine.recv_packet(connection, (void*)pipes.next_input_address(), pipes.next_input_length(),
                                      server.connections.offset_of(connection) * 2u);
}

void automata_t::operator()() noexcept {

    if (is_corrupted())
        return close_gracefully();

    switch (connection.stage) {

    case stage_t::waiting_to_accept_k:

        if (completed_result == -ECANCELED) {
            server.release_connection(connection);
            server.reserved_connections--;
            server.consider_accepting_new_connection();
            return;
        }

        // Check if accepting the new connection request worked out.
        server.reserved_connections--;
        server.active_connections++;
        server.stats.added_connections.fetch_add(1, std::memory_order_relaxed);
        connection.descriptor = descriptor_t{completed_result};
        return receive_next();

    case stage_t::expecting_reception_k:

        // From documentation:
        // > If used, the timeout specified in the command will cancel the linked command,
        // > unless the linked command completes before the timeout. The timeout will complete
        // > with -ETIME if the timer expired and the linked request was attempted canceled,
        // > or -ECANCELED if the timer got canceled because of completion of the linked request.
        //
        // So we expect only two outcomes here:
        // 1. reception expired with: `ECANCELED`, and subsequent timeout expired with `ETIME`.
        // 2. reception can continue and subsequent timer returned `ECANCELED`.
        //
        // If the following timeout request has happened,
        // we don't want to do anything here. Let's leave the faith of
        // this connection to the subsequent timer to decide.
        if (completed_result == -ECANCELED) {
            connection.sleep_ns += connection.next_wakeup;
            connection.next_wakeup *= sleep_growth_factor_k;
            completed_result = 0;
        }

        // No data was received.
        if (completed_result == 0) {
            connection.empty_transmits++;
            return should_release() ? close_gracefully() : receive_next();
        }

        // Absorb the arrived data.
        server.stats.bytes_received.fetch_add(completed_result, std::memory_order_relaxed);
        server.stats.packets_received.fetch_add(1, std::memory_order_relaxed);
        connection.empty_transmits = 0;
        connection.sleep_ns = 0;
        if (!connection.pipes.absorb_input(completed_result)) {
            ucall_call_reply_error_out_of_memory(this);
            return send_next();
        }

        // If we have reached the end of the stream,
        // it is time to analyze the contents
        // and send back a response.
        if (connection.protocol.is_input_complete(connection.pipes.input_span())) {
            server.engine.raise_request(scratch, connection.pipes, connection.protocol, this);

            connection.pipes.release_inputs();
            // Some requests require no response at all,
            // so we can go back to listening the port.
            if (!connection.pipes.has_outputs()) {
                connection.exchanges++;
                if (connection.exchanges >= server.max_lifetime_exchanges)
                    return close_gracefully();
                else
                    return receive_next();
            } else {
                connection.pipes.prepare_more_outputs();
                return send_next();
            }
        }
        // We are looking for more data to come
        else if (connection.pipes.shift_input_to_dynamic()) {
            return receive_next();
        }
        // We may fail to allocate memory to receive the next input
        else {
            ucall_call_reply_error_out_of_memory(this);
            return send_next();
        }

    case stage_t::responding_in_progress_k:

        connection.empty_transmits = completed_result == 0 ? ++connection.empty_transmits : 0;

        if (should_release())
            return close_gracefully();

        server.stats.bytes_sent.fetch_add(completed_result, std::memory_order_relaxed);
        server.stats.packets_sent.fetch_add(1, std::memory_order_relaxed);
        connection.pipes.mark_submitted_outputs(completed_result);
        if (!connection.pipes.has_remaining_outputs()) {
            connection.exchanges++;
            if (connection.exchanges >= server.max_lifetime_exchanges)
                return close_gracefully();
            else
                return receive_next();
        } else {
            connection.pipes.prepare_more_outputs();
            return send_next();
        }

    case stage_t::waiting_to_close_k:
        return server.release_connection(connection);

    case stage_t::log_stats_k:
        server.log_and_reset_stats();
        return server.submit_stats_heartbeat();

    case stage_t::unknown_k:
        return;
    }
}

sj::simdjson_result<sjd::element> param_at(ucall_call_t call, ucall_str_t name, size_t name_len) noexcept {
    automata_t& automata = *reinterpret_cast<automata_t*>(call);
    scratch_space_t& scratch = automata.scratch;
    name_len = string_length(name, name_len);
    return scratch.point_to_param({name, name_len});
}

sj::simdjson_result<sjd::element> param_at(ucall_call_t call, size_t position) noexcept {
    automata_t& automata = *reinterpret_cast<automata_t*>(call);
    scratch_space_t& scratch = automata.scratch;
    return scratch.point_to_param(position);
}
} // namespace unum::ucall

void ucall_add_procedure(ucall_server_t punned_server, ucall_str_t name, ucall_callback_t callback,
                         ucall_callback_tag_t callback_tag) {
    unum::ucall::server_t& server = *reinterpret_cast<unum::ucall::server_t*>(punned_server);
    if (server.engine.callbacks.size() + 1 < server.engine.callbacks.capacity())
        server.engine.callbacks.push_back_reserved({name, callback, callback_tag});
}

void ucall_take_calls(ucall_server_t punned_server, uint16_t thread_idx) {
    unum::ucall::server_t* server = reinterpret_cast<unum::ucall::server_t*>(punned_server);
    if (!thread_idx && server->logs_file_descriptor > 0)
        server->submit_stats_heartbeat();
    while (true) {
        ucall_take_call(punned_server, thread_idx);
    }
}

void ucall_take_call(ucall_server_t punned_server, uint16_t thread_idx) {
    // Unlike the classical synchronous interface, this implements only a part of the connection machine,
    // is responsible for checking if a specific request has been completed. All of the submitted
    // memory must be preserved until we get the confirmation.
    unum::ucall::server_t* server = reinterpret_cast<unum::ucall::server_t*>(punned_server);
    bool waiting_new_connection = false;
    if (!thread_idx)
        waiting_new_connection = server->consider_accepting_new_connection();

    constexpr std::size_t completed_max_k{16};
    unum::ucall::completed_event_t completed_events[completed_max_k]{};

    std::size_t completed_count = server->network_engine.pop_completed_events<completed_max_k>(completed_events);

    for (std::size_t i = 0; i != completed_count; ++i) {
        unum::ucall::completed_event_t& completed = completed_events[i];

        unum::ucall::automata_t automata{
            *server, //
            server->spaces[thread_idx],
            *completed.connection_ptr,
            completed.result,
        };

        // If everything is fine, let automata work in its normal regime.
        automata();
    }
}

void ucall_call_reply_content(ucall_call_t call, ucall_str_t body, size_t body_len) {
    unum::ucall::automata_t& automata = *reinterpret_cast<unum::ucall::automata_t*>(call);
    unum::ucall::connection_t& connection = automata.connection;
    unum::ucall::scratch_space_t& scratch = automata.scratch;
    // No response is needed for "id"-less notifications.
    if (scratch.dynamic_id.empty())
        return;

    body_len = unum::ucall::string_length(body, body_len);
    struct iovec iovecs[unum::ucall::iovecs_for_content_k] {};
    unum::ucall::fill_with_content(iovecs, scratch.dynamic_id, std::string_view(body, body_len), true);
    connection.pipes.append_outputs<unum::ucall::iovecs_for_content_k>(iovecs);
}

void ucall_call_reply_error(ucall_call_t call, int code_int, ucall_str_t note, size_t note_len) {
    unum::ucall::automata_t& automata = *reinterpret_cast<unum::ucall::automata_t*>(call);
    unum::ucall::connection_t& connection = automata.connection;
    unum::ucall::scratch_space_t& scratch = automata.scratch;
    // No response is needed for "id"-less notifications.
    if (scratch.dynamic_id.empty())
        return;

    note_len = unum::ucall::string_length(note, note_len);
    char code[unum::ucall::max_integer_length_k]{};
    std::to_chars_result res = std::to_chars(code, code + unum::ucall::max_integer_length_k, code_int);
    auto code_len = res.ptr - code;
    if (res.ec != std::error_code())
        return ucall_call_reply_error_unknown(call);

    struct iovec iovecs[unum::ucall::iovecs_for_error_k] {};
    unum::ucall::fill_with_error(iovecs, scratch.dynamic_id, std::string_view(code, code_len),
                                 std::string_view(note, note_len), true);
    if (!connection.pipes.append_outputs<unum::ucall::iovecs_for_error_k>(iovecs))
        return ucall_call_reply_error_out_of_memory(call);
}

void ucall_call_reply_error_invalid_params(ucall_call_t call) {
    return ucall_call_reply_error(call, -32602, "Invalid method param(s).", 24);
}

void ucall_call_reply_error_unknown(ucall_call_t call) {
    return ucall_call_reply_error(call, -32603, "Unknown error.", 14);
}

void ucall_call_reply_error_out_of_memory(ucall_call_t call) {
    return ucall_call_reply_error(call, -32000, "Out of memory.", 14);
}

bool ucall_param_named_bool(ucall_call_t call, ucall_str_t name, size_t name_len, bool* result_ptr) {
    if (auto value = unum::ucall::param_at(call, name, name_len); value.is_bool()) {
        *result_ptr = value.get_bool().value_unsafe();
        return true;
    } else
        return false;
}

bool ucall_param_named_i64(ucall_call_t call, ucall_str_t name, size_t name_len, int64_t* result_ptr) {
    if (auto value = unum::ucall::param_at(call, name, name_len); value.is_int64()) {
        *result_ptr = value.get_int64().value_unsafe();
        return true;
    } else
        return false;
}

bool ucall_param_named_f64(ucall_call_t call, ucall_str_t name, size_t name_len, double* result_ptr) {
    if (auto value = unum::ucall::param_at(call, name, name_len); value.is_double()) {
        *result_ptr = value.get_double().value_unsafe();
        return true;
    } else
        return false;
}

bool ucall_param_named_str(ucall_call_t call, ucall_str_t name, size_t name_len, ucall_str_t* result_ptr,
                           size_t* result_len_ptr) {
    if (auto value = unum::ucall::param_at(call, name, name_len); value.is_string()) {
        *result_ptr = value.get_string().value_unsafe().data();
        *result_len_ptr = value.get_string_length().value_unsafe();
        return true;
    } else
        return false;
}

bool ucall_param_positional_bool(ucall_call_t call, size_t position, bool* result_ptr) {
    if (auto value = unum::ucall::param_at(call, position); value.is_bool()) {
        *result_ptr = value.get_bool().value_unsafe();
        return true;
    } else
        return false;
}

bool ucall_param_positional_i64(ucall_call_t call, size_t position, int64_t* result_ptr) {
    if (auto value = unum::ucall::param_at(call, position); value.is_int64()) {
        *result_ptr = value.get_int64().value_unsafe();
        return true;
    } else
        return false;
}

bool ucall_param_positional_f64(ucall_call_t call, size_t position, double* result_ptr) {
    if (auto value = unum::ucall::param_at(call, position); value.is_double()) {
        *result_ptr = value.get_double().value_unsafe();
        return true;
    } else
        return false;
}

bool ucall_param_positional_str(ucall_call_t call, size_t position, ucall_str_t* result_ptr, size_t* result_len_ptr) {
    if (auto value = unum::ucall::param_at(call, position); value.is_string()) {
        *result_ptr = value.get_string().value_unsafe().data();
        *result_len_ptr = value.get_string_length().value_unsafe();
        return true;
    } else
        return false;
}
