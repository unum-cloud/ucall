/**
 * @brief A benchmark for the construction speed of the UNSW index
 * and the resulting accuracy (recall) of the Approximate Nearest Neighbors
 * Search queries.
 */
#include <charconv> // `std::to_chars`
#include <thread>
#include <vector>

#include "ujrpc/ujrpc.h"

static void sum(ujrpc_call_t call) {
    int64_t a{}, b{};
    char c_str[256]{};
    bool got_a = ujrpc_param_named_i64(call, "a", 0, &a);
    bool got_b = ujrpc_param_named_i64(call, "b", 0, &b);
    if (!got_a || !got_b)
        return ujrpc_call_reply_error(call, 1, "Missing integer argument a and/or b", 0);

    std::to_chars_result print = std::to_chars(&c_str[0], &c_str[0] + sizeof(c_str), a + b, 10);
    ujrpc_call_reply_content(call, &c_str[0], print.ptr - &c_str[0]);
}

static void bot_or_not(ujrpc_call_t call) {
    char const* text_ptr{};
    size_t text_len{};
    bool got_text = ujrpc_param_named_str(call, "text", 4, &text_ptr, &text_len);
    if (!got_text)
        return ujrpc_call_reply_error(call, 1, "A tweet has to have a text field!", 0);

    bool is_bot = true; // TODO
    char const* result = is_bot ? "1" : "0";
    ujrpc_call_reply_content(call, result, 1);
}

int main(int argc, char** argv) {
    ujrpc_server_t server{};
    ujrpc_config_t config{};
    config.max_threads = 3;
    config.max_concurrent_connections = 10000;
    config.queue_depth = 4096 * config.max_threads;
    config.max_lifetime_exchanges = 100;
    ujrpc_init(&config, &server);
    if (!server)
        return -1;

    ujrpc_add_procedure(server, "sum", &sum);
    ujrpc_add_procedure(server, "bot_or_not", &bot_or_not);

    std::vector<std::thread> threads;
    for (uint16_t i = 0; i != config.max_threads; ++i)
        threads.emplace_back(&ujrpc_take_calls, server, i);
    for (auto& thread : threads)
        thread.join();

    ujrpc_free(server);
    return 0;
}