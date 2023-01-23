/**
 * @brief JSON-RPC implementation for TCP/IP stack. Supports:
 * > Millions of concurrent statefull connections.
 * > Hundreds of physical execution threads.
 * > Both HTTP and HTTP-less raw JSON-RPC calls.
 *
 * Currently 2 I/O mechanisms are implemented:
 * > Synchronous POSIX interface for old Linux, MaxOS and BSD.
 * > Asynchrnous interrupt-less `io_uring` for modern Linux.
 * Subsequently, many functions come in both `posix` or `uring` variants.
 *
 * "Life of da packet":
 * - take call
 * - extract HTTP headers, if present: `forward_packet_wout_http_headers`
 * - split single-shot and batch requests: `split_packet_into_calls`
 * - for singular `posix` requests:
 * --- find the needed function call: `validate_and_forward_call` <<<<<<<< change to return?
 * ----- let users to populate the answer via: `ujrpc_call_reply_content`
 * ----- let users to log an error via: `ujrpc_call_reply_error`
 * --- synchronously reply: `posix_send_reply`
 * - for batch or `uring` requests allocate copies of replies to preserve until send
 * ---
 *
 * Suggested compilation flags:
 * -fno-exceptions
 *
 * Notable links:
 * https://man7.org/linux/man-pages/dir_by_project.html#liburing
 * https://jvns.ca/blog/2017/06/03/async-io-on-linux--select--poll--and-epoll/
 * https://stackoverflow.com/a/17665015/2766161
 */
#include <fcntl.h>      // `fcntl`
#include <netinet/in.h> // `sockaddr_in`
#include <stdlib.h>     // `std::aligned_malloc`
#include <sys/socket.h> // `recv`, `setsockopt`
#include <sys/types.h>
#include <sys/uio.h>
#include <unistd.h>

#if __linuxer__
#include <liburing.h>
#endif

#include <charconv> // `std::to_chars`

#include <simdjson.h>

#include "ujrpc/ujrpc.h"

#include "helpers/connections_rr.hpp"
#include "helpers/parse.hpp"
#include "helpers/reply.hpp"
#include "helpers/shared.hpp"

namespace sj = simdjson;
namespace sjd = sj::dom;
using namespace unum::ujrpc;

struct engine_t {
    descriptor_t socket{};
    std::size_t max_batch_size{};
#if __linuxer__
    struct io_uring ring {};
#endif
    /// @brief An array of function callbacks. Can be in dozens.
    array_gt<named_callback_t> callbacks{};
    /// @brief A circular container of reusable connections. Can be in millions.
    connections_rr_t connections{};
    /// @brief Same number of them, as max physical threads. Can be in hundreds.
    buffer_gt<scratch_space_t> spaces{};
    /// @brief A shared buffer for the reserved io-vecs of
    buffer_gt<struct iovec> iovecs_tape{};
};

void posix_send_reply(engine_t& engine, connection_t& connection) {
    if (!connection.response.iovecs_count)
        return;

    struct msghdr message {};
    message.msg_iov = connection.response.iovecs;
    message.msg_iovlen = connection.response.iovecs_count;
    sendmsg(connection.descriptor, &message, 0);
}

/**
 * @brief Analyzes the contents of the packet, bifurcating batched and singular JSON-RPC requests.
 */
void split_packet_into_calls(engine_t& engine, connection_t& connection) {

    scratch_space_t& scratch = *(scratch_space_t*)connection.scratch;
    auto json_body = scratch.dynamic_packet;
    auto one_or_many = scratch.dynamic_parser->parse(json_body.data(), json_body.size(), false);
    if (one_or_many.error() != sj::SUCCESS)
        return ujrpc_call_reply_error(&connection, -32700, "Invalid JSON was received by the server.", 40);

    // The major difference between batch and single-request paths is that
    // in the first case we need to keep a copy of the data somewhere,
    // until answers to all requests are accumulated and we can submit them
    // simultaneously.
    // Linux supports `MSG_MORE` flag for submissions, which could have helped,
    // but it is less effictive than assembling a copy here.
    if (one_or_many.is_array()) {
        sjd::array many = one_or_many.get_array();
        auto embedded_iovecs = connection.response.iovecs;
        if (many.size() > embedded_batch_capacity_k) {
            // TODO: Allocate io-vec buffers of needed quantity
        }

        scratch.is_batch = true;
        scratch.is_async = false;
        for (sjd::element const one : many) {
            scratch.tree = one;
            validate_and_forward_call(engine, connection);
        }
        posix_send_reply(engine, connection);
        if (embedded_iovecs != connection.response.iovecs)
            std::free(std::exchange(connection.response.iovecs, embedded_iovecs));
    } else {
        scratch.is_batch = false;
        scratch.is_async = false;
        scratch.tree = one_or_many.value();
        validate_and_forward_call(engine, connection);
        posix_send_reply(engine, connection);
    }
}

