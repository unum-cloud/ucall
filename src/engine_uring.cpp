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

#include <liburing.h>

#include <charconv> // `std::to_chars`

#include <simdjson.h>

#include "ujrpc/ujrpc.h"

#include "helpers/parse.hpp"
#include "helpers/reply.hpp"
#include "helpers/shared.hpp"

namespace sj = simdjson;
namespace sjd = sj::dom;
using namespace unum::ujrpc;

enum class stage_t {
    pre_accept_k = 0,
    pre_receive_k,
    pre_close_k,
    post_close_k,
};

struct exchange_buffer_t {
    // We always read the data into this buffer.
    // If it goes beyond it, we allocate dynamic memory, move existing contents there,
    // and then reallocate.
    alignas(align_k) char embedded[embedded_packet_capacity_k + sj::SIMDJSON_PADDING]{};
    std::size_t length{};
    std::size_t dynamic_capacity{};
    char* dynamic{};

    std::string_view view() const noexcept { return {dynamic ? dynamic : embedded, length}; }
    std::size_t capacity() const noexcept { return dynamic ? dynamic_capacity : embedded_packet_capacity_k; }
    void shrink_to_fit() noexcept {
        std::free(dynamic);
        length = 0;
        dynamic_capacity = 0;
    }
};

template <std::size_t iovecs_len_ak> void append(exchange_buffer_t& buffer, struct iovec* iovecs) noexcept {
    std::size_t added_length = 0;
#pragma unroll full
    for (std::size_t i = 0; i != iovecs_len_ak; ++i)
        added_length += iovecs[i].iov_len;
    if (buffer.length + added_length > buffer.capacity()) {
        // TODO:
    }
#pragma unroll full
    for (std::size_t i = 0; i != iovecs_len_ak; ++i) {
        std::memcpy(buffer.embedded + buffer.length, iovecs[i].iov_base, iovecs[i].iov_len);
        buffer.length += iovecs[i].iov_len;
    }
}

struct connection_t {
    exchange_buffer_t input{};
    exchange_buffer_t output{};

    /// @brief The file descriptor of the statefull connection over TCP.
    descriptor_t descriptor{};
    stage_t stage{};
    struct sockaddr client_addr {};
    socklen_t client_addr_len{sizeof(client_addr)};
    scratch_space_t* scratch_ptr{};
};

struct engine_t {
    descriptor_t socket{};
    struct io_uring ring {};
    std::atomic<std::size_t> active_connections{};
    std::atomic<std::size_t> reserved_connections{};

    /// @brief An array of function callbacks. Can be in dozens.
    array_gt<named_callback_t> callbacks{};
    /// @brief A circular container of reusable connections. Can be in millions.
    pool_gt<connection_t> connections{};
    /// @brief Same number of them, as max physical threads. Can be in hundreds.
    buffer_gt<scratch_space_t> spaces{};
};

