/*
 * Copyright 2024. All rights reserved.
 * Author: hsuloong@outlook.com
 * Created on: 2024.03.06
 */

#pragma once

#include <atomic>
#include <cassert>
#include <condition_variable>
#include <cstddef>
#include <mutex>
#include <thread>
#include <vector>

namespace shanzhai_tf {

/*
多线程同步，最多支持 65535 个 Waiter
基本原理：
（1）Notifier::state_分为3部分
   1.1 高32位，修改计数，CommitWait、CancelWait、Notify、NotifyN会修改
   1.2 低32位的高16位，标记PrepareWait数量
   1.3 低32位的低16位，记录等待中的Waiter，采用链表的形式
（2）初始化时低16位全1，也就是没有任何Waiter
（3）PrepareWait = 把Notifier::state_当前值保存在Waiter::epoch_，然后1.2节部分+1
（4）CommitWait = 标记Waiter::state_为kNotSignaled，计算epoch=Waiter::state_高32位以及低32位的高16位左移16位之和epoch
   3.1 如果epoch大于1.1，说明前面还有处于PrepareWait的等待线程，那么让出cpu
   3.2 如果epoch小于1.1，说明自己被通知了，否则不会出现这种情况
   3.3 如果epoch等于1.1，说明之前没有PrepareWait线程也没有被通知，那么就把自己加到等待队列
（5）CancelWait：和CommitWait流程差不多
（6）Notify = 唤醒一个或者多个等待者
   6.1 如果等待列表和1.2为空，那么直接返回
   6.2 如果notify所有，那么清空1.2、1.3并修改1.1+1.2数量
   6.3 如果不是，那么优先唤醒一个PrepareWait线程，否则唤醒在等待列表的线程
*/

class Notifier {
 public:
  struct Waiter {
    std::atomic<Waiter *> next_;
    uint64_t epoch_;
    enum : unsigned { kNotSignaled = 0, kWaiting = 1, kSignaled = 2 };

    std::mutex mutex_;
    std::condition_variable cv_;
    unsigned state_;
  };

 private:
  // state_ layout
  // [0, kStackBits)-等待栈.
  // [kStackBits, kStackBits + kWaiterBits)-PrepareWait总数.
  // [kStackBits + kWaiterBits, 64)-修改计数.
  static const uint64_t kStackBits = 16;
  static const uint64_t kStackMask = (1ull << kStackBits) - 1;  // 低32位的低16全1，高16全0
  static const uint64_t kWaiterBits = 16;
  static const uint64_t kWaiterShift = 16;
  static const uint64_t kWaiterMask = ((1ull << kWaiterBits) - 1) << kWaiterShift;  // 低32位的高16位全1，低16全0
  static const uint64_t kEpochBits = 32;
  static const uint64_t kEpochShift = 32;
  static const uint64_t kEpochMask = ((1ull << kEpochBits) - 1) << kEpochShift;  // 高32位全1，低32全0

  static const uint64_t kWaiterInc = 1ull << kWaiterBits;
  static const uint64_t kEpochInc = 1ull << kEpochShift;

 public:
  explicit Notifier(size_t N);
  ~Notifier();

  void PrepareWait(Waiter *w);
  void CommitWait(Waiter *w);
  void CancelWait(Waiter *w);
  void Notify(bool all);
  void NotifyN(size_t n);

  Waiter *GetWaiter(size_t idx);

