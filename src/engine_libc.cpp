#include <netinet/in.h> // `sockaddr_in`
#include <stdlib.h>     // `std::aligned_malloc`
#include <sys/socket.h> // `recv`, `setsockopt`
#include <sys/types.h>
#include <sys/uio.h>
#include <unistd.h>

#include <charconv> // `std::to_chars`

#include <simdjson.h>

#include "shared.hpp"
#include "ujrpc/ujrpc.h"

namespace sj = simdjson;
namespace sjd = sj::dom;
using namespace unum::ujrpc;

struct connection_t {
    int descriptor{};
    sjd::parser parser{};
    sjd::element request_tree{};
    std::string_view request_id{};
    bool is_batch{};
    struct iovec* response_iovecs{};
    char** response_copies{};
    std::size_t response_iovecs_count{};
    std::size_t response_copies_count{};
    char param_json_pointer[json_pointer_capacity_k]{};
    alignas(align_k) char request_string[embedded_buffer_capacity_k + sj::SIMDJSON_PADDING]{};
};

struct engine_libc_t {
    int socket_descriptor{};
    named_callback_t* callbacks{};
    std::size_t callbacks_count{};
    std::size_t callbacks_capacity{};
    std::size_t max_batch_size{};
    connection_t active_connection{};
};

void forward_call(engine_libc_t& engine) {
    connection_t& connection = engine.active_connection;
    sjd::element const& doc = connection.request_tree;
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
    engine.active_connection.request_id = "null";

    // Make sure we have such a method:
    auto method_name = method.get_string().value_unsafe();
    auto callbacks_end = engine.callbacks + engine.callbacks_count;
    auto callback_it = std::find_if(engine.callbacks, callbacks_end,
                                    [=](named_callback_t const& callback) { return callback.name == method_name; });
    if (callback_it == callbacks_end)
        return ujrpc_call_reply_error(&connection, -32601, "Method not found.", 17);

    return callback_it->callback(&connection);
}

void send_reply(engine_libc_t& engine) {
    connection_t& connection = engine.active_connection;
    if (!connection.response_iovecs_count)
        return;

    struct msghdr message {};
    message.msg_iov = connection.response_iovecs;
    message.msg_iovlen = connection.response_iovecs_count;
    sendmsg(connection.descriptor, &message, 0);
}

void forward_call_or_calls(engine_libc_t& engine, sjd::parser& parser, std::string_view body) {
    connection_t& connection = engine.active_connection;
    auto one_or_many = parser.parse(body.data(), body.size(), false);
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
        auto embedded_iovecs = connection.response_iovecs;
        if (many.size() > embedded_batch_capacity_k) {
        }

        connection.is_batch = true;
        for (sjd::element const one : many) {
            connection.request_tree = one;
            forward_call(engine);
        }
        send_reply(engine);
        if (embedded_iovecs != connection.response_iovecs)
            std::free(std::exchange(connection.response_iovecs, embedded_iovecs));
    } else {
        connection.is_batch = false;
        connection.request_tree = one_or_many.value();
        forward_call(engine);
        send_reply(engine);
    }
}

void ujrpc_init(ujrpc_config_t const* config, ujrpc_server_t* server) {

    // Simple sanity check
    if (!server)
        return;

    // Retrieve configs, if present
    int port = config && config->port > 0 ? config->port : 8545;
    int queue_depth = config && config->queue_depth > 0 ? config->queue_depth : 256;
    int batch_capacity = config && config->batch_capacity > 0 ? config->batch_capacity : 1024;
    int max_callbacks = config && config->max_callbacks > 0 ? config->max_callbacks : 128;
    int opt = 1;
    int server_fd = -1;
    engine_libc_t* server_ptr = nullptr;
    struct iovec* embedded_iovecs = nullptr;
    std::size_t iovecs_in_batch = batch_capacity * std::max(iovecs_for_content_k, iovecs_for_error_k);
    sjd::parser parser;

    // By default, let's open TCP port for IPv4.
    struct sockaddr_in address;
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = INADDR_ANY;
    address.sin_port = htons(port);

    // Try allocating all the neccessary memory.
    server_ptr = (engine_libc_t*)std::malloc(sizeof(engine_libc_t));
    if (!server_ptr)
        goto cleanup;
    embedded_iovecs = (struct iovec*)std::malloc(sizeof(struct iovec) * iovecs_in_batch);
    if (!embedded_iovecs)
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
    if (parser.allocate(embedded_buffer_capacity_k, embedded_buffer_capacity_k / 2) != sj::SUCCESS)
        goto cleanup;

    // Initialize all the members.
    new (server_ptr) engine_libc_t();
    server_ptr->socket_descriptor = server_fd;
    server_ptr->max_batch_size = batch_capacity;
    server_ptr->active_connection.response_iovecs = embedded_iovecs;
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
    *server = nullptr;
}

void ujrpc_add_procedure(ujrpc_server_t server, ujrpc_str_t name, ujrpc_callback_t callback) {
    engine_libc_t& engine = *reinterpret_cast<engine_libc_t*>(server);
    if (engine.callbacks_count + 1 >= engine.callbacks_capacity)
        return;
    engine.callbacks[engine.callbacks_count++] = {name, callback};
}

