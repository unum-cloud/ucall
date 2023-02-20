/**
 * @brief JSON-RPC implementation for TCP/IP stack with POSIX calls.
 * @author Ashot Vardanian
 */
#include <arpa/inet.h>  // `inet_addr`
#include <errno.h>      // `strerror`
#include <fcntl.h>      // `fcntl`
#include <netinet/in.h> // `sockaddr_in`
#include <stdlib.h>     // `std::aligned_malloc`
#include <sys/ioctl.h>
#include <sys/socket.h> // `recv`, `setsockopt`
#include <sys/types.h>
#include <sys/uio.h>
#include <unistd.h>

#include <charconv> // `std::to_chars`
#include <chrono>   // `std::chrono`

#include "ujrpc/ujrpc.h"

#include "helpers/log.hpp"
#include "helpers/parse.hpp"
#include "helpers/reply.hpp"
#include "helpers/shared.hpp"

using namespace unum::ujrpc;

using time_clock_t = std::chrono::steady_clock;
using time_point_t = std::chrono::time_point<time_clock_t>;

struct engine_t {
    descriptor_t socket{};
    std::size_t max_batch_size{};

    /// @brief The file descriptor of the stateful connection over TCP.
    descriptor_t connection{};
    /// @brief A small memory buffer to store small requests.
    alignas(align_k) char packet_buffer[ram_page_size_k + sj::SIMDJSON_PADDING]{};
    /// @brief An array of function callbacks. Can be in dozens.
    array_gt<named_callback_t> callbacks{};
    /// @brief Statically allocated memory to process small requests.
    scratch_space_t scratch{};
    /// @brief For batch-requests in synchronous connections we need a place to
    struct batch_response_t {
        buffer_gt<struct iovec> iovecs{};
        buffer_gt<char*> copies{};
        std::size_t iovecs_count{};
        std::size_t copies_count{};
    } batch_response{};

    stats_t stats{};
    std::int32_t logs_file_descriptor{};
    std::string_view logs_format{};
    time_point_t log_last_time{};
};

sj::simdjson_result<sjd::element> param_at(ujrpc_call_t call, ujrpc_str_t name, size_t name_len) noexcept {
    engine_t& engine = *reinterpret_cast<engine_t*>(call);
    scratch_space_t& scratch = engine.scratch;
    name_len = string_length(name, name_len);
    return scratch.point_to_param({name, name_len});
}

sj::simdjson_result<sjd::element> param_at(ujrpc_call_t call, size_t position) noexcept {
    engine_t& engine = *reinterpret_cast<engine_t*>(call);
    scratch_space_t& scratch = engine.scratch;
    return scratch.point_to_param(position);
}

void send_message(engine_t& engine, struct msghdr& message) noexcept {
    auto bytes_sent = sendmsg(engine.connection, &message, 0);
    if (bytes_sent < 0)
        return;
    engine.stats.bytes_sent += bytes_sent;
    engine.stats.packets_sent++;
}

void send_reply(engine_t& engine) noexcept {
    if (!engine.batch_response.iovecs_count)
        return;

    struct msghdr message {};
    message.msg_iov = engine.batch_response.iovecs.data();
    message.msg_iovlen = engine.batch_response.iovecs_count;
    send_message(engine, message);
}

void forward_call(engine_t& engine) noexcept {
    scratch_space_t& scratch = engine.scratch;
    auto callback_or_error = find_callback(engine.callbacks, scratch);
    if (auto error_ptr = std::get_if<default_error_t>(&callback_or_error); error_ptr)
        return ujrpc_call_reply_error(&engine, error_ptr->code, error_ptr->note.data(), error_ptr->note.size());

    named_callback_t call_data = std::get<named_callback_t>(callback_or_error);
    return call_data.callback(&engine, call_data.callback_data);
}

/**
 * @brief Analyzes the contents of the packet, bifurcating batched and singular JSON-RPC requests.
 */
