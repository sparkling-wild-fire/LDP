#pragma once

#include <iostream>
#include <atomic>
#include <thread>
#include <unistd.h>

#include <sys/syscall.h>

/***
 * 线程亲和性限制：设置线程亲和性可能会限制线程的调度灵活性。在某些情况下，操作系统可能无法将线程调度到指定的核心。
 * 错误处理：如果设置亲和性失败，程序会退出。在实际应用中，可能需要更友好的错误处理机制。
 * 资源管理：创建的线程对象需要手动释放，避免内存泄漏。
 */
namespace Common {
  /// Set affinity for current thread to be pinned to the provided core_id.
  // core_id传-1表示不设置亲和性
  inline auto setThreadCore(int core_id) noexcept {
    cpu_set_t cpuset;

    CPU_ZERO(&cpuset);
    CPU_SET(core_id, &cpuset);

    return (pthread_setaffinity_np(pthread_self(), sizeof(cpu_set_t), &cpuset) == 0);
  }

  /// Creates a thread instance, sets affinity on it, assigns it a name and
  /// passes the function to be run on that thread as well as the arguments to the function.
  template<typename T, typename... A>
  inline auto createAndStartThread(int core_id, const std::string &name, T &&func, A &&... args) noexcept {
    auto t = new std::thread([&]() {
      if (core_id >= 0 && !setThreadCore(core_id)) {
        std::cerr << "Failed to set core affinity for " << name << " " << pthread_self() << " to " << core_id << std::endl;
        exit(EXIT_FAILURE);
      }
      std::cerr << "Set core affinity for " << name << " " << pthread_self() << " to " << core_id << std::endl;

      std::forward<T>(func)((std::forward<A>(args))...);
    });

    using namespace std::literals::chrono_literals;
    // 在创建线程后延迟 1 秒，确保线程有足够的时间初始化
    std::this_thread::sleep_for(1s);

    return t;
  }
}
