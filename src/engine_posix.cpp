/**
 * @brief JSON-RPC implementation for TCP/IP stack with POSIX calls.
 * @author Ashot Vardanian
 */

#if defined(WIN32) || defined(_WIN32) || defined(__WIN32__) || defined(__NT__)
#define UCALL_IS_WINDOWS
#include <Ws2tcpip.h>
#include <io.h>
#include <winsock2.h>

#define SHUT_WR SD_SEND
#define SHUT_RD SD_RECEIVE
#define SHUT_RDWR SD_BOTH
// SO_REUSEPORT is not supported on Windows.
#define SO_REUSEPORT 0

#pragma comment(lib, "Ws2_32.lib")
#define UNICODE

#else
#include <arpa/inet.h>  // `inet_addr`
#include <netinet/in.h> // `sockaddr_in`

#include <sys/ioctl.h>
#include <sys/socket.h> // `recv`, `setsockopt`

#include <sys/uio.h>
#include <unistd.h>
#endif

#include <queue>

#include <simdjson.h>

#include "ucall/ucall.h"

#include "automata.hpp"
#include "network.hpp"
#include "server.hpp"

#pragma region Cpp Declaration

namespace sj = simdjson;
namespace sjd = sj::dom;
using namespace unum::ucall;

struct conn_ctx_t {
    connection_t* conn_ptr;
    ssize_t res;
};

struct posix_ctx_t {
    std::queue<conn_ctx_t> res_queue; // TODO replace with custom
    mutex_t queue_mutex;
    memory_map_t fixed_buffers{};
};

void ucall_init(ucall_config_t* config_inout, ucall_server_t* server_out) {

    // Simple sanity check
    if (!server_out && !config_inout)
        return;

    // Retrieve configs, if present
    ucall_config_t& config = *config_inout;
    if (!config.port)
        config.port = 8545u;
    if (!config.queue_depth)
        config.queue_depth = 4096u;
    if (!config.max_callbacks)
        config.max_callbacks = 128u;
    if (!config.max_concurrent_connections)
        config.max_concurrent_connections = 1024u;
    if (!config.max_threads)
        config.max_threads = 1u;
    if (!config.max_lifetime_micro_seconds)
        config.max_lifetime_micro_seconds = 100'000u;
    if (!config.max_lifetime_exchanges)
        config.max_lifetime_exchanges = 100u;
    if (!config.hostname)
        config.hostname = "0.0.0.0";

    // Allocate
    int socket_options{1};
    int socket_descriptor{-1};
    int uring_result{-1};
    posix_ctx_t* uctx = new posix_ctx_t();
    server_t* server_ptr{};
    pool_gt<connection_t> connections{};
    array_gt<named_callback_t> callbacks{};
    buffer_gt<scratch_space_t> spaces{};

    // By default, let's open TCP port for IPv4.
    struct sockaddr_in address {};
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = inet_addr(config.hostname);
    address.sin_port = htons(config.port);

    // Try allocating all the necessary memory.
    server_ptr = (server_t*)std::malloc(sizeof(server_t));
    if (!server_ptr)
        goto cleanup;
    if (!callbacks.reserve(config.max_callbacks))
        goto cleanup;
    if (!uctx->fixed_buffers.reserve(ram_page_size_k * 2u * config.max_concurrent_connections))
        goto cleanup;
    if (!connections.reserve(config.max_concurrent_connections))
        goto cleanup;
    if (!spaces.resize(config.max_threads))
        goto cleanup;
    for (auto& space : spaces)
        if (space.parser.allocate(ram_page_size_k, ram_page_size_k / 2u) != sj::SUCCESS)
            goto cleanup;

    for (std::size_t i = 0; i != config.max_concurrent_connections; ++i) {
        auto& connection = connections.at_offset(i);
        auto inputs = uctx->fixed_buffers.ptr + ram_page_size_k * 2u * i;
        auto outputs = inputs + ram_page_size_k;
        connection.pipes.mount(inputs, outputs);
    }

    // Configure the socket.
    socket_descriptor = socket(AF_INET, SOCK_STREAM, 0);
    if (socket_descriptor < 0)
        goto cleanup;
    if (setsockopt(socket_descriptor, SOL_SOCKET, SO_REUSEADDR | SO_REUSEPORT,
                   reinterpret_cast<char const*>(&socket_options), sizeof(socket_options)) == -1)
        errno;
    if (bind(socket_descriptor, (struct sockaddr*)&address, sizeof(address)) < 0)
        goto cleanup;
    if (listen(socket_descriptor, config.queue_depth) < 0)
        goto cleanup;

    // Initialize all the members.
    new (server_ptr) server_t();
    server_ptr->network_engine.network_data = uctx;
    server_ptr->socket = descriptor_t{socket_descriptor};
    server_ptr->protocol_type = config.protocol;
    server_ptr->max_lifetime_micro_seconds = config.max_lifetime_micro_seconds;
    server_ptr->max_lifetime_exchanges = config.max_lifetime_exchanges;
    server_ptr->engine.callbacks = std::move(callbacks);
    server_ptr->connections = std::move(connections);
    server_ptr->spaces = std::move(spaces);
    server_ptr->logs_file_descriptor = config.logs_file_descriptor;
    server_ptr->logs_format = config.logs_format ? std::string_view(config.logs_format) : std::string_view();
    *server_out = (ucall_server_t)server_ptr;
    return;

cleanup:
    errno;
    if (socket_descriptor >= 0)
        close(socket_descriptor);
    std::free(server_ptr);
    delete uctx;
    *server_out = nullptr;
}