void ujrpc_take_call(ujrpc_server_t server) {
    engine_libc_t& engine = *reinterpret_cast<engine_libc_t*>(server);
    connection_t& connection = engine.active_connection;
    sjd::parser parser;
    ssize_t bytes_received{}, bytes_expected{};
    auto buffer_ptr = &connection.request_string[0];
    auto connection_fd = connection.descriptor = accept(engine.socket_descriptor, (struct sockaddr*)NULL, NULL);
    if (connection_fd < 0) {
        errno;
        close(connection_fd);
        return;
    }

    // Wait until we have input.
    bytes_expected = recv(connection_fd, buffer_ptr, embedded_buffer_capacity_k, MSG_PEEK | MSG_TRUNC);
    if (bytes_expected <= 0) {
        if (bytes_expected < 0)
            errno;
        close(connection_fd);
        return;
    }

    // Either process it in the statically allocated memory,
    // or allocate dynamically, if the message is too long.
    if (bytes_expected <= embedded_buffer_capacity_k) {
        bytes_received = recv(connection_fd, buffer_ptr, embedded_buffer_capacity_k, 0);
        forward_call_or_calls(engine, connection.parser, std::string_view(buffer_ptr, bytes_received));
    } else {
        if (parser.allocate(bytes_expected, bytes_expected / 2) != sj::SUCCESS) {
            ujrpc_call_send_error_out_of_memory(&connection);
            close(connection_fd);
            return;
        }
        buffer_ptr = (char*)std::aligned_alloc(align_k, round_up_to<align_k>(bytes_expected + sj::SIMDJSON_PADDING));
        if (!buffer_ptr) {
            ujrpc_call_send_error_out_of_memory(&connection);
            close(connection_fd);
            return;
        }
        bytes_received = recv(connection_fd, buffer_ptr, bytes_expected, 0);
        forward_call_or_calls(engine, parser, std::string_view(buffer_ptr, bytes_received));
        std::free(buffer_ptr);
    }

    close(connection_fd);
}

void ujrpc_take_calls(ujrpc_server_t server) {
    while (true) {
        ujrpc_take_call(server);
    }
}

void ujrpc_free(ujrpc_server_t server) {
    if (!server)
        return;

    engine_libc_t& engine = *reinterpret_cast<engine_libc_t*>(server);
    if (engine.callbacks)
        std::free(engine.callbacks);
    if (engine.active_connection.response_iovecs)
        std::free(engine.active_connection.response_iovecs);

    engine.~engine_libc_t();
    std::free(server);
}

bool ujrpc_param_named_i64(ujrpc_call_t call, ujrpc_str_t name, int64_t* result_ptr) {
    connection_t& connection = *reinterpret_cast<connection_t*>(call);
    std::memcpy(connection.param_json_pointer, "/params/", 8);
    std::memcpy(connection.param_json_pointer + 8, name, std::strlen(name) + 1);
    auto value = connection.request_tree.at_pointer(connection.param_json_pointer);

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
    if (!connection.is_batch) {
        struct msghdr message {};
        struct iovec iovecs[iovecs_for_content_k] {};
        fill_with_content(iovecs, connection.request_id, std::string_view(body, body_len));
        message.msg_iov = iovecs;
        message.msg_iovlen = iovecs_for_content_k;
        if (sendmsg(connection.descriptor, &message, 0) < 0)
            return ujrpc_call_send_error_unknown(call);
    }

    // In case of a batch request, preserve a copy of data on the heap.
    else {
        auto body_copy = (char*)std::malloc(body_len);
        if (!body_copy)
            return ujrpc_call_send_error_out_of_memory(call);
        std::memcpy(body_copy, body, body_len);
        connection.response_copies[connection.response_copies_count++] = body_copy;
        fill_with_content(connection.response_iovecs + connection.response_iovecs_count, connection.request_id,
                          std::string_view(body, body_len));
        connection.response_iovecs_count += iovecs_for_content_k;
    }
}

void ujrpc_call_send_error_unknown(ujrpc_call_t) {}
void ujrpc_call_send_error_out_of_memory(ujrpc_call_t) {}

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
    if (!connection.is_batch) {
        struct msghdr message {};
        struct iovec iovecs[iovecs_for_error_k] {};
        fill_with_error(iovecs, connection.request_id,    //
                        std::string_view(code, code_len), //
                        std::string_view(note, note_len));
        message.msg_iov = iovecs;
        message.msg_iovlen = iovecs_for_error_k;
        if (sendmsg(connection.descriptor, &message, 0) < 0)
            return ujrpc_call_send_error_unknown(call);
    }

    // In case of a batch request, preserve a copy of data on the heap.
    else {
        auto code_and_node = (char*)std::malloc(code_len + note_len);
        if (!code_and_node)
            return ujrpc_call_send_error_out_of_memory(call);
        std::memcpy(code_and_node, code, code_len);
        std::memcpy(code_and_node + code_len, note, note_len);
        connection.response_copies[connection.response_copies_count++] = code_and_node;
        fill_with_error(connection.response_iovecs + connection.response_iovecs_count, connection.request_id, //
                        std::string_view(code_and_node, code_len),                                            //
                        std::string_view(code_and_node + code_len, note_len));
        connection.response_iovecs_count += iovecs_for_error_k;
    }
}
