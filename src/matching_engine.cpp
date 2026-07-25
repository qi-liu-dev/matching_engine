#include "matching_engine/matching_engine.hpp"

#include <algorithm>
#include <limits>
#include <stdexcept>

namespace matching_engine {
namespace {

Quantity add_depth_quantity(Quantity total, Quantity quantity) {
  if (quantity > std::numeric_limits<Quantity>::max() - total) {
    throw std::overflow_error{"depth quantity exceeds Quantity range"};
  }

  return total + quantity;
}

} // namespace

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

  const MatchPlan plan =
      side == Side::Buy
          ? plan_buy_matches(request.new_quantity, request.new_price)
          : plan_sell_matches(request.new_quantity, request.new_price);
  SubmitResult result{};
  // Complete potentially throwing allocations before touching either book side.
  result.trades.reserve(plan.trade_count);

  if (plan.remaining_quantity > Quantity{0}) {
    if (side == Side::Buy) {
      bids_.try_emplace(request.new_price);
    } else {
      asks_.try_emplace(request.new_price);
    }
  }

  Quantity remaining_quantity = request.new_quantity;
  if (side == Side::Buy) {
    match_buy(request.id, remaining_quantity, request.new_price, result.trades);
  } else {
    match_sell(request.id, remaining_quantity, request.new_price,
               result.trades);
  }

  if (remaining_quantity == Quantity{0}) {
    (void)cancel(request.id);
    return result;
  }

  if (side == Side::Buy) {
    const auto old_level = bids_.find(order.price);
    const auto new_level = bids_.find(request.new_price);
    new_level->second.orders.splice(new_level->second.orders.end(),
                                    old_level->second.orders,
                                    location->second.order);
    if (old_level != new_level && old_level->second.orders.empty()) {
      bids_.erase(old_level);
    }
  } else {
    const auto old_level = asks_.find(order.price);
    const auto new_level = asks_.find(request.new_price);
    new_level->second.orders.splice(new_level->second.orders.end(),
                                    old_level->second.orders,
                                    location->second.order);
    if (old_level != new_level && old_level->second.orders.empty()) {
      asks_.erase(old_level);
    }
  }

