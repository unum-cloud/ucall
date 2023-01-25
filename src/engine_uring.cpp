/**
 * @brief JSON-RPC implementation for TCP/IP stack with `io_uring`.
 *
 * Supports:
 * > Millions of concurrent statefull connections.
 * > Hundreds of physical execution threads.
 * > Both HTTP and HTTP-less raw JSON-RPC calls.
 *
 * Primary structures:
 * - `engine_t`: primary server instance.
 * - `connection_t`: lifetime state of a single TCP connection.
 * - `scratch_space_t`: temporary memory used by a single thread at a time.
 * - `call_automata_t`: automata that accepts and responds to messages.
 *
 * Suggested compilation flags:
 * -fno-exceptions
 *
 * Notable links:
 * https://man7.org/linux/man-pages/dir_by_project.html#liburing
 * https://jvns.ca/blog/2017/06/03/async-io-on-linux--select--poll--and-epoll/
 * https://stackoverflow.com/a/17665015/2766161
 */
#include <fcntl.h>      // `fcntl`
#include <netinet/in.h> // `sockaddr_in`
#include <stdlib.h>     // `std::aligned_malloc`
#include <sys/socket.h> // `recv`, `setsockopt`
#include <sys/types.h>
#include <sys/uio.h>
#include <unistd.h>

#include <liburing.h>

#include <charconv> // `std::to_chars`

#include <simdjson.h>

#include "ujrpc/ujrpc.h"

#include "helpers/parse.hpp"
#include "helpers/reply.hpp"
#include "helpers/shared.hpp"

#pragma region Cpp Declaration

namespace sj = simdjson;
namespace sjd = sj::dom;
using namespace unum::ujrpc;

enum class stage_t {
    waiting_to_accept_k = 0,
    receiving_in_progress_k,
    responding_in_progress_k,
    waiting_to_close_k,
};

struct exchange_buffer_t {
    static constexpr std::size_t embedded_size_k = max_packet_size_k + sj::SIMDJSON_PADDING;

    // We always read the data into this buffer.
    // If it goes beyond it, we allocate dynamic memory, move existing contents there,
    // and then reallocate.
    alignas(align_k) char embedded[embedded_size_k]{};
    char* dynamic{};
    std::size_t length{};
    std::size_t volume{embedded_size_k};

    char* data() noexcept { return dynamic ? dynamic : embedded; }
    char const* data() const noexcept { return dynamic ? dynamic : embedded; }
    std::size_t size() const noexcept { return length; }
    std::size_t capacity() const noexcept { return volume; }
    std::size_t next_capacity() const noexcept { return capacity() * 2; }
    std::string_view view() const noexcept { return {data(), length}; }
    void reset() noexcept { std::free(dynamic), dynamic = nullptr, length = 0, volume = embedded_size_k; }
};

struct connection_t {
    /// @brief A combination of a embedded and dynamic memory pools for content reception.
    /// We always absorb new packets in the embedded part, later moving them into dynamic memory,
    /// if any more data is expected. Requires @b padding at the end, to accelerate parsing.
    exchange_buffer_t input{};
    /// @brief A combination of a embedded and dynamic memory pools for content reception.
    exchange_buffer_t output{};

    /// @brief The file descriptor of the statefull connection over TCP.
    descriptor_t descriptor{};
    stage_t stage{};
    struct sockaddr client_addr {};
    socklen_t client_addr_len{sizeof(client_addr)};
    timestamp_t deadline{};

    std::size_t input_absorbed{};
    std::size_t output_submitted{};

    inline bool expired() const noexcept { return cpu_cycle() > deadline; }
    inline bool submitted_all() const noexcept { return output_submitted >= output.size(); }
    inline std::string_view next_input_chunk() const noexcept {
        return {input.data() + input_absorbed, input.capacity() - input_absorbed};
    }
    inline std::string_view next_output_chunk() const noexcept {
        return {output.data() + output_submitted, output.size() - output_submitted};
    }

