#pragma once

#include <memory>

#include "art_no_destructor.h"

namespace art::impl {

template <typename T>
class SharedImpl {
 public:
  template <typename... Args>
  static std::shared_ptr<T> GetShared(Args... args) {
    static art::impl::NoDestructor<std::weak_ptr<T>> weak_instance;

    if (auto result = weak_instance->lock()) {
      return result;
    }

    auto result = std::shared_ptr<T>(new T(args...));
    std::weak_ptr<T> temp = result;
    weak_instance->swap(temp);
    return result;
  }

  SharedImpl(const SharedImpl&) = delete;
  SharedImpl(const SharedImpl&&) = delete;
  SharedImpl& operator=(const SharedImpl&) = delete;
  SharedImpl& operator=(const SharedImpl&&) = delete;

 protected:
  SharedImpl() = default;
};

}  // namespace art::impl
