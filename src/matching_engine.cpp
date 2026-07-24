#include "matching_engine/matching_engine.hpp"

#include <algorithm>

namespace matching_engine {

SubmitResult MatchingEngine::submit_limit(OrderRequest request) {
  const ErrorCode validation_error = validate_limit_order_request(request);
  if (validation_error != ErrorCode::None) {
    return SubmitResult{.error = validation_error, .trades = {}};
  }

  if (order_index_.contains(request.id)) {
    return SubmitResult{.error = ErrorCode::DuplicateOrderId, .trades = {}};
  }

  if (request.side == Side::Buy) {
    return submit_buy_limit(request);
  }

  return submit_sell_limit(request);
}

SubmitResult MatchingEngine::submit_market(MarketOrderRequest request) {
  const ErrorCode validation_error = validate_market_order_request(request);
  if (validation_error != ErrorCode::None) {
    return SubmitResult{.error = validation_error, .trades = {}};
  }

  if (order_index_.contains(request.id)) {
    return SubmitResult{.error = ErrorCode::DuplicateOrderId, .trades = {}};
  }

  if (request.side == Side::Buy) {
    return submit_buy_market(request);
  }

  return submit_sell_market(request);
}

ErrorCode MatchingEngine::cancel(OrderId id) noexcept {
  const auto location = order_index_.find(id);
  if (location == order_index_.end()) {
    return ErrorCode::UnknownOrderId;
  }

  const Side side = location->second.side;
  const Price price = location->second.price;
  const OrderIterator order = location->second.order;
  order_index_.erase(location);

  if (side == Side::Buy) {
    const auto level = bids_.find(price);
    level->second.orders.erase(order);
    if (level->second.orders.empty()) {
      bids_.erase(level);
    }
    return ErrorCode::None;
  }

  const auto level = asks_.find(price);
  level->second.orders.erase(order);
  if (level->second.orders.empty()) {
    asks_.erase(level);
  }
  return ErrorCode::None;
}

SubmitResult MatchingEngine::replace(ReplaceRequest request) {
  const ErrorCode validation_error = validate_replace_request(request);
  if (validation_error != ErrorCode::None) {
    return SubmitResult{.error = validation_error, .trades = {}};
  }

  const auto location = order_index_.find(request.id);
  if (location == order_index_.end()) {
    return SubmitResult{.error = ErrorCode::UnknownOrderId, .trades = {}};
  }

  const Side side = location->second.side;
  RestingOrder &order = *location->second.order;
  if (request.new_price == order.price &&
      request.new_quantity <= order.quantity) {
    order.quantity = request.new_quantity;
    return {};
  }

  const ErrorCode cancel_error = cancel(request.id);
  if (cancel_error != ErrorCode::None) {
    return SubmitResult{.error = cancel_error, .trades = {}};
  }

  return submit_limit(OrderRequest{.id = request.id,
                                   .side = side,
                                   .price = request.new_price,
                                   .quantity = request.new_quantity});
}

std::optional<Price> MatchingEngine::best_bid() const noexcept {
  if (bids_.empty()) {
    return std::nullopt;
  }

  return bids_.begin()->first;
}

std::optional<Price> MatchingEngine::best_ask() const noexcept {
  if (asks_.empty()) {
    return std::nullopt;
  }

  return asks_.begin()->first;
}

std::vector<DepthLevel> MatchingEngine::depth(Side, std::size_t) const {
  return {};
}

BookSnapshot MatchingEngine::snapshot() const { return {}; }

SubmitResult MatchingEngine::submit_buy_limit(OrderRequest request) {
  SubmitResult result{};

  while (request.quantity > Quantity{0} && !asks_.empty()) {
    auto best_ask = asks_.begin();
    if (best_ask->first > request.price) {
      break;
    }

    auto &resting_orders = best_ask->second.orders;
    while (request.quantity > Quantity{0} && !resting_orders.empty()) {
      auto resting = resting_orders.begin();
      const Quantity trade_quantity =
          std::min(request.quantity, resting->quantity);

      result.trades.push_back(Trade{.aggressor_id = request.id,
                                    .resting_id = resting->id,
                                    .price = resting->price,
                                    .quantity = trade_quantity});

      request.quantity -= trade_quantity;
      resting->quantity -= trade_quantity;

      if (resting->quantity == Quantity{0}) {
        order_index_.erase(resting->id);
        resting_orders.erase(resting);
      }
    }

    if (resting_orders.empty()) {
      asks_.erase(best_ask);
    }
  }

  if (request.quantity > Quantity{0}) {
    rest_order(request);
  }

  return result;
}

SubmitResult MatchingEngine::submit_sell_limit(OrderRequest request) {
  SubmitResult result{};

  while (request.quantity > Quantity{0} && !bids_.empty()) {
    auto best_bid = bids_.begin();
    if (best_bid->first < request.price) {
      break;
    }

    auto &resting_orders = best_bid->second.orders;
    while (request.quantity > Quantity{0} && !resting_orders.empty()) {
      auto resting = resting_orders.begin();
      const Quantity trade_quantity =
          std::min(request.quantity, resting->quantity);

      result.trades.push_back(Trade{.aggressor_id = request.id,
                                    .resting_id = resting->id,
                                    .price = resting->price,
                                    .quantity = trade_quantity});

      request.quantity -= trade_quantity;
      resting->quantity -= trade_quantity;

      if (resting->quantity == Quantity{0}) {
        order_index_.erase(resting->id);
        resting_orders.erase(resting);
      }
    }

    if (resting_orders.empty()) {
      bids_.erase(best_bid);
    }
  }

  if (request.quantity > Quantity{0}) {
    rest_order(request);
  }

  return result;
}

SubmitResult MatchingEngine::submit_buy_market(MarketOrderRequest request) {
  SubmitResult result{};

  while (request.quantity > Quantity{0} && !asks_.empty()) {
    auto best_ask = asks_.begin();
    auto &resting_orders = best_ask->second.orders;

    while (request.quantity > Quantity{0} && !resting_orders.empty()) {
      auto resting = resting_orders.begin();
      const Quantity trade_quantity =
          std::min(request.quantity, resting->quantity);

      result.trades.push_back(Trade{.aggressor_id = request.id,
                                    .resting_id = resting->id,
                                    .price = resting->price,
                                    .quantity = trade_quantity});

      request.quantity -= trade_quantity;
      resting->quantity -= trade_quantity;

      if (resting->quantity == Quantity{0}) {
        order_index_.erase(resting->id);
        resting_orders.erase(resting);
      }
    }

    if (resting_orders.empty()) {
      asks_.erase(best_ask);
    }
  }

  return result;
}

SubmitResult MatchingEngine::submit_sell_market(MarketOrderRequest request) {
  SubmitResult result{};

  while (request.quantity > Quantity{0} && !bids_.empty()) {
    auto best_bid = bids_.begin();
    auto &resting_orders = best_bid->second.orders;

    while (request.quantity > Quantity{0} && !resting_orders.empty()) {
      auto resting = resting_orders.begin();
      const Quantity trade_quantity =
          std::min(request.quantity, resting->quantity);

      result.trades.push_back(Trade{.aggressor_id = request.id,
                                    .resting_id = resting->id,
                                    .price = resting->price,
                                    .quantity = trade_quantity});

      request.quantity -= trade_quantity;
      resting->quantity -= trade_quantity;

      if (resting->quantity == Quantity{0}) {
        order_index_.erase(resting->id);
        resting_orders.erase(resting);
      }
    }

    if (resting_orders.empty()) {
      bids_.erase(best_bid);
    }
  }

  return result;
}

void MatchingEngine::rest_order(OrderRequest request) {
  const RestingOrder order{
      .id = request.id, .price = request.price, .quantity = request.quantity};

  if (request.side == Side::Buy) {
    auto [level, inserted] = bids_.try_emplace(request.price);
    (void)inserted;
    auto order_iterator =
        level->second.orders.insert(level->second.orders.end(), order);
    order_index_.emplace(request.id, OrderLocation{.side = request.side,
                                                   .price = request.price,
                                                   .order = order_iterator});
    return;
  }

  auto [level, inserted] = asks_.try_emplace(request.price);
  (void)inserted;
  auto order_iterator =
      level->second.orders.insert(level->second.orders.end(), order);
  order_index_.emplace(request.id, OrderLocation{.side = request.side,
                                                 .price = request.price,
                                                 .order = order_iterator});
}

} // namespace matching_engine
