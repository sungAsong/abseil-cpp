// Copyright 2017 The Abseil Authors.
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//      http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.
//
// -----------------------------------------------------------------------------
// File: call_once.h
// -----------------------------------------------------------------------------
//
// This header file provides an Abseil version of `std::call_once` for invoking
// a given function at most once, across all threads. This Abseil version is
// faster than the C++11 version and incorporates the C++17 argument-passing
// fix, so that (for example) non-const references may be passed to the invoked
// function.

#ifndef ABSL_BASE_CALL_ONCE_H_
#define ABSL_BASE_CALL_ONCE_H_

#include <atomic>
#include <cstdint>
#include <type_traits>

#include "absl/base/internal/invoke.h"
#include "absl/base/internal/low_level_scheduling.h"
#include "absl/base/internal/raw_logging.h"
#include "absl/base/internal/spinlock_wait.h"

namespace absl {

class once_flag;

namespace base_internal {
// Implementation detail.
std::atomic<uint32_t>* ControlWord(absl::once_flag* flag);
}  // namespace base_internal

// call_once()
//
// For all invocations using a given `once_flag`, invokes a given `fn` exactly
// once across all threads. The first call to `call_once()` with a particular
// `once_flag` argument (that does not throw an exception) will run the
// specified function with the provided `args`; other calls with the same
// `once_flag` argument will not run the function, but will wait
// for the provided function to finish running (if it is still running).
//
// This mechanism provides a safe, simple, and fast mechanism for one-time
// initialization in a multi-threaded process.
//
// Example:
//
// class MyInitClass {
//  public:
//  ...
//  mutable absl::once_flag once_;
//
//  MyInitClass* init() const {
//    absl::call_once(once_, &MyInitClass::Init, this);
//    return ptr_;
//  }
//
template <typename Callable, typename... Args>
void call_once(absl::once_flag& flag, Callable&& fn, Args&&... args);

// once_flag
//
// Objects of this type are used to distinguish calls to `call_once()` and
// ensure the provided function is only invoked once across all threads. This
// type is not copyable or movable. However, it has a `constexpr`
// constructor, and is safe to use as a namespace-scoped global variable.
class once_flag {
 public:
  constexpr once_flag() : control_(0) {}
  once_flag(const once_flag&) = delete;
  once_flag& operator=(const once_flag&) = delete;

 private:
  friend std::atomic<uint32_t>* base_internal::ControlWord(once_flag* flag);
  std::atomic<uint32_t> control_;
};

//------------------------------------------------------------------------------
// End of public interfaces.
// Implementation details follow.
//------------------------------------------------------------------------------

namespace base_internal {

// Like call_once, but uses KERNEL_ONLY scheduling. Intended to be used to
// initialize entities used by the scheduler implementation.
template <typename Callable, typename... Args>
void LowLevelCallOnce(absl::once_flag* flag, Callable&& fn, Args&&... args);

// Disables scheduling while on stack when scheduling mode is non-cooperative.
// No effect for cooperative scheduling modes.
class SchedulingHelper {
 public:
  explicit SchedulingHelper(base_internal::SchedulingMode mode) : mode_(mode) {
    if (mode_ == base_internal::SCHEDULE_KERNEL_ONLY) {
      guard_result_ = base_internal::SchedulingGuard::DisableRescheduling();
    }
  }

  ~SchedulingHelper() {
    if (mode_ == base_internal::SCHEDULE_KERNEL_ONLY) {
      base_internal::SchedulingGuard::EnableRescheduling(guard_result_);
    }
  }

 private:
  base_internal::SchedulingMode mode_;
  bool guard_result_;
};

// Bit patterns for call_once state machine values.  Internal implementation
// detail, not for use by clients.
//
// The bit patterns are arbitrarily chosen from unlikely values, to aid in
// debugging.  However, kOnceInit must be 0, so that a zero-initialized
// once_flag will be valid for immediate use.
enum {
  kOnceInit = 0,
  kOnceRunning = 0x65C2937B,
  kOnceWaiter = 0x05A308D2,
  kOnceDone = 0x3F2D8AB0,
};

template <typename Callable, typename... Args>
void CallOnceImpl(std::atomic<uint32_t>* control,
                  base_internal::SchedulingMode scheduling_mode, Callable&& fn,
                  Args&&... args) {
#ifndef NDEBUG
  {
    uint32_t old_control = control->load(std::memory_order_acquire);
    if (old_control != kOnceInit &&
        old_control != kOnceRunning &&
        old_control != kOnceWaiter &&
        old_control != kOnceDone) {
      ABSL_RAW_LOG(
          FATAL,
          "Unexpected value for control word: %d. Either the control word "
          "has non-static storage duration (where GoogleOnceDynamic might "
          "be appropriate), or there's been a memory corruption.",
          old_control);
    }
  }
#endif  // NDEBUG
  static const base_internal::SpinLockWaitTransition trans[] = {
      {kOnceInit, kOnceRunning, true},
      {kOnceRunning, kOnceWaiter, false},
      {kOnceDone, kOnceDone, true}};

  // Must do this before potentially modifying control word's state.
  base_internal::SchedulingHelper maybe_disable_scheduling(scheduling_mode);
  // Short circuit the simplest case to avoid procedure call overhead.
  uint32_t old_control = kOnceInit;
  if (control->compare_exchange_strong(old_control, kOnceRunning,
                                       std::memory_order_acquire,
                                       std::memory_order_relaxed) ||
      base_internal::SpinLockWait(control, ABSL_ARRAYSIZE(trans), trans,
                                  scheduling_mode) == kOnceInit) {
    base_internal::Invoke(std::forward<Callable>(fn),
                          std::forward<Args>(args)...);
    old_control = control->load(std::memory_order_relaxed);
    control->store(base_internal::kOnceDone, std::memory_order_release);
    if (old_control == base_internal::kOnceWaiter) {
      base_internal::SpinLockWake(control, true);
    }
  }  // else *control is already kOnceDone
}

inline std::atomic<uint32_t>* ControlWord(once_flag* flag) {
  return &flag->control_;
}

template <typename Callable, typename... Args>
void LowLevelCallOnce(absl::once_flag* flag, Callable&& fn, Args&&... args) {
  std::atomic<uint32_t>* once = base_internal::ControlWord(flag);
  uint32_t s = once->load(std::memory_order_acquire);
  if (ABSL_PREDICT_FALSE(s != base_internal::kOnceDone)) {
    base_internal::CallOnceImpl(once, base_internal::SCHEDULE_KERNEL_ONLY,
                                std::forward<Callable>(fn),
                                std::forward<Args>(args)...);
  }
}

}  // namespace base_internal

template <typename Callable, typename... Args>
void call_once(absl::once_flag& flag, Callable&& fn, Args&&... args) {
  std::atomic<uint32_t>* once = base_internal::ControlWord(&flag);
  uint32_t s = once->load(std::memory_order_acquire);
  if (ABSL_PREDICT_FALSE(s != base_internal::kOnceDone)) {
    base_internal::CallOnceImpl(
        once, base_internal::SCHEDULE_COOPERATIVE_AND_KERNEL,
        std::forward<Callable>(fn), std::forward<Args>(args)...);
  }
}

}  // namespace absl

#endif  // ABSL_BASE_CALL_ONCE_H_