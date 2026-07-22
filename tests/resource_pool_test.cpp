#include <atomic>
#include <chrono>
#include <cstdlib>
#include <iostream>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

#include "grparse/resource_pool.h"

namespace {

using namespace std::chrono_literals;

void require(bool condition, const std::string& message) {
  if (!condition) throw std::runtime_error(message);
}

struct Counted {
  explicit Counted(int identity) : id(identity) { live.fetch_add(1); }
  ~Counted() { live.fetch_sub(1); }
  int id;
  static std::atomic<int> live;
};

std::atomic<int> Counted::live{0};

void verify_lazy_construction_and_reuse() {
  std::atomic<int> built{0};
  grparse::ResourcePool<Counted> pool(4, [&built] {
    return std::make_unique<Counted>(built.fetch_add(1));
  });
  require(pool.capacity() == 4, "declared capacity");
  require(pool.live() == 0, "pool must not build resources until first use");

  int identity = -1;
  {
    auto first = pool.acquire();
    require(pool.live() == 1, "first acquire builds exactly one resource");
    identity = first->id;
  }
  {
    auto again = pool.acquire();
    require(pool.live() == 1, "a released slot is reused, not rebuilt");
    require(again->id == identity, "reuse must hand back the same resource");
  }
  require(Counted::live.load() == 1, "the pool owns its resources past lease scope");
}

void verify_prime_builds_every_slot() {
  std::atomic<int> built{0};
  grparse::ResourcePool<Counted> pool(3, [&built] {
    return std::make_unique<Counted>(built.fetch_add(1));
  });
  pool.prime();
  require(pool.live() == 3, "prime must build every slot");
  require(built.load() == 3, "prime must call the factory once per slot");
}

void verify_factory_failure_returns_the_slot() {
  std::atomic<int> attempts{0};
  grparse::ResourcePool<Counted> pool(1, [&attempts]() -> std::unique_ptr<Counted> {
    if (attempts.fetch_add(1) == 0) throw std::runtime_error("cold start failed");
    return std::make_unique<Counted>(7);
  });
  bool threw = false;
  try {
    auto lease = pool.acquire();
  } catch (const std::runtime_error&) {
    threw = true;
  }
  require(threw, "factory failure must propagate");
  // If the failed acquire leaked its slot, this call would block forever.
  auto lease = pool.acquire();
  require(lease->id == 7, "slot is reusable after a failed build");
}

void verify_capacity_bounds_concurrency() {
  constexpr size_t kCapacity = 3;
  grparse::ResourcePool<Counted> pool(kCapacity, [] { return std::make_unique<Counted>(0); });

  std::atomic<int> in_flight{0};
  std::atomic<int> peak{0};
  std::atomic<int> completed{0};
  std::vector<std::thread> workers;
  for (int index = 0; index < 16; ++index) {
    workers.emplace_back([&] {
      for (int round = 0; round < 25; ++round) {
        auto lease = pool.acquire();
        const int current = in_flight.fetch_add(1) + 1;
        int observed = peak.load();
        while (current > observed && !peak.compare_exchange_weak(observed, current)) {
        }
        std::this_thread::sleep_for(200us);
        in_flight.fetch_sub(1);
        completed.fetch_add(1);
      }
    });
  }
  for (auto& worker : workers) worker.join();

  require(completed.load() == 400, "every worker must make progress");
  require(peak.load() > 1, "pool must allow real concurrency");
  require(peak.load() <= static_cast<int>(kCapacity), "pool must never exceed its capacity");
  require(pool.live() <= kCapacity, "pool must not build more than capacity resources");
}

void verify_capacity_must_be_positive() {
  bool threw = false;
  try {
    grparse::ResourcePool<Counted> pool(0, [] { return std::make_unique<Counted>(0); });
  } catch (const std::invalid_argument&) {
    threw = true;
  }
  require(threw, "zero capacity must be rejected");
}

}  // namespace

int main() {
  try {
    verify_lazy_construction_and_reuse();
    verify_prime_builds_every_slot();
    verify_factory_failure_returns_the_slot();
    verify_capacity_bounds_concurrency();
    verify_capacity_must_be_positive();
    require(Counted::live.load() == 0, "every pooled resource must be destroyed with its pool");
    return EXIT_SUCCESS;
  } catch (const std::exception& error) {
    std::cerr << "resource-pool-test: " << error.what() << '\n';
    return EXIT_FAILURE;
  }
}
