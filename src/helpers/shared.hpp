#pragma once
#include <sys/uio.h> // `struct iovec`

#include <numeric>     // `std::iota`
#include <string_view> // `std::string_view`

#ifdef _MSC_VER
#include <intrin.h>
#else
#include <x86intrin.h>
#endif

#include "ujrpc/ujrpc.h" // `ujrpc_callback_t`

namespace unum::ujrpc {

/// @brief To avoid dynamic memory allocations on tiny requests,
/// for every connection we keep a tiny embedded buffer of this capacity.
static constexpr std::size_t max_packet_size_k = 4096;
/// @brief The maximum length of JSON-Pointer, we will use
/// to lookup parameters in heavily nested requests.
/// A performance-oriented API will have maximum depth of 1 token.
/// Some may go as far as 5 token, or roughly 50 character.
static constexpr std::size_t json_pointer_capacity_k = 256;
/// @brief Needed for largest-register-aligned memory addressing.
static constexpr std::size_t align_k = 64;
/// @brief Accessing real time from user-space is very expensive.
/// To approximate we can use CPU cycle counters.
constexpr std::size_t cpu_cycles_per_micro_second_k = 3'000;

enum descriptor_t : int {};
static constexpr descriptor_t bad_descriptor_k{-1};

struct named_callback_t {
    ujrpc_str_t name{};
    ujrpc_callback_t callback{};
};

template <typename element_at> class buffer_gt {
    element_at* elements_{};
    std::size_t capacity_{};
    static_assert(std::is_nothrow_default_constructible<element_at>());

  public:
    buffer_gt() = default;
    buffer_gt(buffer_gt&&) = delete;
    buffer_gt(buffer_gt const&) = delete;
    buffer_gt& operator=(buffer_gt const&) = delete;

    buffer_gt& operator=(buffer_gt&& other) noexcept {
        std::swap(elements_, other.elements_);
        std::swap(capacity_, other.capacity_);
        return *this;
    }
    bool reserve(std::size_t n) noexcept {
        elements_ = (element_at*)std::malloc(sizeof(element_at) * n);
        if (!elements_)
            return false;
        std::uninitialized_default_construct(elements_, elements_ + capacity_);
        capacity_ = n;
        return true;
    }
    ~buffer_gt() noexcept {
        if constexpr (!std::is_trivially_destructible<element_at>())
            std::destroy_n(elements_, capacity_);
        std::free(elements_);
    }
    element_at const* data() const noexcept { return elements_; }
    element_at* data() noexcept { return elements_; }
    element_at* begin() noexcept { return elements_; }
    element_at* end() noexcept { return elements_ + capacity_; }
    std::size_t size() const noexcept { return capacity_; }
    std::size_t capacity() const noexcept { return capacity_; }
    element_at& operator[](std::size_t i) noexcept { return elements_[i]; }
    element_at const& operator[](std::size_t i) const noexcept { return elements_[i]; }
};

template <typename element_at> class array_gt {
    element_at* elements_{};
    std::size_t count_{};
    std::size_t capacity_{};
    static_assert(std::is_nothrow_default_constructible<element_at>());

  public:
    array_gt() = default;
    array_gt(array_gt&&) = delete;
    array_gt(array_gt const&) = delete;
    array_gt& operator=(array_gt const&) = delete;

    array_gt& operator=(array_gt&& other) noexcept {
        std::swap(elements_, other.elements_);
        std::swap(count_, other.count_);
        std::swap(capacity_, other.capacity_);
        return *this;
    }
    bool reserve(std::size_t n) noexcept {
        elements_ = (element_at*)std::malloc(sizeof(element_at) * n);
        if (!elements_)
            return false;
        capacity_ = n;
        std::uninitialized_default_construct(elements_, elements_ + capacity_);
        return true;
    }
    ~array_gt() noexcept {
        if constexpr (!std::is_trivially_destructible<element_at>())
            std::destroy_n(elements_, count_);
        std::free(elements_);
    }
    element_at const* data() const noexcept { return elements_; }
    element_at* data() noexcept { return elements_; }
    element_at* begin() noexcept { return elements_; }
    element_at* end() noexcept { return elements_ + count_; }
    std::size_t size() const noexcept { return count_; }
    std::size_t capacity() const noexcept { return capacity_; }
    element_at& operator[](std::size_t i) noexcept { return elements_[i]; }
    void push_back(element_at&& element) noexcept { new (elements_ + count_++) element_at(element); }
};

template <typename element_at> class pool_gt {
    std::size_t capacity_{};
    std::size_t free_count_{};
    element_at* elements_{};
    std::size_t* free_offsets_{};
    static_assert(std::is_nothrow_default_constructible<element_at>());

  public:
    pool_gt() = default;
    pool_gt(pool_gt&&) = delete;
    pool_gt(pool_gt const&) = delete;
    pool_gt& operator=(pool_gt const&) = delete;

    pool_gt& operator=(pool_gt&& other) noexcept {
        std::swap(capacity_, other.capacity_);
        std::swap(free_count_, other.free_count_);
        std::swap(elements_, other.elements_);
        std::swap(free_offsets_, other.free_offsets_);
        return *this;
    }

    bool reserve(std::size_t n) noexcept {
        auto mem = std::malloc((sizeof(element_at) + sizeof(std::size_t)) * n);
        if (!mem)
            return false;
        elements_ = (element_at*)mem;
        free_offsets_ = (std::size_t*)(elements_ + n);
        free_count_ = capacity_ = n;
        std::uninitialized_default_construct(elements_, elements_ + capacity_);
        std::iota(free_offsets_, free_offsets_ + n, 0ul);
        return true;
    }

    ~pool_gt() noexcept {
        if constexpr (!std::is_trivially_destructible<element_at>())
            std::destroy_n(elements_, capacity_);
        std::free(elements_);
    }
    element_at* alloc() noexcept { return free_count_ ? elements_ + free_offsets_[--free_count_] : nullptr; }
    void release(element_at* released) noexcept { free_offsets_[free_count_++] = released - elements_; }
};

using timestamp_t = std::uint64_t;

inline timestamp_t cpu_cycle() noexcept {
    timestamp_t result;
#ifdef __aarch64__
    /*
     * According to ARM DDI 0487F.c, from Armv8.0 to Armv8.5 inclusive, the
     * system counter is at least 56 bits wide; from Armv8.6, the counter
     * must be 64 bits wide.  So the system counter could be less than 64
     * bits wide and it is attributed with the flag 'cap_user_time_short'
     * is true.
     */
    asm volatile("mrs %0, cntvct_el0" : "=r"(result));
#else
    result = __rdtsc();
#endif
    return result;
}

} // namespace unum::ujrpc
