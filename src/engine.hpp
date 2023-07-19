#pragma once

#include <atomic>

#include "connection.hpp"
#include "helpers/contain/buffer.hpp"
#include "helpers/contain/pool.hpp"
#include "helpers/log.hpp"
#include "helpers/parse/json.hpp"
#include "helpers/shared.hpp"
#include "network.hpp"

namespace unum::ucall {

struct engine_t {
    descriptor_t socket{};
    network_engine_t network_engine{};
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
    /// @brief Pre-allocated buffered to be submitted for shared use.
    memory_map_t fixed_buffers{};

    bool consider_accepting_new_connection() noexcept; // TODO Maybe move this to network engine
    void submit_stats_heartbeat() noexcept;
    void release_connection(connection_t&) noexcept;
    void log_and_reset_stats() noexcept;
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

    connection_t& connection = *con_ptr;
    connection.stage = stage_t::waiting_to_accept_k;

    submission_mutex.lock();
    int result = try_accept(network_engine, socket, connection);
    submission_mutex.unlock();

    if (result < 0) {
        connections.release(con_ptr);
        reserved_connections--;
        return false;
    }

    dismissed_connections = 0;
    return true;
}

void engine_t::submit_stats_heartbeat() noexcept {
    connection_t& connection = stats_pseudo_connection;
    connection.stage = stage_t::log_stats_k;
    connection.next_wakeup.tv_sec = stats_t::default_frequency_secs_k;

    submission_mutex.lock();
    set_stats_heartbeat(network_engine, connection);
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

} // namespace unum::ucall

void ucall_add_procedure(ucall_server_t server, ucall_str_t name, ucall_callback_t callback,
                         ucall_callback_tag_t callback_tag) {
    unum::ucall::engine_t& engine = *reinterpret_cast<unum::ucall::engine_t*>(server);
    if (engine.callbacks.size() + 1 < engine.callbacks.capacity())
        engine.callbacks.push_back_reserved({name, callback, callback_tag});
}