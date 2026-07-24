#pragma once

#include <cstdint>
#include <vector>

namespace matching_engine {

using OrderId = std::uint64_t;
using Price = std::int64_t;
using Quantity = std::uint64_t;
using SequenceNumber = std::uint64_t;

enum class Side { Buy, Sell };

struct OrderRequest {
  OrderId id;
  Side side;
  Price price;
  Quantity quantity;
};

struct MarketOrderRequest {
  OrderId id;
  Side side;
  Quantity quantity;
};

struct ReplaceRequest {
  OrderId id;
  Price new_price;
  Quantity new_quantity;
};

struct Trade {
  OrderId aggressor_id;
  OrderId resting_id;
  Price price;
  Quantity quantity;
};

enum class ErrorCode {
  None,
  DuplicateOrderId,
  UnknownOrderId,
  InvalidPrice,
  InvalidQuantity
};

struct SubmitResult {
  ErrorCode error{ErrorCode::None};
  std::vector<Trade> trades{};

  [[nodiscard]] bool ok() const noexcept { return error == ErrorCode::None; }
};

[[nodiscard]] constexpr bool is_valid_limit_price(Price price) noexcept {
  return price > Price{0};
}

[[nodiscard]] constexpr bool is_valid_quantity(Quantity quantity) noexcept {
  return quantity > Quantity{0};
}

[[nodiscard]] constexpr ErrorCode
validate_limit_order_request(const OrderRequest &request) noexcept {
  if (!is_valid_quantity(request.quantity)) {
    return ErrorCode::InvalidQuantity;
  }

  if (!is_valid_limit_price(request.price)) {
    return ErrorCode::InvalidPrice;
  }

  return ErrorCode::None;
}

[[nodiscard]] constexpr ErrorCode
validate_market_order_request(const MarketOrderRequest &request) noexcept {
  if (!is_valid_quantity(request.quantity)) {
    return ErrorCode::InvalidQuantity;
  }

  return ErrorCode::None;
}

[[nodiscard]] constexpr ErrorCode
validate_replace_request(const ReplaceRequest &request) noexcept {
  if (!is_valid_quantity(request.new_quantity)) {
    return ErrorCode::InvalidQuantity;
  }

  if (!is_valid_limit_price(request.new_price)) {
    return ErrorCode::InvalidPrice;
  }

  return ErrorCode::None;
}

} // namespace matching_engine
