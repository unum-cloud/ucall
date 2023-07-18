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
#include <arpa/inet.h>  // `inet_addr`
#include <netinet/in.h> // `sockaddr_in`
#include <sys/socket.h> //  `setsockopt`
#include <sys/types.h>
#include <sys/uio.h>
#include <unistd.h>

#include <liburing.h>

#include <simdjson.h>

#include "ucall/ucall.h"

#include "server.hpp"

#pragma region Cpp Declaration

namespace sj = simdjson;
namespace sjd = sj::dom;
using namespace unum::ucall;

bool io_check_send_zc() noexcept {
    io_uring_probe* probe = io_uring_get_probe();
    if (!probe)
        return false;

    // Available since 6.0.
    bool res = io_uring_opcode_supported(probe, IORING_OP_SEND_ZC);
    io_uring_free_probe(probe);
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

    // Allocate
    int socket_options{1};
    int socket_descriptor{-1};
    int uring_result{-1};
    struct io_uring uring {};
    struct io_uring_params uring_params {};
    struct io_uring_sqe* uring_sqe{};
    struct io_uring_cqe* uring_cqe{};
    uring_params.features |= IORING_FEAT_FAST_POLL;
    uring_params.features |= IORING_FEAT_SQPOLL_NONFIXED;
    // uring_params.flags |= IORING_SETUP_COOP_TASKRUN;
    uring_params.flags |= IORING_SETUP_SQPOLL;
    uring_params.sq_thread_idle = wakeup_initial_frequency_ns_k;
    // uring_params.flags |= config.max_threads == 1 ? IORING_SETUP_SINGLE_ISSUER : 0; // 6.0+
    engine_t* server_ptr{};
    pool_gt<connection_t> connections{};
    array_gt<named_callback_t> callbacks{};
    buffer_gt<scratch_space_t> spaces{};
    buffer_gt<struct iovec> registered_buffers{};
    memory_map_t fixed_buffers{};

    // By default, let's open TCP port for IPv4.
    struct sockaddr_in address {};
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = inet_addr(config.hostname);
    address.sin_port = htons(config.port);

    // Initialize `io_uring` first, it is the most likely to fail.
    uring_result = io_uring_queue_init_params(config.queue_depth, &uring, &uring_params);
    if (uring_result != 0)
        goto cleanup;

    // Try allocating all the necessary memory.
    server_ptr = (engine_t*)std::malloc(sizeof(engine_t));
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

    // Additional `io_uring` setup.
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
    uring_result = io_uring_register_files_sparse(&uring, config.max_concurrent_connections);
    if (uring_result != 0)
        goto cleanup;
    uring_result =
        io_uring_register_buffers(&uring, registered_buffers.data(), static_cast<unsigned>(registered_buffers.size()));
    if (uring_result != 0)
        goto cleanup;

    // Configure the socket.
    // In the past we would use the normal POSIX call, but we should prefer direct descriptors over it.
    // socket_descriptor = socket(AF_INET, SOCK_STREAM, 0);
    uring_sqe = io_uring_get_sqe(&uring);
    io_uring_prep_socket_direct(uring_sqe, AF_INET, SOCK_STREAM, 0, IORING_FILE_INDEX_ALLOC, 0);
    uring_result = io_uring_submit_and_wait(&uring, 1);
    uring_result = io_uring_wait_cqe(&uring, &uring_cqe);
    socket_descriptor = uring_cqe->res;
    if (socket_descriptor < 0)
        goto cleanup;
    // Not sure if this is required, after we have a kernel with `IORING_OP_SENDMSG_ZC` support, we can check.
    // if (setsockopt(socket_descriptor, SOL_SOCKET, SO_ZEROCOPY, &socket_options, sizeof(socket_options)) == -1)
    //     goto cleanup;
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
    server_ptr->uring = uring;
    server_ptr->has_send_zc = io_check_send_zc();
    server_ptr->logs_file_descriptor = config.logs_file_descriptor;
    server_ptr->logs_format = config.logs_format ? std::string_view(config.logs_format) : std::string_view();
    *server_out = (ucall_server_t)server_ptr;
    return;

cleanup:
    errno;
    if (uring.ring_fd)
        io_uring_queue_exit(&uring);
    if (socket_descriptor >= 0)
        close(socket_descriptor);
    std::free(server_ptr);
    *server_out = nullptr;
}

void ucall_free(ucall_server_t server) {
    if (!server)
        return;

    engine_t& engine = *reinterpret_cast<engine_t*>(server);
    io_uring_unregister_buffers(&engine.uring);
    io_uring_queue_exit(&engine.uring);
    close(engine.socket);
    engine.~engine_t();
    std::free(server);
}
