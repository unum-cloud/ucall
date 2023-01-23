#include "helpers/parse.hpp"

using namespace unum::ujrpc;

bool ujrpc_param_named_i64(ujrpc_call_t call, ujrpc_str_t name, int64_t* result_ptr) {
    connection_t& connection = *reinterpret_cast<connection_t*>(call);
    scratch_space_t& scratch = *(scratch_space_t*)connection.scratch;
    std::memcpy(scratch.json_pointer, "/params/", 8);
    std::memcpy(scratch.json_pointer + 8, name, std::strlen(name) + 1);
    auto value = scratch.tree.at_pointer(scratch.json_pointer);

    if (value.is_int64()) {
        *result_ptr = value.get_int64();
        return true;
    } else if (value.is_uint64()) {
        *result_ptr = static_cast<int64_t>(value.get_uint64());
        return true;
    } else
        return false;
}
