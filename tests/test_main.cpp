#include "matching_engine/domain.hpp"
#include "matching_engine/matching_engine.hpp"

#include <algorithm>
#include <cstdint>
#include <exception>
#include <functional>
#include <iostream>
#include <limits>
#include <map>
#include <optional>
#include <random>
#include <sstream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <type_traits>
#include <unordered_map>
#include <utility>
#include <vector>

namespace {

using matching_engine::BookSnapshot;
using matching_engine::DepthLevel;
using matching_engine::ErrorCode;
using matching_engine::MarketOrderRequest;
using matching_engine::MatchingEngine;
using matching_engine::OrderId;
using matching_engine::OrderRequest;
using matching_engine::Price;
using matching_engine::Quantity;
using matching_engine::ReplaceRequest;
using matching_engine::SequenceNumber;
using matching_engine::Side;
using matching_engine::SnapshotOrder;
using matching_engine::Trade;

using TestFunction = void (*)();

void require(bool condition, std::string_view message) {
  if (!condition) {
    throw std::runtime_error(std::string{message});
  }
}

void require_trade(const Trade &trade, OrderId aggressor_id, OrderId resting_id,
                   Price price, Quantity quantity) {
  require(trade.aggressor_id == aggressor_id,
          "trade aggressor ID did not match");
  require(trade.resting_id == resting_id, "trade resting ID did not match");
  require(trade.price == price, "trade price did not match");
  require(trade.quantity == quantity, "trade quantity did not match");
}

void require_depth_level(const DepthLevel &level, Price price,
                         Quantity quantity) {
  require(level.price == price, "depth price did not match");
  require(level.quantity == quantity, "depth quantity did not match");
}

void require_snapshot_order(const SnapshotOrder &order, OrderId id, Side side,
                            Price price, Quantity quantity,
                            SequenceNumber sequence) {
  require(order.id == id, "snapshot order ID did not match");
  require(order.side == side, "snapshot side did not match");
  require(order.price == price, "snapshot price did not match");
  require(order.quantity == quantity, "snapshot quantity did not match");
  require(order.sequence == sequence, "snapshot sequence did not match");
}

void require_not_crossed(const MatchingEngine &engine) {
  const auto best_bid = engine.best_bid();
  const auto best_ask = engine.best_ask();
  if (best_bid.has_value() && best_ask.has_value()) {
    require(*best_bid < *best_ask,
            "book should not be crossed after processing an order");
  }
}

void domain_aliases_use_fixed_width_integers() {
  static_assert(std::is_same_v<OrderId, std::uint64_t>);
  static_assert(std::is_same_v<Price, std::int64_t>);
  static_assert(std::is_same_v<Quantity, std::uint64_t>);
  static_assert(std::is_same_v<SequenceNumber, std::uint64_t>);
}

void valid_limit_order_request_passes_validation() {
  const OrderRequest request{.id = OrderId{1},
                             .side = Side::Buy,
                             .price = Price{100},
                             .quantity = Quantity{10}};

  require(matching_engine::validate_limit_order_request(request) ==
              ErrorCode::None,
          "positive price and quantity should be a valid limit order request");
}

void zero_quantities_are_rejected() {
  const OrderRequest limit_request{.id = OrderId{1},
                                   .side = Side::Buy,
                                   .price = Price{100},
                                   .quantity = Quantity{0}};
  const MarketOrderRequest market_request{
      .id = OrderId{2}, .side = Side::Sell, .quantity = Quantity{0}};
  const ReplaceRequest replace_request{
      .id = OrderId{3}, .new_price = Price{100}, .new_quantity = Quantity{0}};

  require(matching_engine::validate_limit_order_request(limit_request) ==
              ErrorCode::InvalidQuantity,
          "limit orders must reject zero quantity");
  require(matching_engine::validate_market_order_request(market_request) ==
              ErrorCode::InvalidQuantity,
          "market orders must reject zero quantity");
  require(matching_engine::validate_replace_request(replace_request) ==
              ErrorCode::InvalidQuantity,
          "replacements must reject zero quantity");
}

void invalid_limit_prices_are_rejected() {
  const OrderRequest zero_price{.id = OrderId{1},
                                .side = Side::Buy,
                                .price = Price{0},
                                .quantity = Quantity{10}};
  const OrderRequest negative_price{.id = OrderId{2},
                                    .side = Side::Sell,
                                    .price = Price{-1},
                                    .quantity = Quantity{10}};
  const ReplaceRequest replace_zero_price{
      .id = OrderId{3}, .new_price = Price{0}, .new_quantity = Quantity{10}};

  require(matching_engine::validate_limit_order_request(zero_price) ==
              ErrorCode::InvalidPrice,
          "limit orders must reject a zero price");
  require(matching_engine::validate_limit_order_request(negative_price) ==
              ErrorCode::InvalidPrice,
          "limit orders must reject a negative price");
  require(matching_engine::validate_replace_request(replace_zero_price) ==
              ErrorCode::InvalidPrice,
          "replacements must reject a zero price");
}

void empty_engine_has_no_best_prices() {
  const MatchingEngine engine;

  require(!engine.best_bid().has_value(),
          "empty book should not have a best bid");
  require(!engine.best_ask().has_value(),
          "empty book should not have a best ask");
}

void non_marketable_limit_orders_rest_and_update_best_prices() {
  MatchingEngine engine;

  const auto buy_result =
      engine.submit_limit(OrderRequest{.id = OrderId{1},
                                       .side = Side::Buy,
                                       .price = Price{100},
                                       .quantity = Quantity{10}});
  require(buy_result.ok(), "non-marketable buy should be accepted");
  require(buy_result.trades.empty(), "non-marketable buy should not trade");
  require(engine.best_bid() == Price{100},
          "resting buy should become best bid");
  require(!engine.best_ask().has_value(),
          "book should not have an ask after only a buy rests");
  require_not_crossed(engine);

  const auto sell_result =
      engine.submit_limit(OrderRequest{.id = OrderId{2},
                                       .side = Side::Sell,
                                       .price = Price{105},
                                       .quantity = Quantity{7}});
  require(sell_result.ok(), "non-marketable sell should be accepted");
  require(sell_result.trades.empty(), "non-marketable sell should not trade");
  require(engine.best_bid() == Price{100}, "best bid should stay unchanged");
  require(engine.best_ask() == Price{105},
          "resting sell should become best ask");
  require_not_crossed(engine);
}

void exact_price_match_executes_and_removes_filled_resting_order() {
  MatchingEngine engine;

  require(engine
              .submit_limit(OrderRequest{.id = OrderId{1},
                                         .side = Side::Sell,
                                         .price = Price{100},
                                         .quantity = Quantity{5}})
              .ok(),
          "resting sell should be accepted");

  const auto result =
      engine.submit_limit(OrderRequest{.id = OrderId{2},
                                       .side = Side::Buy,
                                       .price = Price{100},
                                       .quantity = Quantity{5}});

  require(result.ok(), "crossing buy should be accepted");
  require(result.trades.size() == 1U, "exact match should emit one trade");
  require_trade(result.trades.at(0), OrderId{2}, OrderId{1}, Price{100},
                Quantity{5});
  require(!engine.best_bid().has_value(),
          "fully filled incoming buy should not rest");
  require(!engine.best_ask().has_value(),
          "fully filled resting sell should be removed");

  const auto reused_id_result =
      engine.submit_limit(OrderRequest{.id = OrderId{1},
                                       .side = Side::Sell,
                                       .price = Price{101},
                                       .quantity = Quantity{1}});
  require(reused_id_result.ok(),
          "an order ID can be reused after the previous order is inactive");
  require(engine.best_ask() == Price{101}, "reused ID order should rest");
  require_not_crossed(engine);
}

void buy_limit_matches_multiple_ask_price_levels() {
  MatchingEngine engine;
  require(engine
              .submit_limit(OrderRequest{.id = OrderId{1},
                                         .side = Side::Sell,
                                         .price = Price{100},
                                         .quantity = Quantity{2}})
              .ok(),
          "first ask should rest");
  require(engine
              .submit_limit(OrderRequest{.id = OrderId{2},
                                         .side = Side::Sell,
                                         .price = Price{101},
                                         .quantity = Quantity{3}})
              .ok(),
          "second ask should rest");
  require(engine
              .submit_limit(OrderRequest{.id = OrderId{3},
                                         .side = Side::Sell,
                                         .price = Price{102},
                                         .quantity = Quantity{4}})
              .ok(),
          "third ask should rest");

  const auto result =
      engine.submit_limit(OrderRequest{.id = OrderId{10},
                                       .side = Side::Buy,
                                       .price = Price{101},
                                       .quantity = Quantity{10}});

  require(result.ok(), "marketable buy limit should be accepted");
  require(result.trades.size() == 2U,
          "buy should match two eligible ask levels");
  require_trade(result.trades.at(0), OrderId{10}, OrderId{1}, Price{100},
                Quantity{2});
  require_trade(result.trades.at(1), OrderId{10}, OrderId{2}, Price{101},
                Quantity{3});
  require(engine.best_bid() == Price{101},
          "unfilled buy remainder should rest at its limit price");
  require(engine.best_ask() == Price{102},
          "non-marketable ask level should remain best ask");
  require_not_crossed(engine);
}

void sell_limit_matches_multiple_bid_price_levels() {
  MatchingEngine engine;
  require(engine
              .submit_limit(OrderRequest{.id = OrderId{1},
                                         .side = Side::Buy,
                                         .price = Price{103},
                                         .quantity = Quantity{2}})
              .ok(),
          "first bid should rest");
  require(engine
              .submit_limit(OrderRequest{.id = OrderId{2},
                                         .side = Side::Buy,
                                         .price = Price{102},
                                         .quantity = Quantity{3}})
              .ok(),
          "second bid should rest");
  require(engine
              .submit_limit(OrderRequest{.id = OrderId{3},
                                         .side = Side::Buy,
                                         .price = Price{101},
                                         .quantity = Quantity{4}})
              .ok(),
          "third bid should rest");

  const auto result =
      engine.submit_limit(OrderRequest{.id = OrderId{10},
                                       .side = Side::Sell,
                                       .price = Price{102},
                                       .quantity = Quantity{10}});

  require(result.ok(), "marketable sell limit should be accepted");
  require(result.trades.size() == 2U,
          "sell should match two eligible bid levels");
  require_trade(result.trades.at(0), OrderId{10}, OrderId{1}, Price{103},
                Quantity{2});
  require_trade(result.trades.at(1), OrderId{10}, OrderId{2}, Price{102},
                Quantity{3});
  require(engine.best_bid() == Price{101},
          "non-marketable bid level should remain best bid");
  require(engine.best_ask() == Price{102},
          "unfilled sell remainder should rest at its limit price");
  require_not_crossed(engine);
}

void partial_fill_leaves_resting_order_active() {
  MatchingEngine engine;
  require(engine
              .submit_limit(OrderRequest{.id = OrderId{1},
                                         .side = Side::Sell,
                                         .price = Price{100},
                                         .quantity = Quantity{10}})
              .ok(),
          "resting ask should be accepted");

  const auto partial_result =
      engine.submit_limit(OrderRequest{.id = OrderId{2},
                                       .side = Side::Buy,
                                       .price = Price{100},
                                       .quantity = Quantity{4}});

  require(partial_result.ok(), "partial fill buy should be accepted");
  require(partial_result.trades.size() == 1U,
          "partial fill should emit one trade");
  require_trade(partial_result.trades.at(0), OrderId{2}, OrderId{1}, Price{100},
                Quantity{4});
  require(engine.best_ask() == Price{100},
          "partially filled resting ask should remain active");

  const auto final_result =
      engine.submit_limit(OrderRequest{.id = OrderId{3},
                                       .side = Side::Buy,
                                       .price = Price{100},
                                       .quantity = Quantity{6}});
  require(final_result.ok(), "second buy should be accepted");
  require(final_result.trades.size() == 1U,
          "remaining resting quantity should fill in one trade");
  require_trade(final_result.trades.at(0), OrderId{3}, OrderId{1}, Price{100},
                Quantity{6});
  require(!engine.best_ask().has_value(),
          "resting ask should be removed after full fill");
  require_not_crossed(engine);
}

void incoming_remainder_rests_after_partial_fill() {
  MatchingEngine engine;
  require(engine
              .submit_limit(OrderRequest{.id = OrderId{1},
                                         .side = Side::Sell,
                                         .price = Price{100},
                                         .quantity = Quantity{4}})
              .ok(),
          "resting ask should be accepted");

  const auto result =
      engine.submit_limit(OrderRequest{.id = OrderId{2},
                                       .side = Side::Buy,
                                       .price = Price{101},
                                       .quantity = Quantity{10}});

  require(result.ok(), "buy with remainder should be accepted");
  require(result.trades.size() == 1U, "resting ask should be filled");
  require_trade(result.trades.at(0), OrderId{2}, OrderId{1}, Price{100},
                Quantity{4});
  require(engine.best_bid() == Price{101},
          "unfilled incoming buy quantity should rest");
  require(!engine.best_ask().has_value(), "filled ask level should be removed");
  require_not_crossed(engine);
}

void fifo_priority_is_preserved_at_same_price() {
  MatchingEngine engine;
  require(engine
              .submit_limit(OrderRequest{.id = OrderId{1},
                                         .side = Side::Sell,
                                         .price = Price{100},
                                         .quantity = Quantity{2}})
              .ok(),
          "first ask should rest");
  require(engine
              .submit_limit(OrderRequest{.id = OrderId{2},
                                         .side = Side::Sell,
                                         .price = Price{100},
                                         .quantity = Quantity{2}})
              .ok(),
          "second ask should rest behind first ask");

  const auto result =
      engine.submit_limit(OrderRequest{.id = OrderId{3},
                                       .side = Side::Buy,
                                       .price = Price{100},
                                       .quantity = Quantity{3}});

  require(result.ok(), "crossing buy should be accepted");
  require(result.trades.size() == 2U,
          "buy should fill oldest ask first, then next ask");
  require_trade(result.trades.at(0), OrderId{3}, OrderId{1}, Price{100},
                Quantity{2});
  require_trade(result.trades.at(1), OrderId{3}, OrderId{2}, Price{100},
                Quantity{1});

  const auto follow_up =
      engine.submit_limit(OrderRequest{.id = OrderId{4},
                                       .side = Side::Buy,
                                       .price = Price{100},
                                       .quantity = Quantity{1}});
  require(follow_up.ok(), "follow-up buy should be accepted");
  require(follow_up.trades.size() == 1U,
          "remaining quantity should belong to second ask");
  require_trade(follow_up.trades.at(0), OrderId{4}, OrderId{2}, Price{100},
                Quantity{1});
  require(!engine.best_ask().has_value(),
          "same-price level should be removed after all orders fill");
  require_not_crossed(engine);
}

void sell_trade_uses_resting_bid_price() {
  MatchingEngine engine;
  require(engine
              .submit_limit(OrderRequest{.id = OrderId{1},
                                         .side = Side::Buy,
                                         .price = Price{105},
                                         .quantity = Quantity{3}})
              .ok(),
          "resting bid should be accepted");

  const auto result =
      engine.submit_limit(OrderRequest{.id = OrderId{2},
                                       .side = Side::Sell,
                                       .price = Price{100},
                                       .quantity = Quantity{3}});

  require(result.ok(), "marketable sell should be accepted");
  require(result.trades.size() == 1U, "sell should emit one trade");
  require_trade(result.trades.at(0), OrderId{2}, OrderId{1}, Price{105},
                Quantity{3});
  require(!engine.best_bid().has_value(),
          "fully filled resting bid should be removed");
  require_not_crossed(engine);
}

void duplicate_active_order_id_is_rejected_without_mutation() {
  MatchingEngine engine;
  require(engine
              .submit_limit(OrderRequest{.id = OrderId{1},
                                         .side = Side::Buy,
                                         .price = Price{100},
                                         .quantity = Quantity{5}})
              .ok(),
          "first order should rest");

  const auto duplicate_result =
      engine.submit_limit(OrderRequest{.id = OrderId{1},
                                       .side = Side::Sell,
                                       .price = Price{90},
                                       .quantity = Quantity{5}});

  require(duplicate_result.error == ErrorCode::DuplicateOrderId,
          "active duplicate order ID should be rejected");
  require(duplicate_result.trades.empty(),
          "rejected duplicate should not trade");
  require(engine.best_bid() == Price{100},
          "duplicate rejection should not remove original bid");
  require(!engine.best_ask().has_value(),
          "duplicate rejection should not add a sell order");
  require_not_crossed(engine);
}

void market_buy_consumes_asks_in_price_time_priority() {
  MatchingEngine engine;
  require(engine
              .submit_limit(OrderRequest{.id = OrderId{1},
                                         .side = Side::Sell,
                                         .price = Price{100},
                                         .quantity = Quantity{2}})
              .ok(),
          "first best-price ask should rest");
  require(engine
              .submit_limit(OrderRequest{.id = OrderId{2},
                                         .side = Side::Sell,
                                         .price = Price{100},
                                         .quantity = Quantity{3}})
              .ok(),
          "second best-price ask should rest");
  require(engine
              .submit_limit(OrderRequest{.id = OrderId{3},
                                         .side = Side::Sell,
                                         .price = Price{101},
                                         .quantity = Quantity{4}})
              .ok(),
          "next-price ask should rest");

  const auto result = engine.submit_market(MarketOrderRequest{
      .id = OrderId{10}, .side = Side::Buy, .quantity = Quantity{6}});

  require(result.ok(), "market buy should be accepted");
  require(result.trades.size() == 3U,
          "market buy should consume two price levels");
  require_trade(result.trades.at(0), OrderId{10}, OrderId{1}, Price{100},
                Quantity{2});
  require_trade(result.trades.at(1), OrderId{10}, OrderId{2}, Price{100},
                Quantity{3});
  require_trade(result.trades.at(2), OrderId{10}, OrderId{3}, Price{101},
                Quantity{1});
  require(engine.best_ask() == Price{101},
          "partially filled second level should remain");
  require(!engine.best_bid().has_value(), "market buy should never rest");
  require_not_crossed(engine);

  const auto follow_up = engine.submit_market(MarketOrderRequest{
      .id = OrderId{11}, .side = Side::Buy, .quantity = Quantity{3}});
  require(follow_up.ok(), "follow-up market buy should be accepted");
  require(follow_up.trades.size() == 1U,
          "remaining ask quantity should fill in one trade");
  require_trade(follow_up.trades.at(0), OrderId{11}, OrderId{3}, Price{101},
                Quantity{3});
  require(!engine.best_ask().has_value(),
          "fully filled ask level should be removed");
}

void market_sell_consumes_bids_in_price_time_priority() {
  MatchingEngine engine;
  require(engine
              .submit_limit(OrderRequest{.id = OrderId{1},
                                         .side = Side::Buy,
                                         .price = Price{102},
                                         .quantity = Quantity{2}})
              .ok(),
          "first best-price bid should rest");
  require(engine
              .submit_limit(OrderRequest{.id = OrderId{2},
                                         .side = Side::Buy,
                                         .price = Price{102},
                                         .quantity = Quantity{3}})
              .ok(),
          "second best-price bid should rest");
  require(engine
              .submit_limit(OrderRequest{.id = OrderId{3},
                                         .side = Side::Buy,
                                         .price = Price{101},
                                         .quantity = Quantity{4}})
              .ok(),
          "next-price bid should rest");

  const auto result = engine.submit_market(MarketOrderRequest{
      .id = OrderId{10}, .side = Side::Sell, .quantity = Quantity{6}});

  require(result.ok(), "market sell should be accepted");
  require(result.trades.size() == 3U,
          "market sell should consume two price levels");
  require_trade(result.trades.at(0), OrderId{10}, OrderId{1}, Price{102},
                Quantity{2});
  require_trade(result.trades.at(1), OrderId{10}, OrderId{2}, Price{102},
                Quantity{3});
  require_trade(result.trades.at(2), OrderId{10}, OrderId{3}, Price{101},
                Quantity{1});
  require(engine.best_bid() == Price{101},
          "partially filled second level should remain");
  require(!engine.best_ask().has_value(), "market sell should never rest");
  require_not_crossed(engine);
}

void unfilled_market_remainder_is_cancelled_and_not_indexed() {
  MatchingEngine engine;
  require(engine
              .submit_limit(OrderRequest{.id = OrderId{1},
                                         .side = Side::Sell,
                                         .price = Price{100},
                                         .quantity = Quantity{2}})
              .ok(),
          "ask should rest");

  const auto result = engine.submit_market(MarketOrderRequest{
      .id = OrderId{10}, .side = Side::Buy, .quantity = Quantity{5}});

  require(result.ok(), "partially filled market buy should be accepted");
  require(result.trades.size() == 1U, "available ask should produce one trade");
  require_trade(result.trades.at(0), OrderId{10}, OrderId{1}, Price{100},
                Quantity{2});
  require(!engine.best_ask().has_value(),
          "fully consumed ask level should be removed");
  require(!engine.best_bid().has_value(),
          "unfilled market remainder should not become a bid");
  require(engine.cancel(OrderId{10}) == ErrorCode::UnknownOrderId,
          "market order should never enter the active-order index");

  require(engine
              .submit_limit(OrderRequest{.id = OrderId{10},
                                         .side = Side::Buy,
                                         .price = Price{90},
                                         .quantity = Quantity{1}})
              .ok(),
          "market order ID should be immediately reusable");
  require_not_crossed(engine);
}

void market_order_on_empty_book_is_accepted_without_resting() {
  MatchingEngine engine;

  const auto result = engine.submit_market(MarketOrderRequest{
      .id = OrderId{1}, .side = Side::Sell, .quantity = Quantity{3}});

  require(result.ok(), "market order on empty book should be accepted");
  require(result.trades.empty(), "empty book should produce no trades");
  require(!engine.best_bid().has_value(),
          "empty-book market sell should not create a bid");
  require(!engine.best_ask().has_value(),
          "empty-book market sell should not create an ask");
  require(engine.cancel(OrderId{1}) == ErrorCode::UnknownOrderId,
          "empty-book market order should not be indexed");
}

void duplicate_active_id_market_order_is_rejected_without_mutation() {
  MatchingEngine engine;
  require(engine
              .submit_limit(OrderRequest{.id = OrderId{1},
                                         .side = Side::Buy,
                                         .price = Price{100},
                                         .quantity = Quantity{2}})
              .ok(),
          "original bid should rest");

  const auto duplicate = engine.submit_market(MarketOrderRequest{
      .id = OrderId{1}, .side = Side::Sell, .quantity = Quantity{2}});

  require(duplicate.error == ErrorCode::DuplicateOrderId,
          "market order with active ID should be rejected");
  require(duplicate.trades.empty(),
          "rejected duplicate market order should not trade");
  require(engine.best_bid() == Price{100},
          "duplicate rejection should preserve the original bid");

  const auto valid = engine.submit_market(MarketOrderRequest{
      .id = OrderId{2}, .side = Side::Sell, .quantity = Quantity{2}});
  require(valid.ok(), "unique market order should still be accepted");
  require(valid.trades.size() == 1U,
          "original bid should remain matchable after duplicate rejection");
  require_trade(valid.trades.at(0), OrderId{2}, OrderId{1}, Price{100},
                Quantity{2});
}

void empty_book_has_empty_depth_and_snapshot() {
  const MatchingEngine engine;

  require(engine.depth(Side::Buy, 5U).empty(),
          "empty book should have no bid depth");
  require(engine.depth(Side::Sell, 5U).empty(),
          "empty book should have no ask depth");
  require(engine.depth(Side::Buy, 0U).empty(),
          "zero requested levels should return empty depth");
  require(engine.snapshot().orders.empty(),
          "empty book should have an empty snapshot");
}

void depth_aggregates_and_orders_top_price_levels() {
  MatchingEngine engine;
  require(engine
              .submit_limit(OrderRequest{.id = OrderId{1},
                                         .side = Side::Buy,
                                         .price = Price{100},
                                         .quantity = Quantity{2}})
              .ok(),
          "first best bid should rest");
  require(engine
              .submit_limit(OrderRequest{.id = OrderId{2},
                                         .side = Side::Buy,
                                         .price = Price{100},
                                         .quantity = Quantity{3}})
              .ok(),
          "second best bid should rest");
  require(engine
              .submit_limit(OrderRequest{.id = OrderId{3},
                                         .side = Side::Buy,
                                         .price = Price{99},
                                         .quantity = Quantity{4}})
              .ok(),
          "second bid level should rest");
  require(engine
              .submit_limit(OrderRequest{.id = OrderId{4},
                                         .side = Side::Buy,
                                         .price = Price{98},
                                         .quantity = Quantity{5}})
              .ok(),
          "third bid level should rest");
  require(engine
              .submit_limit(OrderRequest{.id = OrderId{5},
                                         .side = Side::Sell,
                                         .price = Price{105},
                                         .quantity = Quantity{6}})
              .ok(),
          "first best ask should rest");
  require(engine
              .submit_limit(OrderRequest{.id = OrderId{6},
                                         .side = Side::Sell,
                                         .price = Price{105},
                                         .quantity = Quantity{7}})
              .ok(),
          "second best ask should rest");
  require(engine
              .submit_limit(OrderRequest{.id = OrderId{7},
                                         .side = Side::Sell,
                                         .price = Price{106},
                                         .quantity = Quantity{8}})
              .ok(),
          "second ask level should rest");

  const auto bid_depth = engine.depth(Side::Buy, 2U);
  require(bid_depth.size() == 2U, "bid depth should honor the level limit");
  require_depth_level(bid_depth.at(0), Price{100}, Quantity{5});
  require_depth_level(bid_depth.at(1), Price{99}, Quantity{4});

  const auto ask_depth = engine.depth(Side::Sell, 10U);
  require(ask_depth.size() == 2U,
          "ask depth should return all available levels");
  require_depth_level(ask_depth.at(0), Price{105}, Quantity{13});
  require_depth_level(ask_depth.at(1), Price{106}, Quantity{8});

  require(engine.depth(Side::Sell, 0U).empty(),
          "zero-level ask depth should be empty");
}

void depth_rejects_aggregate_quantity_overflow() {
  MatchingEngine engine;
  require(engine
              .submit_limit(OrderRequest{
                  .id = OrderId{1},
                  .side = Side::Buy,
                  .price = Price{100},
                  .quantity = std::numeric_limits<Quantity>::max()})
              .ok(),
          "maximum-quantity bid should rest");
  require(engine
              .submit_limit(OrderRequest{.id = OrderId{2},
                                         .side = Side::Buy,
                                         .price = Price{100},
                                         .quantity = Quantity{1}})
              .ok(),
          "second same-price bid should rest");

  bool overflow_detected = false;
  try {
    (void)engine.depth(Side::Buy, 1U);
  } catch (const std::overflow_error &) {
    overflow_detected = true;
  }

  require(overflow_detected,
          "depth should reject an unrepresentable aggregate quantity");
  require(engine.snapshot().orders.size() == 2U,
          "failed depth query should not mutate the book");
}

void snapshot_is_deterministic_in_side_price_fifo_order() {
  MatchingEngine engine;
  require(engine
              .submit_limit(OrderRequest{.id = OrderId{1},
                                         .side = Side::Sell,
                                         .price = Price{106},
                                         .quantity = Quantity{1}})
              .ok(),
          "first ask should rest");
  require(engine
              .submit_limit(OrderRequest{.id = OrderId{2},
                                         .side = Side::Buy,
                                         .price = Price{99},
                                         .quantity = Quantity{2}})
              .ok(),
          "first bid should rest");
  require(engine
              .submit_limit(OrderRequest{.id = OrderId{3},
                                         .side = Side::Sell,
                                         .price = Price{105},
                                         .quantity = Quantity{3}})
              .ok(),
          "best ask should rest");
  require(engine
              .submit_limit(OrderRequest{.id = OrderId{4},
                                         .side = Side::Buy,
                                         .price = Price{100},
                                         .quantity = Quantity{4}})
              .ok(),
          "best bid should rest");
  require(engine
              .submit_limit(OrderRequest{.id = OrderId{5},
                                         .side = Side::Buy,
                                         .price = Price{100},
                                         .quantity = Quantity{5}})
              .ok(),
          "second best-price bid should rest");
  require(engine
              .submit_limit(OrderRequest{.id = OrderId{6},
                                         .side = Side::Sell,
                                         .price = Price{105},
                                         .quantity = Quantity{6}})
              .ok(),
          "second best-price ask should rest");

  const BookSnapshot first = engine.snapshot();
  const BookSnapshot second = engine.snapshot();

  require(first.orders.size() == 6U,
          "snapshot should contain every resting order");
  require(second.orders.size() == first.orders.size(),
          "repeated snapshot should have the same size");
  require_snapshot_order(first.orders.at(0), OrderId{4}, Side::Buy, Price{100},
                         Quantity{4}, SequenceNumber{3});
  require_snapshot_order(first.orders.at(1), OrderId{5}, Side::Buy, Price{100},
                         Quantity{5}, SequenceNumber{4});
  require_snapshot_order(first.orders.at(2), OrderId{2}, Side::Buy, Price{99},
                         Quantity{2}, SequenceNumber{1});
  require_snapshot_order(first.orders.at(3), OrderId{3}, Side::Sell, Price{105},
                         Quantity{3}, SequenceNumber{2});
  require_snapshot_order(first.orders.at(4), OrderId{6}, Side::Sell, Price{105},
                         Quantity{6}, SequenceNumber{5});
  require_snapshot_order(first.orders.at(5), OrderId{1}, Side::Sell, Price{106},
                         Quantity{1}, SequenceNumber{0});

  for (std::size_t index = 0; index < first.orders.size(); ++index) {
    const SnapshotOrder &expected = first.orders.at(index);
    require_snapshot_order(second.orders.at(index), expected.id, expected.side,
                           expected.price, expected.quantity,
                           expected.sequence);
  }
}

void snapshot_tracks_lifecycle_and_sequence_rules() {
  MatchingEngine engine;
  require(engine
              .submit_limit(OrderRequest{.id = OrderId{1},
                                         .side = Side::Sell,
                                         .price = Price{105},
                                         .quantity = Quantity{5}})
              .ok(),
          "first ask should rest");
  require(engine
              .submit_limit(OrderRequest{.id = OrderId{2},
                                         .side = Side::Sell,
                                         .price = Price{105},
                                         .quantity = Quantity{3}})
              .ok(),
          "second ask should rest");

  const auto market_result = engine.submit_market(MarketOrderRequest{
      .id = OrderId{90}, .side = Side::Buy, .quantity = Quantity{2}});
  require(market_result.ok(), "market order should partially fill first ask");
  require(engine.cancel(OrderId{2}) == ErrorCode::None,
          "second ask should cancel");
  require(engine
              .submit_limit(OrderRequest{.id = OrderId{3},
                                         .side = Side::Buy,
                                         .price = Price{100},
                                         .quantity = Quantity{4}})
              .ok(),
          "bid should rest");

  const auto reduce_result = engine.replace(ReplaceRequest{
      .id = OrderId{3}, .new_price = Price{100}, .new_quantity = Quantity{2}});
  require(reduce_result.ok(), "same-price reduction should succeed");

  const auto reduced_snapshot = engine.snapshot();
  require(reduced_snapshot.orders.size() == 2U,
          "snapshot should omit market and cancelled orders");
  require_snapshot_order(reduced_snapshot.orders.at(0), OrderId{3}, Side::Buy,
                         Price{100}, Quantity{2}, SequenceNumber{2});
  require_snapshot_order(reduced_snapshot.orders.at(1), OrderId{1}, Side::Sell,
                         Price{105}, Quantity{3}, SequenceNumber{0});

  const auto reprice_result = engine.replace(ReplaceRequest{
      .id = OrderId{3}, .new_price = Price{101}, .new_quantity = Quantity{2}});
  require(reprice_result.ok(), "non-crossing price change should succeed");

  const auto repriced_snapshot = engine.snapshot();
  require_snapshot_order(repriced_snapshot.orders.at(0), OrderId{3}, Side::Buy,
                         Price{101}, Quantity{2}, SequenceNumber{3});
  require_snapshot_order(repriced_snapshot.orders.at(1), OrderId{1}, Side::Sell,
                         Price{105}, Quantity{3}, SequenceNumber{0});

  const auto bid_depth = engine.depth(Side::Buy, 1U);
  const auto ask_depth = engine.depth(Side::Sell, 1U);
  require_depth_level(bid_depth.at(0), Price{101}, Quantity{2});
  require_depth_level(ask_depth.at(0), Price{105}, Quantity{3});
  require_not_crossed(engine);
}

void add_three_same_price_asks(MatchingEngine &engine) {
  for (const OrderId id : {OrderId{1}, OrderId{2}, OrderId{3}}) {
    require(engine
                .submit_limit(OrderRequest{.id = id,
                                           .side = Side::Sell,
                                           .price = Price{100},
                                           .quantity = Quantity{1}})
                .ok(),
            "same-price ask should rest");
  }
}

void cancel_removes_head_middle_and_tail_orders() {
  {
    MatchingEngine engine;
    add_three_same_price_asks(engine);

    require(engine.cancel(OrderId{1}) == ErrorCode::None,
            "cancelling the head order should succeed");
    const auto result =
        engine.submit_limit(OrderRequest{.id = OrderId{10},
                                         .side = Side::Buy,
                                         .price = Price{100},
                                         .quantity = Quantity{2}});

    require(result.trades.size() == 2U, "two non-cancelled asks should match");
    require_trade(result.trades.at(0), OrderId{10}, OrderId{2}, Price{100},
                  Quantity{1});
    require_trade(result.trades.at(1), OrderId{10}, OrderId{3}, Price{100},
                  Quantity{1});
    require(!engine.best_ask().has_value(),
            "cancelled head should not remain in the level");
  }

  {
    MatchingEngine engine;
    add_three_same_price_asks(engine);

    require(engine.cancel(OrderId{2}) == ErrorCode::None,
            "cancelling the middle order should succeed");
    const auto result =
        engine.submit_limit(OrderRequest{.id = OrderId{10},
                                         .side = Side::Buy,
                                         .price = Price{100},
                                         .quantity = Quantity{2}});

    require(result.trades.size() == 2U, "two non-cancelled asks should match");
    require_trade(result.trades.at(0), OrderId{10}, OrderId{1}, Price{100},
                  Quantity{1});
    require_trade(result.trades.at(1), OrderId{10}, OrderId{3}, Price{100},
                  Quantity{1});
    require(!engine.best_ask().has_value(),
            "cancelled middle should not remain in the level");
  }

  {
    MatchingEngine engine;
    add_three_same_price_asks(engine);

    require(engine.cancel(OrderId{3}) == ErrorCode::None,
            "cancelling the tail order should succeed");
    const auto result =
        engine.submit_limit(OrderRequest{.id = OrderId{10},
                                         .side = Side::Buy,
                                         .price = Price{100},
                                         .quantity = Quantity{2}});

    require(result.trades.size() == 2U, "two non-cancelled asks should match");
    require_trade(result.trades.at(0), OrderId{10}, OrderId{1}, Price{100},
                  Quantity{1});
    require_trade(result.trades.at(1), OrderId{10}, OrderId{2}, Price{100},
                  Quantity{1});
    require(!engine.best_ask().has_value(),
            "cancelled tail should not remain in the level");
  }
}

void cancelling_last_order_removes_level_and_index_entry() {
  MatchingEngine engine;
  require(engine
              .submit_limit(OrderRequest{.id = OrderId{1},
                                         .side = Side::Buy,
                                         .price = Price{101},
                                         .quantity = Quantity{1}})
              .ok(),
          "best bid should rest");
  require(engine
              .submit_limit(OrderRequest{.id = OrderId{2},
                                         .side = Side::Buy,
                                         .price = Price{100},
                                         .quantity = Quantity{1}})
              .ok(),
          "second bid should rest");

  require(engine.cancel(OrderId{1}) == ErrorCode::None,
          "cancelling the only order at the best level should succeed");
  require(engine.best_bid() == Price{100},
          "cancelling the last order should remove its price level");
  require(engine.cancel(OrderId{2}) == ErrorCode::None,
          "cancelling the final bid should succeed");
  require(!engine.best_bid().has_value(),
          "book should have no bid after final cancellation");

  require(engine
              .submit_limit(OrderRequest{.id = OrderId{1},
                                         .side = Side::Sell,
                                         .price = Price{105},
                                         .quantity = Quantity{1}})
              .ok(),
          "cancelled ID should be reusable without a stale index entry");
}

void unknown_cancel_returns_error_without_mutation() {
  MatchingEngine engine;
  require(engine
              .submit_limit(OrderRequest{.id = OrderId{1},
                                         .side = Side::Buy,
                                         .price = Price{100},
                                         .quantity = Quantity{2}})
              .ok(),
          "bid should rest");

  require(engine.cancel(OrderId{999}) == ErrorCode::UnknownOrderId,
          "unknown cancellation should return an explicit error");
  require(engine.best_bid() == Price{100},
          "unknown cancellation should not change the book");

  const auto result =
      engine.submit_limit(OrderRequest{.id = OrderId{2},
                                       .side = Side::Sell,
                                       .price = Price{100},
                                       .quantity = Quantity{2}});
  require(result.trades.size() == 1U,
          "original bid should remain matchable after unknown cancellation");
  require_trade(result.trades.at(0), OrderId{2}, OrderId{1}, Price{100},
                Quantity{2});
}

void same_price_non_increasing_replace_preserves_priority() {
  MatchingEngine engine;
  require(engine
              .submit_limit(OrderRequest{.id = OrderId{1},
                                         .side = Side::Sell,
                                         .price = Price{100},
                                         .quantity = Quantity{3}})
              .ok(),
          "first ask should rest");
  require(engine
              .submit_limit(OrderRequest{.id = OrderId{2},
                                         .side = Side::Sell,
                                         .price = Price{100},
                                         .quantity = Quantity{3}})
              .ok(),
          "second ask should rest");

  const auto no_op_result = engine.replace(ReplaceRequest{
      .id = OrderId{1}, .new_price = Price{100}, .new_quantity = Quantity{3}});
  require(no_op_result.ok(), "same price and quantity should be a no-op");
  require(no_op_result.trades.empty(), "no-op replacement should not trade");

  const auto reduce_result = engine.replace(ReplaceRequest{
      .id = OrderId{1}, .new_price = Price{100}, .new_quantity = Quantity{2}});
  require(reduce_result.ok(), "same-price quantity reduction should succeed");
  require(reduce_result.trades.empty(),
          "same-price quantity reduction should not trade");

  const auto match_result =
      engine.submit_limit(OrderRequest{.id = OrderId{10},
                                       .side = Side::Buy,
                                       .price = Price{100},
                                       .quantity = Quantity{3}});
  require(match_result.trades.size() == 2U,
          "reduced head and next ask should both match");
  require_trade(match_result.trades.at(0), OrderId{10}, OrderId{1}, Price{100},
                Quantity{2});
  require_trade(match_result.trades.at(1), OrderId{10}, OrderId{2}, Price{100},
                Quantity{1});
}

void quantity_increase_loses_priority_without_stale_index_entry() {
  MatchingEngine engine;
  require(engine
              .submit_limit(OrderRequest{.id = OrderId{1},
                                         .side = Side::Sell,
                                         .price = Price{100},
                                         .quantity = Quantity{1}})
              .ok(),
          "first ask should rest");
  require(engine
              .submit_limit(OrderRequest{.id = OrderId{2},
                                         .side = Side::Sell,
                                         .price = Price{100},
                                         .quantity = Quantity{1}})
              .ok(),
          "second ask should rest");

  const auto replace_result = engine.replace(ReplaceRequest{
      .id = OrderId{1}, .new_price = Price{100}, .new_quantity = Quantity{2}});
  require(replace_result.ok(), "quantity increase should succeed");
  require(replace_result.trades.empty(),
          "non-crossing quantity increase should not trade");

  const auto match_result =
      engine.submit_limit(OrderRequest{.id = OrderId{10},
                                       .side = Side::Buy,
                                       .price = Price{100},
                                       .quantity = Quantity{2}});
  require(match_result.trades.size() == 2U,
          "newer order and replaced order should both match");
  require_trade(match_result.trades.at(0), OrderId{10}, OrderId{2}, Price{100},
                Quantity{1});
  require_trade(match_result.trades.at(1), OrderId{10}, OrderId{1}, Price{100},
                Quantity{1});

  require(engine.cancel(OrderId{1}) == ErrorCode::None,
          "remaining replacement should have a valid index entry");
  require(engine
              .submit_limit(OrderRequest{.id = OrderId{1},
                                         .side = Side::Sell,
                                         .price = Price{101},
                                         .quantity = Quantity{1}})
              .ok(),
          "replaced then cancelled ID should be reusable");
}

void price_change_loses_priority_at_new_level() {
  MatchingEngine engine;
  require(engine
              .submit_limit(OrderRequest{.id = OrderId{1},
                                         .side = Side::Sell,
                                         .price = Price{101},
                                         .quantity = Quantity{1}})
              .ok(),
          "order to be repriced should rest");
  require(engine
              .submit_limit(OrderRequest{.id = OrderId{2},
                                         .side = Side::Sell,
                                         .price = Price{100},
                                         .quantity = Quantity{1}})
              .ok(),
          "existing order at replacement price should rest");

  const auto replace_result = engine.replace(ReplaceRequest{
      .id = OrderId{1}, .new_price = Price{100}, .new_quantity = Quantity{1}});
  require(replace_result.ok(), "price-changing replacement should succeed");
  require(replace_result.trades.empty(),
          "non-crossing price change should not trade");

  const auto match_result =
      engine.submit_limit(OrderRequest{.id = OrderId{10},
                                       .side = Side::Buy,
                                       .price = Price{100},
                                       .quantity = Quantity{2}});
  require(match_result.trades.size() == 2U,
          "both asks at the replacement price should match");
  require_trade(match_result.trades.at(0), OrderId{10}, OrderId{2}, Price{100},
                Quantity{1});
  require_trade(match_result.trades.at(1), OrderId{10}, OrderId{1}, Price{100},
                Quantity{1});
}

void crossing_price_change_matches_immediately() {
  MatchingEngine engine;
  require(engine
              .submit_limit(OrderRequest{.id = OrderId{1},
                                         .side = Side::Buy,
                                         .price = Price{95},
                                         .quantity = Quantity{3}})
              .ok(),
          "bid to be repriced should rest");
  require(engine
              .submit_limit(OrderRequest{.id = OrderId{2},
                                         .side = Side::Sell,
                                         .price = Price{100},
                                         .quantity = Quantity{3}})
              .ok(),
          "opposite ask should rest");

  const auto replace_result = engine.replace(ReplaceRequest{
      .id = OrderId{1}, .new_price = Price{101}, .new_quantity = Quantity{3}});
  require(replace_result.ok(), "crossing price change should succeed");
  require(replace_result.trades.size() == 1U,
          "crossing price change should trade immediately");
  require_trade(replace_result.trades.at(0), OrderId{1}, OrderId{2}, Price{100},
                Quantity{3});
  require(!engine.best_bid().has_value(),
          "fully filled replacement should not rest");
  require(!engine.best_ask().has_value(),
          "fully filled resting ask should be removed");

  require(engine
              .submit_limit(OrderRequest{.id = OrderId{1},
                                         .side = Side::Buy,
                                         .price = Price{90},
                                         .quantity = Quantity{1}})
              .ok(),
          "fully filled replacement ID should be reusable");
}

void invalid_and_unknown_replacements_do_not_mutate_book() {
  MatchingEngine engine;
  require(engine
              .submit_limit(OrderRequest{.id = OrderId{1},
                                         .side = Side::Buy,
                                         .price = Price{100},
                                         .quantity = Quantity{5}})
              .ok(),
          "original bid should rest");

  require(engine.replace(ReplaceRequest{.id = OrderId{1},
                                        .new_price = Price{101},
                                        .new_quantity = Quantity{0}})
                  .error == ErrorCode::InvalidQuantity,
          "zero replacement quantity should be rejected");
  require(engine.replace(ReplaceRequest{.id = OrderId{1},
                                        .new_price = Price{0},
                                        .new_quantity = Quantity{5}})
                  .error == ErrorCode::InvalidPrice,
          "invalid replacement price should be rejected");
  require(engine.replace(ReplaceRequest{.id = OrderId{999},
                                        .new_price = Price{101},
                                        .new_quantity = Quantity{5}})
                  .error == ErrorCode::UnknownOrderId,
          "unknown replacement should return an explicit error");

  const auto match_result =
      engine.submit_limit(OrderRequest{.id = OrderId{2},
                                       .side = Side::Sell,
                                       .price = Price{100},
                                       .quantity = Quantity{5}});
  require(match_result.trades.size() == 1U,
          "original bid should remain after rejected replacements");
  require_trade(match_result.trades.at(0), OrderId{2}, OrderId{1}, Price{100},
                Quantity{5});
}

struct ExpectedOrderState {
  Side side;
  Price price;
  Quantity quantity;
  SequenceNumber sequence;
};

using ExpectedOrders = std::unordered_map<OrderId, ExpectedOrderState>;

[[noreturn]] void
fail_randomized_test(std::uint32_t seed, std::size_t step,
                     const std::vector<std::string> &operations,
                     std::string_view message) {
  std::ostringstream output;
  output << message << "\nseed=" << seed << " step=" << step << "\noperations:";
  for (std::size_t index = 0; index < operations.size(); ++index) {
    output << "\n  " << index << ": " << operations.at(index);
  }
  throw std::runtime_error(output.str());
}

void require_randomized(bool condition, std::uint32_t seed, std::size_t step,
                        const std::vector<std::string> &operations,
                        std::string_view message) {
  if (!condition) {
    fail_randomized_test(seed, step, operations, message);
  }
}

Quantity apply_randomized_trades(const std::vector<Trade> &trades,
                                 OrderId aggressor_id, Side aggressor_side,
                                 Quantity incoming_quantity,
                                 ExpectedOrders &orders, std::uint32_t seed,
                                 std::size_t step,
                                 const std::vector<std::string> &operations) {
  Quantity executed{};

  for (const Trade &trade : trades) {
    require_randomized(trade.aggressor_id == aggressor_id, seed, step,
                       operations, "trade has wrong aggressor ID");
    require_randomized(trade.quantity > Quantity{0}, seed, step, operations,
                       "trade has zero quantity");
    require_randomized(trade.quantity <= incoming_quantity - executed, seed,
                       step, operations,
                       "executed quantity exceeds incoming quantity");

    const auto resting = orders.find(trade.resting_id);
    require_randomized(resting != orders.end(), seed, step, operations,
                       "trade references unknown resting order");
    require_randomized(resting->second.side != aggressor_side, seed, step,
                       operations, "trade matches the same side");
    require_randomized(resting->second.price == trade.price, seed, step,
                       operations, "trade does not use resting price");
    require_randomized(trade.quantity <= resting->second.quantity, seed, step,
                       operations, "trade exceeds resting order quantity");

    executed += trade.quantity;
    resting->second.quantity -= trade.quantity;
    if (resting->second.quantity == Quantity{0}) {
      orders.erase(resting);
    }
  }

  return executed;
}

void verify_randomized_state(MatchingEngine &engine,
                             const ExpectedOrders &orders, OrderId maximum_id,
                             std::uint32_t seed, std::size_t step,
                             const std::vector<std::string> &operations) {
  struct ExpectedSnapshotOrder {
    OrderId id;
    ExpectedOrderState order;
  };

  std::vector<ExpectedSnapshotOrder> expected_snapshot;
  expected_snapshot.reserve(orders.size());
  for (const auto &[id, order] : orders) {
    require_randomized(order.quantity > Quantity{0}, seed, step, operations,
                       "reference model contains zero quantity");
    expected_snapshot.push_back(
        ExpectedSnapshotOrder{.id = id, .order = order});
  }

  std::sort(expected_snapshot.begin(), expected_snapshot.end(),
            [](const ExpectedSnapshotOrder &left,
               const ExpectedSnapshotOrder &right) {
              if (left.order.side != right.order.side) {
                return left.order.side == Side::Buy;
              }
              if (left.order.price != right.order.price) {
                if (left.order.side == Side::Buy) {
                  return left.order.price > right.order.price;
                }
                return left.order.price < right.order.price;
              }
              return left.order.sequence < right.order.sequence;
            });

  const auto compare_snapshot = [&](const BookSnapshot &snapshot,
                                    std::string_view size_error,
                                    std::string_view value_error) {
    require_randomized(snapshot.orders.size() == expected_snapshot.size(), seed,
                       step, operations, size_error);
    for (std::size_t index = 0; index < snapshot.orders.size(); ++index) {
      const SnapshotOrder &actual = snapshot.orders.at(index);
      const ExpectedSnapshotOrder &expected = expected_snapshot.at(index);
      require_randomized(actual.id == expected.id &&
                             actual.side == expected.order.side &&
                             actual.price == expected.order.price &&
                             actual.quantity == expected.order.quantity &&
                             actual.sequence == expected.order.sequence,
                         seed, step, operations, value_error);
    }
  };

  compare_snapshot(engine.snapshot(), "snapshot size disagrees with model",
                   "snapshot content or order disagrees with model");

  std::map<Price, Quantity, std::greater<Price>> expected_bids;
  std::map<Price, Quantity, std::less<Price>> expected_asks;
  for (const auto &[id, order] : orders) {
    (void)id;
    if (order.side == Side::Buy) {
      expected_bids[order.price] += order.quantity;
    } else {
      expected_asks[order.price] += order.quantity;
    }
  }

  const auto bid_depth = engine.depth(Side::Buy, expected_bids.size() + 1U);
  require_randomized(bid_depth.size() == expected_bids.size(), seed, step,
                     operations, "bid depth level count disagrees with model");
  std::size_t depth_index = 0;
  for (const auto &[price, quantity] : expected_bids) {
    require_randomized(bid_depth.at(depth_index).price == price &&
                           bid_depth.at(depth_index).quantity == quantity,
                       seed, step, operations,
                       "bid depth disagrees with model");
    ++depth_index;
  }

  const auto ask_depth = engine.depth(Side::Sell, expected_asks.size() + 1U);
  require_randomized(ask_depth.size() == expected_asks.size(), seed, step,
                     operations, "ask depth level count disagrees with model");
  depth_index = 0;
  for (const auto &[price, quantity] : expected_asks) {
    require_randomized(ask_depth.at(depth_index).price == price &&
                           ask_depth.at(depth_index).quantity == quantity,
                       seed, step, operations,
                       "ask depth disagrees with model");
    ++depth_index;
  }

  const auto best_bid = engine.best_bid();
  const auto best_ask = engine.best_ask();
  require_randomized(
      best_bid == (expected_bids.empty()
                       ? std::optional<Price>{}
                       : std::optional<Price>{expected_bids.begin()->first}),
      seed, step, operations, "best bid disagrees with model");
  require_randomized(
      best_ask == (expected_asks.empty()
                       ? std::optional<Price>{}
                       : std::optional<Price>{expected_asks.begin()->first}),
      seed, step, operations, "best ask disagrees with model");
  if (best_bid.has_value() && best_ask.has_value()) {
    require_randomized(*best_bid < *best_ask, seed, step, operations,
                       "book is crossed");
  }

  for (OrderId id = OrderId{1}; id <= maximum_id; ++id) {
    const auto expected = orders.find(id);
    if (expected == orders.end()) {
      const auto result = engine.replace(ReplaceRequest{
          .id = id, .new_price = Price{100}, .new_quantity = Quantity{1}});
      require_randomized(result.error == ErrorCode::UnknownOrderId, seed, step,
                         operations,
                         "inactive ID still appears in order index");
      continue;
    }

    const auto result = engine.replace(
        ReplaceRequest{.id = id,
                       .new_price = expected->second.price,
                       .new_quantity = expected->second.quantity});
    require_randomized(result.ok() && result.trades.empty(), seed, step,
                       operations,
                       "active order is missing or stale in order index");
  }

  compare_snapshot(engine.snapshot(), "index checks changed snapshot size",
                   "index checks changed snapshot content");
}

void deterministic_randomized_events_preserve_invariants() {
  constexpr std::uint32_t seed = 0x00C0FFEEU;
  constexpr std::size_t event_count = 400U;
  constexpr OrderId maximum_id = OrderId{32};

  std::mt19937 generator{seed};
  MatchingEngine engine;
  ExpectedOrders orders;
  SequenceNumber next_sequence{};
  std::vector<std::string> operations;
  operations.reserve(event_count);

  for (std::size_t step = 0; step < event_count; ++step) {
    const std::uint32_t operation = generator() % 4U;
    const OrderId id =
        OrderId{1} + static_cast<OrderId>(generator() % maximum_id);
    const Side side = generator() % 2U == 0U ? Side::Buy : Side::Sell;
    const Price price =
        Price{95} + static_cast<Price>(generator() % std::uint32_t{11});
    const Quantity quantity =
        Quantity{1} + static_cast<Quantity>(generator() % std::uint32_t{10});

    std::ostringstream event;
    if (operation == 0U) {
      event << "limit id=" << id
            << " side=" << (side == Side::Buy ? "buy" : "sell")
            << " price=" << price << " quantity=" << quantity;
      operations.push_back(event.str());

      const bool duplicate = orders.contains(id);
      const auto result = engine.submit_limit(OrderRequest{
          .id = id, .side = side, .price = price, .quantity = quantity});
      if (duplicate) {
        require_randomized(
            result.error == ErrorCode::DuplicateOrderId &&
                result.trades.empty(),
            seed, step, operations,
            "duplicate limit order did not fail without mutation");
      } else {
        require_randomized(result.ok(), seed, step, operations,
                           "valid limit order failed");
        const Quantity executed = apply_randomized_trades(
            result.trades, id, side, quantity, orders, seed, step, operations);
        const Quantity remainder = quantity - executed;
        if (remainder > Quantity{0}) {
          orders.emplace(id, ExpectedOrderState{.side = side,
                                                .price = price,
                                                .quantity = remainder,
                                                .sequence = next_sequence});
          ++next_sequence;
        }
      }
    } else if (operation == 1U) {
      event << "market id=" << id
            << " side=" << (side == Side::Buy ? "buy" : "sell")
            << " quantity=" << quantity;
      operations.push_back(event.str());

      const bool duplicate = orders.contains(id);
      const auto result = engine.submit_market(
          MarketOrderRequest{.id = id, .side = side, .quantity = quantity});
      if (duplicate) {
        require_randomized(
            result.error == ErrorCode::DuplicateOrderId &&
                result.trades.empty(),
            seed, step, operations,
            "duplicate market order did not fail without mutation");
      } else {
        require_randomized(result.ok(), seed, step, operations,
                           "valid market order failed");
        (void)apply_randomized_trades(result.trades, id, side, quantity, orders,
                                      seed, step, operations);
      }
    } else if (operation == 2U) {
      event << "cancel id=" << id;
      operations.push_back(event.str());

      const bool active = orders.contains(id);
      const ErrorCode error = engine.cancel(id);
      require_randomized(
          error == (active ? ErrorCode::None : ErrorCode::UnknownOrderId), seed,
          step, operations, "cancel result disagrees with model");
      if (active) {
        orders.erase(id);
      }
    } else {
      event << "replace id=" << id << " price=" << price
            << " quantity=" << quantity;
      operations.push_back(event.str());

      const auto existing = orders.find(id);
      const auto result = engine.replace(ReplaceRequest{
          .id = id, .new_price = price, .new_quantity = quantity});
      if (existing == orders.end()) {
        require_randomized(result.error == ErrorCode::UnknownOrderId, seed,
                           step, operations,
                           "unknown replacement did not return error");
      } else {
        require_randomized(result.ok(), seed, step, operations,
                           "valid replacement failed");
        const ExpectedOrderState original = existing->second;
        if (price == original.price && quantity <= original.quantity) {
          require_randomized(result.trades.empty(), seed, step, operations,
                             "priority-preserving replacement traded");
          existing->second.quantity = quantity;
        } else {
          orders.erase(existing);
          const Quantity executed =
              apply_randomized_trades(result.trades, id, original.side,
                                      quantity, orders, seed, step, operations);
          const Quantity remainder = quantity - executed;
          if (remainder > Quantity{0}) {
            orders.emplace(id, ExpectedOrderState{.side = original.side,
                                                  .price = price,
                                                  .quantity = remainder,
                                                  .sequence = next_sequence});
            ++next_sequence;
          }
        }
      }
    }

    verify_randomized_state(engine, orders, maximum_id, seed, step, operations);
  }
}

void invalid_submit_paths_return_validation_errors() {
  MatchingEngine engine;
  const OrderRequest invalid_limit{.id = OrderId{1},
                                   .side = Side::Buy,
                                   .price = Price{0},
                                   .quantity = Quantity{10}};
  const MarketOrderRequest invalid_market{
      .id = OrderId{2}, .side = Side::Sell, .quantity = Quantity{0}};

  require(engine.submit_limit(invalid_limit).error == ErrorCode::InvalidPrice,
          "submit_limit should reject invalid price before matching exists");
  require(
      engine.submit_market(invalid_market).error == ErrorCode::InvalidQuantity,
      "submit_market should reject invalid quantity before matching exists");
}

void run_test(std::string_view name, TestFunction test, int &failures) {
  try {
    test();
    std::cout << "[PASS] " << name << '\n';
  } catch (const std::exception &error) {
    ++failures;
    std::cerr << "[FAIL] " << name << ": " << error.what() << '\n';
  } catch (...) {
    ++failures;
    std::cerr << "[FAIL] " << name << ": unknown exception\n";
  }
}

} // namespace

