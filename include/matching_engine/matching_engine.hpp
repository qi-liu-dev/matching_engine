#pragma once

#include <cstddef>
#include <functional>
#include <list>
#include <map>
#include <optional>
#include <unordered_map>
#include <vector>

#include "matching_engine/domain.hpp"
#include "matching_engine/snapshot.hpp"

namespace matching_engine {

class MatchingEngine {
public:
  MatchingEngine() = default;
  MatchingEngine(const MatchingEngine &) = delete;
  MatchingEngine &operator=(const MatchingEngine &) = delete;
  MatchingEngine(MatchingEngine &&) = delete;
  MatchingEngine &operator=(MatchingEngine &&) = delete;
  ~MatchingEngine() = default;

  [[nodiscard]] SubmitResult submit_limit(OrderRequest request);
  [[nodiscard]] SubmitResult submit_market(MarketOrderRequest request);
  [[nodiscard]] ErrorCode cancel(OrderId id) noexcept;
  [[nodiscard]] SubmitResult replace(ReplaceRequest request);

  [[nodiscard]] std::optional<Price> best_bid() const noexcept;
  [[nodiscard]] std::optional<Price> best_ask() const noexcept;
  [[nodiscard]] std::vector<DepthLevel> depth(Side side,
                                              std::size_t levels) const;
  [[nodiscard]] BookSnapshot snapshot() const;

private:
  struct RestingOrder {
    OrderId id;
    Price price;
    Quantity quantity;
  };

  struct PriceLevel {
    std::list<RestingOrder> orders;
  };

  using OrderIterator = std::list<RestingOrder>::iterator;
  using BidLevels = std::map<Price, PriceLevel, std::greater<Price>>;
  using AskLevels = std::map<Price, PriceLevel, std::less<Price>>;

  struct OrderLocation {
    Side side;
    Price price;
    OrderIterator order;
  };

  [[nodiscard]] SubmitResult submit_buy_limit(OrderRequest request);
  [[nodiscard]] SubmitResult submit_sell_limit(OrderRequest request);
  void rest_order(OrderRequest request);

  BidLevels bids_{};
  AskLevels asks_{};
  std::unordered_map<OrderId, OrderLocation> order_index_{};
};

} // namespace matching_engine