 private:
  std::vector<Waiter> waiters_{};
  std::atomic<uint64_t> state_{0};
};

Notifier::Notifier(size_t N) : waiters_(N) {
  assert(this->waiters_.size() < ((1 << kWaiterBits)) - 1);  // 限制最大的Waiter数量
  // kEpochMask = 4294967295 * kEpochInc;
  this->state_ = kStackMask | (kEpochMask - kEpochInc * this->waiters_.size() * 2);
}

Notifier::~Notifier() {
  assert((this->state_.load() & (kStackMask | kWaiterMask)) == kStackMask);  // 最后没有任何Waiter
}

void Notifier::PrepareWait(Waiter *w) {
  w->epoch_ = this->state_.fetch_add(kWaiterInc, std::memory_order_relaxed);
  std::atomic_thread_fence(std::memory_order_seq_cst);
}

void Notifier::CommitWait(Waiter *w) {
  w->state_ = Waiter::kNotSignaled;
  uint64_t epoch = (w->epoch_ & kEpochMask) + (((w->epoch_ & kWaiterMask) >> kWaiterShift) << kEpochShift);
  uint64_t state = this->state_.load(std::memory_order_seq_cst);
  for (;;) {
    if (static_cast<int64_t>((state & kEpochMask) - epoch) < 0) {
      std::this_thread::yield();
      state = this->state_.load(std::memory_order_seq_cst);
      continue;
    }

    if (static_cast<int64_t>((state & kEpochMask) - epoch) > 0) {
      return;
    }

    assert((state & kWaiterMask) != 0);

    uint64_t new_state = state - kWaiterInc + kEpochInc;

    new_state = static_cast<uint64_t>(new_state & (~kStackMask)) | static_cast<uint64_t>(w - &this->waiters_[0]);
    if ((state & kStackMask) == kStackMask) {
      w->next_.store(nullptr, std::memory_order_relaxed);
    } else {
      w->next_.store(&this->waiters_[state & kStackMask], std::memory_order_relaxed);
    }

    if (this->state_.compare_exchange_weak(state, new_state, std::memory_order_release)) {
      break;
    }
  }

  // park
  {
    std::unique_lock<std::mutex> lock(w->mutex_);
    while (w->state_ != Waiter::kSignaled) {
      w->state_ = Waiter::kWaiting;
      w->cv_.wait(lock);
    }
  }
}

void Notifier::CancelWait(Waiter *w) {
  uint64_t epoch = (w->epoch_ & kEpochMask) + (((w->epoch_ & kWaiterMask) >> kWaiterShift) << kEpochShift);
  uint64_t state = this->state_.load(std::memory_order_relaxed);
  for (;;) {
    if (static_cast<int64_t>((state & kEpochMask) - epoch) < 0) {
      std::this_thread::yield();
      state = this->state_.load(std::memory_order_seq_cst);
      continue;
    }

    if (static_cast<int64_t>((state & kEpochMask) - epoch) > 0) {
      return;
    }

    assert((state & kWaiterMask) != 0);

    uint64_t new_state = state - kWaiterInc + kEpochInc;

    if (this->state_.compare_exchange_weak(state, new_state, std::memory_order_relaxed)) {
      return;
    }
  }
}

void Notifier::Notify(bool all) {
  std::atomic_thread_fence(std::memory_order_seq_cst);
  uint64_t state = this->state_.load(std::memory_order_acquire);
  for (;;) {
    if ((state & kStackMask) == kStackMask && (state & kWaiterMask) == 0) {
      return;
    }
    uint64_t waiters = (state & kWaiterMask) >> kWaiterShift;
    uint64_t new_state = 0;
    if (all) {
      new_state = (state & kEpochMask) + (kEpochInc * waiters) + kStackMask;
    } else if (waiters > 0) {
      new_state = state + kEpochInc - kWaiterInc;
    } else {
      auto w = &this->waiters_[state & kStackMask];
      auto w_next = w->next_.load(std::memory_order_relaxed);
      uint64_t next = kStackMask;
      if (w_next != nullptr) {
        next = static_cast<uint64_t>(w_next - &this->waiters_[0]);
      }
      new_state = (state & kEpochMask) + next;
    }
    if (this->state_.compare_exchange_weak(state, new_state, std::memory_order_acquire)) {
      if (!all && waiters > 0) {
        return;
      }
      if ((state & kStackMask) == kStackMask) {
        return;
      }
      auto w = &this->waiters_[state & kStackMask];
      if (!all) {
        w->next_.store(nullptr, std::memory_order_relaxed);
      }

      Waiter *loop_next = nullptr;
      for (auto iter = w; iter != nullptr; iter = loop_next) {
        loop_next = w->next_.load(std::memory_order_relaxed);
        unsigned loop_state = 0;
        {
          std::unique_lock<std::mutex> loop_lock(iter->mutex_);
          loop_state = iter->state_;
          iter->state_ = Waiter::kSignaled;
        }
        if (loop_state == Waiter::kWaiting) {
          iter->cv_.notify_one();
        }
      }
      return;
    }
  }
}

void Notifier::NotifyN(size_t n) {
  if (n >= this->waiters_.size()) {
    return this->Notify(true);
  } else {
    for (size_t i = 0; i < n; i++) {
      this->Notify(false);
    }
  }
}

Notifier::Waiter *Notifier::GetWaiter(size_t idx) {
  if (idx < this->waiters_.size()) {
    return &this->waiters_[idx];
  }
  return nullptr;
}

}  // namespace shanzhai_tf
