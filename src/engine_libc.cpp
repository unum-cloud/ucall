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

#include <liburing.h>

#include <charconv> // `std::to_chars`

#include <simdjson.h>

#include "shared.hpp"
#include "ujrpc/ujrpc.h"

namespace sj = simdjson;
namespace sjd = sj::dom;
using namespace unum::ujrpc;

enum descriptor_t : int {};
static constexpr descriptor_t bad_descriptor_k{-1};

enum class stage_t {
    pre_accept_k = 0,
    pre_receive_k,
    pre_completion_k,
};

struct connection_t;
struct connections_t;
struct named_callbacks_t;
struct scratch_space_t;
struct engine_t;

struct connection_t {
    descriptor_t descriptor{};
    /// @brief If connection must use an asynchronous interface like io_uring.
    bool is_async{};
    /// @brief The step of an asynchronous machine.
    stage_t stage{};
    /// @brief Determines the probability of reseting the connection, in favor of a new client.
    std::size_t skipped_cycles{};

    struct response_t {
        struct iovec* iovecs{};
        char** copies{};
        std::size_t iovecs_count{};
        std::size_t copies_count{};
    } response{};
};

class connections_t {

    connection_t* circle_{};
    std::size_t count_{};
    std::size_t capacity_{};
    std::size_t idx_newest_{};
    std::size_t idx_oldest_{};
    /// @brief Follows the tail (oldest), or races forward
    /// and cycles around all the active connections, if all
    /// of them are long-livers,
    std::size_t idx_to_poll_{};

  public:
    bool alloc(std::size_t n) noexcept {}

    descriptor_t drop_tail() noexcept {
        descriptor_t old = std::exchange(circle_[idx_oldest_].descriptor, bad_descriptor_k);
        idx_to_poll_ = idx_to_poll_ == idx_oldest_ ? (idx_to_poll_ + 1) % capacity_ : idx_to_poll_;
        idx_oldest_ = (idx_oldest_ + 1) % capacity_;
        count_--;
        return old;
    }

    void push_ahead(descriptor_t new_) noexcept {
        idx_newest_ = (idx_newest_ + 1) % capacity_;
        circle_[idx_newest_].descriptor = new_;
        circle_[idx_newest_].skipped_cycles = 0;
        circle_[idx_newest_].response.copies_count = 0;
        circle_[idx_newest_].response.iovecs_count = 0;
        count_++;
    }

    connection_t& poll() noexcept {
        auto connection_ptr = &circle_[idx_to_poll_];
        auto idx_to_poll_following = (idx_to_poll_ + 1) % count_;
        idx_to_poll_ = idx_to_poll_following == idx_newest_ ? idx_oldest_ : idx_to_poll_following;
        return circle_[idx_to_poll_];
    }

    connection_t& tail() noexcept { return circle_[idx_oldest_]; }
    connection_t& head() noexcept { return circle_[idx_newest_]; }
    std::size_t size() const noexcept { return count_; }
    std::size_t capacity() const noexcept { return capacity_; }
};

struct named_callbacks_t {
    named_callback_t* array{};
    std::size_t count{};
    std::size_t capacity{};

    ~named_callbacks_t() {
        if (array)
            std::free(array);
    }
};

struct scratch_space_t {
    alignas(align_k) char embedded_packet[embedded_packet_capacity_k + sj::SIMDJSON_PADDING]{};
    char json_pointer[json_pointer_capacity_k]{};

    sjd::parser parser{};
    sjd::element tree{};
    std::string_view id{};
    bool is_batch{};
};

struct engine_t {
    int socket_descriptor{};
    std::size_t max_batch_size{};
    struct io_uring ring {};
    /// @brief An array of function callbacks. Can be in dozens.
    named_callbacks_t callbacks{};
    /// @brief Same number of them, as max threads. Can be in hundreds.
    scratch_space_t spaces{};
    /// @brief A ring of reusable connections. Can be in millions.
    connections_t connections{};

    /// @brief A shared buffer for the reserved io-vecs of
    struct iovec* iovecs_tape{};
    bool iovecs_tape_reserve(std::size_t n) noexcept {
        return (iovecs_tape = (struct iovec*)std::malloc(sizeof(struct iovec) * n));
    }
};

