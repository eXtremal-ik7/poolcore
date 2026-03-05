#pragma once

#include <tuple>

// Minimal schema support for IDL-generated tagged structs.
// Full serialization machinery is in idltool/serialize.h.

template<unsigned Tag_, typename T, typename S, T S::*Ptr_>
struct CTaggedField {
  static constexpr unsigned Tag = Tag_;
  static constexpr T S::*Ptr = Ptr_;
  using value_type = T;
  using struct_type = S;
  static const T &get(const S &s) { return s.*Ptr; }
  static T &get(S &s) { return s.*Ptr; }
};

template<typename>
struct MemberPointerTraits;
template<typename T, typename S>
struct MemberPointerTraits<T S::*> {
  using value_type = T;
  using struct_type = S;
};

template<unsigned Tag, auto Ptr>
constexpr auto field() {
  static_assert(Tag > 0 && Tag < 64, "Tag must be in range 1..63");
  using T = typename MemberPointerTraits<decltype(Ptr)>::value_type;
  using S = typename MemberPointerTraits<decltype(Ptr)>::struct_type;
  return CTaggedField<Tag, T, S, Ptr>{};
}
