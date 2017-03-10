/**
 * Copyright (c) 2017-present, Facebook, Inc.
 * All rights reserved.
 *
 * This source code is licensed under the BSD-style license found in the
 * LICENSE file in the root directory of this source tree. An additional grant
 * of patent rights can be found in the PATENTS file in the same directory.
 */

#include "gloo/rendezvous/shm_store.h"

#include <errno.h>
#include <fcntl.h>
#include <stdint.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

#include <iostream>

#include "gloo/common/logging.h"

namespace gloo {
namespace rendezvous {

class pthread_mutexattr {
 public:
  pthread_mutexattr() {
    int rv;
    rv = pthread_mutexattr_init(&attr);
    GLOO_ENFORCE_EQ(rv, 0, "pthread_mutexattr_init");
    rv = pthread_mutexattr_setpshared(&attr, PTHREAD_PROCESS_SHARED);
    GLOO_ENFORCE_EQ(rv, 0, "pthread_mutexattr_setpshared");
  }

  ~pthread_mutexattr() {
    auto rv = pthread_mutexattr_destroy(&attr);
    GLOO_ENFORCE_EQ(rv, 0, "pthread_mutexattr_destroy");
  }

  void init(pthread_mutex_t* mutex) {
    auto rv = pthread_mutex_init(mutex, &attr);
    GLOO_ENFORCE_EQ(rv, 0, "pthread_mutex_init");
  }

  pthread_mutexattr_t attr;
};

class pthread_condattr {
 public:
  pthread_condattr() {
    int rv;
    rv = pthread_condattr_init(&attr);
    GLOO_ENFORCE_EQ(rv, 0, "pthread_condattr_init");
    rv = pthread_condattr_setpshared(&attr, PTHREAD_PROCESS_SHARED);
    GLOO_ENFORCE_EQ(rv, 0, "pthread_condattr_setpshared");
  }

  ~pthread_condattr() {
    auto rv = pthread_condattr_destroy(&attr);
    GLOO_ENFORCE_EQ(rv, 0, "pthread_condattr_destroy");
  }

  void init(pthread_cond_t* cond) {
    auto rv = pthread_cond_init(cond, &attr);
    GLOO_ENFORCE_EQ(rv, 0, "pthread_cond_init");
  }

  pthread_condattr_t attr;
};

ShmStore::ShmStore(const std::string& name) {
  int fd;
  void* addr = nullptr;
  auto length = sizeof(header);

  // Try to open existing object
  for (;;) {
    fd = shm_open(name.c_str(), O_RDWR, 0);
    if (fd == -1) {
      GLOO_ENFORCE_EQ(errno, ENOENT, "shm_open may only fail with ENOENT");

      std::cout << "creating new object" << std::endl;

      // Create new object
      fd = shm_open(name.c_str(), O_RDWR | O_CREAT | O_EXCL, 0700);
      if (fd == -1 && errno == EEXIST) {
        // We raced another object to try and open it; try and open it
        continue;
      }

      // Grow object so it can hold the header
      auto rv = ftruncate(fd, length);
      GLOO_ENFORCE_NE(rv, -1, "ftruncate:", strerror(errno));

      // Map memory
      addr = mmap(nullptr, length, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
      GLOO_ENFORCE_NE(addr, MAP_FAILED, "mmap: ", strerror(errno));

      // Initialize header fields
      struct header* header = (struct header*)addr;
      pthread_mutexattr mattr;
      mattr.init(&header->mutex);
      pthread_condattr cattr;
      cattr.init(&header->cond);
      header->length = length;
      header->offset = length;

      // Set done variable to signal other processes that the memory
      // has been initialized and they can use the mutex and condition
      // variable.
      header->initialized = 1;
      break;
    }

    // Map memory
    addr = mmap(nullptr, length, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    GLOO_ENFORCE_NE(addr, MAP_FAILED, "mmap: ", strerror(errno));

    // Wait for memory to be initialized
    struct header* header = (struct header*)addr;
    while (header->initialized == 0) {
      // Spin; should be done soon
    }

    break;
  }

  fd_ = fd;
  header_ = (struct header*) addr;
  length_ = length;
  offset_ = length;
}

void ShmStore::grow(int nbytes) {
  if (header_->offset + nbytes <= header_->length) {
    return;
  }

  auto newLength = header_->offset + nbytes;
  auto rv = ftruncate(fd_, newLength);
  GLOO_ENFORCE_NE(rv, -1, "ftruncate:", strerror(errno));
  header_->length = newLength;
}

void ShmStore::remap() {
  if (header_->length == length_) {
    return;
  }

  void* addr = mremap(header_, length_, header_->length, 0);
  GLOO_ENFORCE_NE(addr, MAP_FAILED, "mremap: ", strerror(errno));
  header_ = (struct header*) addr;
}

#define ALIGN_WORD(n) (8 * (1 + ((n) - 1) / 8))

void ShmStore::write(const void* data, uint64_t length) {
  char *ptr = ((char*) header_) + header_->offset;
  memcpy(ptr, &length, sizeof(length));
  header_->offset += sizeof(length);
  memcpy(ptr + sizeof(length), data, length);
  header_->offset += ALIGN_WORD(length);
}

void ShmStore::read(std::string& key, std::vector<char>& data) {
  uint64_t length;
  char *base = (char*) header_;

  GLOO_ENFORCE(header_->offset > offset_, "No more bytes to read");

  // Key length
  memcpy(&length, base + offset_, sizeof(length));
  offset_ += sizeof(length);

  // Key contents
  key.resize(length);
  memcpy(key.data(), base + offset_, length);
  offset_ += ALIGN_WORD(length);

  // Data length
  memcpy(&length, base + offset_, sizeof(length));
  offset_ += sizeof(length);

  // Data contents
  data.resize(length);
  memcpy(data.data(), base + offset_, length);
  offset_ += ALIGN_WORD(length);
}

void ShmStore::catchup() {
  while (header_->offset > offset) {
    std::string key;
    std::vector<char> data;
    read(key, data);
    map_[key] = std::move(data);
  }
}

void ShmStore::set(const std::string& key, const std::vector<char>& data) {
  auto nbytes =
    sizeof(uint64_t) + ALIGN_WORD(key.size()) +
    sizeof(uint64_t) + ALIGN_WORD(data.size());

  pthread_mutex_lock(&header_->mutex);

  // Housekeeping
  grow(nbytes);
  remap();

  // Add key/data to tail
  write(key.data(), key.size());
  write(data.data(), data.size());

  pthread_mutex_unlock(&header_->mutex);
}

std::vector<char> ShmStore::get(const std::string& key) {
  pthread_mutex_lock(&header_->mutex);

  // Keep reading new keys
  std::string key;
  std::vector<char> result;
  while (header_->offset > offset) {
    read();
  }

  pthread_mutex_unlock(&header_->mutex);
  return
}

void ShmStore::wait(const std::vector<std::string>& keys) {
  GLOO_ENFORCE(false);
}

} // namespace rendezvous
} // namespace gloo
