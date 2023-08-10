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

struct event_data_t {
    connection_t* connection;
    descriptor_t descriptor;
    void* buffer;
    size_t buf_len;
    bool is_accept;
    bool is_close;
    bool active;

    event_data_t() noexcept { reset(); }

    void reset() noexcept {
        connection = nullptr;
        descriptor = invalid_descriptor_k;
        buffer = nullptr;
        buf_len = 0;
        is_accept = false;
        is_close = false;
        active = false;
    }
};

struct epoll_ctx_t {
    descriptor_t epoll{};
    array_gt<event_data_t> event_log{};

    size_t map_index(descriptor_t fd) const noexcept { return fd % event_log.capacity(); }
    event_data_t& get_data(descriptor_t fd) noexcept { return event_log[map_index(fd)]; }
};

static int setnonblocking(int sockfd) {
    return fcntl(sockfd, F_SETFD, fcntl(sockfd, F_GETFD, 0) | O_NONBLOCK) == -1 ? -1 : 0;
}

static int epoll_ctl_am(int epfd, int op, int fd, int keep_fd = -1) {
    struct epoll_event ev;
    ev.events = op;
    ev.data.fd = keep_fd == -1 ? fd : keep_fd;
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
    int socket_options{1};
    epoll_ctx_t* ectx = new epoll_ctx_t();

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
    if (!ectx->event_log.reserve(config.queue_depth))
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
    if (setsockopt(socket_descriptor, SOL_SOCKET, SO_REUSEADDR | SO_REUSEPORT,
                   reinterpret_cast<char const*>(&socket_options), sizeof(socket_options)) == -1)
        errno;
    if (bind(socket_descriptor, (struct sockaddr*)&address, sizeof(address)) < 0)
        goto cleanup;
    if (setnonblocking(socket_descriptor) < 0)
        goto cleanup;
    if (listen(socket_descriptor, config.queue_depth) < 0)
        goto cleanup;
    ectx->epoll = epoll_create(1);
    if (ectx->epoll < 0)
        goto cleanup;
    setnonblocking(socket_descriptor);

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
    if (ectx)
        delete ectx;
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
    close(server.socket);
    server.~server_t();
    std::free(punned_server);
    delete ctx;
}

int network_engine_t::try_accept(descriptor_t socket, connection_t& connection) noexcept {
    epoll_ctx_t* ctx = reinterpret_cast<epoll_ctx_t*>(network_data);
    event_data_t& data = ctx->get_data(socket);
    if (data.active)
        return -ECANCELED;

    data.connection = &connection;
    data.descriptor = socket;
    data.active = true;
    data.is_accept = true;
    if (epoll_ctl_am(ctx->epoll, EPOLLIN | EPOLLET | EPOLLONESHOT, socket) < 0) {
        data.reset();
        return -ECANCELED;
    }
    return 0;
}

void network_engine_t::set_stats_heartbeat(connection_t& connection) noexcept {}

