/**
 * @file ujrpc.h
 * @author Ashot Vardanian
 * @date Feb 3, 2023
 * @addtogroup C
 *
 * @brief Binary Interface for Uninterrupted JSON RPC.
 *
 * ## Basic Usage
 *
 * To use UJRPC, at the very least you need the following few functions:
 *
 * - `ujrpc_init()` - to start a server,
 * - `ujrpc_add_procedure()` - to register a callback,
 * - `ujrpc_call_reply_content()` - to submit a reply from inside the callback,
 * - `ujrpc_take_calls()` - to run the polling loop,
 * - `ujrpc_free()` - to shut down.
 *
 * Assuming the C language has no reflection, and we want to avoid dynamic
 * memory allocations, there are a few functions to fetch the function arguments
 * from inside the callback. For named arguments those are:
 *
 * - `ujrpc_param_named_bool()`.
 * - `ujrpc_param_named_i64()`.
 * - `ujrpc_param_named_f64()`.
 * - `ujrpc_param_named_str()`.
 *
 * Similarly there is a `positional` counterpart to every `named` function.
 */

#pragma once

#ifdef __cplusplus
extern "C" {
#endif

#include <stdbool.h> // `bool`
#include <stddef.h>  // `size_t`
#include <stdint.h>  // `int64_t`

typedef void* ujrpc_server_t;
typedef void* ujrpc_call_t;
typedef void* ujrpc_callback_tag_t;
typedef char const* ujrpc_str_t;

typedef void (*ujrpc_callback_t)(ujrpc_call_t, ujrpc_callback_tag_t);

/**
 * @brief Configuration parameters for `ujrpc_init()`.
 */
typedef struct ujrpc_config_t {
    char const* interface;
    uint16_t port;
    uint16_t queue_depth;
    uint16_t max_callbacks;
    uint16_t max_threads;

    /// @brief Common choices, aside from a TCP socket are:
    /// > STDOUT_FILENO: console output.
    /// > STDERR_FILENO: errors.
    int32_t logs_file_descriptor;
    /// @brief Can be:
    /// > "human" will print human-readable unit-normalized lines.
    /// > "json" will output newline-delimited JSONs documents.
    char const* logs_format;

    uint16_t max_batch_size;
    uint32_t max_concurrent_connections;
    uint32_t max_lifetime_micro_seconds;
    uint32_t max_lifetime_exchanges;

    /// @brief Enable SSL.
    bool use_ssl;
    /// @brief Private Key required for SSL.
    const char* ssl_pk_path;
    /// @brief At least one certificate is required for SSL.
    const char** ssl_crts_path;
    /// @brief Certificates count.
    size_t ssl_crts_cnt;
} ujrpc_config_t;

/**
 * @param config Input and output argument, that will be updated to export set configuration.
 * @param server Output variable, which, on success, will be an initialized server.
 * Don't forget to free its memory with `ujrpc_free()` at the end.
 */
void ujrpc_init(ujrpc_config_t* config, ujrpc_server_t* server);

void ujrpc_free(ujrpc_server_t);

/**
 * @brief Registers a function callback to be triggered by the server,
 * when a matching request arrives.
 *
 * @param server Must be pre-initialized with `ujrpc_init()`.
 * @param name The string to be matched against "method" in every JSON request.
 * @param callback Function pointer to the callback.
 * @param callback_tag Optional payload/tag, often pointing to metadata about
 * expected "params", mostly used for higher-level runtimes, like CPython.
 */
void ujrpc_add_procedure(      //
    ujrpc_server_t server,     //
    ujrpc_str_t name,          //
    ujrpc_callback_t callback, //
    ujrpc_callback_tag_t callback_tag);

/**
 * @brief Perform a single blocking round of polling on the current calling thread.
 *
 * @param thread_idx Assuming that the `::server` itself has memory reserves for every
 * thread, the caller must provide a `::thread_idx` uniquely identifying current thread
 * with a number from zero to `::ujrpc_config_t::max_threads`.
 */
void ujrpc_take_call(ujrpc_server_t server, uint16_t thread_idx);

/**
 * @brief Blocks current thread, replying to requests in a potentially more efficient
 * way, than just a `while` loop calling `ujrpc_take_call()`.
 *
 * @param thread_idx Assuming that the `::server` itself has memory reserves for every
 * thread, the caller must provide a `::thread_idx` uniquely identifying current thread
 * with a number from zero to `::ujrpc_config_t::max_threads`.
 */
void ujrpc_take_calls(ujrpc_server_t server, uint16_t thread_idx);

bool ujrpc_param_named_bool(    //
    ujrpc_call_t call,          //
    ujrpc_str_t json_pointer,   //
    size_t json_pointer_length, //
    bool* output);

bool ujrpc_param_named_i64(     //
    ujrpc_call_t call,          //
    ujrpc_str_t json_pointer,   //
    size_t json_pointer_length, //
    int64_t* output);

bool ujrpc_param_named_f64(     //
    ujrpc_call_t call,          //
    ujrpc_str_t json_pointer,   //
    size_t json_pointer_length, //
    double* output);

bool ujrpc_param_named_str(     //
    ujrpc_call_t call,          //
    ujrpc_str_t json_pointer,   //
    size_t json_pointer_length, //
    ujrpc_str_t* output, size_t* output_length);

bool ujrpc_param_positional_bool(ujrpc_call_t, size_t, bool*);
bool ujrpc_param_positional_i64(ujrpc_call_t, size_t, int64_t*);
bool ujrpc_param_positional_f64(ujrpc_call_t, size_t, double*);
bool ujrpc_param_positional_str(ujrpc_call_t, size_t, ujrpc_str_t*, size_t*);

/**
 * @param call Encapsulates the context and the arguments of the current request.
 * @param json_reply The response to send, which must be a valid JSON string.
 * @param json_reply_length An option length of `::json_reply`.
 */
void ujrpc_call_reply_content(ujrpc_call_t call, ujrpc_str_t json_reply, size_t json_reply_length);

/**
 * @param call Encapsulates the context and the arguments of the current request.
 * @param error_message An optional string.
 * @param error_message_length An option length of `::json_reply`.
 */
void ujrpc_call_reply_error(ujrpc_call_t call, int error_code, ujrpc_str_t error_message, size_t error_message_length);

void ujrpc_call_reply_error_invalid_params(ujrpc_call_t);
void ujrpc_call_reply_error_out_of_memory(ujrpc_call_t);
void ujrpc_call_reply_error_unknown(ujrpc_call_t);

bool ujrpc_param_named_json(ujrpc_call_t, ujrpc_str_t, size_t, ujrpc_str_t*, size_t*); // TODO
bool ujrpc_param_positional_json(ujrpc_call_t, size_t, ujrpc_str_t*, size_t*);         // TODO

#ifdef __cplusplus
} /* end extern "C" */
#endif