    template <std::size_t iovecs_count_ak> bool append_output(struct iovec const* iovecs) noexcept;
    std::string_view reserve_input() noexcept;
};

struct engine_t {
    descriptor_t socket{};
    struct io_uring ring {};

    std::atomic<std::size_t> active_connections{};
    std::atomic<std::size_t> reserved_connections{};
    uint32_t lifetime_microsec_limit{};

    std::mutex submission_mutex{};
    std::mutex completion_mutex{};

    /// @brief An array of function callbacks. Can be in dozens.
    array_gt<named_callback_t> callbacks{};
    /// @brief A circular container of reusable connections. Can be in millions.
    pool_gt<connection_t> connections{};
    /// @brief Same number of them, as max physical threads. Can be in hundreds.
    buffer_gt<scratch_space_t> spaces{};
};

struct call_automata_t {
    engine_t& engine;
    scratch_space_t& scratch;

    int uring_response{};
    struct io_uring_sqe* sqe{};
    struct io_uring_cqe* cqe{};
    struct __kernel_timespec timeout {};
    struct io_uring_cqe cqe_copy {};

    void operator()() noexcept;

    // Computed properties:
    connection_t& connection_ref() const noexcept { return *(connection_t*)cqe_copy.user_data; }
    int syscall_response() const noexcept { return cqe_copy.res; }
    bool received_full_request() const noexcept;

    // Control-flow:
    bool consider_accepting_new_connection() noexcept;
    bool pop_entry_from_uring_completion_queue() noexcept;
    bool release_connection_if_corrupted() noexcept;

    // Submitting to io_uring:
    void send_next() noexcept;
    void receive_next() noexcept;
    void close_gracefully() noexcept;

    // Helpers:
    void raise_call() noexcept;
    void raise_call_or_calls() noexcept;
    void parse_request() noexcept;
    void release_connection() noexcept;
};

#pragma endregion Cpp Declaration
#pragma region C Definitions

void ujrpc_init(ujrpc_config_t const* config, ujrpc_server_t* server) {

    // Simple sanity check
    if (!server)
        return;

    // Retrieve configs, if present
    uint16_t port = config && config->port > 0 ? config->port : 8545u;
    uint16_t queue_depth = config && config->queue_depth > 0 ? config->queue_depth : 4096u;
    uint16_t callbacks_capacity = config && config->callbacks_capacity > 0 ? config->callbacks_capacity : 128u;
    uint16_t connections_capacity = config && config->connections_capacity > 0 ? config->connections_capacity : 1024u;
    uint16_t threads_limit = config && config->threads_limit ? config->threads_limit : 1u;
    uint32_t lifetime_microsec_limit =
        config && config->lifetime_microsec_limit ? config->lifetime_microsec_limit : 100'000u;

    // Allocate
    int socket_options = 1;
    int socket_descriptor = -1;
    int uring_result = -1;
    struct io_uring ring {};
    engine_t* server_ptr{};
    pool_gt<connection_t> connections{};
    array_gt<named_callback_t> callbacks{};
    buffer_gt<scratch_space_t> spaces{};

    // By default, let's open TCP port for IPv4.
    struct sockaddr_in address {};
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = htonl(INADDR_ANY);
    address.sin_port = htons(port);

    // Initialize `io_uring` first, it is the most likely to fail.
    uring_result = io_uring_queue_init(queue_depth, &ring, 0);
    if (uring_result != 0)
        goto cleanup;

    // Try allocating all the neccessary memory.
    server_ptr = (engine_t*)std::malloc(sizeof(engine_t));
    if (!server_ptr)
        goto cleanup;
    if (!callbacks.reserve(callbacks_capacity))
        goto cleanup;
    if (!connections.reserve(connections_capacity))
        goto cleanup;
    if (!spaces.reserve(threads_limit))
        goto cleanup;
    for (auto& space : spaces)
        if (space.parser.allocate(max_packet_size_k, max_packet_size_k / 2) != sj::SUCCESS)
            goto cleanup;

    // Configure the socket.
    socket_descriptor = socket(AF_INET, SOCK_STREAM, 0);
    if (socket_descriptor < 0)
        goto cleanup;
    if (setsockopt(socket_descriptor, SOL_SOCKET, SO_REUSEADDR | SO_REUSEPORT, &socket_options,
                   sizeof(socket_options)) < 0)
        goto cleanup;
    if (bind(socket_descriptor, (struct sockaddr*)&address, sizeof(address)) < 0)
        goto cleanup;
    if (listen(socket_descriptor, queue_depth) < 0)
        goto cleanup;

    // Initialize all the members.
    new (server_ptr) engine_t();
    server_ptr->socket = descriptor_t{socket_descriptor};
    server_ptr->lifetime_microsec_limit = lifetime_microsec_limit;
    server_ptr->callbacks = std::move(callbacks);
    server_ptr->connections = std::move(connections);
    server_ptr->spaces = std::move(spaces);
    server_ptr->ring = ring;
    *server = (ujrpc_server_t)server_ptr;
    return;

cleanup:
    errno;
    if (ring.ring_fd)
        io_uring_queue_exit(&ring);
    if (socket_descriptor >= 0)
        close(socket_descriptor);
    if (server_ptr)
        std::free(server_ptr);
    *server = nullptr;
}

