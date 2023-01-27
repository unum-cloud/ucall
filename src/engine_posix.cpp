/**
 * Notable links:
 * https://man7.org/linux/man-pages/dir_by_project.html#liburing
 * https://stackoverflow.com/a/17665015/2766161
 */
#include <fcntl.h>      // `fcntl`
#include <netinet/in.h> // `sockaddr_in`
#include <stdlib.h>     // `std::aligned_malloc`
#include <sys/socket.h> // `recv`, `setsockopt`
#include <sys/types.h>
#include <sys/uio.h>
#include <unistd.h>

#include <charconv> // `std::to_chars`

#include "ujrpc/ujrpc.h"

#include "helpers/parse.hpp"
#include "helpers/reply.hpp"
#include "helpers/shared.hpp"

using namespace unum::ujrpc;

struct engine_t {
    descriptor_t socket{};
    std::size_t max_batch_size{};

    /// @brief The file descriptor of the statefull connection over TCP.
    descriptor_t connection{};
    /// @brief A small memory buffer to store small requests.
    alignas(align_k) char packet_buffer[max_packet_size_k + sj::SIMDJSON_PADDING]{};
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
};

void send_reply(engine_t& engine) {
    if (!engine.batch_response.iovecs_count)
        return;

    struct msghdr message {};
    message.msg_iov = engine.batch_response.iovecs.data();
    message.msg_iovlen = engine.batch_response.iovecs_count;
    sendmsg(engine.connection, &message, 0);
}

void forward_call(engine_t& engine) {
    scratch_space_t& scratch = engine.scratch;
    auto callback_or_error = find_callback(engine.callbacks, scratch);
    if (auto error_ptr = std::get_if<default_error_t>(&callback_or_error); error_ptr)
        return ujrpc_call_reply_error(&engine, error_ptr->code, error_ptr->note.data(), error_ptr->note.size());

    auto callback = std::get<ujrpc_callback_t>(callback_or_error);
    return callback(&engine);
}

/**
 * @brief Analyzes the contents of the packet, bifurcating batched and singular JSON-RPC requests.
 */
void forward_call_or_calls(engine_t& engine) {
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
    // but it is less effictive than assembling a copy here.
    if (one_or_many.is_array()) {
        sjd::array many = one_or_many.get_array();
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
            std::free(engine.batch_response.copies[response_idx]);
    } else {
        scratch.is_batch = false;
        scratch.tree = one_or_many.value();
        forward_call(engine);
        send_reply(engine);
    }
}

void forward_packet(engine_t& engine) {
    scratch_space_t& scratch = engine.scratch;
    auto json_or_error = strip_http_headers(scratch.dynamic_packet);
    if (auto error_ptr = std::get_if<default_error_t>(&json_or_error); error_ptr)
        return ujrpc_call_reply_error(&engine, error_ptr->code, error_ptr->note.data(), error_ptr->note.size());

    scratch.dynamic_packet = std::get<parsed_request_t>(json_or_error).body;
    return forward_call_or_calls(engine);
}

void ujrpc_take_call(ujrpc_server_t server, uint16_t) {
    engine_t& engine = *reinterpret_cast<engine_t*>(server);
    scratch_space_t& scratch = engine.scratch;
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
    auto buffer_ptr = &engine.packet_buffer[0];
    auto bytes_expected = recv(engine.connection, buffer_ptr, max_packet_size_k, MSG_PEEK | MSG_TRUNC);
    if (bytes_expected <= 0) {
        close(engine.connection);
        return;
    }

    // Either process it in the statically allocated memory,
    // or allocate dynamically, if the message is too long.
    if (bytes_expected <= max_packet_size_k) {
        auto bytes_received = recv(engine.connection, buffer_ptr, max_packet_size_k, 0);
        scratch.dynamic_parser = &scratch.parser;
        scratch.dynamic_packet = std::string_view(buffer_ptr, bytes_received);
        forward_packet(engine);
    } else {
        sjd::parser parser;
        if (parser.allocate(bytes_expected, bytes_expected / 2) != sj::SUCCESS) {
            ujrpc_call_send_error_out_of_memory(&engine);
            return;
        }
        buffer_ptr = (char*)std::aligned_alloc(align_k, round_up_to<align_k>(bytes_expected + sj::SIMDJSON_PADDING));
        if (!buffer_ptr) {
            ujrpc_call_send_error_out_of_memory(&engine);
            return;
        }

        auto bytes_received = recv(engine.connection, buffer_ptr, bytes_expected, 0);
        scratch.dynamic_parser = &parser;
        scratch.dynamic_packet = std::string_view(buffer_ptr, bytes_received);
        forward_packet(engine);
        std::free(buffer_ptr);
    }

    close(engine.connection);
}