void forward_call_or_calls(engine_t& engine) noexcept {
    scratch_space_t& scratch = engine.scratch;
    sjd::parser& parser = *scratch.dynamic_parser;
    std::string_view json_body = scratch.dynamic_packet;
    auto one_or_many = parser.parse(json_body.data(), json_body.size(), false);
    if (one_or_many.error() != sj::SUCCESS)
        return ujrpc_call_reply_error(&engine, -32700, "Invalid JSON was received by the server.", 40);

    engine.batch_response.iovecs_count = 0;
    engine.batch_response.copies_count = 0;
    // The major difference between batch and single-request paths is that
    // in the first case we need to keep a copy of the data somewhere,
    // until answers to all requests are accumulated and we can submit them
    // simultaneously.
    // Linux supports `MSG_MORE` flag for submissions, which could have helped,
    // but it is less effective than assembling a copy here.
    if (one_or_many.is_array()) {
        sjd::array many = one_or_many.get_array().value_unsafe();
        scratch.is_batch = false;
        if (many.size() > engine.max_batch_size)
            return ujrpc_call_reply_error(&engine, -32603, "Too many requests in the batch.", 31);

        // Start a JSON array.
        scratch.is_batch = true;
        engine.batch_response.iovecs[0].iov_base = (void*)"[";
        engine.batch_response.iovecs[0].iov_len = 1;
        engine.batch_response.iovecs_count++;

        for (sjd::element const one : many) {
            scratch.tree = one;
            forward_call(engine);
        }

        // Drop the last comma. Yeah, it's ugly.
        auto last_bucket = (char*)engine.batch_response.iovecs[engine.batch_response.iovecs_count - 1].iov_base;
        if (last_bucket[engine.batch_response.iovecs[engine.batch_response.iovecs_count - 1].iov_len - 1] == ',')
            engine.batch_response.iovecs[engine.batch_response.iovecs_count - 1].iov_len--;

        // Close the last bracket of the JSON array.
        engine.batch_response.iovecs[engine.batch_response.iovecs_count].iov_base = (void*)"]";
        engine.batch_response.iovecs[engine.batch_response.iovecs_count].iov_len = 1;
        engine.batch_response.iovecs_count++;

        send_reply(engine);

        // Deallocate copies of received responses:
        for (std::size_t response_idx = 0; response_idx != engine.batch_response.copies_count; ++response_idx)
            std::free(std::exchange(engine.batch_response.copies[response_idx], nullptr));
    } else {
        scratch.is_batch = false;
        scratch.tree = one_or_many.value_unsafe();
        forward_call(engine);
        send_reply(engine);
    }
}

void forward_packet(engine_t& engine) noexcept {
    scratch_space_t& scratch = engine.scratch;
    auto json_or_error = split_body_headers(scratch.dynamic_packet);
    if (auto error_ptr = std::get_if<default_error_t>(&json_or_error); error_ptr)
        return ujrpc_call_reply_error(&engine, error_ptr->code, error_ptr->note.data(), error_ptr->note.size());

    scratch.dynamic_packet = std::get<parsed_request_t>(json_or_error).body;
    return forward_call_or_calls(engine);
}

