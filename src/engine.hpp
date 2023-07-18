#pragma once

#include <atomic>

#include "connection.hpp"
#include "helpers/contain/buffer.hpp"
#include "helpers/contain/pool.hpp"
#include "helpers/log.hpp"
#include "helpers/shared.hpp"

namespace unum::ucall {

struct engine_t {
    descriptor_t socket{};
    struct io_uring uring {};
    bool has_send_zc{};

    std::atomic<std::size_t> active_connections{};
    std::atomic<std::size_t> reserved_connections{};
    std::atomic<std::size_t> dismissed_connections{};
    std::uint32_t max_lifetime_micro_seconds{};
    std::uint32_t max_lifetime_exchanges{};

    mutex_t submission_mutex{};
    mutex_t completion_mutex{};
    mutex_t connections_mutex{};

    stats_t stats{};
    connection_t stats_pseudo_connection{};
    std::int32_t logs_file_descriptor{};
    std::string_view logs_format{};

    /// @brief An array of function callbacks. Can be in dozens.
    array_gt<named_callback_t> callbacks{};
    /// @brief A circular container of reusable connections. Can be in millions.
    pool_gt<connection_t> connections{};
    /// @brief Same number of them, as max physical threads. Can be in hundreds.
    buffer_gt<scratch_space_t> spaces{};
    /// @brief Pre-allocated buffered to be submitted to `io_uring` for shared use.
    memory_map_t fixed_buffers{};

    bool consider_accepting_new_connection() noexcept;
    void submit_stats_heartbeat() noexcept;
    void release_connection(connection_t&) noexcept;
    void log_and_reset_stats() noexcept;

    template <std::size_t max_count_ak> std::size_t pop_completed(completed_event_t*) noexcept;
};

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

    int uring_result{};
    struct io_uring_sqe* uring_sqe{};
    connection_t& connection = *con_ptr;
    connection.stage = stage_t::waiting_to_accept_k;
    submission_mutex.lock();

    uring_sqe = io_uring_get_sqe(&uring);
    io_uring_prep_accept_direct(uring_sqe, socket, &connection.client_address, &connection.client_address_len, 0,
                                IORING_FILE_INDEX_ALLOC);
    io_uring_sqe_set_data(uring_sqe, &connection);

    // Accepting new connections can be time-less.
    // io_uring_sqe_set_flags(uring_sqe, IOSQE_IO_LINK);
    // uring_sqe = io_uring_get_sqe(&uring);
    // io_uring_prep_link_timeout(uring_sqe, &connection.next_wakeup, 0);
    // io_uring_sqe_set_data(uring_sqe, NULL);

    uring_result = io_uring_submit(&uring);
    submission_mutex.unlock();
    if (uring_result < 0) {
        connections.release(con_ptr);
        reserved_connections--;
        return false;
    }

    dismissed_connections = 0;
    return true;
}

void engine_t::submit_stats_heartbeat() noexcept {
    int uring_result{};
    struct io_uring_sqe* uring_sqe{};
    connection_t& connection = stats_pseudo_connection;
    connection.stage = stage_t::log_stats_k;
    connection.next_wakeup.tv_sec = stats_t::default_frequency_secs_k;
    submission_mutex.lock();

    uring_sqe = io_uring_get_sqe(&uring);
    io_uring_prep_timeout(uring_sqe, &connection.next_wakeup, 0, 0);
    io_uring_sqe_set_data(uring_sqe, &connection);
    uring_result = io_uring_submit(&uring);
    submission_mutex.unlock();
}

void engine_t::log_and_reset_stats() noexcept {
    static char printed_message_k[ram_page_size_k]{};
    auto len = logs_format == "json" //
                   ? stats.log_json(printed_message_k, ram_page_size_k)
                   : stats.log_human_readable(printed_message_k, ram_page_size_k, stats_t::default_frequency_secs_k);
    len = write(logs_file_descriptor, printed_message_k, len);
}

void engine_t::release_connection(connection_t& connection) noexcept {
    auto is_active = connection.stage != stage_t::waiting_to_accept_k;
    connection.reset();
    connections_mutex.lock();
    connections.release(&connection);
    connections_mutex.unlock();
    active_connections -= is_active;
    stats.closed_connections.fetch_add(is_active, std::memory_order_relaxed);
}

template <std::size_t max_count_ak> std::size_t engine_t::pop_completed(completed_event_t* events) noexcept {

    unsigned uring_head{};
    unsigned completed{};
    unsigned passed{};
    struct io_uring_cqe* uring_cqe{};

    completion_mutex.lock();
    io_uring_for_each_cqe(&uring, uring_head, uring_cqe) {
        ++passed;
        if (!uring_cqe->user_data)
            continue;
        events[completed].connection_ptr = (connection_t*)uring_cqe->user_data;
        events[completed].stage = events[completed].connection_ptr->stage;
        events[completed].result = uring_cqe->res;
        ++completed;
        if (completed == max_count_ak)
            break;
    }

    io_uring_cq_advance(&uring, passed);
    completion_mutex.unlock();
    return completed;
}

} // namespace unum::ucall

void ucall_add_procedure(ucall_server_t server, ucall_str_t name, ucall_callback_t callback,
                         ucall_callback_tag_t callback_tag) {
    unum::ucall::engine_t& engine = *reinterpret_cast<unum::ucall::engine_t*>(server);
    if (engine.callbacks.size() + 1 < engine.callbacks.capacity())
        engine.callbacks.push_back_reserved({name, callback, callback_tag});
}