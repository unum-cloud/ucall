/**
 * @brief JSON-RPC implementation for TCP/IP stack with POSIX calls.
 * @author Ashot Vardanian
 */
#include <arpa/inet.h>  // `inet_addr`
#include <errno.h>      // `strerror`
#include <fcntl.h>      // `fcntl`
#include <netinet/in.h> // `sockaddr_in`
#include <stdlib.h>     // `std::aligned_malloc`
#include <sys/ioctl.h>
#include <sys/socket.h> // `recv`, `setsockopt`
#include <sys/types.h>
#include <sys/uio.h>
#include <unistd.h>

#include <charconv> // `std::to_chars`
#include <chrono>   // `std::chrono`
#include <mbedtls/ctr_drbg.h>
#include <mbedtls/entropy.h>
#include <mbedtls/net_sockets.h>
#include <mbedtls/ssl.h>

#include "ujrpc/ujrpc.h"

#include "helpers/log.hpp"
#include "helpers/parse.hpp"
#include "helpers/reply.hpp"
#include "helpers/shared.hpp"

using namespace unum::ujrpc;

using time_clock_t = std::chrono::steady_clock;
using time_point_t = std::chrono::time_point<time_clock_t>;

static constexpr std::size_t initial_buffer_size_k = ram_page_size_k * 4;

struct ujrpc_ssl_context_t {
    ujrpc_ssl_context_t() noexcept = default;

    ~ujrpc_ssl_context_t() noexcept {
        mbedtls_x509_crt_free(&srvcert);
        mbedtls_pk_free(&pkey);
        mbedtls_ssl_free(&ssl);
        mbedtls_ssl_config_free(&conf);
        // #if defined(MBEDTLS_SSL_CACHE_C)
        //     mbedtls_ssl_cache_free(&cache);
        // #endif
        mbedtls_ctr_drbg_free(&ctr_drbg);
        mbedtls_entropy_free(&entropy);
    }

    int init(const char* pk_path, const char** crts_path, size_t crts_cnt) {
        mbedtls_ssl_init(&ssl);
        mbedtls_ssl_config_init(&conf);
        // #if defined(MBEDTLS_SSL_CACHE_C)
        //     mbedtls_ssl_cache_init(&cache);
        // #endif
        mbedtls_x509_crt_init(&srvcert);
        mbedtls_pk_init(&pkey);
        mbedtls_entropy_init(&entropy);
        mbedtls_ctr_drbg_init(&ctr_drbg);
        int ret = 0;

        // Seed the RNG
        if ((ret = mbedtls_ctr_drbg_seed(&ctr_drbg, mbedtls_entropy_func, &entropy, NULL, 0)) != 0)
            // TODO Use personalization string. Required or Optional ?
            return ret;

        // Load Private Key
        if ((ret = mbedtls_pk_parse_keyfile(&pkey, pk_path, NULL, NULL, &ctr_drbg)) != 0)
            // TODO Use Password. Required or Optional ?
            return ret;

        // Load Certificates
        for (size_t i = 0; i < crts_cnt; ++i)
            if ((ret = mbedtls_x509_crt_parse_file(&srvcert, crts_path[i])) != 0)
                // TODO Notify which certificate was invalid ?
                return ret;

        if ((ret = mbedtls_ssl_config_defaults(&conf, MBEDTLS_SSL_IS_SERVER, MBEDTLS_SSL_TRANSPORT_STREAM,
                                               MBEDTLS_SSL_PRESET_DEFAULT)) != 0)
            return ret;

        mbedtls_ssl_conf_rng(&conf, mbedtls_ctr_drbg_random, &ctr_drbg);

        // #if defined(MBEDTLS_SSL_CACHE_C)
        //     mbedtls_ssl_conf_session_cache(&conf, &cache, mbedtls_ssl_cache_get, mbedtls_ssl_cache_set);
        // #endif

        mbedtls_ssl_conf_ca_chain(&conf, srvcert.next, NULL);
        if ((ret = mbedtls_ssl_conf_own_cert(&conf, &srvcert, &pkey)) != 0)
            return ret;

        if ((ret = mbedtls_ssl_setup(&ssl, &conf)) != 0)
            return ret;

        return 0;
    }