template <size_t max_count_ak> std::size_t network_engine_t::pop_completed_events(completed_event_t* events) noexcept {
    epoll_ctx_t* ctx = reinterpret_cast<epoll_ctx_t*>(network_data);
    struct epoll_event ep_events[max_count_ak];
    size_t completed = 0;
    int num_events = epoll_wait(ctx->epoll, ep_events, max_count_ak, max_inactive_duration_ns_k / 1'000'000);
    if (num_events < 0)
        return 0;

    for (int i = 0; i < num_events; ++i) {
        descriptor_t fd = ep_events[i].data.fd;
        event_data_t& data = ctx->get_data(fd);
        connection_t* connection = data.connection;

        if (data.is_accept) {
            descriptor_t conn_sock =
                accept(fd, (struct sockaddr*)&connection->client_address, &connection->client_address_len);
            setnonblocking(conn_sock);
            events[completed].connection_ptr = connection;
            events[completed].result = conn_sock;
            if (conn_sock > 0) {
                ctx->get_data(conn_sock).active = true;
                ctx->get_data(conn_sock).connection = connection;
            }
            epoll_ctl(ctx->epoll, EPOLL_CTL_DEL, fd, NULL);
            data.active = false;
            ++completed;
            continue;
        } else if (ep_events[i].events & EPOLLIN) {
            if (data.is_close) {
                epoll_ctl(ctx->epoll, EPOLL_CTL_DEL, data.descriptor, NULL);
                epoll_ctl(ctx->epoll, EPOLL_CTL_DEL, connection->descriptor, NULL);
                events[completed].connection_ptr = connection;
                events[completed].result = close(connection->descriptor);
                close(data.descriptor);
                ++completed;
                data.reset();
                continue;
            } else {
                events[completed].connection_ptr = connection;
                events[completed].result = recv(connection->descriptor, data.buffer, data.buf_len, MSG_NOSIGNAL);
                ++completed;
                epoll_ctl(ctx->epoll, EPOLL_CTL_DEL, connection->descriptor, NULL);
            }
        } else if (ep_events[i].events & EPOLLOUT) {
            events[completed].connection_ptr = connection;
            events[completed].result = send(connection->descriptor, data.buffer, data.buf_len, MSG_NOSIGNAL);
            ++completed;
            epoll_ctl(ctx->epoll, EPOLL_CTL_DEL, connection->descriptor, NULL);
        }

        if (ep_events[i].events & (EPOLLRDHUP | EPOLLHUP | EPOLLERR) && data.active) {
            events[completed].connection_ptr = connection;
            events[completed].result = -ECONNRESET;
            data.active = false;
            ++completed;
        }
    }
    return completed;
}

bool network_engine_t::is_canceled(ssize_t res, connection_t const& connection) noexcept { return res == -ECANCELED; };

bool network_engine_t::is_corrupted(ssize_t res, unum::ucall::connection_t const& conn) noexcept {
    return res == -EBADF || res == -EPIPE || res == -ECONNRESET;
};

void network_engine_t::close_connection_gracefully(connection_t& connection) noexcept {

    descriptor_t timer_fd = timerfd_create(CLOCK_MONOTONIC, 0);
    itimerspec timer_spec;
    timer_spec.it_value.tv_sec = 0;
    timer_spec.it_value.tv_nsec = 1;
    timer_spec.it_interval.tv_sec = 0;
    timer_spec.it_interval.tv_nsec = 0;
    timerfd_settime(timer_fd, 0, &timer_spec, NULL);

    epoll_ctx_t* ctx = reinterpret_cast<epoll_ctx_t*>(network_data);
    event_data_t& data = ctx->get_data(connection.descriptor);
    data.descriptor = timer_fd;
    data.is_close = true;
    epoll_ctl_am(ctx->epoll, EPOLLIN, timer_fd, connection.descriptor);
}

void network_engine_t::send_packet(connection_t& connection, void* buffer, size_t buf_len, size_t buf_index) noexcept {
    epoll_ctx_t* ctx = reinterpret_cast<epoll_ctx_t*>(network_data);
    event_data_t& data = ctx->get_data(connection.descriptor);
    if (!data.active)
        return;
    data.buffer = buffer;
    data.buf_len = buf_len;
    epoll_ctl_am(ctx->epoll, EPOLLOUT | EPOLLET | EPOLLRDHUP | EPOLLHUP | EPOLLONESHOT, connection.descriptor);
}

void network_engine_t::recv_packet(connection_t& connection, void* buffer, size_t buf_len, size_t buf_index) noexcept {
    epoll_ctx_t* ctx = reinterpret_cast<epoll_ctx_t*>(network_data);
    event_data_t& data = ctx->get_data(connection.descriptor);
    if (!data.active)
        return;
    data.buffer = buffer;
    data.buf_len = buf_len;
    epoll_ctl_am(ctx->epoll, EPOLLIN | EPOLLET | EPOLLRDHUP | EPOLLHUP | EPOLLONESHOT, connection.descriptor);
}