void ujrpc_add_procedure(ujrpc_server_t server, ujrpc_str_t name, ujrpc_callback_t callback) {
    engine_t& engine = *reinterpret_cast<engine_t*>(server);
    if (engine.callbacks.size() + 1 < engine.callbacks.capacity())
        engine.callbacks.push_back({name, callback});
}

void ujrpc_free(ujrpc_server_t server) {
    if (!server)
        return;

    engine_t& engine = *reinterpret_cast<engine_t*>(server);
    io_uring_queue_exit(&engine.ring);
    engine.~engine_t();
    std::free(server);
}

void ujrpc_take_calls(ujrpc_server_t server, uint16_t thread_idx) {
    while (true) {
        ujrpc_take_call(server, thread_idx);
    }
}

void ujrpc_take_call(ujrpc_server_t server, uint16_t thread_idx) {
    // Unlike the classical synchronous interface, this implements only a part of the connection machine,
    // is responsible for checking if a specific request has been completed. All of the submitted
    // memory must be preserved until we get the confirmation.
    engine_t& engine = *reinterpret_cast<engine_t*>(server);
    return call_automata_t{engine, engine.spaces[thread_idx]}();
}

void ujrpc_call_reply_content(ujrpc_call_t call, ujrpc_str_t body, size_t body_len) {
    call_automata_t& call_automata = *reinterpret_cast<call_automata_t*>(call);
    connection_t& connection = call_automata.connection_ref();
    scratch_space_t& scratch = call_automata.scratch;
    if (scratch.id.empty())
        // No response is needed for "id"-less notifications.
        return;
    if (!body_len)
        body_len = std::strlen(body);

    struct iovec iovecs[iovecs_for_content_k] {};
    fill_with_content(iovecs, scratch.id, std::string_view(body, body_len), true);
    connection.append_output<iovecs_for_content_k>(iovecs);
}

void ujrpc_call_reply_error(ujrpc_call_t call, int code_int, ujrpc_str_t note, size_t note_len) {
    call_automata_t& call_automata = *reinterpret_cast<call_automata_t*>(call);
    connection_t& connection = call_automata.connection_ref();
    scratch_space_t& scratch = call_automata.scratch;
    if (scratch.id.empty())
        // No response is needed for "id"-less notifications.
        return;
    if (!note_len)
        note_len = std::strlen(note);

    char code[16]{};
    std::to_chars_result res = std::to_chars(code, code + sizeof(code), code_int);
    auto code_len = res.ptr - code;
    if (res.ec != std::error_code())
        return ujrpc_call_send_error_unknown(call);

    struct iovec iovecs[iovecs_for_error_k] {};
    fill_with_error(iovecs, scratch.id, std::string_view(code, code_len), std::string_view(note, note_len), true);
    connection.append_output<iovecs_for_error_k>(iovecs);
}

