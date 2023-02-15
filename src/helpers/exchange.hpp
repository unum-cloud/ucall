#pragma once

#include "reply.hpp"  // `iovecs_length`
#include "shared.hpp" // `array_gt`

namespace unum::ujrpc {

struct exchange_pipe_t {
    char* embedded{};
    std::size_t embedded_used{};
    array_gt<char> dynamic{};

    span_gt<char> span() noexcept {
        return dynamic.size() ? span_gt<char>{dynamic.begin(), dynamic.end()}
                              : span_gt<char>{embedded, embedded + embedded_used};
    }
};

class exchange_pipes_t {
    /// @brief A combination of a embedded and dynamic memory pools for content reception.
    /// We always absorb new packets in the embedded part, later moving them into dynamic memory,
    /// if any more data is expected. Requires @b padding at the end, to accelerate parsing.
    exchange_pipe_t input_{};
    /// @brief A combination of a embedded and dynamic memory pools for content reception.
    exchange_pipe_t output_{};
    std::size_t output_submitted_{};

  public:
    exchange_pipes_t() noexcept = default;
    exchange_pipes_t(exchange_pipes_t&&) = delete;
    exchange_pipes_t(exchange_pipes_t const&) = delete;
    exchange_pipes_t& operator=(exchange_pipes_t&&) = delete;
    exchange_pipes_t& operator=(exchange_pipes_t const&) = delete;

    void mount(char* inputs, char* outputs) noexcept {
        input_.embedded = inputs;
        output_.embedded = outputs;
    }

#pragma region Context Switching

    void release_inputs() noexcept {
        input_.dynamic.reset();
        input_.embedded_used = 0;
    }
    void release_outputs() noexcept {
        output_.dynamic.reset();
        output_.embedded_used = 0;
        output_submitted_ = 0;
    }

    span_gt<char> input_span() noexcept { return input_.span(); }
    span_gt<char> output_span() noexcept { return output_.span(); }

#pragma endregion

#pragma region Piping Inputs
    char* next_input_address() noexcept { return input_.embedded; }
    std::size_t next_input_length() const noexcept { return ram_page_size_k; }

    bool shift_input_to_dynamic() noexcept {
        if (!input_.dynamic.append_n(input_.embedded, input_.embedded_used))
            return false;
        input_.embedded_used = 0;
        return true;
    }

    bool absorb_input(std::size_t embedded_used) noexcept {
        input_.embedded_used = embedded_used;
        if (!input_.dynamic.size())
            return true;
        return shift_input_to_dynamic();
    }

#pragma endregion
#pragma region Piping Outputs

    void mark_submitted_outputs(std::size_t n) noexcept { output_submitted_ += n; }
    void prepare_more_outputs() noexcept {
        if (!output_.dynamic.size())
            return;
        output_.embedded_used = std::min(output_.dynamic.size() - output_submitted_, ram_page_size_k);
        std::memcpy(output_.embedded, output_.dynamic.data() + output_submitted_, output_.embedded_used);
    }
    bool has_outputs() const noexcept { return std::max(output_.embedded_used, output_.dynamic.size()); }
    bool has_remaining_outputs() const noexcept {
        return output_submitted_ < std::max(output_.embedded_used, output_.dynamic.size());
    }
    char const* next_output_address() const noexcept {
        return output_.dynamic.size() ? output_.embedded : output_.embedded + output_submitted_;
    }

    std::size_t next_output_length() const noexcept {
        return output_.dynamic.size() ? output_.embedded_used : output_.embedded_used - output_submitted_;
    }

    template <std::size_t> bool append_outputs(struct iovec const*) noexcept;

#pragma endregion

#pragma region Replacing

    void output_pop_back() noexcept {
        if (output_.dynamic.size())
            output_.dynamic.pop_back();
        else
            output_.embedded_used--;
    }

    void push_back_reserved(char c) noexcept {
        if (output_.dynamic.size())
            output_.dynamic.push_back_reserved(c);
        else
            output_.embedded[output_.embedded_used++] = c;
    }

#pragma endregion
};

template <std::size_t iovecs_count_ak> //
bool exchange_pipes_t::append_outputs(struct iovec const* iovecs) noexcept {
    std::size_t added_length = iovecs_length<iovecs_count_ak>(iovecs);
    bool was_in_embedded = !output_.dynamic.size();
    bool fit_into_embedded = output_.embedded_used + added_length < ram_page_size_k;

    if (was_in_embedded && fit_into_embedded) {
        iovecs_memcpy<iovecs_count_ak>(iovecs, output_.embedded + output_.embedded_used);
        output_.embedded_used += added_length;
        return true;
    } else {
        if (!output_.dynamic.reserve(output_.dynamic.size() + output_.embedded_used + added_length))
            return false;
        if (!was_in_embedded)
            if (!output_.dynamic.append_n(output_.embedded, output_.embedded_used))
                return false;
        output_.embedded_used = 0;
        for (std::size_t i = 0; i != iovecs_count_ak; ++i)
            if (!output_.dynamic.append_n((char const*)iovecs[i].iov_base, iovecs[i].iov_len))
                return false;
        return true;
    }
}

} // namespace unum::ujrpc