#pragma once
#include <string_view>

template <typename element_at> class span_gt {
    element_at* begin_{};
    element_at* end_{};

  public:
    span_gt(element_at* b, element_at* e) noexcept : begin_(b), end_(e) {}
    span_gt(span_gt&&) = delete;
    span_gt(span_gt const&) = delete;
    span_gt& operator=(span_gt const&) = delete;

    [[nodiscard]] element_at const* data() const noexcept { return begin_; }
    [[nodiscard]] element_at* data() noexcept { return begin_; }
    [[nodiscard]] element_at* begin() noexcept { return begin_; }
    [[nodiscard]] element_at* end() noexcept { return end_; }
    [[nodiscard]] std::size_t size() const noexcept { return end_ - begin_; }
    [[nodiscard]] element_at& operator[](std::size_t i) noexcept { return begin_[i]; }
    [[nodiscard]] element_at const& operator[](std::size_t i) const noexcept { return begin_[i]; }
    operator std::basic_string_view<element_at>() const noexcept { return {data(), size()}; }
};
