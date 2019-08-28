/**
 * Copyright (c) 2019-present, Facebook, Inc.
 * All rights reserved.
 *
 * This source code is licensed under the BSD-style license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include <cstdint>
#include <deque>
#include <tuple>
#include <vector>

namespace gloo {
namespace transport {

// The tally class captures the number of remote pending send and recv
// operations, by rank and by slot. The number of pending operations
// is maintained on a per-context basis.
//
// Its purpose is to:
// * Tell if a local send operation can be executed immediately. This
//   is the case if there is a remote pending recv. If there isn't,
//   the local send operation must be delayed until its destination
//   process has posted a pending recv operation.
// * Match a local recv-from-any operation to pending send operations.
//
class Tally final {
public:
  Tally(int size);

  ~Tally();

private:
  // PendingOperations maintains counters for remote pending send and
  // recv operations for a single rank and a single slot. Under
  // nominal operation this class likely has a very short lifetime and
  // should therefore be as lightweight as possible.
  class PendingOperations final {
  public:
    using count_t = int8_t;

    PendingOperations(uint64_t slot) : slot(slot), send_(0), recv_(0) {}

    bool empty() const { return send_ == 0 && recv_ == 0; }

    count_t getSend() const { return send_; }

    count_t getRecv() const { return recv_; }

    count_t updateSend(count_t v) {
      send_ += v;
      return send_;
    }

    count_t updateRecv(count_t v) {
      recv_ += v;
      return recv_;
    }

  private:
    int8_t send_;
    int8_t recv_;

  public:
    const uint64_t slot;
  };

  // The number of ranks is fixed. Therefore, to store these
  // operations, we use a vector at the top level to get to the rank
  // specific tally. We assume the number of pending operations per
  // rank is small, so instead of storing the per-slot in a map, we
  // use a deque and perform linear search to find the tally for a
  // specific slot. This strategy should avoid heap allocations under
  // nominal execution (i.e. with a few hot slots).
  std::vector<std::deque<PendingOperations>> pendingOperations_;
};

} // namespace transport
} // namespace gloo
