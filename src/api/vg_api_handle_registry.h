#ifndef VG_API_VG_API_HANDLE_REGISTRY_H_
#define VG_API_VG_API_HANDLE_REGISTRY_H_

#include <memory>
#include <mutex>
#include <unordered_map>

namespace vg_api {

// Deterministic stale-handle rejection for every heap-allocated opaque
// handle type except VgAllocation (which stays a direct core::Allocation*
// cast -- see ADR-044). insert() takes ownership; erase() drops it. contains()
// is the required check before touching any handle from a public entry
// point, so a destroyed-then-reused pointer returns VG_ERROR_STALE_HANDLE
// instead of undefined behavior.
template <typename T>
class HandleRegistry {
 public:
  T* insert(std::unique_ptr<T> obj) {
    std::lock_guard<std::mutex> lock(mutex_);
    T* p = obj.get();
    live_.emplace(p, std::move(obj));
    return p;
  }

  bool contains(T* p) const {
    std::lock_guard<std::mutex> lock(mutex_);
    return p != nullptr && live_.count(p) != 0;
  }

  void erase(T* p) {
    std::lock_guard<std::mutex> lock(mutex_);
    live_.erase(p);
  }

 private:
  mutable std::mutex mutex_;
  std::unordered_map<T*, std::unique_ptr<T>> live_;
};

}  // namespace vg_api

#endif
