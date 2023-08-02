#pragma once

#include "globals.hpp"

#if defined(UCALL_IS_WINDOWS)
#include <Windows.h>

#else
#include <sys/mman.h>

#endif

#include <atomic>
#include <cerrno>
#include <cstring>
#include <memory>
#include <string_view> // `std::string_view`

#if defined(__x86_64__)
#ifdef _MSC_VER
#include <intrin.h>
#else
#include <x86intrin.h>
#endif
#endif

#include "ucall/ucall.h" // `ucall_callback_t`

namespace unum::ucall {

struct default_error_t {
    int code{};
    std::string_view note;
};

inline timestamp_t cpu_cycle() noexcept {
    timestamp_t result;
#ifdef __aarch64__
    /*
     * According to ARM DDI 0487F.c, from Armv8.0 to Armv8.5 inclusive, the
     * system counter is at least 56 bits wide; from Armv8.6, the counter
     * must be 64 bits wide.  So the system counter could be less than 64
     * bits wide and it is attributed with the flag 'cap_user_time_short'
     * is true.
     */
    asm volatile("mrs %0, cntvct_el0" : "=r"(result));
#else
    result = __rdtsc();
#endif
    return result;
}

inline std::size_t string_length(char const* c_str, std::size_t optional_length) noexcept {
    return c_str && !optional_length ? std::strlen(c_str) : optional_length;
}

/**
 * @brief Rounds integer to the next multiple of a given number. Is needed for aligned memory allocations.
 */
template <std::size_t step_ak> constexpr std::size_t round_up_to(std::size_t n) noexcept {
    return ((n + step_ak - 1) / step_ak) * step_ak;
}

struct parsed_request_t {
    std::string_view type{};
    std::string_view keep_alive{};
    std::string_view content_type{};
    std::string_view content_length{};
    std::string_view body{};
};

enum class stage_t {
    waiting_to_accept_k = 0,
    expecting_reception_k,
    responding_in_progress_k,
    waiting_to_close_k,
    log_stats_k,
    unknown_k,
};

class alignas(align_k) mutex_t {
    std::atomic<bool> flag{false};

  public:
    void lock() noexcept {
        while (flag.exchange(true, std::memory_order_relaxed))
            ;
        std::atomic_thread_fence(std::memory_order_acquire);
    }

    void unlock() noexcept {
        std::atomic_thread_fence(std::memory_order_release);
        flag.store(false, std::memory_order_relaxed);
    }
};

struct memory_map_t {
    char* ptr{};
    std::size_t length{};

    memory_map_t() = default;
    memory_map_t(memory_map_t const&) = delete;
    memory_map_t& operator=(memory_map_t const&) = delete;

    memory_map_t(memory_map_t&& other) noexcept {
        std::swap(ptr, other.ptr);
        std::swap(length, other.length);
    }

    memory_map_t& operator=(memory_map_t&& other) noexcept {
        std::swap(ptr, other.ptr);
        std::swap(length, other.length);
        return *this;
    }

    bool reserve(std::size_t length) noexcept {
#if defined(UCALL_IS_WINDOWS)
        HANDLE hFile = INVALID_HANDLE_VALUE;
        HANDLE hMap = CreateFileMapping(hFile, nullptr, PAGE_READWRITE, 0, length, nullptr);
        if (hMap == nullptr)
            return false;

        char* new_ptr = (char*)MapViewOfFile(hMap, FILE_MAP_ALL_ACCESS, 0, 0, length); // Map a view of the file
        if (new_ptr == nullptr) {
            CloseHandle(hMap);
            return false;
        }
#else
        // Use mmap for Linux and other POSIX systems
        // Make sure that the `length` is a multiple of `page_size`
        // auto page_size = sysconf(_SC_PAGE_SIZE);
        char* new_ptr = (char*)mmap(ptr, length, PROT_WRITE | PROT_READ, MAP_ANONYMOUS | MAP_PRIVATE, -1, 0);
        if (new_ptr == MAP_FAILED) {
            errno;
            return false;
        }
#endif
        std::memset(new_ptr, 0, length);
        ptr = new_ptr;
        return true;
    }

    ~memory_map_t() noexcept {
#if defined(UCALL_IS_WINDOWS)
        if (ptr)
            UnmapViewOfFile(ptr); // Unmap the view of the file
#else
        if (ptr)
            munmap(ptr, length);
#endif
        ptr = nullptr;
        length = 0;
    }
};

} // namespace unum::ucall
