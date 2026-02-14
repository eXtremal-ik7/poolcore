#pragma once

#include <chrono>
#include <cstdint>

class Timestamp {
public:
  using Duration = std::chrono::milliseconds;

  Timestamp() = default;
  Timestamp(Duration d) : Value_(d) {}
  explicit Timestamp(int64_t ms) : Value_(ms) {}

  // Static constructors
  static Timestamp now() {
    return Timestamp(std::chrono::duration_cast<Duration>(
        std::chrono::system_clock::now().time_since_epoch()));
  }

  static Timestamp fromUnixTime(int64_t seconds) {
    return Timestamp(seconds * 1000);
  }

  // Conversion
  int64_t toUnixTime() const { return Value_.count() / 1000; }
  int64_t count() const { return Value_.count(); }

  // Arithmetic operators
  template<typename Rep, typename Period>
  Timestamp operator+(std::chrono::duration<Rep, Period> d) const { return Timestamp(Value_ + std::chrono::duration_cast<Duration>(d)); }
  template<typename Rep, typename Period>
  Timestamp operator-(std::chrono::duration<Rep, Period> d) const { return Timestamp(Value_ - std::chrono::duration_cast<Duration>(d)); }
  Duration operator-(Timestamp other) const { return Value_ - other.Value_; }

  template<typename Rep, typename Period>
  Timestamp &operator+=(std::chrono::duration<Rep, Period> d) { Value_ += std::chrono::duration_cast<Duration>(d); return *this; }
  template<typename Rep, typename Period>
  Timestamp &operator-=(std::chrono::duration<Rep, Period> d) { Value_ -= std::chrono::duration_cast<Duration>(d); return *this; }

  // Align up to grid boundary (stays in place if already aligned)
  template<typename Rep, typename Period>
  Timestamp alignUp(std::chrono::duration<Rep, Period> grid) const {
    int64_t g = std::chrono::duration_cast<Duration>(grid).count();
    return Timestamp(((Value_.count() + g - 1) / g) * g);
  }

  // Align to next grid boundary (always moves forward)
  template<typename Rep, typename Period>
  Timestamp alignNext(std::chrono::duration<Rep, Period> grid) const {
    int64_t g = std::chrono::duration_cast<Duration>(grid).count();
    return Timestamp((Value_.count() / g + 1) * g);
  }

  // Comparison operators
  bool operator<(Timestamp other) const { return Value_ < other.Value_; }
  bool operator<=(Timestamp other) const { return Value_ <= other.Value_; }
  bool operator>(Timestamp other) const { return Value_ > other.Value_; }
  bool operator>=(Timestamp other) const { return Value_ >= other.Value_; }
  bool operator==(Timestamp other) const { return Value_ == other.Value_; }
  bool operator!=(Timestamp other) const { return Value_ != other.Value_; }

private:
  Duration Value_{0};
};

// Returns overlap fraction of [recordBegin, recordEnd) within [cellBegin, cellEnd).
// Point-in-time (recordBegin == recordEnd): 1.0 if point is in cell, 0.0 otherwise.
// Normal: 1.0 if record fits entirely in cell, proportional fraction otherwise.
inline double overlapFraction(Timestamp recordBegin, Timestamp recordEnd, Timestamp cellBegin, Timestamp cellEnd)
{
  auto recordRange = recordEnd - recordBegin;
  if (recordRange == Timestamp::Duration(0))
    return (recordBegin >= cellBegin && recordBegin < cellEnd) ? 1.0 : 0.0;

  Timestamp overlapBegin = std::max(recordBegin, cellBegin);
  Timestamp overlapEnd = std::min(recordEnd, cellEnd);
  auto overlapRange = overlapEnd - overlapBegin;

  if (overlapRange >= recordRange)
    return 1.0;
  if (overlapRange <= Timestamp::Duration(0))
    return 0.0;
  return static_cast<double>(overlapRange.count()) / static_cast<double>(recordRange.count());
}

struct TimeInterval {
  Timestamp TimeBegin;
  Timestamp TimeEnd;

  TimeInterval() = default;
  TimeInterval(Timestamp begin, Timestamp end) : TimeBegin(begin), TimeEnd(end) {}

  Timestamp::Duration duration() const { return TimeEnd - TimeBegin; }
  bool isValid() const { return TimeEnd >= TimeBegin; }

  // Expand interval to include another interval
  void expand(TimeInterval other) {
    TimeBegin = std::min(TimeBegin, other.TimeBegin);
    TimeEnd = std::max(TimeEnd, other.TimeEnd);
  }

  // Create point-in-time interval (TimeBegin == TimeEnd)
  static TimeInterval point(Timestamp t) { return TimeInterval(t, t); }
};