void ujrpc_take_call(ujrpc_server_t server, uint16_t) {
    engine_t& engine = *reinterpret_cast<engine_t*>(server);
    scratch_space_t& scratch = engine.scratch;

    // Log stats, if enough time has passed since last call.
    if (engine.logs_file_descriptor > 0) {
        auto now = time_clock_t::now();
        auto dt = std::chrono::duration_cast<std::chrono::milliseconds>(now - engine.log_last_time).count();
        if (dt > stats_t::default_frequency_secs_k * 1000ul) {
            auto len = engine.logs_format == "json" //
                           ? engine.stats.log_json(engine.packet_buffer, ram_page_size_k)
                           : engine.stats.log_human_readable(engine.packet_buffer, ram_page_size_k, dt / 1000ul);
            len = write(engine.logs_file_descriptor, engine.packet_buffer, len);
            engine.log_last_time = now;
        }
    }

    // If no pending connections are present on the queue, and the
    // socket is not marked as nonblocking, accept() blocks the caller
    // until a connection is present. If the socket is marked
    // nonblocking and no pending connections are present on the queue,
    // accept() fails with the error EAGAIN or EWOULDBLOCK.
    int connection_fd = accept(engine.socket, (struct sockaddr*)NULL, NULL);
    if (connection_fd < 0) {
        // Drop the last error.
        errno;
        return;
    }

    // Wait until we have input.
    engine.connection = descriptor_t{connection_fd};
    engine.stats.added_connections++;
    engine.stats.closed_connections++;
    auto buffer_ptr = &engine.packet_buffer[0];
    // size_t bytes_received = recv(engine.connection, buffer_ptr, http_head_size_k, MSG_PEEK);

    // auto json_or_error = split_body_headers(std::string_view(buffer_ptr, bytes_received));
    // if (auto error_ptr = std::get_if<default_error_t>(&json_or_error); error_ptr)
    //     return ujrpc_call_reply_error(&engine, error_ptr->code, error_ptr->note.data(), error_ptr->note.size());
    // parsed_request_t request = std::get<parsed_request_t>(json_or_error);

    size_t bytes_expected = 0;
    if (ioctl(engine.connection, FIONREAD, &bytes_expected) == -1) {
        close(engine.connection);
        return;
    }
    // auto res = std::from_chars(request.content_length.begin(), request.content_length.end(), bytes_expected);
    // bytes_expected += (request.body.begin() - buffer_ptr);
    // if (res.ec == std::errc::invalid_argument || bytes_expected <= 0) {
    //     close(engine.connection);
    //     return;
    // }

    // Either process it in the statically allocated memory,
    // or allocate dynamically, if the message is too long.
    if (bytes_expected <= ram_page_size_k) {
        size_t bytes_received = recv(engine.connection, buffer_ptr, ram_page_size_k, 0);
        scratch.dynamic_parser = &scratch.parser;
        scratch.dynamic_packet = std::string_view(buffer_ptr, bytes_received);
        engine.stats.bytes_received += bytes_received;
        engine.stats.packets_received++;
        forward_packet(engine);
    } else {
        sjd::parser parser;
        if (parser.allocate(bytes_expected, bytes_expected / 2) != sj::SUCCESS)
            return ujrpc_call_reply_error_out_of_memory(&engine);

        buffer_ptr = (char*)std::aligned_alloc(align_k, round_up_to<align_k>(bytes_expected + sj::SIMDJSON_PADDING));
        if (!buffer_ptr)
            return ujrpc_call_reply_error_out_of_memory(&engine);

        size_t bytes_received = recv(engine.connection, buffer_ptr, bytes_expected, 0);
        scratch.dynamic_parser = &parser;
        scratch.dynamic_packet = std::string_view(buffer_ptr, bytes_received);
        engine.stats.bytes_received += bytes_received;
        engine.stats.packets_received++;
        forward_packet(engine);
        std::free(buffer_ptr);
        buffer_ptr = nullptr;
    }

    close(engine.connection);
}

void ujrpc_init(ujrpc_config_t* config_inout, ujrpc_server_t* server_out) {

    // Simple sanity check
    if (!server_out || !config_inout)
        return;

    // Retrieve configs, if present
    ujrpc_config_t& config = *config_inout;
    if (!config.port)
        config.port = 8545u;
    if (!config.queue_depth)
        config.queue_depth = 128u;
    if (!config.max_callbacks)
        config.max_callbacks = 128u;
    if (!config.max_batch_size)
        config.max_batch_size = 1024u;
    if (!config.interface)
        config.interface = "0.0.0.0";

    // Some limitations are hard-coded for this non-concurrent implementation
    config.max_threads = 1u;
    config.max_concurrent_connections = 1;
    config.max_lifetime_micro_seconds = 0u;
    config.max_lifetime_exchanges = 1u;

    int received_errno{};
    int socket_options{1};
    int socket_descriptor{-1};
    engine_t* server_ptr = nullptr;
    buffer_gt<struct iovec> embedded_iovecs;
    buffer_gt<char*> embedded_copies;
    array_gt<named_callback_t> embedded_callbacks;
    sjd::parser parser;

    // By default, let's open TCP port for IPv4.
    struct sockaddr_in address;
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = inet_addr(config.interface);
    address.sin_port = htons(config.port);

    // Try allocating all the necessary memory.
    server_ptr = (engine_t*)std::malloc(sizeof(engine_t));
    if (!server_ptr)
        goto cleanup;
    // In the worst case we may have `max_batch_size` requests, where each will
    // need `iovecs_for_content_k` or `iovecs_for_error_k` of `iovec` structures,
    // plus two for the opening and closing bracket of JSON.
    if (!embedded_iovecs.resize(config.max_batch_size * std::max(iovecs_for_content_k, iovecs_for_error_k) + 2))
        goto cleanup;
    if (!embedded_copies.resize(config.max_batch_size))
        goto cleanup;
    if (!embedded_callbacks.reserve(config.max_callbacks))
        goto cleanup;
    socket_descriptor = socket(AF_INET, SOCK_STREAM, 0);
    if (socket_descriptor < 0)
        goto cleanup;
    // Optionally configure the socket, but don't always expect it to succeed.
    if (setsockopt(socket_descriptor, SOL_SOCKET, SO_REUSEADDR | SO_REUSEPORT, &socket_options,
                   sizeof(socket_options)) == -1)
        errno;
    if (bind(socket_descriptor, (struct sockaddr*)&address, sizeof(address)) < 0)
        goto cleanup;
    if (listen(socket_descriptor, config.queue_depth) < 0)
        goto cleanup;
    if (parser.allocate(ram_page_size_k, ram_page_size_k / 2) != sj::SUCCESS)
        goto cleanup;

    // Initialize all the members.
    new (server_ptr) engine_t();
    server_ptr->socket = descriptor_t{socket_descriptor};
    server_ptr->max_batch_size = config.max_batch_size;
    server_ptr->callbacks = std::move(embedded_callbacks);
    server_ptr->scratch.parser = std::move(parser);
    server_ptr->batch_response.copies = std::move(embedded_copies);
    server_ptr->batch_response.iovecs = std::move(embedded_iovecs);
    server_ptr->logs_file_descriptor = config.logs_file_descriptor;
    server_ptr->logs_format = config.logs_format ? std::string_view(config.logs_format) : std::string_view();
    server_ptr->log_last_time = time_clock_t::now();
    *server_out = (ujrpc_server_t)server_ptr;
    return;

cleanup:
    received_errno = errno;
    if (socket_descriptor >= 0)
        close(socket_descriptor);
    std::free(server_ptr);
    *server_out = nullptr;
}

