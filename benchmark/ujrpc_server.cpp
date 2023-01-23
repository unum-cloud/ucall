/**
 * @brief A benchmark for the construction speed of the UNSW index
 * and the resulting accuracy (recall) of the Approximate Nearest Neighbors
 * Search queries.
 */
#include <charconv> // `std::to_chars`

#include "ujrpc/ujrpc.h"

static void sum(ujrpc_call_t call) {
    int64_t a{}, b{};
    char c_str[256]{};
    bool got_a = ujrpc_param_named_i64(call, "a", &a);
    bool got_b = ujrpc_param_named_i64(call, "b", &b);
    if (!got_a || !got_b)
        return ujrpc_call_reply_error(call, 1, "Missing integer argument a and/or b", 0);

    std::to_chars_result print = std::to_chars(&c_str[0], &c_str[0] + sizeof(c_str), a + b, 10);
    ujrpc_call_reply_content(call, &c_str[0], print.ptr - &c_str[0]);
}

int main(int argc, char** argv) {
    ujrpc_server_t server{};
    ujrpc_config_t config{};
    ujrpc_init(&config, &server);
    if (!server)
        return -1;

    ujrpc_add_procedure(server, "sum", &sum);
    ujrpc_take_calls(server, 0);
    ujrpc_free(server);
    return 0;
}