    mbedtls_ssl_context ssl;
    mbedtls_ssl_config conf;
    mbedtls_pk_context pkey;
    mbedtls_x509_crt srvcert;
    mbedtls_entropy_context entropy;
    mbedtls_ctr_drbg_context ctr_drbg;
};

struct engine_t {
    engine_t() = default;

    ~engine_t() noexcept { delete ssl_ctx; }

    descriptor_t socket;

    /// @brief Establishes an SSL connection if SSL is enabled, otherwise the `ssl_ctx` is unused and uninitialized.
    ujrpc_ssl_context_t* ssl_ctx = nullptr;

    /// @brief The file descriptor of the stateful connection over TCP.
    descriptor_t connection;
    /// @brief A small memory buffer to store small requests.
    alignas(align_k) char packet_buffer[ram_page_size_k + sj::SIMDJSON_PADDING];
    /// @brief An array of function callbacks. Can be in dozens.
    array_gt<named_callback_t> callbacks;
    /// @brief Statically allocated memory to process small requests.
    scratch_space_t scratch;
    /// @brief For batch-requests in synchronous connections we need a place to
    struct batch_response_t {
        array_gt<char> buffer;
        // buffer_gt<char*> copies;
        // std::size_t copies_count;
    } batch_response;

    stats_t stats;
    std::int32_t logs_file_descriptor;
    std::string_view logs_format;
    time_point_t log_last_time;
};

sj::simdjson_result<sjd::element> param_at(ujrpc_call_t call, ujrpc_str_t name, size_t name_len) noexcept {
    engine_t& engine = *reinterpret_cast<engine_t*>(call);
    scratch_space_t& scratch = engine.scratch;
    name_len = string_length(name, name_len);
    return scratch.point_to_param({name, name_len});
}

sj::simdjson_result<sjd::element> param_at(ujrpc_call_t call, size_t position) noexcept {
    engine_t& engine = *reinterpret_cast<engine_t*>(call);
    scratch_space_t& scratch = engine.scratch;
    return scratch.point_to_param(position);
}

void send_message(engine_t& engine, array_gt<char> const& message) noexcept {
    long bytes_sent = 0;
    if (engine.ssl_ctx)
        bytes_sent =
            mbedtls_ssl_write(&engine.ssl_ctx->ssl, reinterpret_cast<uint8_t const*>(message.data()), message.size());
    else
        bytes_sent = send(engine.connection, message.data(), message.size(), 0);

    if (bytes_sent < 0) {
        if (errno == EMSGSIZE)
            ujrpc_call_reply_error_out_of_memory(&engine);
        return;
    }
    engine.stats.bytes_sent += bytes_sent;
    engine.stats.packets_sent++;
}

void send_reply(engine_t& engine) noexcept { // TODO Is this required?
    if (!engine.batch_response.buffer.size())
        return;

    send_message(engine, engine.batch_response.buffer);
}

void forward_call(engine_t& engine) noexcept {
    scratch_space_t& scratch = engine.scratch;
    auto callback_or_error = find_callback(engine.callbacks, scratch);
    if (auto error_ptr = std::get_if<default_error_t>(&callback_or_error); error_ptr)
        return ujrpc_call_reply_error(&engine, error_ptr->code, error_ptr->note.data(), error_ptr->note.size());

    named_callback_t call_data = std::get<named_callback_t>(callback_or_error);
    return call_data.callback(&engine, call_data.callback_tag);
}

/**
 * @brief Analyzes the contents of the packet, bifurcating batched and singular JSON-RPC requests.
 */
