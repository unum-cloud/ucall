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
#include <arpa/inet.h>  // `inet_addr`
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
static constexpr std::size_t wakeup_initial_frequency_ns_k = 3'000;
static constexpr std::size_t max_inactive_duration_ns_k = 100'000'000'000;
static constexpr std::size_t stats_heartbeat_s_k = 5;

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
    log_stats_k,
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

    struct sockaddr client_address {};
    socklen_t client_address_len{sizeof(struct sockaddr)};

    std::size_t input_absorbed{};
    std::size_t output_submitted{};

    /// @brief Accumulated duration of sleep cycles.
    std::size_t sleep_ns{};
    std::size_t empty_transmits{};
    std::size_t exchanges{};

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
    std::uint32_t max_lifetime_micro_seconds{};
    std::uint32_t max_lifetime_exchanges{};

    std::mutex submission_mutex{};
    std::mutex completion_mutex{};
    std::mutex connections_mutex{};

    std::atomic<std::size_t> stats_new_connections{};
    std::atomic<std::size_t> stats_closed_connections{};
    std::atomic<std::size_t> stats_bytes_received{};
    std::atomic<std::size_t> stats_bytes_sent{};
    std::atomic<std::size_t> stats_packets_received{};
    std::atomic<std::size_t> stats_packets_sent{};
    connection_t stats_pseudo_connection{};

    /// @brief An array of function callbacks. Can be in dozens.
    array_gt<named_callback_t> callbacks{};
    /// @brief A circular container of reusable connections. Can be in millions.
    pool_gt<connection_t> connections{};
    /// @brief Same number of them, as max physical threads. Can be in hundreds.
    buffer_gt<scratch_space_t> spaces{};

    bool consider_accepting_new_connection() noexcept;
    void submit_stats_heartbeat() noexcept;
    completed_event_t pop_completed() noexcept;
    void release_connection(connection_t&) noexcept;
    void log_and_reset_stats() noexcept;
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
    void parse_and_raise_request() noexcept;
};

#pragma endregion Cpp Declaration
#pragma region C Definitions

void ujrpc_init(ujrpc_config_t* config_inout, ujrpc_server_t* server_out) {

    // Simple sanity check
    if (!server_out && !config_inout)
        return;

    // Retrieve configs, if present
    ujrpc_config_t& config = *config_inout;
    if (!config.port)
        config.port = 8545u;
    if (!config.queue_depth)
        config.queue_depth = 4096u;
    if (!config.callbacks_capacity)
        config.callbacks_capacity = 128u;
    if (!config.max_concurrent_connections)
        config.max_concurrent_connections = 100'000;
    if (!config.max_threads)
        config.max_threads = 1u;
    if (!config.max_lifetime_micro_seconds)
        config.max_lifetime_micro_seconds = 100'000u;
    if (!config.max_lifetime_exchanges)
        config.max_lifetime_exchanges = 100u;
    if (!config.interface)
        config.interface = "0.0.0.0";

    // Allocate
    int socket_options{1};
    int socket_descriptor{-1};
    int uring_result{-1};
    struct io_uring ring {};
    auto ring_flags{0};
    // 5.19+
    // ring_flags |= IORING_SETUP_COOP_TASKRUN;
    // 6.0+
    // ring_flags |= config.max_threads == 1 ? IORING_SETUP_SINGLE_ISSUER : 0;
    engine_t* server_ptr{};
    pool_gt<connection_t> connections{};
    array_gt<named_callback_t> callbacks{};
    buffer_gt<scratch_space_t> spaces{};

    // By default, let's open TCP port for IPv4.
    struct sockaddr_in address {};
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = inet_addr(config.interface);
    address.sin_port = htons(config.port);

    // Initialize `io_uring` first, it is the most likely to fail.
    uring_result = io_uring_queue_init(config.queue_depth, &ring, ring_flags);
    if (uring_result != 0)
        goto cleanup;

    // Try allocating all the neccessary memory.
    server_ptr = (engine_t*)std::malloc(sizeof(engine_t));
    if (!server_ptr)
        goto cleanup;
    if (!callbacks.reserve(config.callbacks_capacity))
        goto cleanup;
    if (!connections.reserve(config.max_concurrent_connections))
        goto cleanup;
    if (!spaces.reserve(config.max_threads))
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
    if (listen(socket_descriptor, config.queue_depth) < 0)
        goto cleanup;

    // Initialize all the members.
    new (server_ptr) engine_t();
    server_ptr->socket = descriptor_t{socket_descriptor};
    server_ptr->max_lifetime_micro_seconds = config.max_lifetime_micro_seconds;
    server_ptr->max_lifetime_exchanges = config.max_lifetime_exchanges;
    server_ptr->callbacks = std::move(callbacks);
    server_ptr->connections = std::move(connections);
    server_ptr->spaces = std::move(spaces);
    server_ptr->ring = ring;
    *server_out = (ujrpc_server_t)server_ptr;
    return;

cleanup:
    errno;
    if (ring.ring_fd)
        io_uring_queue_exit(&ring);
    if (socket_descriptor >= 0)
        close(socket_descriptor);
    if (server_ptr)
        std::free(server_ptr);
    *server_out = nullptr;
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
    engine_t& engine = *reinterpret_cast<engine_t*>(server);
    if (!thread_idx)
        engine.submit_stats_heartbeat();
    while (true) {
        ujrpc_take_call(server, thread_idx);
    }
}

