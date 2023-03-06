# UJRPC C SDK

C is the "Lingua franca" of modern computing, and we use it as our primary interface.
We wanted to make it as lightweight as possible and to allow optional arguments without dynamic allocations and named lookups.
So unlike the Python layer, we expect the user to manually extract the arguments from the call context with `ujrpc_param_named_i64()`, and its siblings.
The minimal sum example could look like this.

```c
#include <ujrpc/ujrpc.h>

static void sum(ujrpc_call_t call, ujrpc_callback_tag_t) {
    int64_t a{}, b{};
    char c_str[256]{};
    bool got_a = ujrpc_param_named_i64(call, "a", 0, &a);
    bool got_b = ujrpc_param_named_i64(call, "b", 0, &b);
    if (!got_a || !got_b)
        return ujrpc_call_reply_error_invalid_params(call);

    std::to_chars_result print = std::to_chars(&c_str[0], &c_str[0] + sizeof(c_str), a + b, 10);
    ujrpc_call_reply_content(call, &c_str[0], print.ptr - &c_str[0]);
}

int main(int argc, char** argv) {

    ujrpc_server_t server{};
    ujrpc_config_t config{};

    ujrpc_init(&config, &server);
    ujrpc_add_procedure(server, "sum", &sum, NULL);
    ujrpc_take_calls(server, 0);
    ujrpc_free(server);
    return 0;
}
```

Longer than Python version, but tiny by C standards, and super fast.
