#pragma once

#include <atomic>
#include <concepts>
#include <functional>
#include <memory>
#include <string>
#include <thread>
#include <type_traits>
#include <utility>

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
#else
// A view of the thread's stop flag; `stop_requested()` is what worker loops check.
class StopToken {
public:
  StopToken() noexcept = default;
  [[nodiscard]] bool stop_requested() const noexcept { return mFlag != nullptr && mFlag->load(std::memory_order_relaxed); }
  [[nodiscard]] bool stop_possible() const noexcept { return mFlag != nullptr; }

private:
  friend class Thread;
  explicit StopToken(std::shared_ptr<std::atomic<bool>> flag) noexcept : mFlag(std::move(flag)) {}
  std::shared_ptr<std::atomic<bool>> mFlag;
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
  explicit Thread(Options options, Function&& function)
  : mStop{std::make_shared<std::atomic<bool>>(false)} {
    auto stop = mStop;
    mThread = std::thread{[options = std::move(options), function = std::forward<Function>(function), stop]() mutable {
      set_current(options);
      std::invoke(function, StopToken{stop});
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
  [[nodiscard]] StopToken get_stop_token() const noexcept { return StopToken{mStop}; }
  bool request_stop() noexcept { return mStop != nullptr && !mStop->exchange(true, std::memory_order_relaxed); }
  void join() { mThread.join(); }
  std::thread::native_handle_type native_handle() { return mThread.native_handle(); }

private:
  std::thread mThread;
  std::shared_ptr<std::atomic<bool>> mStop;   // shared with the running function through StopToken
#endif
};

} // namespace aurora::thread
