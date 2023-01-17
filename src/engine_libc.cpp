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

static constexpr ssize_t buffer_capacity_k = 4096;
static constexpr size_t callbacks_capacity_k = 128;
static constexpr size_t json_pointer_capacity_k = 256;

template <std::size_t multiple_ak> constexpr std::size_t round_up_to(std::size_t n) {
    return ((n + multiple_ak - 1) / multiple_ak) * multiple_ak;
}

struct named_callback_t {
    ujrpc_str_t name{};
    ujrpc_callback_t callback{};
};

struct engine_libc_t {
    int socket_descriptor{};
    named_callback_t callbacks[callbacks_capacity_k]{};
    size_t callbacks_count{};
    char json_pointer[json_pointer_capacity_k]{};
    char buffer[buffer_capacity_k + sj::SIMDJSON_PADDING]{};
    sjd::parser parser;
    sjd::element current_request;
};

void forward_call(engine_libc_t& engine) {

    sjd::element const& doc = engine.current_request;
    if (!doc.is_object())
        return;

    // We don't support JSON-RPC before version 2.0.
    sjd::element version = doc["jsonrpc"];
    if (!version.is_string() || version.get_string().value() != "2.0")
        return;

    // Check if the shape of the requst is correct:
    sjd::element id = doc["id"];
    sjd::element method = doc["method"];
    sjd::element params = doc["params"];
    if (!method.is_string() || (!params.is_array() || !params.is_object()) ||
        (!id.is_string() || !id.is_int64() || !id.is_uint64()))
        return;

    // Make sure we have such a method:
    auto method_name = method.get_string().value_unsafe();
    auto callbacks_end = engine.callbacks + engine.callbacks_count;
    auto callback_it = std::find_if(engine.callbacks, callbacks_end,
                                    [=](named_callback_t const& callback) { return callback.name == method_name; });
    if (callback_it == callbacks_end)
        return;

    //
    auto call = (ujrpc_call_t)&engine;
    callback_it->callback(call);
}

void forward_call(engine_libc_t& engine, sjd::parser& parser, std::string_view body) {
    auto one_or_many = parser.parse(body.data(), body.size(), false);
    if (one_or_many.is_array())
        for (sjd::element const one : one_or_many) {
            engine.current_request = one;
            forward_call(engine);
        }
    else {
        engine.current_request = one_or_many.value();
        forward_call(engine);
    }
}

void ujrpc_init(int port, int queue_depth, ujrpc_server_t* server) {

    if (port < 0)
        port = 8545;
    if (queue_depth < 0)
        queue_depth = 256;

    auto server_ptr = (engine_libc_t*)std::malloc(sizeof(engine_libc_t));
    if (!server_ptr)
        return;
    new (server_ptr) engine_libc_t();

    // By default, let's open TCP port for IPv4.
    auto server_fd = socket(AF_INET, SOCK_STREAM, 0);
    if (server_fd < 0)
        return;

    int opt = 1;
    struct sockaddr_in address;
    auto opt_res = setsockopt(server_fd, SOL_SOCKET, SO_REUSEADDR | SO_REUSEPORT, &opt, sizeof(opt));
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = INADDR_ANY;
    address.sin_port = htons(port);

    auto bind_res = bind(server_fd, (struct sockaddr*)&address, sizeof(address));
    auto listen_res = listen(server_fd, queue_depth);
}

void ujrpc_add_procedure(ujrpc_server_t server, ujrpc_str_t name, ujrpc_callback_t callback) {
    engine_libc_t& engine = *reinterpret_cast<engine_libc_t*>(server);
    if (engine.callbacks_count + 1 < callbacks_capacity_k)
        engine.callbacks[++engine.callbacks_count] = {name, callback};
}

void ujrpc_take_call(ujrpc_server_t server) {
    engine_libc_t& engine = *reinterpret_cast<engine_libc_t*>(server);
    auto conn_fd = accept(engine.socket_descriptor, (struct sockaddr*)NULL, NULL);

    // Determine the input size
    auto size_flags = MSG_PEEK | MSG_TRUNC;
    ssize_t size = recv(engine.socket_descriptor, engine.buffer, buffer_capacity_k, size_flags);
    if (size <= buffer_capacity_k) {
        [[maybe_unused]] ssize_t received = recv(engine.socket_descriptor, engine.buffer, buffer_capacity_k, 0);

    } else {
        sjd::parser dynamic_parser;
        sj::error_code parser_error = dynamic_parser.allocate(size, size / 2);
        auto dynamic_buffer = std::aligned_alloc(16, round_up_to<16>(size + simdjson::SIMDJSON_PADDING));
        [[maybe_unused]] ssize_t received = recv(engine.socket_descriptor, dynamic_buffer, size, 0);
        std::free(dynamic_buffer);
    }

    close(conn_fd);
}

void ujrpc_take_calls(ujrpc_server_t server) {
    while (true) {
        ujrpc_take_call(server);
    }
}

void ujrpc_free(ujrpc_server_t server) {
    if (server) {
        engine_libc_t& engine = *reinterpret_cast<engine_libc_t*>(server);
        engine.~engine_libc_t();
        std::free(server);
    }
}

bool ujrpc_param_named_i64(ujrpc_call_t call, ujrpc_str_t name, int64_t* result_ptr) {
    engine_libc_t& engine = *reinterpret_cast<engine_libc_t*>(call);
    std::memcpy(engine.json_pointer, "/params/", 8);
    std::memcpy(engine.json_pointer + 8, name, std::strlen(name) + 1);
    auto value = engine.current_request.at_pointer(engine.json_pointer);

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
    engine_libc_t& engine = *reinterpret_cast<engine_libc_t*>(call);
    char const* prefix = R"({"jsonrpc":"2.0","id":"1", "method": )";
    // {"jsonrpc": "2.0", "error": {"code": -32601, "message": "Method not found"}, "id": "1"}
    struct iovec buffers[3];
    struct msghdr message;
    message.msg_iov = buffers;
    message.msg_iovlen = 3;

    sendmsg(engine.socket_descriptor, &message, 0);
}

void ujrpc_call_reply_error(ujrpc_call_t call, int, ujrpc_str_t, size_t) {}
