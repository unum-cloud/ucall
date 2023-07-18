#pragma once
#include <cstddef>

namespace unum::ucall {

enum descriptor_t : int {};

/// @brief When preparing replies to requests, instead of allocating
/// a new tape and joining them together, we assemble the requests
/// `iovec`-s to pass to the kernel.
static constexpr std::size_t iovecs_for_content_k = 5;
static constexpr std::size_t iovecs_for_error_k = 7;
/// @brief JSON-RPC can be transmitted over HTTP, meaning that we
/// must return headers with the Status Code, Content Type, and,
/// most importantly, the Content Length, as well as some padding
/// afterwards. Expected response headers are:
/// https://stackoverflow.com/a/25586633/2766161
static constexpr std::size_t iovecs_for_http_response_k = 1;

static constexpr char const* http_header_k =
    "HTTP/1.1 200 OK\r\nContent-Length:          \r\nContent-Type: application/json\r\n\r\n";
static constexpr std::size_t http_header_size_k = 78;
static constexpr std::size_t http_header_length_offset_k = 33;
static constexpr std::size_t http_header_length_capacity_k = 9;

/// @brief To avoid dynamic memory allocations on tiny requests,
/// for every connection we keep a tiny embedded buffer of this capacity.
static constexpr std::size_t ram_page_size_k = 4096;
/// @brief  Expected max length of http headers
static constexpr std::size_t http_head_max_size_k = 1024;
/// @brief The maximum length of JSON-Pointer, we will use
/// to lookup parameters in heavily nested requests.
/// A performance-oriented API will have maximum depth of 1 token.
/// Some may go as far as 5 token, or roughly 50 character.
static constexpr std::size_t json_pointer_capacity_k = 256;
/// @brief Number of bytes in a printed integer.
/// Used either for error codes, or for request IDs.
static constexpr std::size_t max_integer_length_k = 32;
/// @brief Needed for largest-register-aligned memory addressing.
static constexpr std::size_t align_k = 64;
/// @brief Accessing real time from user-space is very expensive.
/// To approximate we can use CPU cycle counters.
constexpr std::size_t cpu_cycles_per_micro_second_k = 3'000;

// /// @brief As we use SIMDJSON, we don't want to fill our message buffers entirely.
// /// If there is a @b padding at the end, matching the size of the largest CPU register
// /// on the machine, we would avoid copies.
// static constexpr std::size_t max_embedded_length_k{ram_page_size_k - sj::SIMDJSON_PADDING};

static constexpr descriptor_t invalid_descriptor_k{-1};
static constexpr std::size_t max_inactive_duration_ns_k{100'000'000'000};
static constexpr std::size_t wakeup_initial_frequency_ns_k{3'000};
static constexpr std::size_t sleep_growth_factor_k{4};

static constexpr descriptor_t bad_descriptor_k{-1};

using timestamp_t = std::uint64_t;

} // namespace unum::ucall
