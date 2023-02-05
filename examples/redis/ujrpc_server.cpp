/**
 * @brief Example of building a Redis-like in-memory store with UJRPC.
 */
#include <cstdio> // `std::printf`
#include <string>
#include <unordered_map>

#include "ujrpc/ujrpc.h"

static std::unordered_map<std::string, std::string> store;

static void set(ujrpc_call_t call) {
    char const* key_ptr{};
    char const* value_ptr{};
    size_t key_len{};
    size_t value_len{};
    bool key_found = ujrpc_param_named_str(call, "key", 3, &key_ptr, &key_len);
    bool value_found = ujrpc_param_named_str(call, "value", 5, &value_ptr, &value_len);
    if (!key_found || !value_found)
        return ujrpc_call_reply_error_invalid_params(call);

    try {
        store.insert_or_assign(std::string_view{key_ptr, key_len}, std::string_view{value_ptr, value_len});
        return ujrpc_call_reply_content(call, "OK", 2);
    } catch (...) {
        return ujrpc_call_reply_error_out_of_memory(call);
    }
}

static void get(ujrpc_call_t call) {
    char const* key_ptr{};
    size_t key_len{};
    bool key_found = ujrpc_param_named_str(call, "key", 4, &key_ptr, &key_len);
    if (!key_found)
        return ujrpc_call_reply_error_invalid_params(call);

    auto iterator = store.find(std::string_view{key_ptr, key_len});
    if (iterator == store.end())
        return ujrpc_call_reply_content(call, "", 0);
    else
        return ujrpc_call_reply_content(call, iterator.second.c_str(), iterator.second.size());
}

int main(int argc, char** argv) {
    ujrpc_server_t server{};
    ujrpc_config_t config{};
    config.port = 6379;
    ujrpc_init(&config, &server);
    if (!server) {
        std::printf("Failed to initialize server!\n");
        return -1;
    }

    std::printf("Initialized server!\n");
    ujrpc_add_procedure(server, "set", &set);
    ujrpc_add_procedure(server, "get", &get);

    ujrpc_take_calls(server, 0);
    ujrpc_free(server);
    return 0;
}