void forward_call(engine_t& engine, connection_t& connection) {
    sjd::element const& doc = connection.request.tree;
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
    connection.request.id = "null";

    // Make sure we have such a method:
    auto method_name = method.get_string().value_unsafe();
    auto callbacks_end = engine.callbacks + engine.callbacks_count;
    auto callback_it = std::find_if(engine.callbacks, callbacks_end,
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
void forward_call_or_calls(engine_t& engine, connection_t& connection, sjd::parser& parser,
                           std::string_view json_body) {
    auto one_or_many = parser.parse(json_body.data(), json_body.size(), false);
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
        }

        connection.request.is_batch = true;
        for (sjd::element const one : many) {
            connection.request.tree = one;
            forward_call(engine, connection);
        }
        posix_send_reply(engine, connection);
        if (embedded_iovecs != connection.response.iovecs)
            std::free(std::exchange(connection.response.iovecs, embedded_iovecs));
    } else {
        connection.request.is_batch = false;
        connection.request.tree = one_or_many.value();
        forward_call(engine, connection);
        posix_send_reply(engine, connection);
    }
}

/**
 * @brief Analyzes the contents of the packet, bifurcating pure JSON-RPC from HTTP1-based.
 * @warning This doesn't check the headers for validity or additional metadata.
 */
void forward_packet(engine_t& engine, connection_t& connection, sjd::parser& parser, std::string_view body) {
    // A typical HTTP-header may look like this
    // POST /myservice HTTP/1.1
    // Host: rpc.example.com
    // Content-Type: application/json
    // Content-Length: ...
    // Accept: application/json
    std::string_view expected = "POST";
    if (body.size() > expected.size() && body.substr(0, expected.size()) == expected) {
        auto pos = body.find("\r\n\r\n");
        return pos == std::string_view::npos
                   ? ujrpc_call_reply_error(&connection, -32700, "Invalid JSON was received by the server.", 40)
                   : forward_call_or_calls(engine, connection, parser, body.substr(pos));
    } else
        return forward_call_or_calls(engine, connection, parser, body);
}