void forward_call_or_calls(engine_t& engine) noexcept {
    scratch_space_t& scratch = engine.scratch;
    sjd::parser& parser = *scratch.dynamic_parser;
    std::string_view json_body = scratch.dynamic_packet;
    auto one_or_many = parser.parse(json_body.data(), json_body.size(), false);
    if (one_or_many.error() != sj::SUCCESS)
        return ujrpc_call_reply_error(&engine, -32700, "Invalid JSON was received by the server.", 40);

    // The major difference between batch and single-request paths is that
    // in the first case we need to keep a copy of the data somewhere,
    // until answers to all requests are accumulated and we can submit them
    // simultaneously.
    // Linux supports `MSG_MORE` flag for submissions, which could have helped,
    // but it is less effective than assembling a copy here.
    if (one_or_many.is_array()) {
        sjd::array many = one_or_many.get_array().value_unsafe();
        scratch.is_batch = false;

        // Start a JSON array.
        scratch.is_batch = true;
        bool res = true;
        if (scratch.is_http)
            res &= engine.batch_response.buffer.append_n(http_header_k, http_header_size_k);

        res &= engine.batch_response.buffer.append_n("[", 1);

        for (sjd::element const one : many) {
            scratch.tree = one;
            forward_call(engine);
        }

        if (engine.batch_response.buffer[engine.batch_response.buffer.size() - 1] == ',')
            engine.batch_response.buffer.pop_back();

        res &= engine.batch_response.buffer.append_n("]", 1);

        if (!res)
            return ujrpc_call_reply_error_out_of_memory(&engine);

        if (scratch.is_http)
            set_http_content_length(engine.batch_response.buffer.data(),
                                    engine.batch_response.buffer.size() - http_header_size_k);

        send_reply(engine);

        engine.batch_response.buffer.reset();
    } else {
        scratch.is_batch = false;
        scratch.tree = one_or_many.value_unsafe();
        forward_call(engine);
        engine.batch_response.buffer.reset();
    }
}

void forward_packet(engine_t& engine) noexcept {
    scratch_space_t& scratch = engine.scratch;
    auto json_or_error = split_body_headers(scratch.dynamic_packet);
    if (auto error_ptr = std::get_if<default_error_t>(&json_or_error); error_ptr)
        return ujrpc_call_reply_error(&engine, error_ptr->code, error_ptr->note.data(), error_ptr->note.size());

    auto request = std::get<parsed_request_t>(json_or_error);
    scratch.is_http = request.type.size();
    scratch.dynamic_packet = request.body;
    return forward_call_or_calls(engine);
}

int ssl_send(void* ctx, const unsigned char* buf, size_t len) {
    mbedtls_net_context* conn = reinterpret_cast<mbedtls_net_context*>(ctx);
    ssize_t ret = send(conn->fd, buf, len, 0);
    return ret;
}

int ssl_recv(void* ctx, unsigned char* buf, size_t len) {
    mbedtls_net_context* conn = reinterpret_cast<mbedtls_net_context*>(ctx);
    ssize_t ret = recv(conn->fd, buf, len, MSG_WAITALL);
    return ret;
}

int recv_next(engine_t& engine, char* buf, size_t len, int flags) {
    if (len == 0)
        return 0;

    if (engine.ssl_ctx)
        return mbedtls_ssl_read(&engine.ssl_ctx->ssl, reinterpret_cast<uint8_t*>(buf), len);

    return recv(engine.connection, buf, len, flags);
}

