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
#include "helpers/shared.hpp"

namespace sj = simdjson;
namespace sjd = sj::dom;
using namespace unum::ujrpc;

struct scratch_space_t {
    alignas(align_k) char embedded_packet[embedded_packet_capacity_k + sj::SIMDJSON_PADDING]{};
    char json_pointer[json_pointer_capacity_k]{};

    sjd::parser parser{};
    sjd::element tree{};
    std::string_view id{};
    bool is_batch{};
    bool is_async{};

    sjd::parser* dynamic_parser{};
    std::string_view dynamic_packet{};
};

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

void validate_and_forward_call(engine_t& engine, connection_t& connection) {
    scratch_space_t& scratch = *(scratch_space_t*)connection.scratch_space;
    sjd::element const& doc = scratch.tree;
    if (!doc.is_object())
        return ujrpc_call_reply_error(&connection, -32600, "The JSON sent is not a valid request object.", 44);

    // We don't support JSON-RPC before version 2.0.
    sj::simdjson_result<sjd::element> version = doc["jsonrpc"];
    if (!version.is_string() || version.get_string().value() != "2.0")
        return ujrpc_call_reply_error(&connection, -32600, "The request doesn't specify the 2.0 version.", 44);

    // Check if the shape of the requst is correct:
    sj::simdjson_result<sjd::element> id = doc["id"];
    bool id_invalid = !id.is_string() && !id.is_int64() && !id.is_uint64();
    if (id_invalid)
        return ujrpc_call_reply_error(&connection, -32600, "The request must have integer or string id.", 43);
    sj::simdjson_result<sjd::element> method = doc["method"];
    bool method_invalid = !method.is_string();
    if (method_invalid)
        return ujrpc_call_reply_error(&connection, -32600, "The method must be a string.", 28);
    sj::simdjson_result<sjd::element> params = doc["params"];
    bool params_present_and_invalid = !params.is_array() && !params.is_object() && params.error() == sj::SUCCESS;
    if (params_present_and_invalid)
        return ujrpc_call_reply_error(&connection, -32600, "Parameters can only be passed in arrays or objects.", 51);

    // TODO: Patch SIMD-JSON to extract the token
    scratch.id = "null";

    // Make sure we have such a method:
    auto method_name = method.get_string().value_unsafe();
    auto callbacks_end = engine.callbacks.data() + engine.callbacks.size();
    auto callback_it = std::find_if(engine.callbacks.data(), callbacks_end,
                                    [=](named_callback_t const& callback) { return callback.name == method_name; });
    if (callback_it == callbacks_end)
        return ujrpc_call_reply_error(&connection, -32601, "Method not found.", 17);

    return callback_it->callback(&connection);
}

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

    scratch_space_t& scratch = *(scratch_space_t*)connection.scratch_space;
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

/**
 * @brief Analyzes the contents of the packet, bifurcating pure JSON-RPC from HTTP1-based.
 * @warning This doesn't check the headers for validity or additional metadata.
 */
void forward_packet_wout_http_headers(engine_t& engine, connection_t& connection) {
    scratch_space_t& scratch = *(scratch_space_t*)connection.scratch_space;
    // A typical HTTP-header may look like this
    // POST /myservice HTTP/1.1
    // Host: rpc.example.com
    // Content-Type: application/json
    // Content-Length: ...
    // Accept: application/json
    std::string_view expected = "POST";
    std::string_view body = scratch.dynamic_packet;
    if (scratch.dynamic_packet.size() > expected.size() &&
        scratch.dynamic_packet.substr(0, expected.size()) == expected) {
        auto pos = scratch.dynamic_packet.find("\r\n\r\n");
        if (pos == std::string_view::npos)
            ujrpc_call_reply_error(&connection, -32700, "Invalid JSON was received by the server.", 40);
        scratch.dynamic_packet = scratch.dynamic_packet.substr(pos);
        split_packet_into_calls(engine, connection);
    } else
        return split_packet_into_calls(engine, connection);
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
    connection.scratch_space = &scratch;

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
    uint16_t max_callbacks = config && config->max_callbacks > 0 ? config->max_callbacks : 128u;
    uint16_t max_connections = config && config->max_connections > 0 ? config->max_connections : 1024u;
    uint16_t max_threads = config && config->max_threads ? config->max_threads : 1u;
    uint32_t max_lifetime_microsec = config && config->max_lifetime_microsec ? config->max_lifetime_microsec : 100'000u;

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
    if (!callbacks.alloc(max_callbacks))
        goto cleanup;
    if (!connections.alloc(max_connections))
        goto cleanup;
    if (!spaces.alloc(max_threads))
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

bool ujrpc_param_named_i64(ujrpc_call_t call, ujrpc_str_t name, int64_t* result_ptr) {
    connection_t& connection = *reinterpret_cast<connection_t*>(call);
    scratch_space_t& scratch = *(scratch_space_t*)connection.scratch_space;
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

void ujrpc_call_reply_content(ujrpc_call_t call, ujrpc_str_t body, size_t body_len) {
    connection_t& connection = *reinterpret_cast<connection_t*>(call);
    scratch_space_t& scratch = *(scratch_space_t*)connection.scratch_space;
    if (!body_len)
        body_len = std::strlen(body);

    // In case of a sinle request - immediately push into the socket.
    if (!scratch.is_batch && !scratch.is_async) {
        struct msghdr message {};
        struct iovec iovecs[iovecs_for_content_k] {};
        fill_with_content(iovecs, scratch.id, std::string_view(body, body_len));
        message.msg_iov = iovecs;
        message.msg_iovlen = iovecs_for_content_k;
        if (sendmsg(connection.descriptor, &message, 0) < 0)
            return ujrpc_call_send_error_unknown(call);
    }

    // In case of a batch or async request, preserve a copy of data on the heap.
    else {
        auto body_copy = (char*)std::malloc(body_len);
        if (!body_copy)
            return ujrpc_call_send_error_out_of_memory(call);
        std::memcpy(body_copy, body, body_len);
        connection.response.copies[connection.response.copies_count++] = body_copy;
        fill_with_content(connection.response.iovecs + connection.response.iovecs_count, scratch.id,
                          std::string_view(body, body_len));
        connection.response.iovecs_count += iovecs_for_content_k;
    }
}

void ujrpc_call_reply_error(ujrpc_call_t call, int code_int, ujrpc_str_t note, size_t note_len) {

    connection_t& connection = *reinterpret_cast<connection_t*>(call);
    scratch_space_t& scratch = *(scratch_space_t*)connection.scratch_space;
    if (!note_len)
        note_len = std::strlen(note);

    char code[16]{};
    std::to_chars_result res = std::to_chars(code, code + sizeof(code), code_int);
    auto code_len = res.ptr - code;
    if (res.ec != std::error_code())
        return ujrpc_call_send_error_unknown(call);

    // In case of a sinle request - immediately push into the socket.
    if (!scratch.is_batch && !scratch.is_async) {
        struct msghdr message {};
        struct iovec iovecs[iovecs_for_error_k] {};
        fill_with_error(iovecs, scratch.id,               //
                        std::string_view(code, code_len), //
                        std::string_view(note, note_len));
        message.msg_iov = iovecs;
        message.msg_iovlen = iovecs_for_error_k;
        if (sendmsg(connection.descriptor, &message, 0) < 0)
            return ujrpc_call_send_error_unknown(call);
    }

    // In case of a batch or async request, preserve a copy of data on the heap.
    else {
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