void uring_take_call(ujrpc_server_t server) {
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

connection_t* posix_refresh(engine_t& engine) {
    // If no pending connections are present on the queue, and the
    // socket is not marked as nonblocking, accept() blocks the caller
    // until a connection is present. If the socket is marked
    // nonblocking and no pending connections are present on the queue,
    // accept() fails with the error EAGAIN or EWOULDBLOCK.
    int connection_fd = accept(engine.socket_descriptor, (struct sockaddr*)NULL, NULL);
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

void posix_take_call(ujrpc_server_t server) {
    engine_t& engine = *reinterpret_cast<engine_t*>(server);
    connection_t* connection_ptr = posix_refresh(engine);
    connection_t& connection = *connection_ptr;

    // Wait until we have input.
    auto bytes_expected = recv(connection.descriptor, nullptr, 0, MSG_PEEK | MSG_TRUNC | MSG_DONTWAIT);
    int error = errno;
    if (error == EAGAIN || error == EWOULDBLOCK) {
        connection.skipped_cycles++;
        return;
    } else if (error != 0) {
        close(drop_if_tail(engine, connection_ptr));
        return;
    }

    // Either process it in the statically allocated memory,
    // or allocate dynamically, if the message is too long.
    if (bytes_expected <= embedded_packet_capacity_k) {
        auto buffer_ptr = &connection.embedded_packet[0];
        auto bytes_received = recv(connection.descriptor, buffer_ptr, embedded_packet_capacity_k, 0);
        forward_packet(engine, connection, connection.request.parser, std::string_view(buffer_ptr, bytes_received));
    } else {
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
        forward_packet(engine, connection, parser, std::string_view(buffer_ptr, bytes_received));
        std::free(buffer_ptr);
    }
}

void ujrpc_take_call(ujrpc_server_t server) { return posix_take_call(server); }

void ujrpc_init(ujrpc_config_t const* config, ujrpc_server_t* server) {

    // Simple sanity check
    if (!server)
        return;

    // Retrieve configs, if present
    int port = config && config->port > 0 ? config->port : 8545u;
    int queue_depth = config && config->queue_depth > 0 ? config->queue_depth : 256u;
    int batch_capacity = config && config->batch_capacity > 0 ? config->batch_capacity : 1024u;
    int max_callbacks = config && config->max_callbacks > 0 ? config->max_callbacks : 128u;
    int max_connections = config && config->max_connections > 0 ? config->max_connections : 1024u;
    int opt = 1;
    int server_fd = -1;
    engine_t* server_ptr = nullptr;
    struct iovec* embedded_iovecs = nullptr;
    connection_t* embedded_connections = nullptr;
    named_callback_t* embedded_callbacks = nullptr;
    std::size_t iovecs_in_batch = batch_capacity * std::max(iovecs_for_content_k, iovecs_for_error_k);
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
    embedded_iovecs = (struct iovec*)std::malloc(sizeof(struct iovec) * iovecs_in_batch);
    if (!embedded_iovecs)
        goto cleanup;
    embedded_callbacks = (named_callback_t*)std::malloc(sizeof(named_callback_t) * max_callbacks);
    if (!embedded_callbacks)
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
    if (parser.allocate(embedded_packet_capacity_k, embedded_packet_capacity_k / 2) != sj::SUCCESS)
        goto cleanup;

    // Initialize all the members.
    new (server_ptr) engine_t();
    server_ptr->socket_descriptor = server_fd;
    server_ptr->max_batch_size = batch_capacity;
    server_ptr->callbacks = embedded_callbacks;
    server_ptr->callbacks_capacity = max_callbacks;
    server_ptr->active_connection.response.iovecs = embedded_iovecs;
    server_ptr->active_connection.parser = std::move(parser);
    *server = (ujrpc_server_t)server_ptr;
    return;

cleanup:
    errno;
    if (server_fd >= 0)
        close(server_fd);
    if (server_ptr)
        std::free(server_ptr);
    if (embedded_iovecs)
        std::free(embedded_iovecs);
    if (embedded_callbacks)
        std::free(embedded_callbacks);
    *server = nullptr;
}

void ujrpc_add_procedure(ujrpc_server_t server, ujrpc_str_t name, ujrpc_callback_t callback) {
    engine_t& engine = *reinterpret_cast<engine_t*>(server);
    if (engine.callbacks_count + 1 >= engine.callbacks_capacity)
        return;
    engine.callbacks[engine.callbacks_count++] = {name, callback};
}

void ujrpc_take_calls(ujrpc_server_t server) {
    while (true) {
        ujrpc_take_call(server);
    }
}

void ujrpc_free(ujrpc_server_t server) {
    if (!server)
        return;

    engine_t& engine = *reinterpret_cast<engine_t*>(server);
    if (engine.callbacks)
        std::free(engine.callbacks);
    if (engine.active_connection.response.iovecs)
        std::free(engine.active_connection.response.iovecs);

    engine.~engine_t();
    std::free(server);
}

bool ujrpc_param_named_i64(ujrpc_call_t call, ujrpc_str_t name, int64_t* result_ptr) {
    connection_t& connection = *reinterpret_cast<connection_t*>(call);
    std::memcpy(connection.json_pointer, "/params/", 8);
    std::memcpy(connection.json_pointer + 8, name, std::strlen(name) + 1);
    auto value = connection.request.tree.at_pointer(connection.json_pointer);

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
    if (!body_len)
        body_len = std::strlen(body);

    // In case of a sinle request - immediately push into the socket.
    if (!connection.request.is_batch && !connection.is_async) {
        struct msghdr message {};
        struct iovec iovecs[iovecs_for_content_k] {};
        fill_with_content(iovecs, connection.request.id, std::string_view(body, body_len));
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
        fill_with_content(connection.response.iovecs + connection.response.iovecs_count, connection.request.id,
                          std::string_view(body, body_len));
        connection.response.iovecs_count += iovecs_for_content_k;
    }
}

void ujrpc_call_reply_error(ujrpc_call_t call, int code_int, ujrpc_str_t note, size_t note_len) {

    connection_t& connection = *reinterpret_cast<connection_t*>(call);
    if (!note_len)
        note_len = std::strlen(note);

    char code[16]{};
    std::to_chars_result res = std::to_chars(code, code + sizeof(code), code_int);
    auto code_len = res.ptr - code;
    if (res.ec != std::error_code())
        return ujrpc_call_send_error_unknown(call);

    // In case of a sinle request - immediately push into the socket.
    if (!connection.request.is_batch && !connection.is_async) {
        struct msghdr message {};
        struct iovec iovecs[iovecs_for_error_k] {};
        fill_with_error(iovecs, connection.request.id,    //
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
        fill_with_error(connection.response.iovecs + connection.response.iovecs_count, connection.request.id, //
                        std::string_view(code_and_node, code_len),                                            //
                        std::string_view(code_and_node + code_len, note_len));
        connection.response.iovecs_count += iovecs_for_error_k;
    }
}

void ujrpc_call_send_error_unknown(ujrpc_call_t) {}
void ujrpc_call_send_error_out_of_memory(ujrpc_call_t) {}