connection_t* posix_poll_new_or_continue_existing(engine_t& engine) {
    // If no pending connections are present on the queue, and the
    // socket is not marked as nonblocking, accept() blocks the caller
    // until a connection is present. If the socket is marked
    // nonblocking and no pending connections are present on the queue,
    // accept() fails with the error EAGAIN or EWOULDBLOCK.
    int connection_fd = accept(engine.socket, (struct sockaddr*)NULL, NULL);
    if (connection_fd > 0) {
        if (engine.connections.size() == engine.connections.capacity())
            close(engine.connections.drop_tail());
        engine.connections.push_ahead(descriptor_t{connection_fd});
        return &engine.connections.head();
    }

    else {
        // This error is fine, there are no new requests.
        // We can switch to previous callers.
        int error = errno;
        if ((error == EAGAIN || error == EWOULDBLOCK) && engine.connections.size())
            return &engine.connections.poll();
        else
            return nullptr;
    }
}

void posix_take_call(ujrpc_server_t server, uint16_t thread_idx) {
    engine_t& engine = *reinterpret_cast<engine_t*>(server);
    connection_t* connection_ptr = posix_poll_new_or_continue_existing(engine);
    if (!connection_ptr)
        return;
    connection_t& connection = *connection_ptr;
    scratch_space_t& scratch = engine.spaces[thread_idx];
    connection.scratch = &scratch;

    // Wait until we have input.
    auto bytes_expected = recv(connection.descriptor, nullptr, 0, MSG_PEEK | MSG_TRUNC | MSG_WAITALL);
    if (bytes_expected < 0) {
        int error = errno;
        if (error == EAGAIN || error == EWOULDBLOCK) {
            connection.skipped_cycles++;
        } else {
            if (&engine.connections.tail() == connection_ptr)
                close(engine.connections.drop_tail());
        }
        return;
    } else if (bytes_expected == 0) {
        connection.skipped_cycles++;
        return;
    }

    // Either process it in the statically allocated memory,
    // or allocate dynamically, if the message is too long.
    if (bytes_expected <= embedded_packet_capacity_k) {
        auto buffer_ptr = &scratch.embedded_packet[0];
        auto bytes_received = recv(connection.descriptor, buffer_ptr, embedded_packet_capacity_k, 0);
        scratch.dynamic_parser = &scratch.parser;
        scratch.dynamic_packet = std::string_view(buffer_ptr, bytes_received);
        forward_packet_wout_http_headers(engine, connection);
    } else {
        // Let's
        sjd::parser parser;
        if (parser.allocate(bytes_expected, bytes_expected / 2) != sj::SUCCESS) {
            ujrpc_call_send_error_out_of_memory(&connection);
            return;
        }
        auto buffer_ptr =
            (char*)std::aligned_alloc(align_k, round_up_to<align_k>(bytes_expected + sj::SIMDJSON_PADDING));
        if (!buffer_ptr) {
            ujrpc_call_send_error_out_of_memory(&connection);
            return;
        }
        auto bytes_received = recv(connection.descriptor, buffer_ptr, bytes_expected, 0);
        scratch.dynamic_parser = &parser;
        scratch.dynamic_packet = std::string_view(buffer_ptr, bytes_received);
        forward_packet_wout_http_headers(engine, connection);
        std::free(buffer_ptr);
    }
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
    uint16_t threads_limit = config && config->threads_limit ? config->threads_limit : 1u;
    uint32_t lifetime_microsec_limit =
        config && config->lifetime_microsec_limit ? config->lifetime_microsec_limit : 100'000u;

    // Allocate
    int opt = 1;
    int server_fd = -1;
    engine_t* server_ptr = nullptr;
    connections_rr_t connections{};
    array_gt<named_callback_t> callbacks{};
    buffer_gt<scratch_space_t> spaces{};
    buffer_gt<struct iovec> iovecs_tape{};
    std::size_t iovecs_in_batch = batch_capacity * std::max(iovecs_for_content_k, iovecs_for_error_k);

    // By default, let's open TCP port for IPv4.
    struct sockaddr_in address;
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = INADDR_ANY;
    address.sin_port = htons(port);

    // Try allocating all the neccessary memory.
    server_ptr = (engine_t*)std::malloc(sizeof(engine_t));
    if (!server_ptr)
        goto cleanup;
    if (!iovecs_tape.alloc(iovecs_in_batch))
        goto cleanup;
    if (!callbacks.alloc(callbacks_capacity))
        goto cleanup;
    if (!connections.alloc(connections_capacity))
        goto cleanup;
    if (!spaces.alloc(threads_limit))
        goto cleanup;
    for (auto& space : spaces)
        if (space.parser.allocate(embedded_packet_capacity_k, embedded_packet_capacity_k / 2) != sj::SUCCESS)
            goto cleanup;
    server_fd = socket(AF_INET, SOCK_STREAM, 0);
    if (server_fd < 0)
        goto cleanup;
    if (setsockopt(server_fd, SOL_SOCKET, SO_REUSEADDR | SO_REUSEPORT, &opt, sizeof(opt)) < 0)
        goto cleanup;
    if (fcntl(server_fd, F_SETFL, O_NONBLOCK) < 0)
        goto cleanup;
    if (bind(server_fd, (struct sockaddr*)&address, sizeof(address)) < 0)
        goto cleanup;
    if (listen(server_fd, queue_depth) < 0)
        goto cleanup;

    // Initialize all the members.
    new (server_ptr) engine_t();
    server_ptr->socket = descriptor_t{server_fd};
    server_ptr->max_batch_size = batch_capacity;
    server_ptr->callbacks = std::move(callbacks);
    server_ptr->connections = std::move(connections);
    server_ptr->spaces = std::move(spaces);
    server_ptr->iovecs_tape = std::move(iovecs_tape);
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

void ujrpc_take_call(ujrpc_server_t server, uint16_t thread_idx) {
#if __linuxer__
    return uring_take_call(server, thread_idx);
#else
    return posix_take_call(server, thread_idx);
#endif
}

void ujrpc_take_calls(ujrpc_server_t server, uint16_t thread_idx) {
    while (true) {
        ujrpc_take_call(server, thread_idx);
    }
}

void ujrpc_free(ujrpc_server_t server) {
    if (!server)
        return;

    engine_t& engine = *reinterpret_cast<engine_t*>(server);
    engine.~engine_t();
    std::free(server);
}

void ujrpc_call_reply_content(ujrpc_call_t call, ujrpc_str_t body, size_t body_len) {
    connection_t& connection = *reinterpret_cast<connection_t*>(call);
    scratch_space_t& scratch = *(scratch_space_t*)connection.scratch;
    if (!body_len)
        body_len = std::strlen(body);

    auto body_copy = (char*)std::malloc(body_len);
    if (!body_copy)
        return ujrpc_call_send_error_out_of_memory(call);
    std::memcpy(body_copy, body, body_len);
    connection.response.copies[connection.response.copies_count++] = body_copy;
    fill_with_content(connection.response.iovecs + connection.response.iovecs_count, scratch.id,
                      std::string_view(body, body_len));
    connection.response.iovecs_count += iovecs_for_content_k;
}

void ujrpc_call_reply_error(ujrpc_call_t call, int code_int, ujrpc_str_t note, size_t note_len) {

    connection_t& connection = *reinterpret_cast<connection_t*>(call);
    scratch_space_t& scratch = *(scratch_space_t*)connection.scratch;
    if (!note_len)
        note_len = std::strlen(note);

    char code[16]{};
    std::to_chars_result res = std::to_chars(code, code + sizeof(code), code_int);
    auto code_len = res.ptr - code;
    if (res.ec != std::error_code())
        return ujrpc_call_send_error_unknown(call);

    auto code_and_node = (char*)std::malloc(code_len + note_len);
    if (!code_and_node)
        return ujrpc_call_send_error_out_of_memory(call);
    std::memcpy(code_and_node, code, code_len);
    std::memcpy(code_and_node + code_len, note, note_len);
    connection.response.copies[connection.response.copies_count++] = code_and_node;
    fill_with_error(connection.response.iovecs + connection.response.iovecs_count, scratch.id, //
                    std::string_view(code_and_node, code_len),                                 //
                    std::string_view(code_and_node + code_len, note_len));
    connection.response.iovecs_count += iovecs_for_error_k;
}

void ujrpc_call_send_error_unknown(ujrpc_call_t) {}
void ujrpc_call_send_error_out_of_memory(ujrpc_call_t) {}

#if __linuxer__
void uring_take_call(ujrpc_server_t server, uint16_t thread_idx) {
    // Unlike the classical synchronous interface, this implements only a part of the state machine,
    // is responsible for checking if a specific request has been completed. All of the submitted
    // memory must be preserved until we get the confirmation.
    engine_t& engine = *reinterpret_cast<engine_t*>(server);
    struct io_uring_cqe* cqe{};
    struct io_uring_sqe sqe {};
    struct sockaddr_in client_addr {};
    int ret = io_uring_wait_cqe(&engine.ring, &cqe);
    connection_t& connection = *(connection_t*)cqe->user_data;
    if (ret < 0)
        exit(-1);
    if (cqe->res < 0) {
        exit(1);
    }

    switch (connection.stage) {
    case stage_t::pre_accept_k:
        // Check if accepting the new connection request worked out.
        if (ret == -EAGAIN) {
            // No new connection request are present.
            // Try serving existing ones.
        } else {
            // A new connection request is available.
            // Drop the older one in favor of this.
            int old_connection_fd;
            io_uring_prep_close(&sqe, old_connection_fd);
        }

        io_uring_prep_recv(&sqe, connection.descriptor, &connection.embedded_packet[0], embedded_packet_capacity_k,
                           MSG_WAITALL);
        connection.stage = stage_t::pre_receive_k;
        break;
    case stage_t::pre_receive_k:
        // We were hoping to receive some data from the existing connection.
        auto length = cqe->flags;
        if (ret == 0) {
        }

        break;
    case stage_t::pre_completion_k:
        break;
    }

    io_uring_cqe_seen(&engine.ring, cqe);
}

#endif