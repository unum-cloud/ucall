#pragma once

#include "shared.hpp"
#include "ucall/ucall.h"
#include <any>

namespace unum::ucall {
class protocol_t {
  private:
    std::any sp_proto;
    protocol_type_t type;

  public:
    explicit protocol_t(protocol_type_t = protocol_type_t::TCP) noexcept;

    void reset_protocol(protocol_type_t) noexcept;

    void reset() noexcept;

    void prepare_response(exchange_pipes_t&) noexcept;

    bool append_response(exchange_pipes_t&, std::string_view, std::string_view) noexcept;
    bool append_error(exchange_pipes_t&, std::string_view, std::string_view, std::string_view) noexcept;

    void finalize_response(exchange_pipes_t&) noexcept;

    bool is_input_complete(span_gt<char> const&) noexcept;

    std::variant<parsed_request_t, default_error_t> parse(std::string_view) const noexcept;
};
} // namespace unum::ucall
