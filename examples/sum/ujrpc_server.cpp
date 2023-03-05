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

static void sum(ujrpc_call_t call, ujrpc_callback_tag_t) {
    int64_t a{}, b{};
    char c_str[256]{};
    bool got_a = ujrpc_param_named_i64(call, "a", 0, &a);
    bool got_b = ujrpc_param_named_i64(call, "b", 0, &b);
    if (!got_a || !got_b)
        return ujrpc_call_reply_error_invalid_params(call);

    std::to_chars_result print = std::to_chars(&c_str[0], &c_str[0] + sizeof(c_str), a + b, 10);
    ujrpc_call_reply_content(call, &c_str[0], print.ptr - &c_str[0]);
}

int main(int argc, char** argv) {

    cxxopts::Options options("Summation Server", "If device can't sum integers, just send them over with JSON-RPC :)");
    options.add_options()                                                                                             //
        ("h,help", "Print usage")                                                                                     //
        ("nic", "Networking Interface Internal IP to use", cxxopts::value<std::string>()->default_value("127.0.0.1")) //
        ("p,port", "On which port to server JSON-RPC", cxxopts::value<int>()->default_value("8545"))                  //
        ("j,threads", "How many threads to run", cxxopts::value<int>()->default_value("1"))                           //
        ("s,silent", "Silence statistics output", cxxopts::value<bool>()->default_value("false"))                     //
        ;
    auto result = options.parse(argc, argv);
    if (result.count("help")) {
        std::cout << options.help() << std::endl;
        exit(0);
    }

    ujrpc_server_t server{};
    ujrpc_config_t config{};
    config.interface = result["nic"].as<std::string>().c_str();
    config.port = result["port"].as<int>();
    config.max_threads = result["threads"].as<int>();
    config.max_concurrent_connections = 1024;
    config.queue_depth = 4096 * config.max_threads;
    config.max_lifetime_exchanges = UINT32_MAX;
    config.logs_file_descriptor = result["silent"].as<bool>() ? -1 : STDOUT_FILENO;
    config.logs_format = "human";

    ujrpc_init(&config, &server);
    if (!server) {
        std::printf("Failed to start server: %s:%i\n", config.interface, config.port);
        return -1;
    }

    std::printf("Initialized server: %s:%i\n", config.interface, config.port);
    std::printf("- %zu threads\n", static_cast<std::size_t>(config.max_threads));
    std::printf("- %zu max concurrent connections\n", static_cast<std::size_t>(config.max_concurrent_connections));
    if (result["silent"].as<bool>())
        std::printf("- silent\n");

    // Add all the callbacks we need
    ujrpc_add_procedure(server, "sum", &sum, nullptr);

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