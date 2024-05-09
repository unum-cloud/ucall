#pragma once

#include "shared.hpp"
#include "ucall/ucall.h" // ucall_config_t

namespace unum::ucall {

struct conn_t {
    ~conn_t() noexcept { }

    int fd{};
    /// @brief A small memory buffer to store small requests.
    alignas(align_k) char packet_buffer[4 * ram_page_size_k + sj::SIMDJSON_PADDING]{};
    void *user_data;
};

static int socket_fd;
static int(*data_cb)(conn_t *, char *, int );
/**
 * @brief 
 */
inline int net_init(ucall_config_t& config, int(*cb)(conn_t *, char *, int )) noexcept {
    if ( cb == NULL ) return -1;
    data_cb = cb;

    int socket_options{1};

    // By default, let's open TCP port for IPv4.
    struct sockaddr_in address;
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = inet_addr(config.hostname);
    address.sin_port = htons(config.port);

    socket_fd = socket(AF_INET, SOCK_STREAM, 0);
    if (socket_fd < 0)
        goto cleanup;
    // Optionally configure the socket, but don't always expect it to succeed.
    if (setsockopt(socket_fd, SOL_SOCKET, SO_REUSEADDR | SO_REUSEPORT,
                   reinterpret_cast<char const*>(&socket_options), sizeof(socket_options)) == -1)
        errno;
    if (bind(socket_fd, (struct sockaddr*)&address, sizeof(address)) < 0)
        goto cleanup;
    if (listen(socket_fd, config.queue_depth) < 0)
        goto cleanup;

    return socket_fd;

cleanup:
    if (socket_fd >= 0)
        close(socket_fd);

    return -1;
}

/**
 * @brief 
 */
inline void net_run(void *user_data) noexcept {
    
    while(true) {

        int fd = accept(socket_fd, (struct sockaddr*)NULL, NULL);
        if (fd < 0) { errno; return; }
    
        conn_t *conn = (conn_t*)std::malloc(sizeof(conn_t));
        conn->user_data = user_data;
        conn->fd = fd;

        char* buffer_ptr = &conn->packet_buffer[0];
        size_t n = 0, bytes_expected = 0;
        n = recv(fd, buffer_ptr, ram_page_size_k, 0);
        int remaining = data_cb( conn, buffer_ptr, n );
        while ( remaining > 0 ) {
            n = recv(fd, buffer_ptr+remaining, ram_page_size_k, 0);
            remaining = data_cb( conn, buffer_ptr, n+remaining );
        }
        
        std::free(conn);
        close(fd);
    }
/*
#if defined(UCALL_IS_WINDOWS)
        _aligned_free(buffer_ptr);
#else
        std::free(buffer_ptr);
#endif
*/
}
/**
 * @brief 
 */
inline void net_shutdown(ucall_server_t &server) noexcept {
    close(socket_fd);
}

/**
 * @brief 
 */
int net_send_message(int fd, array_gt<char> const& message) noexcept {
    char const* buf = message.data();
    size_t const len = message.size();
    long idx = 0;
    long res = 0;
                              
    while (idx < len && (res = send(fd, buf + idx, len - idx, 0)) > 0)
        idx += res;

    if (res < 0) {
        return res;
    }
    return 0;
}


} // namespace
