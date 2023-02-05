/**
 * @brief Example of building a Redis-like in-memory store with UJRPC.
 *
 * @see Reading materials on using the C++ PyTorch Frontend.
 * https://pytorch.org/tutorials/advanced/cpp_frontend.html
 * https://pytorch.org/cppdocs/installing.html
 */
#include <cstdio> // `std::printf`
#include <string>
#include <unordered_map>

#include <torch/torch.h>

#include "ujrpc/ujrpc.h"

static std::unordered_map<std::string, std::string> store;

static void summarize(ujrpc_call_t call) {
    char const* text_ptr{};
    size_t text_len{};
    bool text_found = ujrpc_param_named_str(call, "text", 3, &text_ptr, &text_len);
    if (!text_found)
        return ujrpc_call_reply_error_invalid_params(call);

    return ujrpc_call_reply_content(call, "OK", 2);
}

static void continue_(ujrpc_call_t call) {
    char const* text_ptr{};
    size_t text_len{};
    bool text_found = ujrpc_param_named_str(call, "text", 4, &text_ptr, &text_len);
    if (!text_found)
        return ujrpc_call_reply_error_invalid_params(call);

    return ujrpc_call_reply_content(call, "", 0);
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
    ujrpc_add_procedure(server, "summarize", &summarize);
    ujrpc_add_procedure(server, "continue", &continue_);

    ujrpc_take_calls(server, 0);
    ujrpc_free(server);
    return 0;
}