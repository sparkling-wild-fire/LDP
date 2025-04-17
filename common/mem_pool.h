#pragma once

#include <cstdint>
#include <vector>
#include <string>

#include "macros.h"

/****
 * 限制：
 * deallocate 方法不会调用对象的析构函数，因此需要在外部确保对象的正确销毁
 * 内存池的大小在初始化时确定，无法动态扩展。如果内存池用完，会触发断言
 * 当前实现不是线程安全的，如果需要在多线程环境中使用，需要添加同步机制
 * 每种类型一个内存池
 */
namespace Common {
  template<typename T>
  class MemPool final {
  public:
    // explicit 关键字用于修饰单参数构造函数或带一个默认参数的构造函数。
    // 它表示该构造函数只能用于显式类型转换，而不能用于隐式类型转换。
    explicit MemPool(std::size_t num_elems) :
        // num_elems个对象（包括T()对象和布尔值is_free_表示是否空闲）
        store_(num_elems, {T(), true}) /* pre-allocation of vector storage. */ {
        // 断言，确保 T 对象是 ObjectBlock 的第一个成员，这有助于后续的内存地址计算。
      ASSERT(reinterpret_cast<const ObjectBlock *>(&(store_[0].object_)) == &(store_[0]), "T object should be first member of ObjectBlock.");
    }

    /// Allocate a new object of type T, use placement new to initialize the object, mark the block as in-use and return the object.
    template<typename... Args>
    T *allocate(Args... args) noexcept {
      auto obj_block = &(store_[next_free_index_]);   // 找到下一个空闲的内存块
      ASSERT(obj_block->is_free_, "Expected free ObjectBlock at index:" + std::to_string(next_free_index_));  // 确保这个对象块是空闲的
      T *ret = &(obj_block->object_);  // 获取对象地址
      // placement new,不会分配新的内存，而是直接在已经分配的内存上构造对象。这在某些场景下非常有用，例如在内存池中复用内存
      ret = new(ret) T(args...); // placement new. 使用 placement new 初始化对象，args... 是构造函数的参数。
      obj_block->is_free_ = false;

      updateNextFreeIndex();
      // 我们使用定位new返回一个T类型的对象，而不是一个与T大小相同的内存块
      return ret;
    }

    /// Return the object back to the pool by marking the block as free again.
    /// Destructor is not called for the object.
    auto deallocate(const T *elem) noexcept {
        // 计算对象在内存中的索引值
      const auto elem_index = (reinterpret_cast<const ObjectBlock *>(elem) - &store_[0]);
      ASSERT(elem_index >= 0 && static_cast<size_t>(elem_index) < store_.size(), "Element being deallocated does not belong to this Memory pool.");
      ASSERT(!store_[elem_index].is_free_, "Expected in-use ObjectBlock at index:" + std::to_string(elem_index));
      store_[elem_index].is_free_ = true;
    }

    // Deleted default, copy & move constructors and assignment-operators.
    // 删除默认构造函数、复制构造函数和移动构造函数。对于复制赋值运算符和移动赋值运算符，我们也会进行同样的操作。
    // 这样做是为了防止在我们不知情的情况下意外调用这些方法
    MemPool() = delete;

    MemPool(const MemPool &) = delete;

    MemPool(const MemPool &&) = delete;

    MemPool &operator=(const MemPool &) = delete;

    MemPool &operator=(const MemPool &&) = delete;

  private:
    /// Find the next available free block to be used for the next allocation.
    auto updateNextFreeIndex() noexcept {
      const auto initial_free_index = next_free_index_;
      while (!store_[next_free_index_].is_free_) {
        ++next_free_index_;
        // 硬件分支预测器几乎总是会预测这个为假。
        if (UNLIKELY(next_free_index_ == store_.size())) { // hardware branch predictor should almost always predict this to be false any ways.
          next_free_index_ = 0;
        }
        if (UNLIKELY(initial_free_index == next_free_index_)) {
          ASSERT(initial_free_index != next_free_index_, "Memory Pool out of space.");
        }
      }
    }

    /// It is better to have one vector of structs with two objects than two vectors of one object.
    /// Consider how these are accessed and cache performance.
    // 缓存局部性更好;减少内存碎片化;数据访问更高效
    struct ObjectBlock {
      T object_;
      bool is_free_ = true;
    };

    /// We could've chosen to use a std::array that would allocate the memory on the stack instead of the heap.
    /// We would have to measure to see which one yields better performance.
    /// It is good to have objects on the stack but performance starts getting worse as the size of the pool increases.
    std::vector<ObjectBlock> store_;

    size_t next_free_index_ = 0;
  };
}
