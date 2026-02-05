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
  Timestamp operator+(Duration d) const { return Timestamp(Value_ + d); }
  Timestamp operator-(Duration d) const { return Timestamp(Value_ - d); }
  Duration operator-(Timestamp other) const { return Value_ - other.Value_; }

  Timestamp &operator+=(Duration d) { Value_ += d; return *this; }
  Timestamp &operator-=(Duration d) { Value_ -= d; return *this; }

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

struct TimeInterval {
  Timestamp TimeBegin;
  Timestamp TimeEnd;

  TimeInterval() = default;
  TimeInterval(Timestamp begin, Timestamp end) : TimeBegin(begin), TimeEnd(end) {}

  Timestamp::Duration duration() const { return TimeEnd - TimeBegin; }
  bool isValid() const { return TimeEnd >= TimeBegin; }

  // Create point-in-time interval (TimeBegin == TimeEnd)
  static TimeInterval point(Timestamp t) { return TimeInterval(t, t); }
};