void ujrpc_init(ujrpc_config_t const* config, ujrpc_server_t* server) {

    // Simple sanity check
    if (!server)
        return;

    // Retrieve configs, if present
    uint16_t port = config && config->port > 0 ? config->port : 8545u;
    uint16_t queue_depth = config && config->queue_depth > 0 ? config->queue_depth : 4096u;
    uint16_t callbacks_capacity = config && config->callbacks_capacity > 0 ? config->callbacks_capacity : 128u;
    uint16_t connections_capacity = config && config->connections_capacity > 0 ? config->connections_capacity : 1024u;
    uint16_t threads_limit = config && config->threads_limit ? config->threads_limit : 1u;
    uint32_t lifetime_microsec_limit =
        config && config->lifetime_microsec_limit ? config->lifetime_microsec_limit : 100'000u;

    // Allocate
    int socket_options = 1;
    int socket_descriptor = -1;
    int uring_result = -1;
    struct io_uring ring {};
    engine_t* server_ptr{};
    pool_gt<connection_t> connections{};
    array_gt<named_callback_t> callbacks{};
    buffer_gt<scratch_space_t> spaces{};

    // By default, let's open TCP port for IPv4.
    struct sockaddr_in address {};
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = htonl(INADDR_ANY);
    address.sin_port = htons(port);

    // Initialize `io_uring` first, it is the most likely to fail.
    uring_result = io_uring_queue_init(queue_depth, &ring, 0);
    if (uring_result != 0)
        goto cleanup;

    // Try allocating all the neccessary memory.
    server_ptr = (engine_t*)std::malloc(sizeof(engine_t));
    if (!server_ptr)
        goto cleanup;
    if (!callbacks.reserve(callbacks_capacity))
        goto cleanup;
    if (!connections.reserve(connections_capacity))
        goto cleanup;
    if (!spaces.reserve(threads_limit))
        goto cleanup;
    for (auto& space : spaces)
        if (space.parser.allocate(embedded_packet_capacity_k, embedded_packet_capacity_k / 2) != sj::SUCCESS)
            goto cleanup;

    // Configure the socket.
    socket_descriptor = socket(AF_INET, SOCK_STREAM, 0);
    if (socket_descriptor < 0)
        goto cleanup;
    if (setsockopt(socket_descriptor, SOL_SOCKET, SO_REUSEADDR | SO_REUSEPORT, &socket_options,
                   sizeof(socket_options)) < 0)
        goto cleanup;
    if (bind(socket_descriptor, (struct sockaddr*)&address, sizeof(address)) < 0)
        goto cleanup;
    if (listen(socket_descriptor, queue_depth) < 0)
        goto cleanup;

    // Initialize all the members.
    new (server_ptr) engine_t();
    server_ptr->socket = descriptor_t{socket_descriptor};
    server_ptr->callbacks = std::move(callbacks);
    server_ptr->connections = std::move(connections);
    server_ptr->spaces = std::move(spaces);
    server_ptr->ring = ring;
    *server = (ujrpc_server_t)server_ptr;
    return;

cleanup:
    errno;
    if (ring.ring_fd)
        io_uring_queue_exit(&ring);
    if (socket_descriptor >= 0)
        close(socket_descriptor);
    if (server_ptr)
        std::free(server_ptr);
    *server = nullptr;
}

void ujrpc_add_procedure(ujrpc_server_t server, ujrpc_str_t name, ujrpc_callback_t callback) {
    engine_t& engine = *reinterpret_cast<engine_t*>(server);
    if (engine.callbacks.size() + 1 < engine.callbacks.capacity())
        engine.callbacks.push_back({name, callback});
}

void ujrpc_free(ujrpc_server_t server) {
    if (!server)
        return;

    engine_t& engine = *reinterpret_cast<engine_t*>(server);
    io_uring_queue_exit(&engine.ring);
    engine.~engine_t();
    std::free(server);
}

void ujrpc_take_calls(ujrpc_server_t server, uint16_t thread_idx) {
    while (true) {
        ujrpc_take_call(server, thread_idx);
    }
}

void forward_call(engine_t& engine, connection_t& connection) noexcept {
    auto callback_or_error = find_callback(engine.callbacks, *connection.scratch_ptr);
    if (auto error_ptr = std::get_if<default_error_t>(&callback_or_error); error_ptr)
        return ujrpc_call_reply_error(&connection, error_ptr->code, error_ptr->note.data(), error_ptr->note.size());

    auto callback = std::get<ujrpc_callback_t>(callback_or_error);
    return callback(&connection);
}

void forward_call_or_calls(engine_t& engine, connection_t& connection) noexcept {
    scratch_space_t& scratch = *connection.scratch_ptr;
    sjd::parser& parser = *scratch.dynamic_parser;
    std::string_view json_body = scratch.dynamic_packet;
    parser.set_max_capacity(json_body.size());
    sjd::document doc;
    auto one_or_many = parser.parse_into_document(doc, json_body.data(), json_body.size(), false);
    if (one_or_many.error() == sj::CAPACITY)
        return ujrpc_call_send_error_out_of_memory(&connection);
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
        scratch.is_batch = true;

        // Start a JSON array.
        connection.output.embedded[0] = '[';
        connection.output.length = 1;

        for (sjd::element const one : many) {
            scratch.tree = one;
            forward_call(engine, connection);
        }

        // Replace the last comma with the closing bracket.
        (connection.output.dynamic ? connection.output.dynamic
                                   : connection.output.embedded)[connection.output.length - 1] = ']';
    } else {
        scratch.is_batch = false;
        scratch.tree = one_or_many.value();
        forward_call(engine, connection);

        // Drop the last comma, if present.
        connection.output.length -= connection.output.length != 0;
    }
}

