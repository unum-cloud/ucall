#pragma once

#include "ucall/ucall.h"

#include "automata.hpp"
#include "containers.hpp"
#include "network.hpp"
#include "server.hpp"

#pragma region C Interface Implementation

void ucall_call_reply_content(ucall_call_t call, ucall_str_t body, size_t body_len) {
    unum::ucall::automata_t& automata = *reinterpret_cast<unum::ucall::automata_t*>(call);
    unum::ucall::connection_t& connection = automata.connection;
    unum::ucall::scratch_space_t& scratch = automata.scratch;
    // No response is needed for "id"-less notifications.
    if (scratch.dynamic_id.empty())
        return;

    body_len = unum::ucall::string_length(body, body_len);
    connection.protocol.append_response(connection.pipes, scratch.dynamic_id, std::string_view(body, body_len));
}

void ucall_call_reply_error(ucall_call_t call, int code_int, ucall_str_t note, size_t note_len) {
    unum::ucall::automata_t& automata = *reinterpret_cast<unum::ucall::automata_t*>(call);
    unum::ucall::connection_t& connection = automata.connection;
    unum::ucall::scratch_space_t& scratch = automata.scratch;
    // No response is needed for "id"-less notifications.
    if (scratch.dynamic_id.empty())
        return;

    note_len = unum::ucall::string_length(note, note_len);
    char code[unum::ucall::max_integer_length_k]{};
    std::to_chars_result res = std::to_chars(code, code + unum::ucall::max_integer_length_k, code_int);
    auto code_len = res.ptr - code;
    if (res.ec != std::error_code())
        return ucall_call_reply_error_unknown(call);

    automata.connection.protocol.prepare_response(connection.pipes);
    if (!connection.protocol.append_error(connection.pipes, scratch.dynamic_id, std::string_view(code, code_len),
                                          std::string_view(note, note_len)))
        return ucall_call_reply_error_out_of_memory(call);
    automata.connection.protocol.finalize_response(connection.pipes);
}

void ucall_call_reply_error_invalid_params(ucall_call_t call) {
    return ucall_call_reply_error(call, -32602, "Invalid method param(s).", 24);
}

void ucall_call_reply_error_unknown(ucall_call_t call) {
    return ucall_call_reply_error(call, -32603, "Unknown error.", 14);
}

void ucall_call_reply_error_out_of_memory(ucall_call_t call) {
    return ucall_call_reply_error(call, -32000, "Out of memory.", 14);
}

bool ucall_param_named_bool(ucall_call_t call, ucall_str_t name, size_t name_len, bool* result_ptr) {
    if (auto value = unum::ucall::param_at(call, name, name_len); value.is_bool()) {
        *result_ptr = value.get_bool().value_unsafe();
        return true;
    } else
        return false;
}

bool ucall_param_named_i64(ucall_call_t call, ucall_str_t name, size_t name_len, int64_t* result_ptr) {
    if (auto value = unum::ucall::param_at(call, name, name_len); value.is_int64()) {
        *result_ptr = value.get_int64().value_unsafe();
        return true;
    } else
        return false;
}

bool ucall_param_named_f64(ucall_call_t call, ucall_str_t name, size_t name_len, double* result_ptr) {
    if (auto value = unum::ucall::param_at(call, name, name_len); value.is_double()) {
        *result_ptr = value.get_double().value_unsafe();
        return true;
    } else
        return false;
}

bool ucall_param_named_str(ucall_call_t call, ucall_str_t name, size_t name_len, ucall_str_t* result_ptr,
                           size_t* result_len_ptr) {
    if (auto value = unum::ucall::param_at(call, name, name_len); value.is_string()) {
        *result_ptr = value.get_string().value_unsafe().data();
        *result_len_ptr = value.get_string_length().value_unsafe();
        return true;
    } else
        return false;
}

bool ucall_param_positional_bool(ucall_call_t call, size_t position, bool* result_ptr) {
    if (auto value = unum::ucall::param_at(call, position); value.is_bool()) {
        *result_ptr = value.get_bool().value_unsafe();
        return true;
    } else
        return false;
}

bool ucall_param_positional_i64(ucall_call_t call, size_t position, int64_t* result_ptr) {
    if (auto value = unum::ucall::param_at(call, position); value.is_int64()) {
        *result_ptr = value.get_int64().value_unsafe();
        return true;
    } else
        return false;
}

bool ucall_param_positional_f64(ucall_call_t call, size_t position, double* result_ptr) {
    if (auto value = unum::ucall::param_at(call, position); value.is_double()) {
        *result_ptr = value.get_double().value_unsafe();
        return true;
    } else
        return false;
}

bool ucall_param_positional_str(ucall_call_t call, size_t position, ucall_str_t* result_ptr, size_t* result_len_ptr) {
    if (auto value = unum::ucall::param_at(call, position); value.is_string()) {
        *result_ptr = value.get_string().value_unsafe().data();
        *result_len_ptr = value.get_string_length().value_unsafe();
        return true;
    } else
        return false;
}

#pragma endregion