#pragma once

#include <iostream>
#include <vector>
#include <atomic>

#include "macros.h"

/*****
 * FIFO队列限制：
 * 固定大小：队列的大小在初始化时确定，无法动态扩展 => 应该扩展，为什么不要queue
 * 线程安全：虽然使用了原子操作，但在高并发场景下，仍然需要确保队列不会溢出
 * 读取空队列：在读取操作中，使用了 ASSERT 来确保不会读取空队列中的元素。在实际应用中，可能需要更友好的错误处理机制。
 *
 * 原子变量能保证线程安全：
 * 1. 避免竞争条件：由于操作是原子性的，多个线程同时更新同一个变量时不会导致数据不一致
 * 2. 可见性：
 *    内存序：原子操作确保了对共享变量的修改对其他线程是可见的。std::atomic 提供了内存序（std::memory_order），确保操作的顺序性
 *    避免缓存不一致：原子操作确保了对共享变量的修改不会被线程的本地缓存所隐藏，从而保证了线程间的可见性
 * 3. 无锁机制：
 *    避免锁的开销：传统锁机制（如互斥锁）在高并发场景下可能导致显著的性能开销，并且可能引发死锁问题。原子操作避免了这些问题，提高了并发性能。
 *
 * 多生产者或多消费者场景下的潜在问题
 *  多生产者问题：
 *      写索引冲突：多个生产者线程同时更新 next_write_index_ 和 num_elements_ 时，可能会导致写索引冲突。
 *      例如，两个生产者线程可能同时获取相同的写位置，导致数据覆盖。
 *      队列溢出：多个生产者线程可能同时增加 num_elements_，导致队列溢出（num_elements_ 超过 store_.size()）。
 *  多消费者问题：
 *      索引冲突：多个消费者线程同时更新 next_read_index_ 和 num_elements_ 时，可能会导致读索引冲突。
 *      例如，两个消费者线程可能同时读取同一个元素，导致数据不一致。
 *      队列下溢：多个消费者线程可能同时减少 num_elements_，导致队列下溢
 *  改进方法：使用无锁队列的变体，Michael-Scott队列；使用锁或其他同步机制
 *
 * 没有锁和互斥量意味着没有上下文切换，正如前面所讨论的，上下文切换是多线程应用中效率低下和延迟的主要来源。
 *
 * 用vector而不用queue:
 * 1. 连续内存分配,充分利用缓存局部性，而queue底层是deque,内存不连续，访问相邻元素缓存失效
 * 2. vector的随机访问效率高，
 * 3. 由于queue的封装性，使用 std::queue 时，即使底层容器是 std::vector，也需要额外的同步机制,
 *      如push时调用push_back，虽然push_back是线程安全的，但是push操作还包括更新队列的内部状态（如队列大小等），这些操作不是原子性的。
 * 4. vector实现无锁队列代码更简洁，queue隐藏了deque结构
 */

namespace Common {
  template<typename T>
  class LFQueue final {
  public:
    explicit LFQueue(std::size_t num_elems) :
        store_(num_elems, T()) /* pre-allocation of vector storage. */ {
    }

    auto getNextToWriteTo() noexcept {
      return &store_[next_write_index_];
    }

    auto updateWriteIndex() noexcept {
      next_write_index_ = (next_write_index_ + 1) % store_.size();
      num_elements_++;
    }

    auto getNextToRead() const noexcept -> const T * {
      return (size() ? &store_[next_read_index_] : nullptr);
    }

    auto updateReadIndex() noexcept {
      next_read_index_ = (next_read_index_ + 1) % store_.size(); // wrap around at the end of container size.
      ASSERT(num_elements_ != 0, "Read an invalid element in:" + std::to_string(pthread_self()));
      num_elements_--;
    }

    auto size() const noexcept {
      return num_elements_.load();
    }

    /// Deleted default, copy & move constructors and assignment-operators.
    LFQueue() = delete;

    LFQueue(const LFQueue &) = delete;

    LFQueue(const LFQueue &&) = delete;

    LFQueue &operator=(const LFQueue &) = delete;

    LFQueue &operator=(const LFQueue &&) = delete;

  private:
    /// Underlying container of data accessed in FIFO order.
    std::vector<T> store_;

    /// Atomic trackers for next index to write new data to and read new data from.
    std::atomic<size_t> next_write_index_ = {0};
    std::atomic<size_t> next_read_index_ = {0};

    std::atomic<size_t> num_elements_ = {0};
  };
}