void forward_packet(engine_t& engine, connection_t& connection, uint16_t thread_idx, std::string_view packet) noexcept {
    scratch_space_t& scratch = engine.spaces[thread_idx];
    connection.scratch_ptr = &scratch;

    auto json_or_error = strip_http_headers(packet);
    if (auto error_ptr = std::get_if<default_error_t>(&json_or_error); error_ptr)
        return ujrpc_call_reply_error(&connection, error_ptr->code, error_ptr->note.data(), error_ptr->note.size());

    scratch.dynamic_packet = packet = std::get<std::string_view>(json_or_error);
    if (packet.size() > embedded_packet_capacity_k) {
        sjd::parser parser;
        if (parser.allocate(packet.size(), packet.size() / 2) != sj::SUCCESS)
            ujrpc_call_send_error_out_of_memory(&engine);
        else {
            scratch.dynamic_parser = &parser;
            return forward_call_or_calls(engine, connection);
        }
    } else {
        scratch.dynamic_parser = &scratch.parser;
        return forward_call_or_calls(engine, connection);
    }
}

void ujrpc_take_call(ujrpc_server_t server, uint16_t thread_idx) {
    // Unlike the classical synchronous interface, this implements only a part of the connection machine,
    // is responsible for checking if a specific request has been completed. All of the submitted
    // memory must be preserved until we get the confirmation.
    engine_t& engine = *reinterpret_cast<engine_t*>(server);
    struct io_uring_sqe* sqe{};
    struct io_uring_cqe* cqe{};
    char* new_dynamic_buffer{};
    std::size_t new_dynamic_capacity{};
    int uring_response{};

    // Add a queue entry to accept new requests, if there are no such requests currently.
    std::size_t currently_awaiting{};
    if (!engine.active_connections && engine.reserved_connections.compare_exchange_strong(currently_awaiting, 1u)) {
        connection_t* connection_ptr = engine.connections.alloc();
        connection_t& connection = *connection_ptr;
        sqe = io_uring_get_sqe(&engine.ring);
        io_uring_prep_accept(sqe, engine.socket, &connection.client_addr, &connection.client_addr_len, 0);
        connection.stage = stage_t::pre_accept_k;
        io_uring_sqe_set_data(sqe, connection_ptr);
        uring_response = io_uring_submit(&engine.ring);
        if (uring_response != 1) {
            engine.connections.release(connection_ptr);
            engine.reserved_connections--;
        }
    }

    // Check if any of our requests have succeeded.
    struct __kernel_timespec timeout {};
    timeout.tv_nsec = 5000;
    uring_response = io_uring_wait_cqe(&engine.ring, &cqe);
    if (uring_response < 0 || !cqe)
        // Error codes:
        // -62: Timer expired.
        return;
    connection_t& connection = *(connection_t*)cqe->user_data;
    auto local_result = cqe->res;
    auto local_flags = cqe->flags;
    switch (-local_result) {
    case EPIPE:
    case EBADF:
        connection.input.shrink_to_fit();
        connection.output.shrink_to_fit();
        engine.connections.release(&connection);
        engine.reserved_connections -= connection.stage == stage_t::pre_accept_k;
        engine.active_connections--;
        io_uring_cqe_seen(&engine.ring, cqe);
        return;
    default:
        break;
    }
    if (local_result < 0) {
        // Unknown error.
        io_uring_cqe_seen(&engine.ring, cqe);
        exit(1);
    }

    switch (connection.stage) {
    case stage_t::pre_accept_k:
        // Check if accepting the new connection request worked out.
        engine.reserved_connections--;
        engine.active_connections++;
        sqe = io_uring_get_sqe(&engine.ring);
        connection.stage = stage_t::pre_receive_k;
        connection.descriptor = descriptor_t{local_result};
        connection.input.length = 0;
        io_uring_prep_recv(sqe, int(connection.descriptor), &connection.input.embedded[0], embedded_packet_capacity_k,
                           0);
        io_uring_sqe_set_data(sqe, &connection);
        uring_response = io_uring_submit(&engine.ring);
        break;
    case stage_t::pre_receive_k:
        // Move into dynamic memory, if we have received a maximum length chunk,
        // or if we have previously used dynamic memory.
        if (local_result >= embedded_packet_capacity_k || connection.input.dynamic_capacity) {
            if (connection.input.length + local_result + sj::SIMDJSON_PADDING > connection.input.dynamic_capacity) {
                new_dynamic_capacity = connection.input.dynamic_capacity ? connection.input.dynamic_capacity * 2
                                                                         : embedded_packet_capacity_k * 2;
                new_dynamic_buffer = (char*)std::aligned_alloc(align_k, new_dynamic_capacity);
                if (new_dynamic_buffer) {
                    connection.input.dynamic = new_dynamic_buffer;
                    connection.input.dynamic_capacity = new_dynamic_capacity;
                } else {
                    // TODO: Report a failure.
                }
            }
            std::memcpy(connection.input.dynamic + connection.input.length, connection.input.embedded, local_result);
        }
        connection.input.length += local_result;

        // If we have reached the end of the stream, it is time to analyze the contents
        // and send back a response.
        if (local_result < embedded_packet_capacity_k) {
            forward_packet(engine, connection, thread_idx, connection.input.view());
            connection.input.shrink_to_fit();
            // Some requests require no response.
            if (!connection.output.view().empty()) {
                connection.stage = stage_t::pre_close_k;
                sqe = io_uring_get_sqe(&engine.ring);
                io_uring_prep_send(sqe, int(connection.descriptor), connection.output.view().data(),
                                   connection.output.view().size(), 0);
                io_uring_sqe_set_data(sqe, &connection);
                uring_response = io_uring_submit(&engine.ring);
            }
        }
        // Poll for more data on that socket.
        else {
            connection.stage = stage_t::pre_receive_k;
            sqe = io_uring_get_sqe(&engine.ring);
            io_uring_prep_recv(sqe, int(connection.descriptor), &connection.input.embedded[0],
                               embedded_packet_capacity_k, 0);
            io_uring_sqe_set_data(sqe, &connection);
            uring_response = io_uring_submit(&engine.ring);
        }

        break;
    case stage_t::pre_close_k:
        // The data has been submitted, we can recalim the dynamic memory and close the TCP connection.
        connection.output.shrink_to_fit();
        connection.stage = stage_t::post_close_k;
        sqe = io_uring_get_sqe(&engine.ring);
        io_uring_prep_close(sqe, int(connection.descriptor));
        io_uring_sqe_set_data(sqe, &connection);
        uring_response = io_uring_submit(&engine.ring);
        break;
    case stage_t::post_close_k:
        // The data has been submitted, we can close the TCP connection.
        engine.connections.release(&connection);
        engine.active_connections--;
        break;
    }

    io_uring_cqe_seen(&engine.ring, cqe);
}

void ujrpc_call_reply_content(ujrpc_call_t call, ujrpc_str_t body, size_t body_len) {
    connection_t& connection = *reinterpret_cast<connection_t*>(call);
    scratch_space_t& scratch = *connection.scratch_ptr;
    if (scratch.id.empty())
        // No response is needed for "id"-less notifications.
        return;
    if (!body_len)
        body_len = std::strlen(body);

    struct iovec iovecs[iovecs_for_content_k] {};
    fill_with_content(iovecs, scratch.id, std::string_view(body, body_len), true);
    append<iovecs_for_content_k>(connection.output, iovecs);
}

void ujrpc_call_reply_error(ujrpc_call_t call, int code_int, ujrpc_str_t note, size_t note_len) {
    connection_t& connection = *reinterpret_cast<connection_t*>(call);
    scratch_space_t& scratch = *connection.scratch_ptr;
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

    struct iovec iovecs[iovecs_for_error_k] {};
    fill_with_error(iovecs, scratch.id, std::string_view(code, code_len), std::string_view(note, note_len), true);
    append<iovecs_for_error_k>(connection.output, iovecs);
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
    connection_t& connection = *reinterpret_cast<connection_t*>(call);
    scratch_space_t& scratch = *connection.scratch_ptr;
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