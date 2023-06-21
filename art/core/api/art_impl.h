#pragma once

#include <memory>

namespace art::impl {

template <typename T>
class APIInterface {
 public:
  template <typename... Args>
  explicit APIInterface(Args... args);

 protected:
  class Impl;

  friend class Impl;

  std::shared_ptr<Impl> impl_;
};

template <typename T>
template <typename... Args>
APIInterface<T>::APIInterface(Args... args) : impl_(Impl::GetShared(args...)) {}

}  // namespace art::impl
