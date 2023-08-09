/**
 * @brief JSON-RPC implementation for TCP/IP stack with `io_uring`.
 *
 * Supports:
 * > Thousands of concurrent stateful connections.
 * > Hundreds of physical execution threads.
 * > Both HTTP and HTTP-less raw JSON-RPC calls.
 *
 * @section Primary structures
 * - `engine_t`: primary server instance.
 * - `connection_t`: lifetime state of a single TCP connection.
 * - `scratch_space_t`: temporary memory used by a single thread at a time.
 * - `automata_t`: automata that accepts and responds to messages.
 *
 * @section Concurrency
 * The whole class is thread safe and can be used with as many threads as
 * defined during construction with `ucall_init`. Some `connection_t`-s
 * can, however, be simultaneously handled by two threads, if one logical
 * operation is split into multiple physical calls:
 *
 *      1.  Receiving packets with timeouts.
 *          This allows us to reconsider closing a connection every once
 *          in a while, instead of loyally waiting for more data to come.
 *      2.  Closing sockets gracefully.
 *
 * @section Linux kernel requirements
 * We need Submission Queue Polling to extract maximum performance from `io_uring`.
 * Many of the requests would get an additional `IOSQE_FIXED_FILE` flag, and the
 * setup call would receive `IORING_SETUP_SQPOLL`. Aside from those, we also
 * need to prioritize following efficient interfaces:
 * - `io_uring_prep_accept_direct` to alloc from reusable files list > 5.19..
 * - `io_uring_prep_read_fixed` to read into registered buffers.
 * - `io_uring_register_buffers`.
 * - `io_uring_register_files_sparse` > 5.19, or `io_uring_register_files` before that.
 * - `IORING_SETUP_COOP_TASKRUN` > 5.19.
 * - `IORING_SETUP_SINGLE_ISSUER` > 6.0.
 *
 * @author Ashot Vardanian
 *
 * @see Notable links:
 * https://man7.org/linux/man-pages/dir_by_project.html#liburing
 * https://jvns.ca/blog/2017/06/03/async-io-on-linux--select--poll--and-epoll/
 * https://stackoverflow.com/a/17665015/2766161
 */

#include <arpa/inet.h> // `inet_addr`
#include <fcntl.h>
#include <netinet/in.h> // `sockaddr_in`
#include <sys/epoll.h>
#include <sys/socket.h>
#include <sys/timerfd.h>
#include <sys/types.h>
#include <unistd.h>

#include <simdjson.h>

#include "ucall/ucall.h"

#include "automata.hpp"
#include "containers.hpp"
#include "network.hpp"
#include "server.hpp"

#pragma region Cpp Declaration

namespace sj = simdjson;
namespace sjd = sj::dom;
using namespace unum::ucall;

static constexpr int timeout_k = 100;

struct event_data_t {
    connection_t* connection{};
    descriptor_t descriptor{-1};
    void* buffer{nullptr};
    size_t buf_len{0};
    bool is_accept{false};
};

struct epoll_ctx_t {
    epoll_ctx_t(int max_events) { pool.reserve(max_events); }
    int epoll{-1};
    pool_gt<event_data_t> pool;
};

static int setnonblocking(int sockfd) {
    return fcntl(sockfd, F_SETFD, fcntl(sockfd, F_GETFD, 0) | O_NONBLOCK) == -1 ? -1 : 0;
}

static int epoll_ctl_am(int epfd, int op, int fd, event_data_t* data) {
    struct epoll_event ev;
    ev.events = op;
    ev.data.ptr = data;
    int res = epoll_ctl(epfd, EPOLL_CTL_ADD, fd, &ev);
    return res;
}

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

    // Allocation
    int socket_descriptor{-1};
    epoll_ctx_t* ectx = new epoll_ctx_t(config.queue_depth);

    // By default, let's open TCP port for IPv4.
    struct sockaddr_in address {};
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = inet_addr(config.hostname);
    address.sin_port = htons(config.port);

    server_t* server_ptr{};
    pool_gt<connection_t> connections{};
    array_gt<named_callback_t> callbacks{};
    buffer_gt<scratch_space_t> spaces{};
    buffer_gt<struct iovec> registered_buffers{};
    memory_map_t fixed_buffers{};

    // Try allocating all the necessary memory.
    server_ptr = (server_t*)std::malloc(sizeof(server_t));
    if (!server_ptr)
        goto cleanup;
    if (!callbacks.reserve(config.max_callbacks))
        goto cleanup;
    if (!fixed_buffers.reserve(ram_page_size_k * 2u * config.max_concurrent_connections))
        goto cleanup;
    if (!connections.reserve(config.max_concurrent_connections))
        goto cleanup;
    if (!spaces.resize(config.max_threads))
        goto cleanup;
    for (auto& space : spaces)
        if (space.parser.allocate(ram_page_size_k, ram_page_size_k / 2u) != sj::SUCCESS)
            goto cleanup;

    if (!registered_buffers.resize(config.max_concurrent_connections * 2u))
        goto cleanup;
    for (std::size_t i = 0; i != config.max_concurrent_connections; ++i) {
        auto& connection = connections.at_offset(i);
        auto inputs = fixed_buffers.ptr + ram_page_size_k * 2u * i;
        auto outputs = inputs + ram_page_size_k;
        connection.pipes.mount(inputs, outputs);

        registered_buffers[i * 2u].iov_base = inputs;
        registered_buffers[i * 2u].iov_len = ram_page_size_k;
        registered_buffers[i * 2u + 1u].iov_base = outputs;
        registered_buffers[i * 2u + 1u].iov_len = ram_page_size_k;
    }

    socket_descriptor = socket(AF_INET, SOCK_STREAM, 0);
    if (socket_descriptor < 0)
        goto cleanup;
    if (bind(socket_descriptor, (struct sockaddr*)&address, sizeof(address)) < 0)
        goto cleanup;
    if (setnonblocking(socket_descriptor) < 0)
        goto cleanup;
    if (listen(socket_descriptor, config.queue_depth) < 0)
        goto cleanup;
    ectx->epoll = epoll_create(1);
    if (ectx->epoll < 0)
        goto cleanup;

    // Initialize all the members.
    new (server_ptr) server_t();
    server_ptr->network_engine.network_data = ectx;
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
    if (ectx->epoll)
        epoll_ctl(ectx->epoll, EPOLL_CTL_DEL, socket_descriptor, nullptr);
    if (socket_descriptor >= 0)
        close(socket_descriptor);
    std::free(server_ptr);
    delete ectx;
    *server_out = nullptr;
}

