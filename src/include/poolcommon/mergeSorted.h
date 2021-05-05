#pragma once

#include <functional>

template<typename Iter1, typename Iter2, typename Less1, typename Less2>
void mergeSorted(Iter1 firstIter,
                 Iter1 firstEnd,
                 Iter2 secondIter,
                 Iter2 secondEnd,
                 Less1 less1,
                 Less2 less2,
                 std::function<void(const typename std::iterator_traits<Iter1>::value_type&)> firstOnly,
                 std::function<void(const typename std::iterator_traits<Iter2>::value_type&)> secondOnly,
                 std::function<void(const typename std::iterator_traits<Iter1>::value_type&, const typename std::iterator_traits<Iter2>::value_type&)> merge)
{
  while (firstIter != firstEnd) {
    if (secondIter == secondEnd) {
      while (firstIter != firstEnd) {
        firstOnly(*firstIter);
        ++firstIter;
      }
      return;
    }

    if (less2(*secondIter, *firstIter)) {
      secondOnly(*secondIter);
      ++secondIter;
    } else if (less1(*firstIter, *secondIter)) {
      firstOnly(*firstIter);
      ++firstIter;
    } else {
      merge(*firstIter, *secondIter);
      ++firstIter;
      ++secondIter;
    }

  }

  while (secondIter != secondEnd) {
    secondOnly(*secondIter);
    ++secondIter;
  }
}
