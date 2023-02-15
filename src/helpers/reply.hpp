#pragma once
#include <sys/uio.h> // `struct iovec`

#include <string_view> // `std::string_view`

namespace unum::ujrpc {

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
static constexpr std::size_t iovecs_for_http_headers_k = 3;

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

template <std::size_t iovecs_len_ak> std::size_t iovecs_length(struct iovec const* iovecs) noexcept {
    std::size_t added_length = 0;
#pragma unroll
    for (std::size_t i = 0; i != iovecs_len_ak; ++i)
        added_length += iovecs[i].iov_len;
    return added_length;
}

template <std::size_t iovecs_len_ak> void iovecs_memcpy(struct iovec const* iovecs, char* output) noexcept {
#pragma unroll
    for (std::size_t i = 0; i != iovecs_len_ak; ++i) {
        std::memcpy(output, iovecs[i].iov_base, iovecs[i].iov_len);
        output += iovecs[i].iov_len;
    }
}

} // namespace unum::ujrpc