void ujrpc_take_call(ujrpc_server_t server, uint16_t thread_idx) {
    // Unlike the classical synchronous interface, this implements only a part of the connection machine,
    // is responsible for checking if a specific request has been completed. All of the submitted
    // memory must be preserved until we get the confirmation.
    engine_t& engine = *reinterpret_cast<engine_t*>(server);
    if (!thread_idx)
        engine.consider_accepting_new_connection();
    completed_event_t completed = engine.pop_completed();
    if (!completed)
        return;

    if (completed.is_corrupted()) {
        return engine.release_connection(completed.connection);
    }

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
    if (scratch.dynamic_id.empty())
        // No response is needed for "id"-less notifications.
        return;

    body_len = string_length(body, body_len);
    struct iovec iovecs[iovecs_for_content_k] {};
    fill_with_content(iovecs, scratch.dynamic_id, std::string_view(body, body_len), true);
    connection.append_output<iovecs_for_content_k>(iovecs);
}

void ujrpc_call_reply_error(ujrpc_call_t call, int code_int, ujrpc_str_t note, size_t note_len) {
    automata_t& automata = *reinterpret_cast<automata_t*>(call);
    connection_t& connection = automata.connection;
    scratch_space_t& scratch = automata.scratch;
    if (scratch.dynamic_id.empty())
        // No response is needed for "id"-less notifications.
        return;

    note_len = string_length(note, note_len);
    char code[max_integer_length_k]{};
    std::to_chars_result res = std::to_chars(code, code + max_integer_length_k, code_int);
    auto code_len = res.ptr - code;
    if (res.ec != std::error_code())
        return ujrpc_call_send_error_unknown(call);

    struct iovec iovecs[iovecs_for_error_k] {};
    fill_with_error(iovecs, scratch.dynamic_id, std::string_view(code, code_len), std::string_view(note, note_len),
                    true);
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

bool ujrpc_param_named_i64(ujrpc_call_t call, ujrpc_str_t name, size_t name_len, int64_t* result_ptr) {

    automata_t& automata = *reinterpret_cast<automata_t*>(call);
    connection_t& connection = automata.connection;
    scratch_space_t& scratch = automata.scratch;
    name_len = string_length(name, name_len);
    std::memcpy(scratch.json_pointer, "/params/", 8);
    std::memcpy(scratch.json_pointer + 8, name, name_len + 1);
    auto value = scratch.tree.at_pointer(scratch.json_pointer);

    if (!value.is_int64())
        return false;

    *result_ptr = value.get_int64().value_unsafe();
    return true;
}

bool ujrpc_param_named_str(ujrpc_call_t call, ujrpc_str_t name, size_t name_len, char const** result_ptr,
                           size_t* result_len_ptr) {

    automata_t& automata = *reinterpret_cast<automata_t*>(call);
    connection_t& connection = automata.connection;
    scratch_space_t& scratch = automata.scratch;
    name_len = string_length(name, name_len);
    std::memcpy(scratch.json_pointer, "/params/", 8);
    std::memcpy(scratch.json_pointer + 8, name, name_len + 1);
    auto value = scratch.tree.at_pointer(scratch.json_pointer);

    if (!value.is_string())
        return false;

    *result_ptr = value.get_string().value_unsafe().data();
    *result_len_ptr = value.get_string_length().value_unsafe();
    return true;
}

#pragma endregion C Definitions
#pragma region Cpp Declarations

bool connection_t::expired() const noexcept {
    if (sleep_ns > max_inactive_duration_ns_k)
        return true;

    return false;
}

void connection_t::reset() noexcept {

    stage = stage_t::unknown_k;
    client_address = {};

    input.reset();
    output.reset();
    input_absorbed = 0;
    output_submitted = 0;

    keep_alive.reset();
    content_length.reset();
    content_type.reset();

    sleep_ns = 0;
    empty_transmits = 0;
    exchanges = 0;
    next_wakeup.tv_nsec = wakeup_initial_frequency_ns_k;
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
    return connection.expired() || engine.dismissed_connections || connection.empty_transmits > 100;
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

void automata_t::parse_and_raise_request() noexcept {

    auto request = connection.input.view();
    auto json_or_error = strip_http_headers(request);
    if (auto error_ptr = std::get_if<default_error_t>(&json_or_error); error_ptr)
        return ujrpc_call_reply_error(this, error_ptr->code, error_ptr->note.data(), error_ptr->note.size());

    auto parsed_request = std::get<parsed_request_t>(json_or_error);
    scratch.is_http = request.size() != parsed_request.body.size();
    scratch.dynamic_packet = parsed_request.body;
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
    polling_timeout.tv_nsec = wakeup_initial_frequency_ns_k;

    completion_mutex.lock();
    uring_response = io_uring_wait_cqe_timeout(&ring, &cqe, &polling_timeout);
    // uring_response = io_uring_wait_cqe(&ring, &cqe);

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

    connections_mutex.lock();
    connection_t* con_ptr = connections.alloc();
    connections_mutex.unlock();
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
    io_uring_prep_accept(sqe, socket, &connection.client_address, &connection.client_address_len, 0);
    io_uring_sqe_set_data(sqe, con_ptr);
    io_uring_sqe_set_flags(sqe, IOSQE_IO_LINK);

    sqe = io_uring_get_sqe(&ring);
    io_uring_prep_link_timeout(sqe, &connection.next_wakeup, 0);
    io_uring_sqe_set_data(sqe, NULL);

    uring_response = io_uring_submit(&ring);
    submission_mutex.unlock();
    if (uring_response != 2) {
        connections.release(con_ptr);
        reserved_connections--;
        return false;
    }

    dismissed_connections = 0;
    return true;
}

void engine_t::submit_stats_heartbeat() noexcept {
    int uring_response{};
    struct io_uring_sqe* sqe{};
    connection_t& connection = stats_pseudo_connection;
    connection.stage = stage_t::log_stats_k;
    connection.next_wakeup.tv_sec = stats_heartbeat_s_k;
    submission_mutex.lock();

    sqe = io_uring_get_sqe(&ring);
    io_uring_prep_timeout(sqe, &connection.next_wakeup, 0, 0);
    io_uring_sqe_set_data(sqe, &connection);
    uring_response = io_uring_submit(&ring);
    submission_mutex.unlock();
}

void engine_t::log_and_reset_stats() noexcept {
    static char printed_message_k[max_packet_size_k]{};
    auto printable_normalized = [](std::atomic<std::size_t>& i) noexcept {
        return printable(i.exchange(0, std::memory_order_relaxed) / stats_heartbeat_s_k);
    };
    auto new_connections = printable_normalized(stats_new_connections);
    auto closed_connections = printable_normalized(stats_closed_connections);
    auto bytes_received = printable_normalized(stats_bytes_received);
    auto bytes_sent = printable_normalized(stats_bytes_sent);
    auto packets_received = printable_normalized(stats_packets_received);
    auto packets_sent = printable_normalized(stats_packets_sent);
    auto len = std::snprintf( //
        printed_message_k, max_packet_size_k,
        "connections: +%.1f %c/s, "
        "-%.1f %c/s, "
        "RX: %.1f %c msgs/s, "
        "%.1f %cb/s, "
        "TX: %.1f %c msgs/s, "
        "%.1f %cb/s.\n",
        new_connections.number, new_connections.suffix,       //
        closed_connections.number, closed_connections.suffix, //
        packets_received.number, packets_received.suffix,     //
        bytes_received.number, bytes_received.suffix,         //
        packets_sent.number, packets_sent.suffix,             //
        bytes_sent.number, bytes_sent.suffix                  //
    );
    std::fwrite(printed_message_k, sizeof(char), len, stdout);
}

void engine_t::release_connection(connection_t& connection) noexcept {
    auto is_active = connection.stage != stage_t::waiting_to_accept_k;
    connection.reset();
    connections_mutex.lock();
    connections.release(&connection);
    connections_mutex.unlock();
    active_connections -= is_active;
    stats_closed_connections.fetch_add(is_active, std::memory_order_relaxed);
}

void automata_t::close_gracefully() noexcept {
    int uring_response{};
    struct io_uring_sqe* sqe{};
    connection.stage = stage_t::waiting_to_close_k;

    // The operations are not expected to complete in exactly the same order
    // as their submissions. So to stop all existing communication on the
    // socket, we can cancel everythin related to its "file descriptor",
    // and then close.
    engine.submission_mutex.lock();
    // sqe = io_uring_get_sqe(&engine.ring);
    // io_uring_prep_cancel(sqe, &connection, IORING_ASYNC_CANCEL_ALL | IOSQE_CQE_SKIP_SUCCESS);
    // io_uring_sqe_set_data(sqe, NULL);
    // io_uring_sqe_set_flags(sqe, IOSQE_IO_HARDLINK);

    sqe = io_uring_get_sqe(&engine.ring);
    io_uring_prep_close(sqe, int(connection.descriptor));
    io_uring_sqe_set_data(sqe, &connection);
    io_uring_sqe_set_flags(sqe, 0 /*IOSQE_CQE_SKIP_SUCCESS*/);
    uring_response = io_uring_submit(&engine.ring);
    engine.submission_mutex.unlock();
}

void automata_t::send_next() noexcept {
    int uring_response{};
    struct io_uring_sqe* sqe{};
    auto next = connection.next_output_chunk();
    connection.stage = stage_t::responding_in_progress_k;
    connection.input_absorbed = 0;
    connection.input.reset();

    engine.submission_mutex.lock();
    sqe = io_uring_get_sqe(&engine.ring);
    // io_uring_prep_send_zc(sqe, int(connection.descriptor), (void*)next.data(), next.size(), 0, 0);
    io_uring_prep_send(sqe, int(connection.descriptor), (void*)next.data(), next.size(), 0);
    io_uring_sqe_set_data(sqe, &connection);
    uring_response = io_uring_submit(&engine.ring);
    engine.submission_mutex.unlock();
}

void automata_t::receive_next() noexcept {
    int uring_response{};
    struct io_uring_sqe* sqe{};
    auto next = connection.next_inputs();
    connection.stage = stage_t::expecting_reception_k;
    connection.output_submitted = 0;
    connection.output.reset();

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
    io_uring_prep_recv(sqe, int(connection.descriptor), (void*)next.data(), next.size(), 0);
    io_uring_sqe_set_data(sqe, &connection);
    io_uring_sqe_set_flags(sqe, IOSQE_IO_LINK);

    // More than other operations this depends on the information coming from the client.
    // We can't afford to keep connections alive indefinitely, so we need to set a timeout
    // on this operation.
    // The `io_uring_prep_link_timeout` is a conveniece method for poorly documented `IORING_OP_LINK_TIMEOUT`.
    sqe = io_uring_get_sqe(&engine.ring);
    io_uring_prep_link_timeout(sqe, &connection.next_wakeup, 0);
    io_uring_sqe_set_data(sqe, NULL);
    uring_response = io_uring_submit(&engine.ring);

    engine.submission_mutex.unlock();
}

void automata_t::operator()() noexcept {

    switch (connection.stage) {

    case stage_t::waiting_to_accept_k:

        if (completed_result == -ECANCELED) {
            engine.release_connection(connection);
            engine.reserved_connections--;
            engine.consider_accepting_new_connection();
            return;
        }

        // Check if accepting the new connection request worked out.
        engine.reserved_connections--;
        engine.active_connections++;
        engine.stats_new_connections.fetch_add(1, std::memory_order_relaxed);
        connection.descriptor = descriptor_t{completed_result};
        return receive_next();

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
            completed_result = 0;
        }

        // No data was received.
        if (completed_result == 0) {
            connection.empty_transmits++;
            return should_release() ? close_gracefully() : receive_next();
        }

        // Absorb the arrived data.
        connection.input.length += completed_result;
        connection.empty_transmits = 0;
        connection.sleep_ns = 0;
        engine.stats_bytes_received.fetch_add(completed_result, std::memory_order_relaxed);
        engine.stats_packets_received.fetch_add(1, std::memory_order_relaxed);

        // If we have reached the end of the stream,
        // it is time to analyze the contents
        // and send back a response.
        if (received_full_request()) {
            parse_and_raise_request();
            connection.input.reset();
            connection.input_absorbed = 0;
            // Some requests require no response at all, so we can go back to listening the port.
            if (!connection.next_output_chunk().empty()) {
                return send_next();
            } else {
                connection.exchanges++;
                if (connection.exchanges >= engine.max_lifetime_exchanges)
                    return close_gracefully();
                else
                    return receive_next();
            }
        }
        // We may fail to allocate memory to receive the next input
        else if (connection.reserve_input().empty()) {
            ujrpc_call_send_error_out_of_memory(this);
            return send_next();
        } else
            return receive_next();

    case stage_t::responding_in_progress_k:

        connection.empty_transmits = completed_result == 0 ? ++connection.empty_transmits : 0;

        if (should_release())
            return close_gracefully();

        connection.output_submitted += completed_result;
        engine.stats_bytes_sent.fetch_add(completed_result, std::memory_order_relaxed);
        engine.stats_packets_sent.fetch_add(1, std::memory_order_relaxed);
        if (connection.submitted_all()) {
            connection.exchanges++;
            if (connection.exchanges >= engine.max_lifetime_exchanges)
                return close_gracefully();
            else
                return receive_next();
        } else
            return send_next();

    case stage_t::waiting_to_close_k:
        return engine.release_connection(connection);

    case stage_t::log_stats_k:
        engine.log_and_reset_stats();
        return engine.submit_stats_heartbeat();
    }
}

#pragma endregion Cpp Declarations
