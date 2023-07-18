#pragma once

template <typename element_at> class array_gt {
    element_at* elements_{};
    std::size_t count_{};
    std::size_t capacity_{};
    static_assert(std::is_nothrow_default_constructible<element_at>());
    static_assert(std::is_trivially_copy_constructible<element_at>(), "Can't use realloc and memcpy");

  public:
    array_gt() = default;
    array_gt(array_gt&&) = delete;
    array_gt(array_gt const&) = delete;
    array_gt& operator=(array_gt const&) = delete;

    array_gt& operator=(array_gt&& other) noexcept {
        std::swap(elements_, other.elements_);
        std::swap(count_, other.count_);
        std::swap(capacity_, other.capacity_);
        return *this;
    }
    [[nodiscard]] bool reserve(std::size_t n) noexcept {
        if (n <= capacity_)
            return true;
        if (!elements_) {
            auto ptr = (element_at*)std::malloc(sizeof(element_at) * n);
            if (!ptr)
                return false;
            elements_ = ptr;
        } else {
            auto ptr = (element_at*)std::realloc(elements_, sizeof(element_at) * n);
            if (!ptr)
                return false;
            elements_ = ptr;
        }
        std::uninitialized_default_construct(elements_ + capacity_, elements_ + n);
        capacity_ = n;
        return true;
    }
    ~array_gt() noexcept { reset(); }
    void reset() noexcept {
        if constexpr (!std::is_trivially_destructible<element_at>())
            std::destroy_n(elements_, count_);
        std::free(elements_);
        elements_ = nullptr;
        capacity_ = count_ = 0;
    }
    [[nodiscard]] element_at const* data() const noexcept { return elements_; }
    [[nodiscard]] element_at* data() noexcept { return elements_; }
    [[nodiscard]] element_at* begin() noexcept { return elements_; }
    [[nodiscard]] element_at* end() noexcept { return elements_ + count_; }
    [[nodiscard]] std::size_t size() const noexcept { return count_; }
    [[nodiscard]] std::size_t capacity() const noexcept { return capacity_; }
    [[nodiscard]] element_at& operator[](std::size_t i) noexcept { return elements_[i]; }

    void push_back_reserved(element_at element) noexcept { new (elements_ + count_++) element_at(std::move(element)); }
    void pop_back(std::size_t n = 1) noexcept { count_ -= n; }
    [[nodiscard]] bool append_n(element_at const* elements, std::size_t n) noexcept {
        if (!reserve(size() + n))
            return false;
        std::memcpy(end(), elements, n * sizeof(element_at));
        count_ += n;
        return true;
    }
};
