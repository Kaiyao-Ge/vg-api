#ifndef VG_CORE_RESOURCE_TYPES_H_
#define VG_CORE_RESOURCE_TYPES_H_

#include <cstdint>

namespace vg::core {

enum class ObjectState { Active, Retired };
enum class PoisonState { Valid, PartiallyProduced, Poisoned };

// 03-system-architecture.md Sec.12's four profiles. A profile never changes
// what a legal program means -- it only decides how much diagnosis and
// instrumentation is paid for, which is why this is a plain input to backends
// rather than a field of any sealed object.
enum class ValidationProfile : uint32_t { CheckedNative, FastNative, ReferenceStrict, Capture };

// Identity of a live allocation version (id + generation). Defined here so
// Arena lookup/retire can take it by name; GraphEpoch is the type that
// *produces* these refs.
struct PointerRef {
  uint64_t allocation{};
  uint32_t generation{};
};

// index+generation capability reference into a FacetPool slot -- never a
// raw backend texture/sampler pointer (03-system-architecture.md Sec.10).
struct FacetRef {
  uint32_t index{};
  uint32_t generation{};
};

// Allocation + generation + representation epoch. Same idea for the
// representation-aware lookup overloads.
struct RepresentationRef {
  uint64_t allocation{};
  uint32_t allocation_generation{};
  uint32_t representation_epoch{};
};

// One address domain per Device for v1.1 (ADR-044 disclosed narrowing);
// multiple domains (e.g. a distinct host-visible staging domain) are
// deferred past F1.
struct AddressDomain {
  uint32_t kind{};
};

}  // namespace vg::core

#endif