void ujrpc_add_procedure(ujrpc_server_t server, ujrpc_str_t name, ujrpc_callback_t callback,
                         ujrpc_data_t callback_data) {
    engine_t& engine = *reinterpret_cast<engine_t*>(server);
    if (engine.callbacks.size() + 1 < engine.callbacks.capacity())
        engine.callbacks.push_back_reserved({name, callback, callback_data});
}

void ujrpc_take_calls(ujrpc_server_t server, uint16_t) {
    while (true)
        ujrpc_take_call(server, 0);
}

void ujrpc_free(ujrpc_server_t server) {
    if (!server)
        return;

    engine_t& engine = *reinterpret_cast<engine_t*>(server);
    engine.~engine_t();
    std::free(server);
}

void ujrpc_call_reply_content(ujrpc_call_t call, ujrpc_str_t body, size_t body_len) {
    engine_t& engine = *reinterpret_cast<engine_t*>(call);
    scratch_space_t& scratch = engine.scratch;
    // No response is needed for "id"-less notifications.
    if (scratch.dynamic_id.empty())
        return;
    if (!body_len)
        body_len = std::strlen(body);

    // In case of a single request - immediately push into the socket.
    if (!scratch.is_batch) {
        struct msghdr message {};
        struct iovec iovecs[iovecs_for_content_k] {};
        fill_with_content(iovecs, scratch.dynamic_id, std::string_view(body, body_len));
        message.msg_iov = iovecs;
        message.msg_iovlen = iovecs_for_content_k;
        send_message(engine, message);
    }

    // In case of a batch or async request, preserve a copy of data on the heap.
    else {
        auto body_copy = (char*)std::malloc(body_len);
        if (!body_copy)
            return ujrpc_call_reply_error_out_of_memory(call);
        std::memcpy(body_copy, body, body_len);
        engine.batch_response.copies[engine.batch_response.copies_count++] = body_copy;
        fill_with_content(engine.batch_response.iovecs.data() + engine.batch_response.iovecs_count, scratch.dynamic_id,
                          std::string_view(body, body_len), true);
        engine.batch_response.iovecs_count += iovecs_for_content_k;
    }
}