void ujrpc_take_call(ujrpc_server_t server, uint16_t) {
    engine_t& engine = *reinterpret_cast<engine_t*>(server);
    scratch_space_t& scratch = engine.scratch;

    // Log stats, if enough time has passed since last call.
    if (engine.logs_file_descriptor > 0) {
        auto now = time_clock_t::now();
        auto dt = std::chrono::duration_cast<std::chrono::milliseconds>(now - engine.log_last_time).count();
        if (dt > stats_t::default_frequency_secs_k * 1000ul) {
            auto len = engine.logs_format == "json" //
                           ? engine.stats.log_json(engine.packet_buffer, ram_page_size_k)
                           : engine.stats.log_human_readable(engine.packet_buffer, ram_page_size_k, dt / 1000ul);
            len = write(engine.logs_file_descriptor, engine.packet_buffer, len);
            engine.log_last_time = now;
        }
    }

    // If no pending connections are present on the queue, and the
    // socket is not marked as nonblocking, accept() blocks the caller
    // until a connection is present. If the socket is marked
    // nonblocking and no pending connections are present on the queue,
    // accept() fails with the error EAGAIN or EWOULDBLOCK.
    int connection_fd = accept(engine.socket, (struct sockaddr*)NULL, NULL);
    if (connection_fd < 0) {
        // Drop the last error.
        errno;
        return;
    }

    mbedtls_net_context client_ctx;

    if (engine.ssl_ctx) {
        client_ctx.fd = connection_fd;
        mbedtls_ssl_set_bio(&engine.ssl_ctx->ssl, &client_ctx, ssl_send, ssl_recv, NULL);
        int ret = 0;
        while ((ret = mbedtls_ssl_handshake(&engine.ssl_ctx->ssl)) != 0)
            if (ret != MBEDTLS_ERR_SSL_WANT_READ && ret != MBEDTLS_ERR_SSL_WANT_WRITE) {
                mbedtls_net_free(&client_ctx);
                mbedtls_ssl_session_reset(&engine.ssl_ctx->ssl);
                return;
            }
    }

    // Wait until we have input.
    engine.connection = descriptor_t{connection_fd};
    engine.stats.added_connections++;
    engine.stats.closed_connections++;
    char* buffer_ptr = &engine.packet_buffer[0];

    size_t bytes_received = 0, bytes_expected = 0;
    if (engine.ssl_ctx)
        bytes_received =
            mbedtls_ssl_read(&engine.ssl_ctx->ssl, reinterpret_cast<uint8_t*>(buffer_ptr), http_head_max_size_k);
    else
        bytes_received = recv(engine.connection, buffer_ptr, http_head_max_size_k, 0);

    auto json_or_error = split_body_headers(std::string_view(buffer_ptr, bytes_received));
    if (auto error_ptr = std::get_if<default_error_t>(&json_or_error); error_ptr)
        return ujrpc_call_reply_error(&engine, error_ptr->code, error_ptr->note.data(), error_ptr->note.size());
    parsed_request_t request = std::get<parsed_request_t>(json_or_error);
    auto res = std::from_chars(request.content_length.begin(), request.content_length.end(), bytes_expected);
    bytes_expected += (request.body.begin() - buffer_ptr);

    if (res.ec == std::errc::invalid_argument || bytes_expected <= 0)
        if (ioctl(engine.connection, FIONREAD, &bytes_expected) == -1 || bytes_expected == 0)
            bytes_expected = bytes_received; // TODO what?

    // Either process it in the statically allocated memory,
    // or allocate dynamically, if the message is too long.
    size_t bytes_left = bytes_expected - bytes_received;

    if (bytes_expected <= ram_page_size_k) {
        bytes_received += recv_next(engine, buffer_ptr + bytes_received, bytes_left, MSG_WAITALL);
        scratch.dynamic_parser = &scratch.parser;
        scratch.dynamic_packet = std::string_view(buffer_ptr, bytes_received);
        engine.stats.bytes_received += bytes_received;
        engine.stats.packets_received++;
        forward_packet(engine);
    } else {
        sjd::parser parser;
        if (parser.allocate(bytes_expected, bytes_expected / 2) != sj::SUCCESS)
            return ujrpc_call_reply_error_out_of_memory(&engine);

        buffer_ptr = (char*)std::aligned_alloc(align_k, round_up_to<align_k>(bytes_expected + sj::SIMDJSON_PADDING));
        if (!buffer_ptr)
            return ujrpc_call_reply_error_out_of_memory(&engine);

        memcpy(buffer_ptr, &engine.packet_buffer[0], bytes_received);

        bytes_received += recv_next(engine, buffer_ptr + bytes_received, bytes_left, MSG_WAITALL);
        scratch.dynamic_parser = &parser;
        scratch.dynamic_packet = std::string_view(buffer_ptr, bytes_received);
        engine.stats.bytes_received += bytes_received;
        engine.stats.packets_received++;
        forward_packet(engine);
        std::free(buffer_ptr);
        buffer_ptr = nullptr;
    }

    if (engine.ssl_ctx) {
        int ret = 0;
        while ((ret = mbedtls_ssl_close_notify(&engine.ssl_ctx->ssl)) < 0)
            if (ret != MBEDTLS_ERR_SSL_WANT_READ && ret != MBEDTLS_ERR_SSL_WANT_WRITE)
                break;

        mbedtls_ssl_session_reset(&engine.ssl_ctx->ssl);
    }
    shutdown(connection_fd, SHUT_WR);
    // If later on some UB is detected for client not recieving full data,
    // then it may be required to put a `recv` with timeout between `shutdown` and `close`
    close(connection_fd);
}