void ujrpc_init(ujrpc_config_t const* config, ujrpc_server_t* server) {

    // Simple sanity check
    if (!server)
        return;

    // Retrieve configs, if present
    uint16_t port = config && config->port > 0 ? config->port : 8545u;
    uint16_t queue_depth = config && config->queue_depth > 0 ? config->queue_depth : 256u;
    uint16_t batch_capacity = config && config->batch_capacity > 0 ? config->batch_capacity : 1024u;
    uint16_t callbacks_capacity = config && config->callbacks_capacity > 0 ? config->callbacks_capacity : 128u;
    uint16_t connections_capacity = config && config->connections_capacity > 0 ? config->connections_capacity : 1024u;
    int opt = 1;
    int server_fd = -1;
    engine_t* server_ptr = nullptr;
    buffer_gt<struct iovec> embedded_iovecs;
    buffer_gt<char*> embedded_copies;
    array_gt<named_callback_t> embedded_callbacks;
    sjd::parser parser;

    // By default, let's open TCP port for IPv4.
    struct sockaddr_in address;
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = INADDR_ANY;
    address.sin_port = htons(port);

    // Try allocating all the neccessary memory.
    server_ptr = (engine_t*)std::malloc(sizeof(engine_t));
    if (!server_ptr)
        goto cleanup;
    // In the worst case we may have `batch_capacity` requests, where each will
    // need `iovecs_for_content_k` or `iovecs_for_error_k` of `iovec` structures,
    // plus two for the opening and closing bracket of JSON.
    if (!embedded_iovecs.reserve(batch_capacity * std::max(iovecs_for_content_k, iovecs_for_error_k) + 2))
        goto cleanup;
    if (!embedded_copies.reserve(batch_capacity))
        goto cleanup;
    if (!embedded_callbacks.reserve(callbacks_capacity))
        goto cleanup;
    server_fd = socket(AF_INET, SOCK_STREAM, 0);
    if (server_fd < 0)
        goto cleanup;
    if (setsockopt(server_fd, SOL_SOCKET, SO_REUSEADDR | SO_REUSEPORT, &opt, sizeof(opt)) < 0)
        goto cleanup;
    if (bind(server_fd, (struct sockaddr*)&address, sizeof(address)) < 0)
        goto cleanup;
    if (listen(server_fd, queue_depth) < 0)
        goto cleanup;
    if (parser.allocate(max_packet_size_k, max_packet_size_k / 2) != sj::SUCCESS)
        goto cleanup;

    // Initialize all the members.
    new (server_ptr) engine_t();
    server_ptr->socket = descriptor_t{server_fd};
    server_ptr->max_batch_size = batch_capacity;
    server_ptr->callbacks = std::move(embedded_callbacks);
    server_ptr->scratch.parser = std::move(parser);
    server_ptr->batch_response.copies = std::move(embedded_copies);
    server_ptr->batch_response.iovecs = std::move(embedded_iovecs);
    *server = (ujrpc_server_t)server_ptr;
    return;

cleanup:
    errno;
    if (server_fd >= 0)
        close(server_fd);
    if (server_ptr)
        std::free(server_ptr);
    *server = nullptr;
}

