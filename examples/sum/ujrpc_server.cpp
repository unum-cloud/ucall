/**
 * @brief Example of a web server built with UJRPC in C++.
 */
#include <charconv> // `std::to_chars`
#include <cstdio>   // `std::fprintf`
#include <thread>
#include <unistd.h> // `STDOUT_FILENO`
#include <vector>

#include <cxxopts.hpp>

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

    cxxopts::Options options("Summation Server", "If device can't sum integers, just send them over with JSON-RPC :)");
    options.add_options()                                                                                             //
        ("h,help", "Print usage")                                                                                     //
        ("nic", "Networking Interface Internal IP to use", cxxopts::value<std::string>()->default_value("127.0.0.1")) //
        ("p,port", "On which port to server JSON-RPC", cxxopts::value<int>()->default_value("8545"))                  //
        ("j,threads", "How many threads to run", cxxopts::value<int>()->default_value("1"))                           //
        ("s,silent", "Silence statistics output", cxxopts::value<bool>())                                             //
        ;
    auto result = options.parse(argc, argv);
    if (result.count("help")) {
        std::cout << options.help() << std::endl;
        exit(0);
    }

    ujrpc_server_t server{};
    ujrpc_config_t config{};
    config.interface = options.count("nic") ? result["port"].as<std::string>().c_str() : nullptr;
    config.port = result["port"].as<int>();
    config.max_threads = result["threads"].as<int>();
    config.max_concurrent_connections = 1024;
    config.queue_depth = 4096 * config.max_threads;
    config.max_lifetime_exchanges = 512;
    config.logs_file_descriptor = result.count("silent") ? -1 : STDOUT_FILENO;
    config.logs_format = "human";

    ujrpc_init(&config, &server);
    if (!server) {
        std::printf("Failed to initialize server!\n");
        return -1;
    }

    std::printf("Initialized server!\n");
    ujrpc_add_procedure(server, "sum", &sum);
    ujrpc_add_procedure(server, "bot_or_not", &bot_or_not);

    if (config.max_threads > 1) {
        std::vector<std::thread> threads;
        for (uint16_t i = 0; i != config.max_threads; ++i)
            threads.emplace_back(&ujrpc_take_calls, server, i);
        for (auto& thread : threads)
            thread.join();
    } else
        ujrpc_take_calls(server, 0);

    ujrpc_free(server);
    return 0;
}