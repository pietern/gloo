/**
 * Copyright (c) 2017-present, Facebook, Inc.
 * All rights reserved.
 *
 * This source code is licensed under the BSD-style license found in the
 * LICENSE file in the root directory of this source tree. An additional grant
 * of patent rights can be found in the PATENTS file in the same directory.
 */

#pragma once

#include "gloo/rendezvous/store.h"

#include <pthread.h>

#include <unordered_map>

namespace gloo {
namespace rendezvous {

class ShmStore : public Store {
 public:
  explicit ShmStore(const std::string& name);
  virtual ~ShmStore() {}

  virtual void set(const std::string& key, const std::vector<char>& data)
      override;

  virtual std::vector<char> get(const std::string& key) override;

  virtual void wait(const std::vector<std::string>& keys) override;

 protected:
  struct header {
    volatile uint64_t initialized;
    volatile uint64_t length;
    volatile uint64_t offset;
    pthread_mutex_t mutex;
    pthread_cond_t cond;
  };

  int fd_;
  struct header* header_;
  uint64_t length_;
  uint64_t offset_;

  std::unordered_map<std::string, std::vector<char>> map_;

private:
  void grow(int nbytes);
  void remap();
  void write(const void* data, uint64_t length);
  void read(std::string& key, std::vector<char>& data);
};

} // namespace rendezvous
} // namespace gloo
