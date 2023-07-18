#pragma once

#include <charconv> // `std::to_chars`
#include <optional> // `std::optional`

#include <simdjson.h>

#include "connection.hpp"
#include "engine.hpp"
#include "helpers/parse/http.hpp"
#include "helpers/parse/json.hpp"
#include "helpers/reply.hpp"
#include "helpers/shared.hpp"

namespace unum::ucall {

struct automata_t {

    engine_t& engine;
    scratch_space_t& scratch;
    connection_t& connection;
    stage_t completed_stage{};
    int completed_result{};

    void operator()() noexcept;
    bool is_corrupted() const noexcept { return completed_result == -EPIPE || completed_result == -EBADF; }

    // Computed properties:
    bool received_full_request() const noexcept;
    bool should_release() const noexcept;

    // Submitting to io_uring:
    void send_next() noexcept;
    void receive_next() noexcept;
    void close_gracefully() noexcept;

    // Helpers:
    void raise_call() noexcept;
    void raise_call_or_calls() noexcept;
    void parse_and_raise_request() noexcept;
};

bool automata_t::should_release() const noexcept {
    return connection.expired() || engine.dismissed_connections || connection.empty_transmits > 100;
}

bool automata_t::received_full_request() const noexcept {

    auto span = connection.pipes.input_span();
    // if (!connection.content_length) {
    size_t bytes_expected = 0;

    auto json_or_error = split_body_headers(std::string_view(span.data(), span.size()));
    if (auto error_ptr = std::get_if<default_error_t>(&json_or_error); error_ptr)
        return true;
    parsed_request_t request = std::get<parsed_request_t>(json_or_error);

    auto res = std::from_chars(request.content_length.begin(), request.content_length.end(), bytes_expected);
    bytes_expected += (request.body.begin() - span.data());

    if (res.ec == std::errc::invalid_argument || bytes_expected <= 0)
        // TODO: Maybe not a HTTP request, What to do?
        return true;

    connection.content_length = bytes_expected;
    // }

    if (span.size() < *connection.content_length)
        return false;

    return true;
}

void automata_t::raise_call() noexcept {
    auto callback_or_error = find_callback(engine.callbacks, scratch);
    if (auto error_ptr = std::get_if<default_error_t>(&callback_or_error); error_ptr)
        return ucall_call_reply_error(this, error_ptr->code, error_ptr->note.data(), error_ptr->note.size());

    named_callback_t named_callback = std::get<named_callback_t>(callback_or_error);
    return named_callback.callback(this, named_callback.callback_tag);
}

void automata_t::raise_call_or_calls() noexcept {
    exchange_pipes_t& pipes = connection.pipes;
    sjd::parser& parser = *scratch.dynamic_parser;
    std::string_view json_body = scratch.dynamic_packet;
    parser.set_max_capacity(json_body.size());
    auto one_or_many = parser.parse(json_body.data(), json_body.size(), false);
    if (one_or_many.error() == sj::CAPACITY)
        return ucall_call_reply_error_out_of_memory(this);

    // We may need to prepend the response with HTTP headers.
    if (scratch.is_http)
        pipes.append_reserved(http_header_k, http_header_size_k);

    size_t body_size = pipes.output_span().size();

    if (one_or_many.error() != sj::SUCCESS)
        return ucall_call_reply_error(this, -32700, "Invalid JSON was received by the server.", 40);

    // Check if we hve received a batch request.
    else if (one_or_many.is_array()) {
        sjd::array many = one_or_many.get_array().value_unsafe();
        scratch.is_batch = true;

        // Start a JSON array. Originally it must fit into `embedded` part.
        pipes.push_back_reserved('[');

        for (sjd::element const one : many) {
            scratch.tree = one;
            raise_call();
        }

        // Replace the last comma with the closing bracket.
        pipes.output_pop_back();
        pipes.push_back_reserved(']');
    }
    // This is a single request
    else {
        scratch.is_batch = false;
        scratch.tree = one_or_many.value_unsafe();
        raise_call();

        if (scratch.dynamic_id.empty()) {
            pipes.push_back_reserved('{');
            pipes.push_back_reserved('}');
        } else if (pipes.has_outputs()) // Drop the last comma, if present.
            pipes.output_pop_back();
    }

    // Now, as we know the length of the whole response, we can update
    // the HTTP headers to indicate thr real "Content-Length".
    if (scratch.is_http) {
        auto output = pipes.output_span();
        body_size = output.size() - body_size;
        if (!set_http_content_length(output.data(), body_size))
            return ucall_call_reply_error_out_of_memory(this);
    }
}

void automata_t::parse_and_raise_request() noexcept {
    auto request = connection.pipes.input_span();
    auto parsed_request_or_error = split_body_headers(request);
    if (auto error_ptr = std::get_if<default_error_t>(&parsed_request_or_error); error_ptr)
        // TODO: This error message may have to be wrapped into an HTTP header separately
        return ucall_call_reply_error(this, error_ptr->code, error_ptr->note.data(), error_ptr->note.size());

    auto parsed_request = std::get<parsed_request_t>(parsed_request_or_error);
    scratch.is_http = request.size() != parsed_request.body.size();
    scratch.dynamic_packet = parsed_request.body;
    if (scratch.dynamic_packet.size() > ram_page_size_k) {
        sjd::parser parser;
        if (parser.allocate(scratch.dynamic_packet.size(), scratch.dynamic_packet.size() / 2) != sj::SUCCESS)
            return ucall_call_reply_error_out_of_memory(this);
        else {
            scratch.dynamic_parser = &parser;
            return raise_call_or_calls();
        }
    } else {
        scratch.dynamic_parser = &scratch.parser;
        return raise_call_or_calls();
    }
}

void automata_t::close_gracefully() noexcept {
    int uring_result{};
    struct io_uring_sqe* uring_sqe{};
    connection.stage = stage_t::waiting_to_close_k;

    // The operations are not expected to complete in exactly the same order
    // as their submissions. So to stop all existing communication on the
    // socket, we can cancel everything related to its "file descriptor",
    // and then close.
    engine.submission_mutex.lock();
    uring_sqe = io_uring_get_sqe(&engine.uring);
    io_uring_prep_cancel_fd(uring_sqe, int(connection.descriptor), 0);
    io_uring_sqe_set_data(uring_sqe, NULL);
    io_uring_sqe_set_flags(uring_sqe, IOSQE_IO_HARDLINK);

    uring_sqe = io_uring_get_sqe(&engine.uring);
    io_uring_prep_shutdown(uring_sqe, int(connection.descriptor), SHUT_WR);
    io_uring_sqe_set_data(uring_sqe, NULL);
    io_uring_sqe_set_flags(uring_sqe, IOSQE_IO_HARDLINK);

    uring_sqe = io_uring_get_sqe(&engine.uring);
    io_uring_prep_close(uring_sqe, int(connection.descriptor));
    io_uring_sqe_set_data(uring_sqe, &connection);
    io_uring_sqe_set_flags(uring_sqe, 0);

    uring_result = io_uring_submit(&engine.uring);
    engine.submission_mutex.unlock();
}

void automata_t::send_next() noexcept {
    exchange_pipes_t& pipes = connection.pipes;
    int uring_result{};
    struct io_uring_sqe* uring_sqe{};
    connection.stage = stage_t::responding_in_progress_k;
    pipes.release_inputs();

    // TODO: Test and benchmark the `send_zc option`.
    engine.submission_mutex.lock();
    uring_sqe = io_uring_get_sqe(&engine.uring);
    if (engine.has_send_zc) {
        io_uring_prep_send_zc_fixed(uring_sqe, int(connection.descriptor), (void*)pipes.next_output_address(),
                                    pipes.next_output_length(), 0, 0,
                                    engine.connections.offset_of(connection) * 2u + 1u);
    } else {
        io_uring_prep_send(uring_sqe, int(connection.descriptor), (void*)pipes.next_output_address(),
                           pipes.next_output_length(), 0);
        uring_sqe->flags |= IOSQE_FIXED_FILE;
        uring_sqe->buf_index = engine.connections.offset_of(connection) * 2u + 1u;
    }
    io_uring_sqe_set_data(uring_sqe, &connection);
    io_uring_sqe_set_flags(uring_sqe, 0);
    uring_result = io_uring_submit(&engine.uring);
    engine.submission_mutex.unlock();
}

void automata_t::receive_next() noexcept {
    exchange_pipes_t& pipes = connection.pipes;
    int uring_result{};
    struct io_uring_sqe* uring_sqe{};
    connection.stage = stage_t::expecting_reception_k;
    pipes.release_outputs();

    engine.submission_mutex.lock();

    // Choosing between `recv` and `read` system calls:
    // > If a zero-length datagram is pending, read(2) and recv() with a
    // > flags argument of zero provide different behavior. In this
    // > circumstance, read(2) has no effect (the datagram remains
    // > pending), while recv() consumes the pending datagram.
    // https://man7.org/linux/man-pages/man2/recv.2.html
    //
    // In this case we are waiting for an actual data, not some artificial wakeup.
    uring_sqe = io_uring_get_sqe(&engine.uring);
    io_uring_prep_read_fixed(uring_sqe, int(connection.descriptor), (void*)pipes.next_input_address(),
                             pipes.next_input_length(), 0, engine.connections.offset_of(connection) * 2u);
    io_uring_sqe_set_data(uring_sqe, &connection);
    io_uring_sqe_set_flags(uring_sqe, IOSQE_IO_LINK);

    // More than other operations this depends on the information coming from the client.
    // We can't afford to keep connections alive indefinitely, so we need to set a timeout
    // on this operation.
    // The `io_uring_prep_link_timeout` is a convenience method for poorly documented `IORING_OP_LINK_TIMEOUT`.
    uring_sqe = io_uring_get_sqe(&engine.uring);
    io_uring_prep_link_timeout(uring_sqe, &connection.next_wakeup, 0);
    io_uring_sqe_set_data(uring_sqe, NULL);
    io_uring_sqe_set_flags(uring_sqe, 0);
    uring_result = io_uring_submit(&engine.uring);

    engine.submission_mutex.unlock();
}

void automata_t::operator()() noexcept {

    if (is_corrupted())
        return close_gracefully();

    switch (connection.stage) {

    case stage_t::waiting_to_accept_k:

        if (completed_result == -ECANCELED) {
            engine.release_connection(connection);
            engine.reserved_connections--;
            engine.consider_accepting_new_connection();
            return;
        }

        // Check if accepting the new connection request worked out.
        engine.reserved_connections--;
        engine.active_connections++;
        engine.stats.added_connections.fetch_add(1, std::memory_order_relaxed);
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
            connection.sleep_ns += connection.next_wakeup.tv_nsec;
            connection.next_wakeup.tv_nsec *= sleep_growth_factor_k;
            completed_result = 0;
        }

        // No data was received.
        if (completed_result == 0) {
            connection.empty_transmits++;
            return should_release() ? close_gracefully() : receive_next();
        }

        // Absorb the arrived data.
        engine.stats.bytes_received.fetch_add(completed_result, std::memory_order_relaxed);
        engine.stats.packets_received.fetch_add(1, std::memory_order_relaxed);
        connection.empty_transmits = 0;
        connection.sleep_ns = 0;
        if (!connection.pipes.absorb_input(completed_result)) {
            ucall_call_reply_error_out_of_memory(this);
            return send_next();
        }

        // If we have reached the end of the stream,
        // it is time to analyze the contents
        // and send back a response.
        if (received_full_request()) {
            parse_and_raise_request();
            connection.pipes.release_inputs();
            // Some requests require no response at all,
            // so we can go back to listening the port.
            if (!connection.pipes.has_outputs()) {
                connection.exchanges++;
                if (connection.exchanges >= engine.max_lifetime_exchanges)
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

        engine.stats.bytes_sent.fetch_add(completed_result, std::memory_order_relaxed);
        engine.stats.packets_sent.fetch_add(1, std::memory_order_relaxed);
        connection.pipes.mark_submitted_outputs(completed_result);
        if (!connection.pipes.has_remaining_outputs()) {
            connection.exchanges++;
            if (connection.exchanges >= engine.max_lifetime_exchanges)
                return close_gracefully();
            else
                return receive_next();
        } else {
            connection.pipes.prepare_more_outputs();
            return send_next();
        }

    case stage_t::waiting_to_close_k:
        return engine.release_connection(connection);

    case stage_t::log_stats_k:
        engine.log_and_reset_stats();
        return engine.submit_stats_heartbeat();

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

void ucall_take_calls(ucall_server_t server, uint16_t thread_idx) {
    unum::ucall::engine_t& engine = *reinterpret_cast<unum::ucall::engine_t*>(server);
    if (!thread_idx && engine.logs_file_descriptor > 0)
        engine.submit_stats_heartbeat();
    while (true) {
        ucall_take_call(server, thread_idx);
    }
}

void ucall_take_call(ucall_server_t server, uint16_t thread_idx) {
    // Unlike the classical synchronous interface, this implements only a part of the connection machine,
    // is responsible for checking if a specific request has been completed. All of the submitted
    // memory must be preserved until we get the confirmation.
    unum::ucall::engine_t& engine = *reinterpret_cast<unum::ucall::engine_t*>(server);
    if (!thread_idx)
        engine.consider_accepting_new_connection();

    constexpr std::size_t completed_max_k{16};
    unum::ucall::completed_event_t completed_events[completed_max_k]{};
    std::size_t completed_count = engine.pop_completed<completed_max_k>(completed_events);

    for (std::size_t i = 0; i != completed_count; ++i) {
        unum::ucall::completed_event_t& completed = completed_events[i];
        unum::ucall::automata_t automata{
            engine, //
            engine.spaces[thread_idx],
            *completed.connection_ptr,
            completed.stage,
            completed.result,
        };

        // If everything is fine, let automata work in its normal regime.
        automata();
    }
}
