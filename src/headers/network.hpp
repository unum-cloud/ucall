#pragma once

#include <cstddef>

#include "connection.hpp"

struct network_engine_t {
    unum::ucall::network_data_t network_data;

    int try_accept(unum::ucall::descriptor_t, unum::ucall::connection_t&);
    void set_stats_heartbeat(unum::ucall::connection_t&);
    void send_packet(unum::ucall::connection_t&, void*, std::size_t, std::size_t);
    void recv_packet(unum::ucall::connection_t&, void*, std::size_t, std::size_t);
    void close_connection_gracefully(unum::ucall::connection_t&);
    std::size_t pop_completed_events(unum::ucall::completed_event_t*);
};