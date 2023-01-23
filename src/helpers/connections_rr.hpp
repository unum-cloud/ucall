#pragma once
#include "shared.hpp"

namespace unum::ujrpc {

/**
 * @brief Round-Robin construct for reusable connection states.
 */
class connections_rr_t {

    connection_t* circle_{};
    std::size_t count_{};
    std::size_t capacity_{};
    std::size_t idx_newest_{};
    std::size_t idx_oldest_{};
    /// @brief Follows the tail (oldest), or races forward
    /// and cycles around all the active connections, if all
    /// of them are long-livers,
    std::size_t idx_to_poll_{};

  public:
    inline connections_rr_t& operator=(connections_rr_t&& other) noexcept {
        std::swap(circle_, other.circle_);
        std::swap(count_, other.count_);
        std::swap(capacity_, other.capacity_);
        std::swap(idx_newest_, other.idx_newest_);
        std::swap(idx_oldest_, other.idx_oldest_);
        std::swap(idx_to_poll_, other.idx_to_poll_);
        return *this;
    }

    inline bool alloc(std::size_t n) noexcept {

        auto cons = (connection_t*)std::malloc(sizeof(connection_t) * n);
        if (!cons)
            return false;
        circle_ = cons;
        capacity_ = n;
        return true;
    }

    inline descriptor_t drop_tail() noexcept {
        descriptor_t old = std::exchange(circle_[idx_oldest_].descriptor, bad_descriptor_k);
        idx_to_poll_ = idx_to_poll_ == idx_oldest_ ? (idx_to_poll_ + 1) % capacity_ : idx_to_poll_;
        idx_oldest_ = (idx_oldest_ + 1) % capacity_;
        count_--;
        return old;
    }

    inline void push_ahead(descriptor_t new_) noexcept {
        circle_[idx_newest_].descriptor = new_;
        circle_[idx_newest_].skipped_cycles = 0;
        circle_[idx_newest_].response.copies_count = 0;
        circle_[idx_newest_].response.iovecs_count = 0;
        idx_newest_ = (idx_newest_ + 1) % capacity_;
        count_++;
    }

    inline connection_t& poll() noexcept {
        auto connection_ptr = &circle_[idx_to_poll_];
        auto idx_to_poll_following = (idx_to_poll_ + 1) % count_;
        idx_to_poll_ = idx_to_poll_following == idx_newest_ ? idx_oldest_ : idx_to_poll_following;
        return circle_[idx_to_poll_];
    }

    inline connection_t& tail() noexcept { return circle_[idx_oldest_]; }
    inline connection_t& head() noexcept { return circle_[idx_newest_ - 1]; }
    inline std::size_t size() const noexcept { return count_; }
    inline std::size_t capacity() const noexcept { return capacity_; }
};

} // namespace unum::ujrpc