int main() {
  int failures = 0;
  const std::vector<std::pair<std::string_view, TestFunction>> tests{
      {"domain aliases use fixed-width integers",
       domain_aliases_use_fixed_width_integers},
      {"valid limit order request passes validation",
       valid_limit_order_request_passes_validation},
      {"zero quantities are rejected", zero_quantities_are_rejected},
      {"invalid limit prices are rejected", invalid_limit_prices_are_rejected},
      {"empty engine has no best prices", empty_engine_has_no_best_prices},
      {"non-marketable limit orders rest and update best prices",
       non_marketable_limit_orders_rest_and_update_best_prices},
      {"exact price match executes and removes filled resting order",
       exact_price_match_executes_and_removes_filled_resting_order},
      {"buy limit matches multiple ask price levels",
       buy_limit_matches_multiple_ask_price_levels},
      {"sell limit matches multiple bid price levels",
       sell_limit_matches_multiple_bid_price_levels},
      {"partial fill leaves resting order active",
       partial_fill_leaves_resting_order_active},
      {"incoming remainder rests after partial fill",
       incoming_remainder_rests_after_partial_fill},
      {"FIFO priority is preserved at same price",
       fifo_priority_is_preserved_at_same_price},
      {"sell trade uses resting bid price", sell_trade_uses_resting_bid_price},
      {"duplicate active order ID is rejected without mutation",
       duplicate_active_order_id_is_rejected_without_mutation},
      {"market buy consumes asks in price-time priority",
       market_buy_consumes_asks_in_price_time_priority},
      {"market sell consumes bids in price-time priority",
       market_sell_consumes_bids_in_price_time_priority},
      {"unfilled market remainder is cancelled and not indexed",
       unfilled_market_remainder_is_cancelled_and_not_indexed},
      {"market order on empty book is accepted without resting",
       market_order_on_empty_book_is_accepted_without_resting},
      {"duplicate active ID market order is rejected without mutation",
       duplicate_active_id_market_order_is_rejected_without_mutation},
      {"empty book has empty depth and snapshot",
       empty_book_has_empty_depth_and_snapshot},
      {"depth aggregates and orders top price levels",
       depth_aggregates_and_orders_top_price_levels},
      {"depth rejects aggregate quantity overflow",
       depth_rejects_aggregate_quantity_overflow},
      {"snapshot is deterministic in side-price-FIFO order",
       snapshot_is_deterministic_in_side_price_fifo_order},
      {"snapshot tracks lifecycle and sequence rules",
       snapshot_tracks_lifecycle_and_sequence_rules},
      {"cancel removes head, middle, and tail orders",
       cancel_removes_head_middle_and_tail_orders},
      {"cancelling last order removes level and index entry",
       cancelling_last_order_removes_level_and_index_entry},
      {"unknown cancel returns error without mutation",
       unknown_cancel_returns_error_without_mutation},
      {"same-price non-increasing replace preserves priority",
       same_price_non_increasing_replace_preserves_priority},
      {"quantity increase loses priority without stale index entry",
       quantity_increase_loses_priority_without_stale_index_entry},
      {"price change loses priority at new level",
       price_change_loses_priority_at_new_level},
      {"crossing price change matches immediately",
       crossing_price_change_matches_immediately},
      {"invalid and unknown replacements do not mutate book",
       invalid_and_unknown_replacements_do_not_mutate_book},
      {"invalid submit paths return validation errors",
       invalid_submit_paths_return_validation_errors},
      {"deterministic randomized events preserve invariants",
       deterministic_randomized_events_preserve_invariants},
  };

  for (const auto &[name, test] : tests) {
    run_test(name, test, failures);
  }

  return failures == 0 ? 0 : 1;
}
