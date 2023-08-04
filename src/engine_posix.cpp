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
// SO_REUSEPORT, MSG_NOSIGNAL is not supported on Windows.
#define SO_REUSEPORT 0
#define MSG_NOSIGNAL 0
#pragma comment(lib, "Ws2_32.lib")
#define UNICODE

#else
#include <arpa/inet.h> // `inet_addr`
#include <fcntl.h>
#include <netinet/in.h> // `sockaddr_in`

#include <sys/ioctl.h>
#include <sys/socket.h> // `recv`, `setsockopt`

#include <sys/uio.h>
#include <unistd.h>
#endif

#include <queue>

#include <openssl/engine.h>
#include <openssl/err.h>
#include <openssl/pem.h>
#include <picotls.h>
#include <picotls/openssl.h>

#include "ucall/ucall.h"

#include "automata.hpp"
#include "network.hpp"
#include "server.hpp"

#pragma region Cpp Declaration

namespace sj = simdjson;
namespace sjd = sj::dom;
using namespace unum::ucall;

static constexpr std::size_t initial_buffer_size_k = ram_page_size_k * 4;

struct ucall_ssl_context_t {

    constexpr ucall_ssl_context_t() noexcept
        : certs{{nullptr}}, sign_certificate({{nullptr}}), verify_certificate({{nullptr}}), hand_props({{{nullptr}}}),
          tls(), ssl({.random_bytes = ptls_openssl_random_bytes,
                      .get_time = &ptls_get_time,
                      .key_exchanges = ptls_openssl_key_exchanges,
                      .cipher_suites = ptls_openssl_cipher_suites,
                      .certificates = {certs, 0},
                      .sign_certificate = &sign_certificate.super}){};

    int init(const char* pk_path, const char** crts_path, size_t crts_cnt) noexcept {
        FILE* fp;
        X509* cert;

        // Read Certificates
        for (size_t i = 0; i < crts_cnt; ++i) {
            if ((fp = fopen(crts_path[i], "r")) == nullptr)
                return -1;

            while ((cert = PEM_read_X509(fp, nullptr, nullptr, nullptr)) != nullptr) {
                ptls_iovec_t* dst = ssl.certificates.list + ssl.certificates.count++;
                dst->len = i2d_X509(cert, &dst->base);
            }

            fclose(fp);
        }
        if (ptls_openssl_init_verify_certificate(&verify_certificate, nullptr) != 0)
            return -1;

        if (ssl.certificates.count == 0)
            return -1;
        ssl.verify_certificate = &verify_certificate.super;

        if ((fp = fopen(pk_path, "r")) == nullptr)
            return -1;

        EVP_PKEY* pkey = PEM_read_PrivateKey(fp, nullptr, nullptr, nullptr);
        fclose(fp);

        if (pkey == nullptr)
            return -1;

        int rv = ptls_openssl_init_sign_certificate(&sign_certificate, pkey);
        EVP_PKEY_free(pkey);
        if (rv)
            return -1;

        return 0;
    }

    ssize_t do_handshake(descriptor_t fd, void* buf, size_t len) {
        ptls_buffer_t wbuf, rbuf;
        uint8_t sm_buf[ram_page_size_k];

        ptls_buffer_init(&rbuf, buf, len);
        ptls_buffer_init(&wbuf, sm_buf, ram_page_size_k);
        ssize_t ret = 0;
        ssize_t read_ret = 0;
        while (!ptls_handshake_is_complete(tls[fd])) {
            read_ret = recv(fd, rbuf.base, rbuf.capacity, MSG_NOSIGNAL);
            if (read_ret == -1) {
                ret = -errno;
                break;
            }
            rbuf.off = read_ret;
            ret = ptls_handshake(tls[fd], &wbuf, rbuf.base, &rbuf.off, &hand_props);
            if (ret != PTLS_ERROR_IN_PROGRESS && ret != PTLS_ALERT_CLOSE_NOTIFY)
                break;

            ret = send(fd, wbuf.base, wbuf.off, MSG_NOSIGNAL);
            if (ret == -1) {
                ret = errno;
                break;
            }
            wbuf.off = 0;
        }

        if (ret >= 0) {
            ret = read_ret - rbuf.off;
            memmove(rbuf.base, rbuf.base + rbuf.off, ret);
        }

        // ptls_buffer_dispose(&rbuf);
        ptls_buffer_dispose(&wbuf);
        return ret;
    }

    ptls_iovec_t certs[16];
    ptls_openssl_sign_certificate_t sign_certificate;
    ptls_openssl_verify_certificate_t verify_certificate;
    ptls_handshake_properties_t hand_props;
    array_gt<ptls_t*> tls;
    ptls_context_t ssl;
};

struct conn_ctx_t {
    connection_t* conn_ptr;
    ssize_t res;
};

