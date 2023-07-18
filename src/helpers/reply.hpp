#pragma once

#if defined(__linux__) // iovec required only for uring
#define UCALL_IS_LINUX
#include <sys/uio.h> // `struct iovec`
#endif

#include <string_view> // `std::string_view`

#include "helpers/globals.hpp"

namespace unum::ucall {

#if defined(UCALL_IS_LINUX)

size_t fill_with_content(struct iovec* buffers, std::string_view request_id, std::string_view body,
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
    return buffers[0].iov_len + buffers[1].iov_len + buffers[2].iov_len + buffers[3].iov_len + buffers[4].iov_len;
}

size_t fill_with_error(struct iovec* buffers, std::string_view request_id, std::string_view error_code,
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
    return 22 + request_id.size() + 17 + error_code.size() + 12 + error_message.size() + 3 + append_comma;
}

#endif
} // namespace unum::ucall