void ujrpc_init(ujrpc_config_t* config_inout, ujrpc_server_t* server_out) {

    // Simple sanity check
    if (!server_out || !config_inout)
        return;

    // Retrieve configs, if present
    ujrpc_config_t& config = *config_inout;
    if (!config.port)
        config.port = 8545u;
    if (!config.queue_depth)
        config.queue_depth = 128u;
    if (!config.max_callbacks)
        config.max_callbacks = 128u;
    if (!config.interface)
        config.interface = "0.0.0.0";
    if (config.use_ssl && !(config.ssl_pk_path || config.ssl_crts_cnt))
        return;

    // Some limitations are hard-coded for this non-concurrent implementation
    config.max_threads = 1u;
    config.max_concurrent_connections = 1;
    config.max_lifetime_micro_seconds = 0u;
    config.max_lifetime_exchanges = 1u;

    int received_errno{};
    int socket_options{1};
    int socket_descriptor{-1};
    engine_t* server_ptr = nullptr;
    array_gt<char> buffer;
    array_gt<named_callback_t> embedded_callbacks;
    ujrpc_ssl_context_t* ssl_context = nullptr;
    sjd::parser parser;

    // By default, let's open TCP port for IPv4.
    struct sockaddr_in address;
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = inet_addr(config.interface);
    address.sin_port = htons(config.port);

    // Try allocating all the necessary memory.
    server_ptr = (engine_t*)std::malloc(sizeof(engine_t));
    if (!server_ptr)
        goto cleanup;
    if (!buffer.reserve(initial_buffer_size_k))
        goto cleanup;
    if (!embedded_callbacks.reserve(config.max_callbacks))
        goto cleanup;
    socket_descriptor = socket(AF_INET, SOCK_STREAM, 0);
    if (socket_descriptor < 0)
        goto cleanup;
    // Optionally configure the socket, but don't always expect it to succeed.
    if (setsockopt(socket_descriptor, SOL_SOCKET, SO_REUSEADDR | SO_REUSEPORT, &socket_options,
                   sizeof(socket_options)) == -1)
        errno;
    if (bind(socket_descriptor, (struct sockaddr*)&address, sizeof(address)) < 0)
        goto cleanup;
    if (listen(socket_descriptor, config.queue_depth) < 0)
        goto cleanup;
    if (config.use_ssl) {
        ssl_context = new ujrpc_ssl_context_t();
        if (ssl_context->init(config.ssl_pk_path, config.ssl_crts_path, config.ssl_crts_cnt) != 0)
            goto cleanup;
    }
    if (parser.allocate(ram_page_size_k, ram_page_size_k / 2) != sj::SUCCESS)
        goto cleanup;

    // Initialize all the members.
    new (server_ptr) engine_t();
    server_ptr->socket = descriptor_t{socket_descriptor};
    server_ptr->callbacks = std::move(embedded_callbacks);
    server_ptr->scratch.parser = std::move(parser);
    server_ptr->batch_response.buffer = std::move(buffer);
    server_ptr->logs_file_descriptor = config.logs_file_descriptor;
    server_ptr->logs_format = config.logs_format ? std::string_view(config.logs_format) : std::string_view();
    server_ptr->log_last_time = time_clock_t::now();
    server_ptr->ssl_ctx = ssl_context;
    *server_out = (ujrpc_server_t)server_ptr;
    return;

cleanup:
    received_errno = errno;
    if (socket_descriptor >= 0)
        close(socket_descriptor);
    std::free(server_ptr);
    *server_out = nullptr;
    delete ssl_context;
}