void ujrpc_call_send_error_invalid_params(ujrpc_call_t call) {
    return ujrpc_call_reply_error(call, -32602, "Invalid method param(s).", 24);
}

void ujrpc_call_send_error_unknown(ujrpc_call_t call) {
    return ujrpc_call_reply_error(call, -32603, "Unknown error.", 14);
}

void ujrpc_call_send_error_out_of_memory(ujrpc_call_t call) {
    return ujrpc_call_reply_error(call, -32000, "Out of memory.", 14);
}

bool ujrpc_param_named_i64(ujrpc_call_t call, ujrpc_str_t name, int64_t* result_ptr) {
    call_automata_t& call_automata = *reinterpret_cast<call_automata_t*>(call);
    connection_t& connection = call_automata.connection_ref();
    scratch_space_t& scratch = call_automata.scratch;

    std::memcpy(scratch.json_pointer, "/params/", 8);
    std::memcpy(scratch.json_pointer + 8, name, std::strlen(name) + 1);
    auto value = scratch.tree.at_pointer(scratch.json_pointer);

    if (value.is_int64()) {
        *result_ptr = value.get_int64();
        return true;
    } else if (value.is_uint64()) {
        *result_ptr = static_cast<int64_t>(value.get_uint64());
        return true;
    } else
        return false;
}

#pragma endregion C Definitions
#pragma region Cpp Declarations

std::string_view connection_t::reserve_input() noexcept {
    if (input.size() + max_packet_size_k + sj::SIMDJSON_PADDING < input.capacity())
        return {input.data() + input.size(), max_packet_size_k};

    else if (auto dynamic_replacement = std::aligned_alloc(align_k, input.next_capacity()); dynamic_replacement) {
        std::memcpy(dynamic_replacement, input.data(), input.size());
        input.dynamic = (char*)dynamic_replacement;
        input.volume = input.next_capacity();
        return {input.dynamic, input.volume - input.size()};
    } else
        return {};
}

template <std::size_t iovecs_count_ak> //
bool connection_t::append_output(struct iovec const* iovecs) noexcept {
    std::size_t added_length = iovecs_length<iovecs_count_ak>(iovecs);

    if (output.size() + added_length > output.capacity()) {
        // TODO: More efficient growth strategy, in powers of two and till next page size multiple
        if (auto dynamic_replacement = std::malloc(output.size() + added_length); dynamic_replacement) {
            std::memcpy(dynamic_replacement, output.data(), output.size());
            output.dynamic = (char*)dynamic_replacement;
            output.volume = output.size() + added_length;
        } else
            return false;
    }

    iovecs_memcpy<iovecs_count_ak>(iovecs, output.data() + output.size());
    output.length += added_length;
    return true;
}

void call_automata_t::raise_call() noexcept {
    auto callback_or_error = find_callback(engine.callbacks, scratch);
    if (auto error_ptr = std::get_if<default_error_t>(&callback_or_error); error_ptr)
        return ujrpc_call_reply_error(this, error_ptr->code, error_ptr->note.data(), error_ptr->note.size());

    auto callback = std::get<ujrpc_callback_t>(callback_or_error);
    return callback(this);
}

