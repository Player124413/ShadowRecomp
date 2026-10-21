#pragma once

#include <atomic>
#include <concepts>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <type_traits>
#include <utility>
#include <vector>

#if defined(__has_include)
#if __has_include(<version>)
#include <version>
#endif
#endif

// Android's libc++ ships no std::jthread / std::stop_token (the NDK's C++ runtime leaves the C++20 stop-token API out),
// so on those targets the thread is a plain std::thread plus a shared stop flag. The public API stays the same for
// callers: a function taking a stop token, request_stop(), join() - and StopToken is whichever of the two is available.
#if defined(__cpp_lib_jthread)
#define AURORA_HAS_JTHREAD 1
#include <stop_token>
#else
#define AURORA_HAS_JTHREAD 0
#endif

namespace aurora::thread {

enum class Priority { Low, Normal, High };
enum class Affinity { None, SharedCache };

struct Options {
  std::string name;
  Priority priority = Priority::Normal;
  Affinity affinity = Affinity::None;
};

// Applies thread options to the current thread
void set_current(const Options& options) noexcept;

#if AURORA_HAS_JTHREAD
using StopToken = std::stop_token;

// Same call shape as the fallback below (and as std::stop_callback), so a translation unit can name one or the other
// without caring which C++ runtime it is being built against.
template <typename Function>
class StopCallback {
public:
  StopCallback(StopToken token, Function function) : mCallback{std::move(token), std::move(function)} {}

  StopCallback(const StopCallback&) = delete;
  StopCallback& operator=(const StopCallback&) = delete;
  StopCallback(StopCallback&&) = delete;
  StopCallback& operator=(StopCallback&&) = delete;

private:
  std::stop_callback<Function> mCallback;
};
#else
namespace detail {
// The stop flag plus the callbacks that have to run when it is set (a worker's std::stop_callback in the standard
// library is how a thread is woken out of a condition variable).
struct StopState {
  std::atomic<bool> flag{false};
  std::mutex mutex;
  std::vector<std::pair<uint64_t, std::shared_ptr<std::function<void()>>>> callbacks;
  uint64_t next_id = 1;
};
} // namespace detail

// A view of the thread's stop flag; `stop_requested()` is what worker loops check.
class StopToken {
public:
  StopToken() noexcept = default;
  [[nodiscard]] bool stop_requested() const noexcept {
    return mState != nullptr && mState->flag.load(std::memory_order_relaxed);
  }
  [[nodiscard]] bool stop_possible() const noexcept { return mState != nullptr; }

private:
  template <typename>
  friend class StopCallback;
  friend class Thread;
  explicit StopToken(std::shared_ptr<detail::StopState> state) noexcept : mState(std::move(state)) {}
  std::shared_ptr<detail::StopState> mState;
};

// Runs `function` when the thread is asked to stop (and immediately if that already happened).
template <typename Function>
class StopCallback {
public:
  StopCallback(StopToken token, Function function) : mState(std::move(token.mState)) {
    if (!mState) {
      return;
    }
    auto stored = std::make_shared<std::function<void()>>(std::move(function));
    if (mState->flag.load(std::memory_order_relaxed)) {
      (*stored)();
      return;
    }
    std::lock_guard lock{mState->mutex};
    mId = mState->next_id++;
    mState->callbacks.emplace_back(mId, std::move(stored));
  }

  ~StopCallback() { unregister(); }

  StopCallback(const StopCallback&) = delete;
  StopCallback& operator=(const StopCallback&) = delete;
  StopCallback(StopCallback&&) = delete;
  StopCallback& operator=(StopCallback&&) = delete;

private:
  void unregister() noexcept {
    if (!mState || mId == 0) {
      return;
    }
    std::lock_guard lock{mState->mutex};
    std::erase_if(mState->callbacks, [this](const auto& entry) { return entry.first == mId; });
    mId = 0;
  }

  std::shared_ptr<detail::StopState> mState;
  uint64_t mId = 0;
};
#endif

class Thread {
public:
  Thread() noexcept = default;

#if AURORA_HAS_JTHREAD
  template <typename Function>
    requires std::invocable<std::decay_t<Function>&, StopToken>
  explicit Thread(Options options, Function&& function)
  : mThread{[options = std::move(options), function = std::forward<Function>(function)](std::stop_token token) mutable {
    set_current(options);
    std::invoke(function, token);
  }} {}

  Thread(Thread&&) noexcept = default;
  Thread& operator=(Thread&&) noexcept = default;
  Thread(const Thread&) = delete;
  Thread& operator=(const Thread&) = delete;
  ~Thread() = default;

  [[nodiscard]] bool joinable() const noexcept { return mThread.joinable(); }
  [[nodiscard]] std::thread::id get_id() const noexcept { return mThread.get_id(); }
  [[nodiscard]] std::stop_token get_stop_token() const noexcept { return mThread.get_stop_token(); }
  bool request_stop() noexcept { return mThread.request_stop(); }
  void join() { mThread.join(); }
  std::jthread::native_handle_type native_handle() { return mThread.native_handle(); }

private:
  std::jthread mThread;
#else
  template <typename Function>
    requires std::invocable<std::decay_t<Function>&, StopToken>
  explicit Thread(Options options, Function&& function) : mState{std::make_shared<detail::StopState>()} {
    auto state = mState;
    mThread = std::thread{[options = std::move(options), function = std::forward<Function>(function), state]() mutable {
      set_current(options);
      std::invoke(function, StopToken{state});
    }};
  }

  Thread(Thread&&) noexcept = default;
  Thread& operator=(Thread&&) noexcept = default;
  Thread(const Thread&) = delete;
  Thread& operator=(const Thread&) = delete;
  // As std::jthread does: ask the thread to finish, then wait for it.
  ~Thread() {
    if (mThread.joinable()) {
      request_stop();
      mThread.join();
    }
  }

  [[nodiscard]] bool joinable() const noexcept { return mThread.joinable(); }
  [[nodiscard]] std::thread::id get_id() const noexcept { return mThread.get_id(); }
  [[nodiscard]] StopToken get_stop_token() const noexcept { return StopToken{mState}; }
  bool request_stop() noexcept {
    if (!mState) {
      return false;
    }
    bool expected = false;
    if (!mState->flag.compare_exchange_strong(expected, true, std::memory_order_relaxed)) {
      return false;   // someone got there first: the callbacks have run (or are running)
    }
    // Copy the callbacks out first: one of them may destroy its own StopCallback, which needs the mutex.
    std::vector<std::shared_ptr<std::function<void()>>> callbacks;
    {
      std::lock_guard lock{mState->mutex};
      callbacks.reserve(mState->callbacks.size());
      for (const auto& entry : mState->callbacks) {
        callbacks.push_back(entry.second);
      }
    }
    for (const auto& callback : callbacks) {
      (*callback)();
    }
    return true;
  }
  void join() { mThread.join(); }
  std::thread::native_handle_type native_handle() { return mThread.native_handle(); }

private:
  std::thread mThread;
  std::shared_ptr<detail::StopState> mState;   // shared with the running function through StopToken
#endif
};

} // namespace aurora::thread
