// Copyright (c) 2020 Ivan K.
// Copyright (c) 2020 The BCNode developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#pragma once

#include "tagged_ptr.h"

template<typename T, typename deleter>
static inline void apply_deleter(T *ptr) {
  deleter d;
  d(ptr);
}

template<typename T>
class intrusive_ptr_default_deleter {
public:
  void operator()(T *object) { delete object; }
};

template<typename T, typename deleter, unsigned tagSize> class intrusive_ptr;

template<typename T, typename deleter = intrusive_ptr_default_deleter<T>, unsigned tagSize = 9>
class atomic_intrusive_ptr {
private:
  // <tag> is reference count cache, it appiles to object reference counter after pointer desctuction
  atomic_tagged_ptr<T, tagSize> _ptr;

public:
  static constexpr uintptr_t StrongRef    = 0x0000000100000000ULL;
  static constexpr uintptr_t WeakRef      = 0x0000000000000001ULL;
  static constexpr uintptr_t ReferenceCacheLimit = 1ULL << (tagSize - 1);

public:
  static void object_addref(T *object, uintptr_t tag) {
    object->ref_fetch_add(tag);
  }

  static void object_release(T *object, uintptr_t tag) {
    auto result = object->ref_fetch_sub(tag);
    if (result == tag)
      apply_deleter<T, deleter>(object);
  }

  void flush(T *object, uintptr_t initialTag) {
    // Atomic exchanging cached reference counter with 0 if pointer to object not changed by concurrent thread
    // Unfortunatelly, we can use only CAS loop here
    // Good news, we need call this function one time per ReferenceCacheLimit(default: 256) pointer copy calls

    // Protect object from delete by adding StrongRef
    // TODO: check variant object->intrusive_ptr_addref(ReferenceCacheLimit*WeakRef) it can be also correct and requires 1 atomic less
    if (object) {
      object_addref(object, StrongRef);

      uintptr_t tag = initialTag;
      for (;;) {
        // Return immediately on success CAS
        tagged_ptr<T, tagSize> oldValue(object, tag);
        tagged_ptr<T, tagSize> newValue(object, 0);
        if (_ptr.compare_and_exchange(oldValue, newValue)) {
          // Correct object reference counter
          object_release(object, StrongRef - oldValue.tag());
          return;
        }

        // Check current object pointer
        // If it was changed by current thread, it will handle reference cache value
        if (oldValue.pointer() != object) {
          object_release(object, StrongRef);
          return;
        }

        // If pointer reference counter below ReferenceCacheLimit, other thread already did flush work
        tag = oldValue.tag();
        if (tag < ReferenceCacheLimit)
          return;
      }
    } else {
      uintptr_t tag = initialTag;
      for (;;) {
        tagged_ptr<T, tagSize> oldValue(nullptr, tag);
        tagged_ptr<T, tagSize> newValue(nullptr, 0);
        if (_ptr.compare_and_exchange(oldValue, newValue) ||
            oldValue.pointer() != nullptr ||
            oldValue.tag() < ReferenceCacheLimit)
          return;
        tag = oldValue.tag();
      }
    }
  }

public:
  atomic_intrusive_ptr() : _ptr(nullptr) {}
  atomic_intrusive_ptr(T *object) : _ptr(object) { object_addref(_ptr, StrongRef); }
  ~atomic_intrusive_ptr() { reset(); }

  atomic_intrusive_ptr(atomic_intrusive_ptr &ptr) {
    // Read pointer value and increment weak reference counter
    tagged_ptr<T, tagSize> link = ptr._ptr.fetch_add(1);
    T *object = link.pointer();
    uintptr_t tag = link.tag();

    _ptr.set(object, 0);

    // Flush reference counter cache if limit exceeded
    if (tag >= ReferenceCacheLimit)
      ptr.flush(object, tag+1);
    // Decrement recently created weak ref and add new strong ref atomically
    if (object)
      object_addref(object, StrongRef - WeakRef);
  }

  atomic_intrusive_ptr(atomic_intrusive_ptr &&ptr) { _ptr = ptr._ptr.exchange(nullptr);}

  void reset(T *object = nullptr) {
    if (object)
      object_addref(object, StrongRef);

    tagged_ptr<T, tagSize> prev = _ptr.exchange(object, 0);
    T *prevObject = prev.pointer();
    if (prevObject)
      object_release(prevObject, StrongRef - prev.tag());
  }