void call_automata_t::raise_call_or_calls() noexcept {
    connection_t& connection = connection_ref();
    sjd::parser& parser = *scratch.dynamic_parser;
    std::string_view json_body = scratch.dynamic_packet;
    parser.set_max_capacity(json_body.size());
    sjd::document doc;
    auto one_or_many = parser.parse_into_document(doc, json_body.data(), json_body.size(), false);
    if (one_or_many.error() == sj::CAPACITY)
        return ujrpc_call_send_error_out_of_memory(this);
    if (one_or_many.error() != sj::SUCCESS)
        return ujrpc_call_reply_error(this, -32700, "Invalid JSON was received by the server.", 40);

    // The major difference between batch and single-request paths is that
    // in the first case we need to keep a copy of the data somewhere,
    // until answers to all requests are accumulated and we can submit them
    // simultaneously.
    // Linux supports `MSG_MORE` flag for submissions, which could have helped,
    // but it is less effictive than assembling a copy here.
    if (one_or_many.is_array()) {
        sjd::array many = one_or_many.get_array();
        scratch.is_batch = true;

        // Start a JSON array. Originally it must fit into `embedded` part.
        connection.output.embedded[0] = '[';
        connection.output.length = 1;

        for (sjd::element const one : many) {
            scratch.tree = one;
            raise_call();
        }

        // Replace the last comma with the closing bracket.
        (connection.output.dynamic ? connection.output.dynamic
                                   : connection.output.embedded)[connection.output.length - 1] = ']';
    } else {
        scratch.is_batch = false;
        scratch.tree = one_or_many.value();
        raise_call();

        // Drop the last comma, if present.
        connection.output.length -= connection.output.length != 0;
    }
}

void call_automata_t::parse_request() noexcept {

    auto& connection = connection_ref();
    auto request = connection.input.view();
    auto json_or_error = strip_http_headers(request);
    if (auto error_ptr = std::get_if<default_error_t>(&json_or_error); error_ptr)
        return ujrpc_call_reply_error(this, error_ptr->code, error_ptr->note.data(), error_ptr->note.size());

    scratch.dynamic_packet = std::get<std::string_view>(json_or_error);
    if (scratch.dynamic_packet.size() > max_packet_size_k) {
        sjd::parser parser;
        if (parser.allocate(scratch.dynamic_packet.size(), scratch.dynamic_packet.size() / 2) != sj::SUCCESS)
            ujrpc_call_send_error_out_of_memory(this);
        else {
            scratch.dynamic_parser = &parser;
            return raise_call_or_calls();
        }
    } else {
        scratch.dynamic_parser = &scratch.parser;
        return raise_call_or_calls();
    }
}

bool call_automata_t::received_full_request() const noexcept {
    // TODO:
    return true;
}

bool call_automata_t::pop_entry_from_uring_completion_queue() noexcept {
    // timeout.tv_nsec = 5000;
    // uring_response = io_uring_wait_cqe_timeout(&engine.ring, &cqe, &timeout);
    uring_response = io_uring_wait_cqe(&engine.ring, &cqe);
    if (uring_response < 0 || !cqe)
        // Error codes:
        // -62: Timer expired.
        return false;

    std::memcpy(&cqe_copy, cqe, sizeof(cqe_copy));
    io_uring_cqe_seen(&engine.ring, cqe);
    return true;
}

bool call_automata_t::consider_accepting_new_connection() noexcept {
    std::size_t reserved_connections{};
    if (!engine.reserved_connections.compare_exchange_strong(reserved_connections, 1u))
        return false;

    connection_t* con_ptr = engine.connections.alloc();
    if (!con_ptr)
        exit(1);

    connection_t& connection = *con_ptr;
    sqe = io_uring_get_sqe(&engine.ring);
    io_uring_prep_accept(sqe, engine.socket, &connection.client_addr, &connection.client_addr_len, 0);
    connection.stage = stage_t::waiting_to_accept_k;
    io_uring_sqe_set_data(sqe, con_ptr);
    uring_response = io_uring_submit(&engine.ring);
    if (uring_response != 1) {
        engine.connections.release(con_ptr);
        engine.reserved_connections--;
        return false;
    }
    return true;
}

