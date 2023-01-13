#pragma once
#include <stdint.h>

typedef int64_t ujrpc_i64_t;
typedef double ujrpc_f64_t;
typedef char const *ujrpc_str_t;

typedef void *ujrpc_server_t;

typedef void *ujrpc_call_context_t;

typedef void (*ujrpc_callback_t)(ujrpc_call_context_t *);

void ujrpc_init(ujrpc_server_t *);

void ujrpc_add_callback(ujrpc_server_t, ujrpc_str_t, ujrpc_callback_t);

void ujrpc_context_get_argument_i64(ujrpc_server_t, ujrpc_call_context_t, ujrpc_str_t, ujrpc_i64_t *);

void ujrpc_context_get_argument_f64(ujrpc_server_t, ujrpc_call_context_t, ujrpc_str_t, ujrpc_f64_t *);

void ujrpc_context_get_argument_str(ujrpc_server_t, ujrpc_call_context_t, ujrpc_str_t, ujrpc_str_t *);

void ujrpc_run_event_cycle(ujrpc_server_t);

void ujrpc_run_event_loop(ujrpc_server_t);

void ujrpc_free(ujrpc_server_t);