  bool compare_and_exchange(T *oldValue, T *newValue) {
    if (newValue)
      object_addref(newValue, StrongRef);

    for (;;) {
      tagged_ptr<T, tagSize> prev = tagged_ptr<T, tagSize>(oldValue, _ptr.tag());
      if (_ptr.compare_and_exchange(prev, tagged_ptr<T, tagSize>(newValue, 0))) {
        T *prevObject = prev.pointer();
        if (prevObject)
          object_release(prevObject, StrongRef - prev.tag());
        return true;
      } else if (prev.pointer() != oldValue) {
        if (newValue)
          object_release(newValue, StrongRef);
        return false;
      }
    }
  }

  T *get() const { return _ptr.pointer(); }

  friend intrusive_ptr<T, deleter, tagSize>;
};

template<typename T, typename deleter = intrusive_ptr_default_deleter<T>, unsigned tagSize = 9>
class intrusive_ptr {
private:
  T *_ptr;

public:
  static void object_addref(T *object, uintptr_t tag) {
    object->ref_fetch_add(tag);
  }

  static void object_release(T *object, uintptr_t tag) {
    auto result = object->ref_fetch_sub(tag);
    if (result == tag)
      apply_deleter<T, deleter>(object);
  }

public:
  intrusive_ptr() : _ptr(nullptr) {}
  intrusive_ptr(T *object) : _ptr(object) {
    if (_ptr)
      object_addref(_ptr, 1);
  }
  ~intrusive_ptr() {
    if (_ptr)
      object_release(_ptr, atomic_intrusive_ptr<T, deleter, tagSize>::WeakRef);
  }

  intrusive_ptr(const intrusive_ptr &ptr) {
    _ptr = ptr._ptr;
    if (_ptr != nullptr)
      object_addref(_ptr, atomic_intrusive_ptr<T, deleter, tagSize>::WeakRef);
  }

  intrusive_ptr(atomic_intrusive_ptr<T, deleter, tagSize> &ptr) {
    // Read pointer value and increment weak reference counter
    tagged_ptr<T, tagSize> link = ptr._ptr.fetch_add(1);
    _ptr = link.pointer();
    uintptr_t tag = link.tag();

    // Flush reference counter cache if limit exceeded
    if (tag >= atomic_intrusive_ptr<T, deleter, tagSize>::ReferenceCacheLimit)
      ptr.flush(_ptr, tag + 1);
  }

  intrusive_ptr(intrusive_ptr &&ptr) : _ptr(ptr._ptr) {
    ptr._ptr = nullptr;
  }

  intrusive_ptr &operator=(T *ptr) {
    if (ptr)
      object_addref(_ptr, 1);
    if (_ptr)
      object_release(_ptr, atomic_intrusive_ptr<T, deleter, tagSize>::WeakRef);
    _ptr = ptr;
    return *this;
  }

  intrusive_ptr &operator=(const intrusive_ptr &ptr) {
    if (ptr._ptr != nullptr)
      object_addref(_ptr, atomic_intrusive_ptr<T, deleter, tagSize>::WeakRef);
    if (_ptr)
      object_release(_ptr, atomic_intrusive_ptr<T, deleter, tagSize>::WeakRef);
    _ptr = ptr._ptr;
  }

  intrusive_ptr &operator=(intrusive_ptr &&ptr) {
    _ptr = ptr._ptr;
    ptr._ptr = nullptr;
    return *this;
  }

  intrusive_ptr &operator=(atomic_intrusive_ptr<T, deleter, tagSize> &ptr) {
    // Read pointer value and increment weak reference counter
    tagged_ptr<T, tagSize> link = ptr._ptr.fetch_add(1);

    if (_ptr)
      object_release(_ptr, atomic_intrusive_ptr<T, deleter, tagSize>::WeakRef);
    _ptr = link.pointer();
    uintptr_t tag = link.tag();

    // Flush reference counter cache if limit exceeded
    if (tag >= atomic_intrusive_ptr<T, deleter, tagSize>::ReferenceCacheLimit)
      ptr.flush(_ptr, tag + 1);
    return *this;
  }

  T *get() const { return _ptr; }
};