void call_automata_t::release_connection() noexcept {
    auto& connection = connection_ref();
    connection.input.reset();
    connection.output.reset();
    engine.connections.release(&connection);
    engine.reserved_connections -= connection.stage == stage_t::waiting_to_accept_k;
    engine.active_connections--;
}

bool call_automata_t::release_connection_if_corrupted() noexcept {
    auto code = syscall_response();
    if (code >= 0)
        return false;

    switch (-code) {
    case EPIPE:
    case EBADF:
        release_connection();
        break;
        // Unknown error.
    default:
        exit(1);
        break;
    }
    return true;
}

void call_automata_t::close_gracefully() noexcept {
    auto& connection = connection_ref();
    connection.output.reset();
    connection.stage = stage_t::waiting_to_close_k;
    sqe = io_uring_get_sqe(&engine.ring);
    io_uring_prep_close(sqe, int(connection.descriptor));
    io_uring_sqe_set_data(sqe, &connection);
    uring_response = io_uring_submit(&engine.ring);
}

void call_automata_t::send_next() noexcept {
    auto& connection = connection_ref();
    auto next = connection.next_output_chunk();
    connection.stage = stage_t::responding_in_progress_k;
    sqe = io_uring_get_sqe(&engine.ring);
    io_uring_prep_send(sqe, int(connection.descriptor), (void*)next.data(), next.size(), 0);
    io_uring_sqe_set_data(sqe, &connection);
    uring_response = io_uring_submit(&engine.ring);
}

void call_automata_t::receive_next() noexcept {
    auto& connection = connection_ref();
    auto next = connection.next_input_chunk();
    connection.stage = stage_t::receiving_in_progress_k;
    sqe = io_uring_get_sqe(&engine.ring);
    io_uring_prep_recv(sqe, int(connection.descriptor), (void*)next.data(), next.size(), 0);
    io_uring_sqe_set_data(sqe, &connection);
    uring_response = io_uring_submit(&engine.ring);

    // TODO: Add timeout:
}

void call_automata_t::operator()() noexcept {

    cqe = nullptr;
    sqe = nullptr;
    consider_accepting_new_connection();

    if (!pop_entry_from_uring_completion_queue())
        return;

    if (release_connection_if_corrupted())
        return;

    connection_t& connection = connection_ref();
    switch (connection.stage) {

    case stage_t::waiting_to_accept_k:
        // Check if accepting the new connection request worked out.
        engine.reserved_connections--;
        engine.active_connections++;
        connection.descriptor = descriptor_t{syscall_response()};
        connection.deadline = cpu_cycle() + cpu_cycles_per_micro_second_k * 100'000'000;
        connection.input_absorbed = 0;
        connection.output_submitted = 0;
        std::memset(connection.input.data(), 0, connection.input.capacity());
        receive_next();
        break;

    case stage_t::receiving_in_progress_k:

        if (connection.expired())
            return close_gracefully();
        if (syscall_response() == 0)
            return receive_next();

        // Absorb the arrived data.
        connection.input.length += syscall_response();
        if (received_full_request()) {
            // If we have reached the end of the stream, it is time to analyze the contents
            // and send back a response.
            parse_request();
            // Reclaim dynamic memory.
            connection.input.reset();
            // Some requests require no response at all.
            if (!connection.next_output_chunk().empty())
                send_next();
        } else {
            if (!connection.reserve_input().empty()) {
                receive_next();
            } else {
                ujrpc_call_send_error_out_of_memory(this);
                send_next();
            }
        }
        break;

    case stage_t::responding_in_progress_k:

        if (connection.expired())
            return close_gracefully();
        if (syscall_response() == 0)
            return send_next();

        connection.output_submitted += syscall_response();
        if (!connection.submitted_all())
            send_next();
        else
            receive_next();
        break;

    case stage_t::waiting_to_close_k:
        release_connection();
        break;
    }
}

#pragma endregion Cpp Declarations
