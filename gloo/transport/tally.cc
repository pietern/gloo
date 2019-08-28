/**
 * Copyright (c) 2019-present, Facebook, Inc.
 * All rights reserved.
 *
 * This source code is licensed under the BSD-style license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include <gloo/transport/tally.h>

namespace gloo {
namespace transport {

Tally::Tally(int size) : pendingOperations_(size) {

}

Tally::~Tally() {
}

} // namespace transport
} // namespace gloo