void ucall_free(ucall_server_t punned_server) {
    if (!punned_server)
        return;

    server_t& server = *reinterpret_cast<server_t*>(punned_server);
    epoll_ctx_t* ctx = reinterpret_cast<epoll_ctx_t*>(server.network_engine.network_data);
    epoll_ctl(ctx->epoll, EPOLL_CTL_DEL, server.socket, nullptr);
    close(server.socket);
    server.~server_t();
    std::free(punned_server);
    delete ctx;
}

int network_engine_t::try_accept(descriptor_t socket, connection_t& connection) {
    epoll_ctx_t* ctx = reinterpret_cast<epoll_ctx_t*>(network_data);
    auto data = ctx->pool.alloc();
    data->connection = &connection;
    data->descriptor = socket;
    data->is_accept = true;
    epoll_ctl_am(ctx->epoll, EPOLLIN | EPOLLOUT | EPOLLET, socket, data);
    return 0;
}

void network_engine_t::set_stats_heartbeat(connection_t& connection) {}

template <size_t max_count_ak> std::size_t network_engine_t::pop_completed_events(completed_event_t* events) {
    epoll_ctx_t* ctx = reinterpret_cast<epoll_ctx_t*>(network_data);
    struct epoll_event ep_events[max_count_ak];
    int completed = 0;
    int num_events = epoll_wait(ctx->epoll, ep_events, max_count_ak, timeout_k);
    if (num_events < 0)
        return 0;
    for (int i = 0; i < num_events; ++i) {
        auto data = (event_data_t*)ep_events[i].data.ptr;
        auto& connection = *data->connection;
        if (data->is_accept) {
            int conn_sock =
                accept(data->descriptor, (struct sockaddr*)&connection.client_address, &connection.client_address_len);
            setnonblocking(conn_sock);
            events[completed].connection_ptr = &connection;
            events[completed].result = conn_sock;
            epoll_ctl(ctx->epoll, EPOLL_CTL_DEL, data->descriptor, NULL);
            ++completed;
        } else if (ep_events[i].events & EPOLLIN) {
            events[completed].connection_ptr = &connection;
            events[completed].result = recv(connection.descriptor, data->buffer, data->buf_len, 0);
            epoll_ctl(ctx->epoll, EPOLL_CTL_DEL, connection.descriptor, NULL);
            ++completed;
            ctx->pool.release(data);
        } else if (ep_events[i].events & EPOLLOUT) {
            events[completed].connection_ptr = &connection;
            events[completed].result = send(connection.descriptor, data->buffer, data->buf_len, 0);
            epoll_ctl(ctx->epoll, EPOLL_CTL_DEL, connection.descriptor, NULL);
            ctx->pool.release(data);
            ++completed;
        }
        if (ep_events[i].events & (EPOLLRDHUP | EPOLLHUP)) {
            epoll_ctl(ctx->epoll, EPOLL_CTL_DEL, connection.descriptor, NULL);
            events[completed].connection_ptr = &connection;
            events[completed].result = close(connection.descriptor);
            ctx->pool.release(data);
            ++completed;
        }
    }
    return completed;
}

bool network_engine_t::is_canceled(ssize_t res, connection_t const& connection) { return res == -ECANCELED; };

bool network_engine_t::is_corrupted(ssize_t res, unum::ucall::connection_t const& conn) {
    return res == -EBADF || res == -EPIPE || res == 0;
};

void network_engine_t::close_connection_gracefully(connection_t& connection) {
    epoll_ctx_t* ctx = reinterpret_cast<epoll_ctx_t*>(network_data);
    auto data = ctx->pool.alloc();
    data->connection = &connection;
    epoll_ctl_am(ctx->epoll, EPOLLET | EPOLLRDHUP | EPOLLHUP, connection.descriptor, data);
}

void network_engine_t::send_packet(connection_t& connection, void* buffer, size_t buf_len, size_t buf_index) {
    epoll_ctx_t* ctx = reinterpret_cast<epoll_ctx_t*>(network_data);
    auto data = ctx->pool.alloc();
    data->connection = &connection;
    data->buffer = buffer;
    data->buf_len = buf_len;
    epoll_ctl_am(ctx->epoll, EPOLLOUT | EPOLLET | EPOLLRDHUP | EPOLLHUP, connection.descriptor, data);
}

void network_engine_t::recv_packet(connection_t& connection, void* buffer, size_t buf_len, size_t buf_index) {
    epoll_ctx_t* ctx = reinterpret_cast<epoll_ctx_t*>(network_data);
    auto data = ctx->pool.alloc();
    data->connection = &connection;
    data->buffer = buffer;
    data->buf_len = buf_len;
    epoll_ctl_am(ctx->epoll, EPOLLIN | EPOLLET | EPOLLRDHUP | EPOLLHUP, connection.descriptor, data);
}