void ujrpc_call_reply_error(ujrpc_call_t call, int code_int, ujrpc_str_t note, size_t note_len) {
    engine_t& engine = *reinterpret_cast<engine_t*>(call);
    scratch_space_t& scratch = engine.scratch;
    // No response is needed for "id"-less notifications.
    if (scratch.dynamic_id.empty())
        return;
    if (!note_len)
        note_len = std::strlen(note);

    char code[max_integer_length_k]{};
    std::to_chars_result res = std::to_chars(code, code + max_integer_length_k, code_int);
    auto code_len = res.ptr - code;
    if (res.ec != std::error_code())
        return ujrpc_call_reply_error_unknown(call);

    // In case of a single request - immediately push into the socket.
    if (!scratch.is_batch) {
        struct msghdr message {};
        struct iovec iovecs[iovecs_for_error_k] {};
        fill_with_error(iovecs, scratch.dynamic_id,       //
                        std::string_view(code, code_len), //
                        std::string_view(note, note_len));
        message.msg_iov = iovecs;
        message.msg_iovlen = iovecs_for_error_k;
        send_message(engine, message);
    }

    // In case of a batch or async request, preserve a copy of data on the heap.
    else {
        auto code_and_node = (char*)std::malloc(code_len + note_len);
        if (!code_and_node)
            return ujrpc_call_reply_error_out_of_memory(call);
        std::memcpy(code_and_node, code, code_len);
        std::memcpy(code_and_node + code_len, note, note_len);
        engine.batch_response.copies[engine.batch_response.copies_count++] = code_and_node;
        fill_with_error(engine.batch_response.iovecs.data() + engine.batch_response.iovecs_count,
                        scratch.dynamic_id,                        //
                        std::string_view(code_and_node, code_len), //
                        std::string_view(code_and_node + code_len, note_len), true);
        engine.batch_response.iovecs_count += iovecs_for_error_k;
    }
}

void ujrpc_call_reply_error_invalid_params(ujrpc_call_t call) {
    return ujrpc_call_reply_error(call, -32602, "Invalid method param(s).", 24);
}

void ujrpc_call_reply_error_unknown(ujrpc_call_t call) {
    return ujrpc_call_reply_error(call, -32603, "Unknown error.", 14);
}

void ujrpc_call_reply_error_out_of_memory(ujrpc_call_t call) {
    return ujrpc_call_reply_error(call, -32000, "Out of memory.", 14);
}

bool ujrpc_param_named_bool(ujrpc_call_t call, ujrpc_str_t name, size_t name_len, bool* result_ptr) {
    if (auto value = param_at(call, name, name_len); value.is_bool()) {
        *result_ptr = value.get_bool().value_unsafe();
        return true;
    } else
        return false;
}

bool ujrpc_param_named_i64(ujrpc_call_t call, ujrpc_str_t name, size_t name_len, int64_t* result_ptr) {
    if (auto value = param_at(call, name, name_len); value.is_int64()) {
        *result_ptr = value.get_int64().value_unsafe();
        return true;
    } else
        return false;
}

bool ujrpc_param_named_f64(ujrpc_call_t call, ujrpc_str_t name, size_t name_len, double* result_ptr) {
    if (auto value = param_at(call, name, name_len); value.is_double()) {
        *result_ptr = value.get_double().value_unsafe();
        return true;
    } else
        return false;
}

bool ujrpc_param_named_str(ujrpc_call_t call, ujrpc_str_t name, size_t name_len, ujrpc_str_t* result_ptr,
                           size_t* result_len_ptr) {
    if (auto value = param_at(call, name, name_len); value.is_string()) {
        *result_ptr = value.get_string().value_unsafe().data();
        *result_len_ptr = value.get_string_length().value_unsafe();
        return true;
    } else
        return false;
}

bool ujrpc_param_positional_bool(ujrpc_call_t call, size_t position, bool* result_ptr) {
    if (auto value = param_at(call, position); value.is_bool()) {
        *result_ptr = value.get_bool().value_unsafe();
        return true;
    } else
        return false;
}

bool ujrpc_param_positional_i64(ujrpc_call_t call, size_t position, int64_t* result_ptr) {
    if (auto value = param_at(call, position); value.is_int64()) {
        *result_ptr = value.get_int64().value_unsafe();
        return true;
    } else
        return false;
}

bool ujrpc_param_positional_f64(ujrpc_call_t call, size_t position, double* result_ptr) {
    if (auto value = param_at(call, position); value.is_double()) {
        *result_ptr = value.get_double().value_unsafe();
        return true;
    } else
        return false;
}

bool ujrpc_param_positional_str(ujrpc_call_t call, size_t position, ujrpc_str_t* result_ptr, size_t* result_len_ptr) {
    if (auto value = param_at(call, position); value.is_string()) {
        *result_ptr = value.get_string().value_unsafe().data();
        *result_len_ptr = value.get_string_length().value_unsafe();
        return true;
    } else
        return false;
}