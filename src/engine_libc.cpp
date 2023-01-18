#include <netinet/in.h> // `sockaddr_in`
#include <stdlib.h>     // `std::aligned_malloc`
#include <sys/socket.h> // `recv`, `setsockopt`
#include <sys/types.h>
#include <sys/uio.h>
#include <unistd.h>

#include <simdjson.h>

#include "ujrpc/ujrpc.h"

namespace sj = simdjson;
namespace sjd = simdjson::dom;

/// @brief To avoid dynamic memory allocations on tiny requests,
/// for every connection we keep a tiny embedded buffer of this capacity.
static constexpr std::size_t embedded_buffer_capacity_k = 4096;
/// @brief The maximum length of JSON-Pointer, we will use
/// to lookup parameters in heavily nested requests.
/// A performance-oriented API will have maximum depth of 1 token.
/// Some may go as far as 5 token, or roughly 50 character.
static constexpr std::size_t json_pointer_capacity_k = 256;
/// @brief Assuming we have a static 4KB `embedded_buffer_capacity_k`
/// for our messages, we may receive an entirely invalid request like:
///     [0,0,0,0,...]
/// It will be recognized as a batch request with up to 2048 unique
/// requests, and each will be replied with an error message.
static constexpr std::size_t embedded_batch_capacity_k = 2048;
/// @brief When preparing replies to requests, instead of allocating
/// a new tape and joining them together, we assemble the requests
/// `iovec`-s to pass to the kernel.
static constexpr std::size_t iovecs_per_response_k = 7;
/// @brief Needed for largest-register-aligned memory addressing.
static constexpr std::size_t align_k = 64;

template <std::size_t multiple_ak> constexpr std::size_t round_up_to(std::size_t n) {
    return ((n + multiple_ak - 1) / multiple_ak) * multiple_ak;
}

struct named_callback_t {
    ujrpc_str_t name{};
    ujrpc_callback_t callback{};
};

struct connection_t {
    int descriptor{};
    sjd::parser parser{};
    sjd::element request_tree{};
    std::string_view request_id{};
    bool is_batch{};
    struct iovec* response_iovecs{};
    std::size_t response_iovecs_count{};
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

void forward_call(engine_libc_t& engine, sjd::parser& parser, std::string_view body) {
    connection_t& connection = engine.active_connection;
    auto one_or_many = parser.parse(body.data(), body.size(), false);
    if (one_or_many.error() != sj::SUCCESS)
        return ujrpc_call_reply_error(&connection, -32700, "Invalid JSON was received by the server.", 40);

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
    embedded_iovecs = (struct iovec*)std::malloc(sizeof(struct iovec) * batch_capacity * iovecs_per_response_k);
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
    auto connection_fd = connection.descriptor = accept(engine.socket_descriptor, (struct sockaddr*)NULL, NULL);
    if (connection_fd < 0) {
        errno;
        return;
    }

    // Wait until we have input
    auto size_flags = MSG_PEEK | MSG_TRUNC;
    auto buffer_ptr = &connection.request_string[0];
    ssize_t size = recv(connection_fd, buffer_ptr, embedded_buffer_capacity_k, size_flags);
    if (size <= 0) {
        if (size < 0) {
            int error = errno;
        }
        close(connection_fd);
        return;
    }

    // Either process it in the statically allocated memory,
    // or allocate dynamically, if the message is too long.
    if (size <= embedded_buffer_capacity_k) {
        [[maybe_unused]] ssize_t received = recv(connection_fd, buffer_ptr, embedded_buffer_capacity_k, 0);
        forward_call(engine, connection.parser, std::string_view(buffer_ptr, received));
    } else {
        sjd::parser parser;
        sj::error_code parser_error = parser.allocate(size, size / 2);
        buffer_ptr = (char*)std::aligned_alloc(align_k, round_up_to<align_k>(size + simdjson::SIMDJSON_PADDING));
        [[maybe_unused]] ssize_t received = recv(connection_fd, buffer_ptr, size, 0);
        forward_call(engine, parser, std::string_view(buffer_ptr, received));
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
    // Communication example would be:
    // --> {"jsonrpc": "2.0", "method": "subtract", "params": [42, 23], "id": 1}
    // <-- {"jsonrpc": "2.0", "result": 19, "id": 1}
    connection_t& connection = *reinterpret_cast<connection_t*>(call);

    // Put-in the parts in pieces.
    // The kernel with join them.
    struct iovec* buffers = connection.response_iovecs + connection.response_iovecs_count;
    char const* protocol_prefix = R"({"jsonrpc":"2.0","id":)";
    buffers[0].iov_base = (char*)protocol_prefix;
    buffers[0].iov_len = 22;
    buffers[1].iov_base = (char*)connection.request_id.data();
    buffers[1].iov_len = connection.request_id.size();
    char const* result_separator = R"(,"result":)";
    buffers[2].iov_base = (char*)result_separator;
    buffers[2].iov_len = 10;
    buffers[3].iov_base = (char*)body;
    buffers[3].iov_len = body_len;
    char const* protocol_suffix = R"(})";
    buffers[4].iov_base = (char*)protocol_suffix;
    buffers[4].iov_len = 1;
    connection.response_iovecs_count += 5;
}

void ujrpc_call_reply_error(ujrpc_call_t call, int error_code, ujrpc_str_t error_message, size_t error_length) {
    // Communication example would be:
    // --> {"jsonrpc": "2.0", "method": "foobar", "id": "1"}
    // <-- {"jsonrpc": "2.0", "error": {"code": -32601, "message": "Method not found"}, "id": "1"}
    connection_t& connection = *reinterpret_cast<connection_t*>(call);
    if (!error_length)
        error_length = std::strlen(error_message);

    char printed_code[64]{};

    // Put-in the parts in pieces.
    // The kernel with join them.
    struct iovec* buffers = connection.response_iovecs + connection.response_iovecs_count;
    char const* protocol_prefix = R"({"jsonrpc":"2.0","id":)";
    buffers[0].iov_base = (char*)protocol_prefix;
    buffers[0].iov_len = 22;
    buffers[1].iov_base = (char*)connection.request_id.data();
    buffers[1].iov_len = connection.request_id.size();
    char const* result_separator = R"(,"error":)";
    buffers[2].iov_base = (char*)result_separator;
    buffers[2].iov_len = 9;
    buffers[3].iov_base = (char*)error_message;
    buffers[3].iov_len = error_length;
    char const* protocol_suffix = R"(})";
    buffers[4].iov_base = (char*)protocol_suffix;
    buffers[4].iov_len = 1;
    connection.response_iovecs_count += 5;
}
