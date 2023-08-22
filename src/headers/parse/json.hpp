#pragma once
#include <charconv>
#include <variant>

#include <simdjson.h>

#include "shared.hpp"

namespace unum::ucall {

namespace sj = simdjson;
namespace sjd = sj::dom;

struct scratch_space_t {

    sjd::parser parser{};
    std::variant<sjd::element, sjd::array> elements{};

    std::optional<default_error_t> parse(std::string_view json_doc) noexcept {
        if (json_doc.size() > parser.capacity()) {
            if (parser.allocate(json_doc.size(), json_doc.size() / 2) != sj::SUCCESS)
                return default_error_t{-32000, "Out of memory"};
            parser.set_max_capacity(json_doc.size());
        }

        auto one_or_many = parser.parse(json_doc.data(), json_doc.size(), false);

        if (one_or_many.error() == sj::CAPACITY)
            return default_error_t{-32000, "Out of memory"};

        if (one_or_many.error() != sj::SUCCESS)
            return default_error_t{-32700, "Invalid JSON was received by the server."};

        if (one_or_many.is_array())
            elements.emplace<sjd::array>(one_or_many.get_array().value_unsafe());
        else
            elements.emplace<sjd::element>(one_or_many.value_unsafe());

        return std::nullopt;
    }

    bool is_batch() const noexcept { return std::holds_alternative<sjd::array>(elements); }
};

} // namespace unum::ucall
