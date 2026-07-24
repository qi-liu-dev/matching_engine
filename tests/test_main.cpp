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

void valid_market_submit_is_not_implemented_in_milestone_2() {
  MatchingEngine engine;
  const MarketOrderRequest market_request{
      .id = OrderId{2}, .side = Side::Sell, .quantity = Quantity{10}};

  require(engine.submit_market(market_request).error ==
              ErrorCode::UnsupportedOperation,
          "valid market submission is intentionally deferred until the "
          "market-order milestone");
}

void cancel_and_replace_are_not_implemented_in_milestone_2() {
  MatchingEngine engine;
  require(engine
              .submit_limit(OrderRequest{.id = OrderId{1},
                                         .side = Side::Buy,
                                         .price = Price{100},
                                         .quantity = Quantity{5}})
              .ok(),
          "resting order should be accepted");

  require(engine.cancel(OrderId{1}) == ErrorCode::UnsupportedOperation,
          "cancel is intentionally deferred until the cancel/replace "
          "milestone");
  require(engine.replace(ReplaceRequest{.id = OrderId{1},
                                        .new_price = Price{101},
                                        .new_quantity = Quantity{5}})
                  .error == ErrorCode::UnsupportedOperation,
          "replace is intentionally deferred until the cancel/replace "
          "milestone");
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
      {"valid market submit is not implemented in milestone 2",
       valid_market_submit_is_not_implemented_in_milestone_2},
      {"cancel and replace are not implemented in milestone 2",
       cancel_and_replace_are_not_implemented_in_milestone_2},
      {"invalid submit paths return validation errors",
       invalid_submit_paths_return_validation_errors},
  };

  for (const auto &[name, test] : tests) {
    run_test(name, test, failures);
  }

  return failures == 0 ? 0 : 1;
}
