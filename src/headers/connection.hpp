#pragma once

#include <sys/socket.h>

#include "contain/pipe.hpp"
#include "globals.hpp"
#include "parse/protocol/protocol.hpp"
#include "shared.hpp"

namespace unum::ucall {

struct connection_t {

    /// @brief Exchange buffers to pipe information in both directions.
    exchange_pipes_t pipes{};

    /// @brief The file descriptor of the stateful connection over TCP.
    descriptor_t descriptor{invalid_descriptor_k};
    /// @brief Current state at which the automata has arrived.
    stage_t stage{};
    protocol_t protocol{};

    struct sockaddr client_address {};
    socklen_t client_address_len{sizeof(struct sockaddr)};

    /// @brief Accumulated duration of sleep cycles.
    std::size_t sleep_ns{};
    std::size_t empty_transmits{};
    std::size_t exchanges{};

    /// @brief Relative time set for the last wake-up call.
    ssize_t next_wakeup = wakeup_initial_frequency_ns_k;

    connection_t() noexcept {}

    bool expired() const noexcept {
        if (sleep_ns > max_inactive_duration_ns_k)
            return true;

        return false;
    };
    void reset() noexcept {
        stage = stage_t::unknown_k;
        client_address = {};

        pipes.release_inputs();
        pipes.release_outputs();

        sleep_ns = 0;
        empty_transmits = 0;
        exchanges = 0;
        next_wakeup = wakeup_initial_frequency_ns_k;
    };
};

struct completed_event_t {
    connection_t* connection_ptr{};
    int result{};
};
} // namespace unum::ucall