void ujrpc_add_procedure(ujrpc_server_t server, ujrpc_str_t name, ujrpc_callback_t callback,
                         ujrpc_callback_tag_t callback_tag) {
    engine_t& engine = *reinterpret_cast<engine_t*>(server);
    if (engine.callbacks.size() + 1 < engine.callbacks.capacity())
        engine.callbacks.push_back_reserved({name, callback, callback_tag});
}

void ujrpc_take_calls(ujrpc_server_t server, uint16_t) {
    while (true)
        ujrpc_take_call(server, 0);
}

void ujrpc_free(ujrpc_server_t server) {
    if (!server)
        return;

    engine_t* engine = reinterpret_cast<engine_t*>(server);
    close(engine->socket);
    delete engine;
}

bool fill_with_content(array_gt<char>& buffer, std::string_view request_id, std::string_view body,
                       bool add_http = false, bool append_comma = false) {

    // Communication example would be:
    // --> {"jsonrpc": "2.0", "method": "subtract", "params": [42, 23], "id": 1}
    // <-- {"jsonrpc": "2.0", "id": 1, "result": 19}
    bool res = true;
    if (add_http)
        res &= buffer.append_n(http_header_k, http_header_size_k);

    size_t initial_sz = buffer.size();
    res &= buffer.append_n(R"({"jsonrpc":"2.0","id":)", 22);
    res &= buffer.append_n(request_id.data(), request_id.size());
    res &= buffer.append_n(R"(,"result":)", 10);
    res &= buffer.append_n(body.data(), body.size());
    res &= buffer.append_n(R"(},)", 1 + append_comma);
    size_t body_len = buffer.size() - initial_sz;

    if (add_http)
        set_http_content_length(buffer.end() - (body_len + http_header_size_k), body_len);

    return res;
}

bool fill_with_error(array_gt<char>& buffer, std::string_view request_id, std::string_view error_code,
                     std::string_view error_message, bool add_http = false, bool append_comma = false) {

    // Communication example would be:
    // --> {"jsonrpc": "2.0", "method": "foobar", "id": "1"}
    // <-- {"jsonrpc": "2.0", "id": "1", "error": {"code": -32601, "message": "Method not found"}}
    bool res = true;
    if (add_http)
        res &= buffer.append_n(http_header_k, http_header_size_k);

    size_t initial_sz = buffer.size();
    res &= buffer.append_n(R"({"jsonrpc":"2.0","id":)", 22);
    res &= buffer.append_n(request_id.data(), request_id.size());
    res &= buffer.append_n(R"(,"error":{"code":)", 17);
    res &= buffer.append_n(error_code.data(), error_code.size());
    res &= buffer.append_n(R"(,"message":")", 12);
    res &= buffer.append_n(error_message.data(), error_message.size());
    res &= buffer.append_n(R"("}},)", 3 + append_comma);
    size_t body_len = buffer.size() - initial_sz;

    if (add_http)
        set_http_content_length(buffer.end() - (body_len + http_header_size_k), body_len);

    return res;
}

void ujrpc_call_reply_content(ujrpc_call_t call, ujrpc_str_t body, size_t body_len) {
    engine_t& engine = *reinterpret_cast<engine_t*>(call);
    scratch_space_t& scratch = engine.scratch;
    // No response is needed for "id"-less notifications.
    if (scratch.dynamic_id.empty())
        return;
    if (!body_len)
        body_len = std::strlen(body);

    // In case of a single request - immediately push into the socket.
    if (!scratch.is_batch)
        if (fill_with_content(engine.batch_response.buffer, scratch.dynamic_id, //
                              std::string_view(body, body_len), scratch.is_http))
            send_message(engine, engine.batch_response.buffer);
        else
            return ujrpc_call_reply_error_out_of_memory(call);

    else if (!fill_with_content(engine.batch_response.buffer, scratch.dynamic_id, //
                                std::string_view(body, body_len), false, true))
        return ujrpc_call_reply_error_out_of_memory(call);
}