void ujrpc_add_procedure(ujrpc_server_t server, ujrpc_str_t name, ujrpc_callback_t callback) {
    engine_t& engine = *reinterpret_cast<engine_t*>(server);
    if (engine.callbacks.size() + 1 < engine.callbacks.capacity())
        engine.callbacks.push_back({name, callback});
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
    if (scratch.id.empty())
        // No response is needed for "id"-less notifications.
        return;
    if (!body_len)
        body_len = std::strlen(body);

    // In case of a sinle request - immediately push into the socket.
    if (!scratch.is_batch) {
        struct msghdr message {};
        struct iovec iovecs[iovecs_for_content_k] {};
        fill_with_content(iovecs, scratch.id, std::string_view(body, body_len));
        message.msg_iov = iovecs;
        message.msg_iovlen = iovecs_for_content_k;
        if (sendmsg(engine.connection, &message, 0) < 0)
            return ujrpc_call_send_error_unknown(call);
    }

    // In case of a batch or async request, preserve a copy of data on the heap.
    else {
        auto body_copy = (char*)std::malloc(body_len);
        if (!body_copy)
            return ujrpc_call_send_error_out_of_memory(call);
        std::memcpy(body_copy, body, body_len);
        engine.batch_response.copies[engine.batch_response.copies_count++] = body_copy;
        fill_with_content(engine.batch_response.iovecs.data() + engine.batch_response.iovecs_count, scratch.id,
                          std::string_view(body, body_len), true);
        engine.batch_response.iovecs_count += iovecs_for_content_k;
    }
}

void ujrpc_call_reply_error(ujrpc_call_t call, int code_int, ujrpc_str_t note, size_t note_len) {
    engine_t& engine = *reinterpret_cast<engine_t*>(call);
    scratch_space_t& scratch = engine.scratch;
    if (scratch.id.empty())
        // No response is needed for "id"-less notifications.
        return;
    if (!note_len)
        note_len = std::strlen(note);

    char code[16]{};
    std::to_chars_result res = std::to_chars(code, code + sizeof(code), code_int);
    auto code_len = res.ptr - code;
    if (res.ec != std::error_code())
        return ujrpc_call_send_error_unknown(call);

    // In case of a sinle request - immediately push into the socket.
    if (!scratch.is_batch) {
        struct msghdr message {};
        struct iovec iovecs[iovecs_for_error_k] {};
        fill_with_error(iovecs, scratch.id,               //
                        std::string_view(code, code_len), //
                        std::string_view(note, note_len));
        message.msg_iov = iovecs;
        message.msg_iovlen = iovecs_for_error_k;
        if (sendmsg(engine.connection, &message, 0) < 0)
            return ujrpc_call_send_error_unknown(call);
    }

    // In case of a batch or async request, preserve a copy of data on the heap.
    else {
        auto code_and_node = (char*)std::malloc(code_len + note_len);
        if (!code_and_node)
            return ujrpc_call_send_error_out_of_memory(call);
        std::memcpy(code_and_node, code, code_len);
        std::memcpy(code_and_node + code_len, note, note_len);
        engine.batch_response.copies[engine.batch_response.copies_count++] = code_and_node;
        fill_with_error(engine.batch_response.iovecs.data() + engine.batch_response.iovecs_count, scratch.id, //
                        std::string_view(code_and_node, code_len),                                            //
                        std::string_view(code_and_node + code_len, note_len), true);
        engine.batch_response.iovecs_count += iovecs_for_error_k;
    }
}

void ujrpc_call_send_error_invalid_params(ujrpc_call_t call) {
    return ujrpc_call_reply_error(call, -32602, "Invalid method param(s).", 24);
}

void ujrpc_call_send_error_unknown(ujrpc_call_t call) {
    return ujrpc_call_reply_error(call, -32603, "Unknown error.", 14);
}

void ujrpc_call_send_error_out_of_memory(ujrpc_call_t call) {
    return ujrpc_call_reply_error(call, -32000, "Out of memory.", 14);
}

bool ujrpc_param_named_i64(ujrpc_call_t call, ujrpc_str_t name, int64_t* result_ptr) {
    engine_t& engine = *reinterpret_cast<engine_t*>(call);
    scratch_space_t& scratch = engine.scratch;
    std::memcpy(scratch.json_pointer, "/params/", 8);
    std::memcpy(scratch.json_pointer + 8, name, std::strlen(name) + 1);
    auto value = scratch.tree.at_pointer(scratch.json_pointer);

    if (value.is_int64()) {
        *result_ptr = value.get_int64();
        return true;
    } else if (value.is_uint64()) {
        *result_ptr = static_cast<int64_t>(value.get_uint64());
        return true;
    } else
        return false;
}
