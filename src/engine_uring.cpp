/**
 * @brief JSON-RPC implementation for TCP/IP stack with `io_uring`.
 *
 * Supports:
 * > Millions of concurrent statefull connections.
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
 * defined during construction with `ujrpc_init`. Some `connection_t`-s
 * can, however, be simulatenaously handled by two threads, if one logical
 * operation is split into multiple phisical calls:
 *
 *      1.  Receiving packets with timeouts.
 *          This allows us to reconsider closing a connection every once
 *          in a while, instead of loyally waiting for more data to come.
 *      2.  Closing sockets gracefully.
 *
 *
 * @section Suggested compilation flags:
 * -fno-exceptions
 *
 * @author Ashot Vardanian
 * @see Notable links:
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
#include <mutex>    // `std::mutex`
#include <optional> // `std::optional`

#include <simdjson.h>

#include "ujrpc/ujrpc.h"

#include "helpers/parse.hpp"
#include "helpers/reply.hpp"
#include "helpers/shared.hpp"

#pragma region Cpp Declaration

namespace sj = simdjson;
namespace sjd = sj::dom;
using namespace unum::ujrpc;

static constexpr std::size_t sleep_growth_factor_k = 4;
static constexpr std::size_t wakeup_initial_frequency_ns_k = 5'000;
static constexpr std::size_t max_inactive_duration_ns_k = 100'000'000'00;

struct completed_event_t;
struct exchange_buffer_t;
struct connection_t;
struct engine_t;
struct automata_t;

enum class stage_t {
    waiting_to_accept_k = 0,
    expecting_reception_k,
    responding_in_progress_k,
    waiting_to_close_k,
    unknown_k,
};

struct completed_event_t {
    connection_t& connection;
    stage_t stage{};
    int result{};

    explicit operator bool() const noexcept { return stage != stage_t::unknown_k; }
    bool is_corrupted() const noexcept { return result == -EPIPE || result == -EBADF; }
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
    std::string_view view() const noexcept { return {data(), length}; }
    std::string_view view_buffer() const noexcept { return {data(), volume}; }
    std::size_t next_capacity() const noexcept { return capacity() * 2; }
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
    /// @brief Current state at which the automata has arrived.
    stage_t stage{};

    struct sockaddr client_addr {};
    socklen_t client_addr_len{sizeof(client_addr)};

    std::size_t input_absorbed{};
    std::size_t output_submitted{};

    /// @brief Accumulated duration of sleep cycles.
    std::size_t sleep_ns{};
    std::size_t empty_transmissions{};

    /// @brief Relative time set for the last wake-up call.
    struct __kernel_timespec next_wakeup {};
    /// @brief Absolute time extracted from HTTP headers, for the requested lifetime of this channel.
    std::optional<struct __kernel_timespec> keep_alive{};
    /// @brief Expected reception length extracted from HTTP headers.
    std::optional<std::size_t> content_length{};
    /// @brief Expected MIME type of payload extracted from HTTP headers. Generally "application/json".
    std::optional<std::string_view> content_type{};

    bool submitted_all() const noexcept { return output_submitted >= output.size(); }
    std::string_view next_inputs() const noexcept { return input.view_buffer().substr(input_absorbed); }
    std::string_view next_output_chunk() const noexcept { return output.view().substr(output_submitted); }

    template <std::size_t iovecs_count_ak> bool append_output(struct iovec const* iovecs) noexcept;
    std::string_view reserve_input() noexcept;

    bool expired() const noexcept;
    void reset() noexcept;
};

struct engine_t {
    descriptor_t socket{};
    struct io_uring ring {};

    std::atomic<std::size_t> active_connections{};
    std::atomic<std::size_t> reserved_connections{};
    std::atomic<std::size_t> dismissed_connections{};
    uint32_t lifetime_microsec_limit{};

    std::mutex submission_mutex{};
    std::mutex completion_mutex{};

    /// @brief An array of function callbacks. Can be in dozens.
    array_gt<named_callback_t> callbacks{};
    /// @brief A circular container of reusable connections. Can be in millions.
    pool_gt<connection_t> connections{};
    /// @brief Same number of them, as max physical threads. Can be in hundreds.
    buffer_gt<scratch_space_t> spaces{};

    bool consider_accepting_new_connection() noexcept;
    completed_event_t pop_completed() noexcept;
    void release_connection(connection_t&) noexcept;
};

struct automata_t {

    engine_t& engine;
    scratch_space_t& scratch;
    connection_t& connection;
    stage_t completed_stage{};
    int completed_result{};

    void operator()() noexcept;

    // Computed properties:
    bool received_full_request() const noexcept;
    bool should_release() const noexcept;

    // Submitting to io_uring:
    void send_next() noexcept;
    void receive_next() noexcept;
    void close_gracefully() noexcept;

    // Helpers:
    void raise_call() noexcept;
    void raise_call_or_calls() noexcept;
    void parse_request() noexcept;
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
    engine.consider_accepting_new_connection();
    completed_event_t completed = engine.pop_completed();
    if (!completed)
        return;

    if (completed.is_corrupted())
        return engine.release_connection(completed.connection);

    automata_t automata{
        .engine = engine,
        .scratch = engine.spaces[thread_idx],
        .connection = completed.connection,
        .completed_stage = completed.stage,
        .completed_result = completed.result,
    };

    // If everything is fine, let automata work in its normal regime.
    return automata();
}

void ujrpc_call_reply_content(ujrpc_call_t call, ujrpc_str_t body, size_t body_len) {
    automata_t& automata = *reinterpret_cast<automata_t*>(call);
    connection_t& connection = automata.connection;
    scratch_space_t& scratch = automata.scratch;
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
    automata_t& automata = *reinterpret_cast<automata_t*>(call);
    connection_t& connection = automata.connection;
    scratch_space_t& scratch = automata.scratch;
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
    automata_t& automata = *reinterpret_cast<automata_t*>(call);
    connection_t& connection = automata.connection;
    scratch_space_t& scratch = automata.scratch;

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

bool connection_t::expired() const noexcept {
    if (sleep_ns > max_inactive_duration_ns_k)
        return true;

    return false;
}

void connection_t::reset() noexcept {
    // TODO: deadline = cpu_cycle() + cpu_cycles_per_micro_second_k * 100'000'000;
    std::memset(input.data(), 0, input.capacity());

    input_absorbed = 0;
    output_submitted = 0;
    sleep_ns = 0;
    empty_transmissions = 0;
    next_wakeup.tv_nsec = wakeup_initial_frequency_ns_k;
    keep_alive.reset();
    content_length.reset();
    content_type.reset();
}

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

bool automata_t::should_release() const noexcept {
    return connection.expired() || engine.dismissed_connections || connection.empty_transmissions > 100;
}

bool automata_t::received_full_request() const noexcept {
    if (connection.content_length)
        if (connection.input.size() < *connection.content_length)
            return false;

    // TODO: If we don't have explicit information about the expected length,
    // we may attempt parsing it to check if the received chunk is a valid document.
    return true;
}

void automata_t::raise_call() noexcept {
    auto callback_or_error = find_callback(engine.callbacks, scratch);
    if (auto error_ptr = std::get_if<default_error_t>(&callback_or_error); error_ptr)
        return ujrpc_call_reply_error(this, error_ptr->code, error_ptr->note.data(), error_ptr->note.size());

    auto callback = std::get<ujrpc_callback_t>(callback_or_error);
    return callback(this);
}

void automata_t::raise_call_or_calls() noexcept {
    sjd::parser& parser = *scratch.dynamic_parser;
    std::string_view json_body = scratch.dynamic_packet;
    parser.set_max_capacity(json_body.size());
    auto one_or_many = parser.parse(json_body.data(), json_body.size(), false);
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

void automata_t::parse_request() noexcept {

    auto request = connection.input.view();
    auto json_or_error = strip_http_headers(request);
    if (auto error_ptr = std::get_if<default_error_t>(&json_or_error); error_ptr)
        return ujrpc_call_reply_error(this, error_ptr->code, error_ptr->note.data(), error_ptr->note.size());

    scratch.dynamic_packet = std::get<parsed_request_t>(json_or_error).body;
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

completed_event_t engine_t::pop_completed() noexcept {
    int uring_response{};
    struct io_uring_cqe* cqe{};
    struct __kernel_timespec polling_timeout {};
    polling_timeout.tv_nsec = 5000;

    completion_mutex.lock();
    uring_response = io_uring_wait_cqe_timeout(&ring, &cqe, &polling_timeout);

    // Some results are not worth evaluating in automata.
    if (uring_response < 0 || !cqe || !cqe->user_data) {
        // We should skip over timer events without any payload.
        if (cqe && !cqe->user_data)
            io_uring_cq_advance(&ring, 1);
        completion_mutex.unlock();
        return {*(connection_t*)nullptr, stage_t::unknown_k, 0};
    }

    connection_t& connection = *(connection_t*)cqe->user_data;
    completed_event_t completed{
        .connection = connection,
        .stage = connection.stage,
        .result = cqe->res,
    };
    io_uring_cq_advance(&ring, 1);
    completion_mutex.unlock();
    return completed;
}

bool engine_t::consider_accepting_new_connection() noexcept {
    std::size_t reserved_connections_old{};
    if (!reserved_connections.compare_exchange_strong(reserved_connections_old, 1u))
        return false;

    connection_t* con_ptr = connections.alloc();
    if (!con_ptr) {
        dismissed_connections++;
        return false;
    }

    int uring_response{};
    struct io_uring_sqe* sqe{};
    connection_t& connection = *con_ptr;
    connection.stage = stage_t::waiting_to_accept_k;
    submission_mutex.lock();
    sqe = io_uring_get_sqe(&ring);
    io_uring_prep_accept(sqe, socket, &connection.client_addr, &connection.client_addr_len, 0);
    io_uring_sqe_set_data(sqe, con_ptr);
    uring_response = io_uring_submit(&ring);
    submission_mutex.unlock();
    if (uring_response != 1) {
        connections.release(con_ptr);
        reserved_connections--;
        return false;
    }

    dismissed_connections = 0;
    return true;
}

void engine_t::release_connection(connection_t& connection) noexcept {
    connection.input.reset();
    connection.output.reset();
    connections.release(&connection);
    reserved_connections -= connection.stage == stage_t::waiting_to_accept_k;
    active_connections--;
}

// TODO
#define IORING_ASYNC_CANCEL_ALL 0
#define IOSQE_CQE_SKIP_SUCCESS 0

void automata_t::close_gracefully() noexcept {
    int uring_response{};
    struct io_uring_sqe* sqe{};
    connection.output.reset();
    connection.stage = stage_t::waiting_to_close_k;

    // The operations are not expected to complete in exactly the same order
    // as their submissions. So to stop all existing communication on the
    // socket, we can cancel everythin related to its "file descriptor",
    // and then close.
    engine.submission_mutex.lock();
    sqe = io_uring_get_sqe(&engine.ring);
    io_uring_prep_cancel(sqe, &connection, IORING_ASYNC_CANCEL_ALL | IOSQE_CQE_SKIP_SUCCESS);
    io_uring_sqe_set_data(sqe, &connection);
    io_uring_sqe_set_flags(sqe, IOSQE_IO_HARDLINK);
    uring_response = io_uring_submit(&engine.ring);

    sqe = io_uring_get_sqe(&engine.ring);
    io_uring_prep_close(sqe, int(connection.descriptor));
    io_uring_sqe_set_data(sqe, &connection);
    io_uring_sqe_set_flags(sqe, IOSQE_CQE_SKIP_SUCCESS);
    uring_response = io_uring_submit(&engine.ring);
    engine.submission_mutex.unlock();
}

void automata_t::send_next() noexcept {
    int uring_response{};
    struct io_uring_sqe* sqe{};
    auto next = connection.next_output_chunk();
    connection.stage = stage_t::responding_in_progress_k;

    engine.submission_mutex.lock();
    sqe = io_uring_get_sqe(&engine.ring);
    io_uring_prep_write(sqe, int(connection.descriptor), (void*)next.data(), next.size(), 0);
    io_uring_sqe_set_data(sqe, &connection);
    uring_response = io_uring_submit(&engine.ring);
    engine.submission_mutex.unlock();
}

void automata_t::receive_next() noexcept {
    int uring_response{};
    struct io_uring_sqe* sqe{};
    auto next = connection.next_inputs();
    connection.stage = stage_t::expecting_reception_k;

    engine.submission_mutex.lock();

    // Choosing between `recv` and `read` system calls:
    // > If a zero-length datagram is pending, read(2) and recv() with a
    // > flags argument of zero provide different behavior. In this
    // > circumstance, read(2) has no effect (the datagram remains
    // > pending), while recv() consumes the pending datagram.
    // https://man7.org/linux/man-pages/man2/recv.2.html
    //
    // In this case we are waiting for an actual data, not some artificial wakeup.
    sqe = io_uring_get_sqe(&engine.ring);
    io_uring_prep_read(sqe, int(connection.descriptor), (void*)next.data(), next.size(), 0);
    io_uring_sqe_set_data(sqe, &connection);
    io_uring_sqe_set_flags(sqe, IOSQE_IO_LINK);
    uring_response = io_uring_submit(&engine.ring);

    // More than other operations this depends on the information coming from the client.
    // We can't afford to keep connections alive indefinitely, so we need to set a timeout
    // on this operation.
    sqe = io_uring_get_sqe(&engine.ring);
    io_uring_prep_link_timeout(sqe, &connection.next_wakeup, 0);
    io_uring_sqe_set_data(sqe, NULL);
    uring_response = io_uring_submit(&engine.ring);

    engine.submission_mutex.unlock();
}

void automata_t::operator()() noexcept {

    switch (connection.stage) {

    case stage_t::waiting_to_accept_k:

        // Check if accepting the new connection request worked out.
        engine.reserved_connections--;
        engine.active_connections++;
        connection.reset();
        connection.descriptor = descriptor_t{completed_result};
        receive_next();
        break;

    case stage_t::expecting_reception_k:

        // From documentation:
        // > If used, the timeout specified in the command will cancel the linked command,
        // > unless the linked command completes before the timeout. The timeout will complete
        // > with -ETIME if the timer expired and the linked request was attempted canceled,
        // > or -ECANCELED if the timer got canceled because of completion of the linked request.
        //
        // So we expect only two outcomes here:
        // 1. reception expired with: `ECANCELED`, and subsequent timeout expired with `ETIME`.
        // 2. reception can continue and subsequent timer returned `ECANCELED`.
        //
        // If the following timeout request has happened,
        // we don't want to do anything here. Let's leave the faith of
        // this connection to the subsequent timer to decide.
        if (completed_result == -ECANCELED) {
            connection.sleep_ns += connection.next_wakeup.tv_nsec;
            connection.next_wakeup.tv_nsec *= sleep_growth_factor_k;
            if (should_release())
                return close_gracefully();
            return;
        }
        // No data was received.
        else if (completed_result == 0) {
            connection.empty_transmissions++;
            if (should_release())
                return close_gracefully();
            return;
        }

        // Absorb the arrived data.
        connection.input.length += completed_result;
        connection.empty_transmissions = 0;
        connection.sleep_ns = 0;

        if (received_full_request()) {
            // If we have reached the end of the stream,
            // it is time to analyze the contents
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

        connection.empty_transmissions = completed_result == 0 ? ++connection.empty_transmissions : 0;
        if (should_release())
            return close_gracefully();

        connection.output_submitted += completed_result;
        if (!connection.submitted_all())
            send_next();
        else
            receive_next();
        break;

    case stage_t::waiting_to_close_k:
        engine.release_connection(connection);
        break;
    }
}

#pragma endregion Cpp Declarations