void ujrpc_call_reply_error(ujrpc_call_t call, int code_int, ujrpc_str_t note, size_t note_len) {
    engine_t& engine = *reinterpret_cast<engine_t*>(call);
    scratch_space_t& scratch = engine.scratch;
    // No response is needed for "id"-less notifications.
    if (scratch.dynamic_id.empty())
        return;
    if (!note_len)
        note_len = std::strlen(note);

    char code[max_integer_length_k]{};
    std::to_chars_result res = std::to_chars(code, code + max_integer_length_k, code_int);
    auto code_len = res.ptr - code;
    if (res.ec != std::error_code())
        return ujrpc_call_reply_error_unknown(call);

    // In case of a single request - immediately push into the socket.
    if (!scratch.is_batch)
        if (fill_with_error(engine.batch_response.buffer, scratch.dynamic_id, //
                            std::string_view(code, code_len), std::string_view(note, note_len), scratch.is_http))
            send_message(engine, engine.batch_response.buffer);
        else
            return ujrpc_call_reply_error_out_of_memory(call);

    else if (!fill_with_error(engine.batch_response.buffer, scratch.dynamic_id, //
                              std::string_view(code, code_len),                 //
                              std::string_view(note, note_len), false, true))
        return ujrpc_call_reply_error_out_of_memory(call);
}

void ujrpc_call_reply_error_invalid_params(ujrpc_call_t call) {
    return ujrpc_call_reply_error(call, -32602, "Invalid method param(s).", 24);
}

void ujrpc_call_reply_error_unknown(ujrpc_call_t call) {
    return ujrpc_call_reply_error(call, -32603, "Unknown error.", 14);
}

void ujrpc_call_reply_error_out_of_memory(ujrpc_call_t call) {
    return ujrpc_call_reply_error(call, -32000, "Out of memory.", 14);
}

bool ujrpc_param_named_bool(ujrpc_call_t call, ujrpc_str_t name, size_t name_len, bool* result_ptr) {
    if (auto value = param_at(call, name, name_len); value.is_bool()) {
        *result_ptr = value.get_bool().value_unsafe();
        return true;
    } else
        return false;
}

bool ujrpc_param_named_i64(ujrpc_call_t call, ujrpc_str_t name, size_t name_len, int64_t* result_ptr) {
    if (auto value = param_at(call, name, name_len); value.is_int64()) {
        *result_ptr = value.get_int64().value_unsafe();
        return true;
    } else
        return false;
}

bool ujrpc_param_named_f64(ujrpc_call_t call, ujrpc_str_t name, size_t name_len, double* result_ptr) {
    if (auto value = param_at(call, name, name_len); value.is_double()) {
        *result_ptr = value.get_double().value_unsafe();
        return true;
    } else
        return false;
}

bool ujrpc_param_named_str(ujrpc_call_t call, ujrpc_str_t name, size_t name_len, ujrpc_str_t* result_ptr,
                           size_t* result_len_ptr) {
    if (auto value = param_at(call, name, name_len); value.is_string()) {
        *result_ptr = value.get_string().value_unsafe().data();
        *result_len_ptr = value.get_string_length().value_unsafe();
        return true;
    } else
        return false;
}

bool ujrpc_param_positional_bool(ujrpc_call_t call, size_t position, bool* result_ptr) {
    if (auto value = param_at(call, position); value.is_bool()) {
        *result_ptr = value.get_bool().value_unsafe();
        return true;
    } else
        return false;
}

bool ujrpc_param_positional_i64(ujrpc_call_t call, size_t position, int64_t* result_ptr) {
    if (auto value = param_at(call, position); value.is_int64()) {
        *result_ptr = value.get_int64().value_unsafe();
        return true;
    } else
        return false;
}

bool ujrpc_param_positional_f64(ujrpc_call_t call, size_t position, double* result_ptr) {
    if (auto value = param_at(call, position); value.is_double()) {
        *result_ptr = value.get_double().value_unsafe();
        return true;
    } else
        return false;
}

bool ujrpc_param_positional_str(ujrpc_call_t call, size_t position, ujrpc_str_t* result_ptr, size_t* result_len_ptr) {
    if (auto value = param_at(call, position); value.is_string()) {
        *result_ptr = value.get_string().value_unsafe().data();
        *result_len_ptr = value.get_string_length().value_unsafe();
        return true;
    } else
        return false;
}