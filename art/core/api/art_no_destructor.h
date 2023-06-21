#pragma once

#include <new>
#include <type_traits>
#include <utility>

namespace art::impl {

template <typename T>
class NoDestructor {
 public:
  static_assert(
      !std::is_trivially_destructible_v<T>,
      "T is trivially destructible; please use a function-local static "
      "of type T directly instead");

  template <typename... Args>
  explicit NoDestructor(Args&&... args) {
    new (storage_) T(std::forward<Args>(args)...);
  }

  explicit NoDestructor(const T& x) { new (storage_) T(x); }
  explicit NoDestructor(T&& x) { new (storage_) T(std::move(x)); }

  NoDestructor(const NoDestructor&) = delete;
  NoDestructor& operator=(const NoDestructor&) = delete;

  ~NoDestructor() = default;

  const T& operator*() const { return *get(); }
  T& operator*() { return *get(); }

  const T* operator->() const { return get(); }
  T* operator->() { return get(); }

  const T* get() const { return reinterpret_cast<const T*>(storage_); }
  T* get() { return reinterpret_cast<T*>(storage_); }

 private:
  alignas(T) char storage_[sizeof(T)];
};

}  // namespace art::impl
