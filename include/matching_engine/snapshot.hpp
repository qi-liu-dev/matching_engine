#pragma once

#include <vector>

#include "matching_engine/domain.hpp"

namespace matching_engine {

struct DepthLevel {
  Price price;
  Quantity quantity;
};

struct SnapshotOrder {
  OrderId id;
  Side side;
  Price price;
  Quantity quantity;
  SequenceNumber sequence;
};

struct BookSnapshot {
  std::vector<SnapshotOrder> orders{};
};

} // namespace matching_engine