void ucall_free(ucall_server_t punned_server) {
    if (!punned_server)
        return;

    server_t& server = *reinterpret_cast<server_t*>(punned_server);
    posix_ctx_t* ctx = reinterpret_cast<posix_ctx_t*>(server.network_engine.network_data);
    close(server.socket);
    server.~server_t();
    std::free(punned_server);
    delete ctx;
}

int network_engine_t::try_accept(descriptor_t socket, connection_t& connection) {
    posix_ctx_t* ctx = reinterpret_cast<posix_ctx_t*>(network_data);

    struct timeval timeout;
    timeout.tv_sec = 0;
    timeout.tv_usec = connection.next_wakeup;

    // Create fd_set to hold the server_socket
    fd_set readfds;
    FD_ZERO(&readfds);
    FD_SET(socket, &readfds);

    // Wait for connection or timeout
    int select_result = select(socket + 1, &readfds, NULL, NULL, &timeout);

    conn_ctx_t coctx{&connection};

    if (select_result > 0)
        coctx.res = accept(socket, &connection.client_address, &connection.client_address_len);
    else if (select_result == 0)
        coctx.res = -ECANCELED;
    else // -1
        return errno;

    ctx->queue_mutex.lock();
    ctx->res_queue.push(coctx);
    ctx->queue_mutex.unlock();

    return select_result;
}

void network_engine_t::set_stats_heartbeat(connection_t& connection) {
    posix_ctx_t* ctx = reinterpret_cast<posix_ctx_t*>(network_data);

    conn_ctx_t coctx{&connection, 0};
    // TODO what?

    ctx->queue_mutex.lock();
    ctx->res_queue.push(coctx);
    ctx->queue_mutex.unlock();
}

void network_engine_t::close_connection_gracefully(connection_t& connection) {
    posix_ctx_t* ctx = reinterpret_cast<posix_ctx_t*>(network_data);

    conn_ctx_t coctx{&connection, close(connection.descriptor)};
    if (coctx.res == -1)
        coctx.res = errno;

    ctx->queue_mutex.lock();
    ctx->res_queue.push(coctx);
    ctx->queue_mutex.unlock();
}

void network_engine_t::send_packet(connection_t& connection, void* buffer, size_t buf_len, size_t buf_index) {
    posix_ctx_t* ctx = reinterpret_cast<posix_ctx_t*>(network_data);

    conn_ctx_t coctx{&connection, send(connection.descriptor, buffer, buf_len, MSG_DONTWAIT | MSG_NOSIGNAL)};
    if (coctx.res == -1)
        coctx.res = errno;

    ctx->queue_mutex.lock();
    ctx->res_queue.push(coctx);
    ctx->queue_mutex.unlock();
}

void network_engine_t::recv_packet(connection_t& connection, void* buffer, size_t buf_len, size_t buf_index) {
    posix_ctx_t* ctx = reinterpret_cast<posix_ctx_t*>(network_data);

    conn_ctx_t coctx{&connection, recv(connection.descriptor, buffer, buf_len, MSG_DONTWAIT | MSG_NOSIGNAL)};
    if (coctx.res == -1)
        coctx.res = errno;

    ctx->queue_mutex.lock();
    ctx->res_queue.push(coctx);
    ctx->queue_mutex.unlock();
}

bool network_engine_t::is_canceled(ssize_t res, unum::ucall::connection_t const& conn) {
    return res == -ECANCELED || res == EWOULDBLOCK || res == -EAGAIN;
};

template <size_t max_count_ak> std::size_t network_engine_t::pop_completed_events(completed_event_t* events) {
    posix_ctx_t* ctx = reinterpret_cast<posix_ctx_t*>(network_data);

    size_t completed = 0;

    ctx->queue_mutex.lock();

    while (!ctx->res_queue.empty() && completed < max_count_ak) {
        auto& ev = ctx->res_queue.front();

        events[completed].connection_ptr = ev.conn_ptr;
        events[completed].result = ev.res;

        ++completed;
        ctx->res_queue.pop();
    }

    ctx->queue_mutex.unlock();

    return completed;
}