#pragma once
#include <sys/uio.h> // `struct iovec`

#include <string_view> // `std::string_view`

#include "ujrpc/ujrpc.h" // `ujrpc_callback_t`

namespace unum::ujrpc {

/// @brief To avoid dynamic memory allocations on tiny requests,
/// for every connection we keep a tiny embedded buffer of this capacity.
static constexpr std::size_t embedded_packet_capacity_k = 4096;
/// @brief The maximum length of JSON-Pointer, we will use
/// to lookup parameters in heavily nested requests.
/// A performance-oriented API will have maximum depth of 1 token.
/// Some may go as far as 5 token, or roughly 50 character.
static constexpr std::size_t json_pointer_capacity_k = 256;
/// @brief Assuming we have a static 4KB `embedded_packet_capacity_k`
/// for our messages, we may receive an entirely invalid request like:
///     [0,0,0,0,...]
/// It will be recognized as a batch request with up to 2048 unique
/// requests, and each will be replied with an error message.
static constexpr std::size_t embedded_batch_capacity_k = 2048;
/// @brief When preparing replies to requests, instead of allocating
/// a new tape and joining them together, we assemble the requests
/// `iovec`-s to pass to the kernel.
static constexpr std::size_t iovecs_for_content_k = 5;
static constexpr std::size_t iovecs_for_error_k = 7;
/// @brief Needed for largest-register-aligned memory addressing.
static constexpr std::size_t align_k = 64;

enum descriptor_t : int {};
static constexpr descriptor_t bad_descriptor_k{-1};

enum class stage_t {
    pre_accept_k = 0,
    pre_receive_k,
    pre_completion_k,
};

struct named_callback_t {
    ujrpc_str_t name{};
    ujrpc_callback_t callback{};
};

struct connection_t {
    /// @brief The file descriptor of the statefull connection over TCP.
    descriptor_t descriptor{};
    /// @brief The step of an asynchronous machine.
    stage_t stage{};
    /// @brief Determines the probability of reseting the connection, in favor of a new client.
    std::size_t skipped_cycles{};
    /// @brief Pointer to the scratch memory to be used to parse this request.
    void* scratch_space{};

    struct response_t {
        struct iovec* iovecs{};
        char** copies{};
        std::size_t iovecs_count{};
        std::size_t copies_count{};
    } response{};
};

void fill_with_content(struct iovec* buffers, std::string_view request_id, std::string_view body,
                       bool append_comma = false) {

    // Communication example would be:
    // --> {"jsonrpc": "2.0", "method": "subtract", "params": [42, 23], "id": 1}
    // <-- {"jsonrpc": "2.0", "id": 1, "result": 19}
    char const* response_protocol_prefix_k = R"({"jsonrpc":"2.0","id":)";
    buffers[0].iov_base = (char*)response_protocol_prefix_k;
    buffers[0].iov_len = 22;
    buffers[1].iov_base = (char*)request_id.data();
    buffers[1].iov_len = request_id.size();
    char const* result_separator = R"(,"result":)";
    buffers[2].iov_base = (char*)result_separator;
    buffers[2].iov_len = 10;
    buffers[3].iov_base = (char*)body.data();
    buffers[3].iov_len = body.size();
    char const* protocol_suffix = R"(},)";
    buffers[4].iov_base = (char*)protocol_suffix;
    buffers[4].iov_len = 1 + append_comma;
}

void fill_with_error(struct iovec* buffers, std::string_view request_id, std::string_view error_code,
                     std::string_view error_message, bool append_comma = false) {

    // Communication example would be:
    // --> {"jsonrpc": "2.0", "method": "foobar", "id": "1"}
    // <-- {"jsonrpc": "2.0", "id": "1", "error": {"code": -32601, "message": "Method not found"}}
    char const* response_protocol_prefix_k = R"({"jsonrpc":"2.0","id":)";
    buffers[0].iov_base = (char*)response_protocol_prefix_k;
    buffers[0].iov_len = 22;
    buffers[1].iov_base = (char*)request_id.data();
    buffers[1].iov_len = request_id.size();
    char const* error_code_separator = R"(,"error":{"code":)";
    buffers[2].iov_base = (char*)error_code_separator;
    buffers[2].iov_len = 17;
    buffers[3].iov_base = (char*)error_code.data();
    buffers[3].iov_len = error_code.size();
    char const* error_message_separator = R"(,"message":")";
    buffers[4].iov_base = (char*)error_message_separator;
    buffers[4].iov_len = 12;
    buffers[5].iov_base = (char*)error_message.data();
    buffers[5].iov_len = error_message.size();
    char const* protocol_suffix = R"("}},)";
    buffers[6].iov_base = (char*)protocol_suffix;
    buffers[6].iov_len = 3 + append_comma;
}

template <std::size_t multiple_ak> constexpr std::size_t round_up_to(std::size_t n) {
    return ((n + multiple_ak - 1) / multiple_ak) * multiple_ak;
}

template <typename element_at> struct buffer_gt {
    element_at* elements_{};
    std::size_t capacity_{};
    static_assert(std::is_nothrow_default_constructible<element_at>());

    buffer_gt& operator=(buffer_gt&& other) noexcept {
        std::swap(elements_, other.elements_);
        std::swap(capacity_, other.capacity_);
        return *this;
    }
    bool alloc(std::size_t n) noexcept {
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
    element_at* data() noexcept { return elements_; }
    element_at* begin() noexcept { return elements_; }
    element_at* end() noexcept { return elements_ + capacity_; }
    std::size_t size() const noexcept { return capacity_; }
    std::size_t capacity() const noexcept { return capacity_; }
    element_at& operator[](std::size_t i) noexcept { return elements_[i]; }
};

template <typename element_at> struct array_gt {
    element_at* elements_{};
    std::size_t count_{};
    std::size_t capacity_{};

    array_gt& operator=(array_gt&& other) noexcept {
        std::swap(elements_, other.elements_);
        std::swap(count_, other.count_);
        std::swap(capacity_, other.capacity_);
        return *this;
    }
    bool alloc(std::size_t n) noexcept {
        elements_ = (element_at*)std::malloc(sizeof(element_at) * n);
        capacity_ = elements_ ? n : 0;
        return elements_;
    }
    ~array_gt() noexcept {
        if constexpr (!std::is_trivially_destructible<element_at>())
            std::destroy_n(elements_, count_);
        std::free(elements_);
    }
    element_at* data() noexcept { return elements_; }
    element_at* begin() noexcept { return elements_; }
    element_at* end() noexcept { return elements_ + count_; }
    std::size_t size() const noexcept { return count_; }
    std::size_t capacity() const noexcept { return capacity_; }
    element_at& operator[](std::size_t i) noexcept { return elements_[i]; }
    void push_back(element_at&& element) noexcept { new (elements_ + count_++) element_at(element); }
};

template <typename element_at> struct pool_gt {
    element_at* elements_{};
    std::size_t capacity_{};

    pool_gt& operator=(pool_gt&& other) noexcept {
        std::swap(elements_, other.elements_);
        std::swap(capacity_, other.capacity_);
        return *this;
    }
    bool alloc(std::size_t n) noexcept { return (elements_ = (element_at*)std::malloc(sizeof(element_at) * n)); }
    ~pool_gt() noexcept { std::free(elements_); }
    element_at& operator[](std::size_t i) noexcept { return elements_[i]; }
};

} // namespace unum::ujrpc
