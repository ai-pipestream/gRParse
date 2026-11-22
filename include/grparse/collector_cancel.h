#pragma once

// How a collector leg learns that the inbound call that asked for it is
// gone. Two ways exist and a leg may be given either:
//
//   - pushed: the inbound call owns a CancelRegistry, every outbound call a
//     leg makes registers its cancel hook there, and the call's own cancel
//     (a client cancel, a client that stopped reading, a failing finish)
//     runs every hook at once. No thread waits on anything.
//   - polled: a plain predicate a helper thread asks every so often, for
//     callers that have no registry to give (the unary surfaces).
//
// CollectorCancelled carries both, so a leg passes one value down to every
// call it makes, nested markup dials included, and each call takes the
// pushed path whenever the value has a registry behind it.

#include <concepts>
#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <type_traits>
#include <utility>

namespace grparse {

// The cancel hooks of one inbound call's outbound work. cancel() runs every
// hook registered so far, once, and every hook added after it right away.
// Hooks run under the registry's lock, so once remove() returns its hook is
// not running and never will: a hook may capture a ClientContext that dies
// right after. Hooks must therefore be quick and must not touch the registry
// (ClientContext::TryCancel and PageSource::cancel are both just that).
class CancelRegistry {
 public:
  using Token = uint64_t;

  // Registers `hook` and returns the token that removes it. When the
  // registry is already cancelled the hook runs before this returns and the
  // token is 0, which remove() ignores.
  Token add(std::function<void()> hook) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (cancelled_) {
      hook();
      return 0;
    }
    const Token token = ++next_token_;
    hooks_.emplace(token, std::move(hook));
    return token;
  }

  void remove(Token token) {
    if (token == 0) return;
    std::lock_guard<std::mutex> lock(mutex_);
    hooks_.erase(token);
  }

  // Idempotent: the first call runs the hooks, later ones do nothing.
  void cancel() {
    std::lock_guard<std::mutex> lock(mutex_);
    if (cancelled_) return;
    cancelled_ = true;
    for (auto& [token, hook] : hooks_) hook();
    hooks_.clear();
  }

  bool cancelled() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return cancelled_;
  }

  // Hooks registered and not yet removed or run. For tests.
  size_t registered() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return hooks_.size();
  }

 private:
  mutable std::mutex mutex_;
  std::map<Token, std::function<void()>> hooks_;
  Token next_token_ = 0;
  bool cancelled_ = false;
};

// Answers true once the inbound call that asked for a parse has gone away
// (cancelled by its client), so every collector leg it started can cancel
// its own call instead of running out its cap for nobody. An empty value
// (the default everywhere) means nothing to watch.
//
// Built from a bare predicate it is polled (see CancelWatch). Built with a
// registry it is pushed: CancelWatch registers the leg's ClientContext there
// and needs no thread, and the value also reads true once the registry is
// cancelled or gone. The registry is held weakly, so a leg that outlives
// its inbound call reads as cancelled, never keeps the call's state alive.
class CollectorCancelled {
 public:
  CollectorCancelled() = default;

  template <typename Probe>
    requires(!std::same_as<std::remove_cvref_t<Probe>, CollectorCancelled> &&
             std::is_invocable_r_v<bool, Probe&>)
  CollectorCancelled(Probe probe)  // NOLINT(google-explicit-constructor): a predicate is one
      : probe_(std::move(probe)) {}

  CollectorCancelled(std::weak_ptr<CancelRegistry> registry, std::function<bool()> probe = {})
      : probe_(std::move(probe)), registry_(std::move(registry)), pushed_(true) {}

  bool operator()() const {
    if (pushed_) {
      const auto registry = registry_.lock();
      if (registry == nullptr || registry->cancelled()) return true;
    }
    return probe_ ? probe_() : false;
  }

  explicit operator bool() const { return pushed_ || static_cast<bool>(probe_); }

  // True when the value has a registry behind it (live or not).
  bool pushed() const { return pushed_; }
  // The registry, or null when it is gone or the value is polled.
  std::shared_ptr<CancelRegistry> registry() const { return registry_.lock(); }

 private:
  std::function<bool()> probe_;
  std::weak_ptr<CancelRegistry> registry_;
  bool pushed_ = false;
};

}  // namespace grparse
