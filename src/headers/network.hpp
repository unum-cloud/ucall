#pragma once

#include <cstddef>

#include "connection.hpp"

namespace unum::ucall {
struct network_engine_t {
    network_data_t network_data;

    int try_accept(descriptor_t, connection_t&);
    void set_stats_heartbeat(connection_t&);
    void send_packet(connection_t&, void*, std::size_t, std::size_t);
    void recv_packet(connection_t&, void*, std::size_t, std::size_t);
    void close_connection_gracefully(connection_t&);

    bool is_canceled(ssize_t, connection_t const&);
    bool is_corrupted(ssize_t, connection_t const&);

    template <size_t max_count_ak> std::size_t pop_completed_events(completed_event_t*);
};
} // namespace unum::ucall