struct posix_ctx_t {
    std::queue<conn_ctx_t> res_queue; // TODO replace with custom
    std::optional<ucall_ssl_context_t> ssl_ctx;
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
    {
#if defined(UCALL_IS_WINDOWS)
        u_long mode = 1; // 1 to enable non-blocking socket, 0 to disable
        ioctlsocket(socket_descriptor, FIONBIO, &mode);
#else
        fcntl(socket_descriptor, F_SETFL, O_NONBLOCK);
#endif
    }
    if (socket_descriptor < 0)
        goto cleanup;
    if (setsockopt(socket_descriptor, SOL_SOCKET, SO_REUSEADDR | SO_REUSEPORT,
                   reinterpret_cast<char const*>(&socket_options), sizeof(socket_options)) == -1)
        errno;
    if (bind(socket_descriptor, (struct sockaddr*)&address, sizeof(address)) < 0)
        goto cleanup;
    if (listen(socket_descriptor, config.queue_depth) < 0)
        goto cleanup;
    if (config.use_ssl) {
        uctx->ssl_ctx.emplace();
        uctx->ssl_ctx->tls.reserve(config.max_concurrent_connections);
        if (uctx->ssl_ctx->init(config.ssl_private_key_path, config.ssl_certificates_paths,
                                config.ssl_certificates_count) != 0)
            goto cleanup;
    }

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

    conn_ctx_t coctx{&connection};

    coctx.res = accept(socket, &connection.client_address, &connection.client_address_len);
    if (coctx.res == -1)
        coctx.res = -errno;

    if (ctx->ssl_ctx)
        ctx->ssl_ctx->tls[coctx.res] = ptls_new(&ctx->ssl_ctx->ssl, true);

    ctx->queue_mutex.lock();
    ctx->res_queue.push(coctx);
    ctx->queue_mutex.unlock();

    return 0;
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

    if (ctx->ssl_ctx) {
        ptls_free(ctx->ssl_ctx->tls[connection.descriptor]);
        ctx->ssl_ctx->tls[connection.descriptor] = nullptr;
    }
    conn_ctx_t coctx{&connection, close(connection.descriptor)};
    if (coctx.res == -1)
        coctx.res = errno;

    ctx->queue_mutex.lock();
    ctx->res_queue.push(coctx);
    ctx->queue_mutex.unlock();
}

void network_engine_t::send_packet(connection_t& connection, void* buffer, size_t buf_len, size_t buf_index) {
    posix_ctx_t* ctx = reinterpret_cast<posix_ctx_t*>(network_data);
    conn_ctx_t coctx{&connection};
    ssize_t res = 0;

    if (ctx->ssl_ctx) {
        ptls_buffer_t encrypt_buf;
        uint8_t sm_buf[ram_page_size_k];
        ptls_buffer_init(&encrypt_buf, sm_buf, ram_page_size_k);
        res = ptls_send(ctx->ssl_ctx->tls[connection.descriptor], &encrypt_buf, buffer, buf_len);
        if (res == 0) {
            res = send(connection.descriptor, encrypt_buf.base, encrypt_buf.off, MSG_NOSIGNAL);
            coctx.res = (res != encrypt_buf.off) ? -EAGAIN : buf_len;
        } else
            coctx.res = -ECANCELED;
        ptls_buffer_dispose(&encrypt_buf);
    } else {
        res = send(connection.descriptor, (const char*)buffer, buf_len, MSG_NOSIGNAL);
        coctx.res = (res == -1) ? errno : res;
    }
    ctx->queue_mutex.lock();
    ctx->res_queue.push(coctx);
    ctx->queue_mutex.unlock();
}

void network_engine_t::recv_packet(connection_t& connection, void* buffer, size_t buf_len, size_t buf_index) {
    posix_ctx_t* ctx = reinterpret_cast<posix_ctx_t*>(network_data);

    conn_ctx_t coctx{&connection};
    ssize_t res = 0;

    if (ctx->ssl_ctx) {
        ptls_buffer_t decrpyt_buf;
        uint8_t read_buf[ram_page_size_k];

        if (!ptls_handshake_is_complete(ctx->ssl_ctx->tls[connection.descriptor]))
            res = ctx->ssl_ctx->do_handshake(connection.descriptor, read_buf, buf_len);

        if (res < 0)
            coctx.res = -ECANCELED;
        else {
            ptls_buffer_init(&decrpyt_buf, buffer, buf_len);
            size_t recv_size = ram_page_size_k - res;
            ssize_t read_res = recv(connection.descriptor, (char*)read_buf + res, recv_size, MSG_NOSIGNAL);
            res += read_res > 0 ? read_res : 0;
            if (res > 0) {
                res = ptls_receive(ctx->ssl_ctx->tls[connection.descriptor], &decrpyt_buf, read_buf, (size_t*)&res);
                coctx.res = (res == -1) ? -ECANCELED : decrpyt_buf.off;
            } else
                coctx.res = -EAGAIN;
            // ptls_buffer_dispose(&decrpyt_buf);
        }
    } else {
        res = recv(connection.descriptor, (char*)buffer, buf_len, MSG_NOSIGNAL);
        coctx.res = (res == -1) ? errno : res;
    }

    ctx->queue_mutex.lock();
    ctx->res_queue.push(coctx);
    ctx->queue_mutex.unlock();
}

bool network_engine_t::is_canceled(ssize_t res, unum::ucall::connection_t const& conn) {
    return res == -ECANCELED || res == -EWOULDBLOCK || res == -EAGAIN;
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