  order.price = request.new_price;
  order.quantity = remaining_quantity;
  order.sequence = next_sequence_;
  location->second.price = request.new_price;
  ++next_sequence_;
  return result;
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

std::vector<DepthLevel> MatchingEngine::depth(Side side,
                                              std::size_t levels) const {
  std::vector<DepthLevel> result;

  if (side == Side::Buy) {
    for (const auto &[price, level] : bids_) {
      if (result.size() == levels) {
        break;
      }

      Quantity quantity{};
      for (const RestingOrder &order : level.orders) {
        quantity = add_depth_quantity(quantity, order.quantity);
      }
      result.push_back(DepthLevel{.price = price, .quantity = quantity});
    }
    return result;
  }

  for (const auto &[price, level] : asks_) {
    if (result.size() == levels) {
      break;
    }

    Quantity quantity{};
    for (const RestingOrder &order : level.orders) {
      quantity = add_depth_quantity(quantity, order.quantity);
    }
    result.push_back(DepthLevel{.price = price, .quantity = quantity});
  }
  return result;
}

BookSnapshot MatchingEngine::snapshot() const {
  BookSnapshot result;
  result.orders.reserve(order_index_.size());

  for (const auto &[price, level] : bids_) {
    for (const RestingOrder &order : level.orders) {
      result.orders.push_back(SnapshotOrder{.id = order.id,
                                            .side = Side::Buy,
                                            .price = price,
                                            .quantity = order.quantity,
                                            .sequence = order.sequence});
    }
  }

  for (const auto &[price, level] : asks_) {
    for (const RestingOrder &order : level.orders) {
      result.orders.push_back(SnapshotOrder{.id = order.id,
                                            .side = Side::Sell,
                                            .price = price,
                                            .quantity = order.quantity,
                                            .sequence = order.sequence});
    }
  }

  return result;
}

SubmitResult MatchingEngine::submit_buy_limit(OrderRequest request) {
  const MatchPlan plan = plan_buy_matches(request.quantity, request.price);
  SubmitResult result{};
  // Stage the final remainder before matching so allocation failure is atomic.
  result.trades.reserve(plan.trade_count);

  if (plan.remaining_quantity > Quantity{0}) {
    rest_order(OrderRequest{.id = request.id,
                            .side = request.side,
                            .price = request.price,
                            .quantity = plan.remaining_quantity});
  }

  match_buy(request.id, request.quantity, request.price, result.trades);
  return result;
}

SubmitResult MatchingEngine::submit_sell_limit(OrderRequest request) {
  const MatchPlan plan = plan_sell_matches(request.quantity, request.price);
  SubmitResult result{};
  // Stage the final remainder before matching so allocation failure is atomic.
  result.trades.reserve(plan.trade_count);

  if (plan.remaining_quantity > Quantity{0}) {
    rest_order(OrderRequest{.id = request.id,
                            .side = request.side,
                            .price = request.price,
                            .quantity = plan.remaining_quantity});
  }

  match_sell(request.id, request.quantity, request.price, result.trades);
  return result;
}

SubmitResult MatchingEngine::submit_buy_market(MarketOrderRequest request) {
  const MatchPlan plan = plan_buy_matches(request.quantity, std::nullopt);
  SubmitResult result{};
  result.trades.reserve(plan.trade_count);

  match_buy(request.id, request.quantity, std::nullopt, result.trades);
  return result;
}

SubmitResult MatchingEngine::submit_sell_market(MarketOrderRequest request) {
  const MatchPlan plan = plan_sell_matches(request.quantity, std::nullopt);
  SubmitResult result{};
  result.trades.reserve(plan.trade_count);

  match_sell(request.id, request.quantity, std::nullopt, result.trades);
  return result;
}

MatchingEngine::MatchPlan MatchingEngine::plan_buy_matches(
    Quantity quantity, std::optional<Price> limit_price) const noexcept {
  std::size_t trade_count{};
  for (const auto &[price, level] : asks_) {
    if (limit_price.has_value() && price > *limit_price) {
      break;
    }

    for (const RestingOrder &order : level.orders) {
      ++trade_count;
      quantity -= std::min(quantity, order.quantity);
      if (quantity == Quantity{0}) {
        return MatchPlan{.trade_count = trade_count,
                         .remaining_quantity = Quantity{0}};
      }
    }
  }

  return MatchPlan{.trade_count = trade_count, .remaining_quantity = quantity};
}

MatchingEngine::MatchPlan MatchingEngine::plan_sell_matches(
    Quantity quantity, std::optional<Price> limit_price) const noexcept {
  std::size_t trade_count{};
  for (const auto &[price, level] : bids_) {
    if (limit_price.has_value() && price < *limit_price) {
      break;
    }

    for (const RestingOrder &order : level.orders) {
      ++trade_count;
      quantity -= std::min(quantity, order.quantity);
      if (quantity == Quantity{0}) {
        return MatchPlan{.trade_count = trade_count,
                         .remaining_quantity = Quantity{0}};
      }
    }
  }

  return MatchPlan{.trade_count = trade_count, .remaining_quantity = quantity};
}

void MatchingEngine::match_buy(OrderId aggressor_id, Quantity &quantity,
                               std::optional<Price> limit_price,
                               std::vector<Trade> &trades) {
  while (quantity > Quantity{0} && !asks_.empty()) {
    auto best_ask = asks_.begin();
    if (limit_price.has_value() && best_ask->first > *limit_price) {
      break;
    }

    auto &resting_orders = best_ask->second.orders;
    while (quantity > Quantity{0} && !resting_orders.empty()) {
      auto resting = resting_orders.begin();
      const Quantity trade_quantity = std::min(quantity, resting->quantity);

      trades.push_back(Trade{.aggressor_id = aggressor_id,
                             .resting_id = resting->id,
                             .price = resting->price,
                             .quantity = trade_quantity});

      quantity -= trade_quantity;
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
}

void MatchingEngine::match_sell(OrderId aggressor_id, Quantity &quantity,
                                std::optional<Price> limit_price,
                                std::vector<Trade> &trades) {
  while (quantity > Quantity{0} && !bids_.empty()) {
    auto best_bid = bids_.begin();
    if (limit_price.has_value() && best_bid->first < *limit_price) {
      break;
    }

    auto &resting_orders = best_bid->second.orders;
    while (quantity > Quantity{0} && !resting_orders.empty()) {
      auto resting = resting_orders.begin();
      const Quantity trade_quantity = std::min(quantity, resting->quantity);

      trades.push_back(Trade{.aggressor_id = aggressor_id,
                             .resting_id = resting->id,
                             .price = resting->price,
                             .quantity = trade_quantity});

      quantity -= trade_quantity;
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
}

void MatchingEngine::rest_order(OrderRequest request) {
  const RestingOrder order{.id = request.id,
                           .price = request.price,
                           .quantity = request.quantity,
                           .sequence = next_sequence_};

  // Measurements showed that insertion hints cost more in shallow books.
  if (request.side == Side::Buy) {
    auto level = bids_.end();
    bool level_was_inserted = false;
    if (bids_.size() > 1U && request.price > bids_.begin()->first) {
      level = bids_.try_emplace(bids_.begin(), request.price);
      level_was_inserted = true;
    } else {
      const auto [found_level, inserted] = bids_.try_emplace(request.price);
      level = found_level;
      level_was_inserted = inserted;
    }

    std::optional<OrderIterator> inserted_order;
    try {
      inserted_order =
          level->second.orders.insert(level->second.orders.end(), order);
      const auto [index_location, inserted] = order_index_.emplace(
          request.id, OrderLocation{.side = request.side,
                                    .price = request.price,
                                    .order = *inserted_order});
      (void)index_location;
      if (!inserted) {
        throw std::logic_error{"duplicate order ID reached rest_order"};
      }
    } catch (...) {
      if (inserted_order.has_value()) {
        level->second.orders.erase(*inserted_order);
      }
      if (level_was_inserted && level->second.orders.empty()) {
        bids_.erase(level);
      }
      throw;
    }

    ++next_sequence_;
    return;
  }

  auto level = asks_.end();
  bool level_was_inserted = false;
  if (asks_.size() > 1U && request.price < asks_.begin()->first) {
    level = asks_.try_emplace(asks_.begin(), request.price);
    level_was_inserted = true;
  } else {
    const auto [found_level, inserted] = asks_.try_emplace(request.price);
    level = found_level;
    level_was_inserted = inserted;
  }

  std::optional<OrderIterator> inserted_order;
  try {
    inserted_order =
        level->second.orders.insert(level->second.orders.end(), order);
    const auto [index_location, inserted] = order_index_.emplace(
        request.id, OrderLocation{.side = request.side,
                                  .price = request.price,
                                  .order = *inserted_order});
    (void)index_location;
    if (!inserted) {
      throw std::logic_error{"duplicate order ID reached rest_order"};
    }
  } catch (...) {
    if (inserted_order.has_value()) {
      level->second.orders.erase(*inserted_order);
    }
    if (level_was_inserted && level->second.orders.empty()) {
      asks_.erase(level);
    }
    throw;
  }

  ++next_sequence_;
}

} // namespace matching_engine
