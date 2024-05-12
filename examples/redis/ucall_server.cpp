/**
 *  @brief  Example of building a Redis-like in-memory store with UCall.
 */
#include <cstdio>        // `std::printf`
#include <cstring>       // `std::memcpy`
#include <mutex>         // `std::unique_lock`
#include <new>           // `std::bad_alloc`
#include <shared_mutex>  // `std::shared_mutex`
#include <string>        // `std::string`
#include <string_view>   // `std::string_view`
#include <unordered_set> // `std::unordered_set`

#include <ucall/ucall.h>

struct key_value_pair {
    char* key_and_value = nullptr;
    std::size_t key_length = 0;
    std::size_t value_length = 0;

    std::string_view key() const noexcept { return std::string_view(key_and_value, key_length); }
    std::string_view value() const noexcept { return std::string_view(key_and_value + key_length, value_length); }

    key_value_pair() = default;
    key_value_pair(std::string_view key, std::string_view value) : key_length(key.size()), value_length(value.size()) {
        std::size_t total_length = key_length + value_length;
        key_and_value = reinterpret_cast<char*>(std::malloc(total_length));
        if (total_length && !key_and_value)
            throw std::bad_alloc();
        if (key_and_value)
            std::memcpy(key_and_value, key.data(), key_length),
                std::memcpy(key_and_value + key_length, value.data(), value_length);
        else
            key_length = value_length = 0;
    }
    key_value_pair(key_value_pair const& other) : key_length(other.key_length), value_length(other.value_length) {
        std::size_t total_length = key_length + value_length;
        key_and_value = reinterpret_cast<char*>(std::malloc(total_length));
        if (total_length && !key_and_value)
            throw std::bad_alloc();
        if (key_and_value)
            std::memcpy(key_and_value, other.key_and_value, total_length);
        else
            key_length = value_length = 0;
    }
    key_value_pair(key_value_pair&& other) noexcept
        : key_and_value(other.key_and_value), key_length(other.key_length), value_length(other.value_length) {
        other.key_and_value = nullptr;
        other.key_length = other.value_length = 0;
    }
    key_value_pair& operator=(key_value_pair const& other) {
        if (this == &other)
            return *this;

        std::free(key_and_value);
        key_length = other.key_length;
        value_length = other.value_length;
        key_and_value = reinterpret_cast<char*>(std::malloc(key_length + value_length));
        if (key_and_value)
            std::memcpy(key_and_value, other.key_and_value, key_length + value_length);
        else
            key_length = value_length = 0;

        return *this;
    }
    key_value_pair& operator=(key_value_pair&& other) noexcept {
        if (this == &other)
            return *this;

        std::free(key_and_value);
        key_and_value = other.key_and_value;
        key_length = other.key_length;
        value_length = other.value_length;
        other.key_and_value = nullptr;
        other.key_length = other.value_length = 0;

        return *this;
    }
};

struct key_hash {
    using is_transparent = void;
    std::size_t operator()(std::string_view const& key) const noexcept { return std::hash<std::string_view>{}(key); }
    std::size_t operator()(key_value_pair const& pair) const noexcept { return operator()(pair.key()); }
};

struct key_equal {
    using is_transparent = void;
    bool operator()(std::string_view const& lhs, std::string_view const& rhs) const noexcept { return lhs == rhs; }
    bool operator()(key_value_pair const& lhs, std::string_view const& rhs) const noexcept { return lhs.key() == rhs; }
    bool operator()(std::string_view const& lhs, key_value_pair const& rhs) const noexcept { return lhs == rhs.key(); }
};

static std::shared_mutex store_mutex;
static std::unordered_set<key_value_pair, key_hash, key_equal> store;

static void set(ucall_call_t call) {
    ucall_str_t key_ptr{};
    ucall_str_t value_ptr{};
    std::size_t key_len{};
    std::size_t value_len{};
    bool key_found = ucall_param_named_str(call, "key", 3, &key_ptr, &key_len);
    bool value_found = ucall_param_named_str(call, "value", 5, &value_ptr, &value_len);
    if (!key_found || !value_found)
        return ucall_call_reply_error_invalid_params(call);

    std::unique_lock lock(store_mutex);
    store.insert(key_value_pair(std::string_view(key_ptr, key_len), std::string_view(value_ptr, value_len)));
    return ucall_call_reply_content(call, "OK", 2);
}

static void get(ucall_call_t call) {
    ucall_str_t key_ptr{};
    std::size_t key_len{};
    bool key_found = ucall_param_named_str(call, "key", 4, &key_ptr, &key_len);
    if (!key_found)
        return ucall_call_reply_error_invalid_params(call);

    std::shared_lock lock(store_mutex);
    auto iterator = store.find(std::string_view(key_ptr, key_len));
    if (iterator == store.end())
        return ucall_call_reply_content(call, "", 0);
    else
        return ucall_call_reply_content(call, iterator->value().c_str(), iterator->value().size());
}

int main(int argc, char** argv) {
    ucall_server_t server{};
    ucall_config_t config{};
    config.port = 6379;
    ucall_init(&config, &server);
    if (!server) {
        std::printf("Failed to initialize server!\n");
        return -1;
    }

    std::printf("Initialized server!\n");
    ucall_add_procedure(server, "set", &set);
    ucall_add_procedure(server, "get", &get);

    ucall_take_calls(server, 0);
    ucall_free(server);
    return 0;
}