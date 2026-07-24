#include "matching_engine/domain.hpp"
#include "matching_engine/matching_engine.hpp"

#include <cstdint>
#include <exception>
#include <iostream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>
#include <vector>

namespace {

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
  };

  for (const auto &[name, test] : tests) {
    run_test(name, test, failures);
  }

  return failures == 0 ? 0 : 1;
}
