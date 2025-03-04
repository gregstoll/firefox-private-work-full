/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2 -*-
 * vim: set ts=8 sts=2 et sw=2 tw=80:
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef builtin_temporal_ZonedDateTime_h
#define builtin_temporal_ZonedDateTime_h

#include <stdint.h>

#include "builtin/temporal/TemporalTypes.h"
#include "builtin/temporal/Wrapped.h"
#include "js/RootingAPI.h"
#include "js/TypeDecls.h"
#include "js/Value.h"
#include "vm/NativeObject.h"

namespace js {
struct ClassSpec;
}

namespace js::temporal {

class ZonedDateTimeObject : public NativeObject {
 public:
  static const JSClass class_;
  static const JSClass& protoClass_;

  static constexpr uint32_t SECONDS_SLOT = 0;
  static constexpr uint32_t NANOSECONDS_SLOT = 1;
  static constexpr uint32_t TIMEZONE_SLOT = 2;
  static constexpr uint32_t CALENDAR_SLOT = 3;
  static constexpr uint32_t SLOT_COUNT = 4;

  int64_t seconds() const {
    double seconds = getFixedSlot(SECONDS_SLOT).toNumber();
    MOZ_ASSERT(-8'640'000'000'000 <= seconds && seconds <= 8'640'000'000'000);
    return int64_t(seconds);
  }

  int32_t nanoseconds() const {
    int32_t nanoseconds = getFixedSlot(NANOSECONDS_SLOT).toInt32();
    MOZ_ASSERT(0 <= nanoseconds && nanoseconds <= 999'999'999);
    return nanoseconds;
  }

  JSObject* timeZone() const { return &getFixedSlot(TIMEZONE_SLOT).toObject(); }

  JSObject* calendar() const { return &getFixedSlot(CALENDAR_SLOT).toObject(); }

 private:
  static const ClassSpec classSpec_;
};

/**
 * Extract the instant fields from the ZonedDateTime object.
 */
inline Instant ToInstant(const ZonedDateTimeObject* zonedDateTime) {
  return {zonedDateTime->seconds(), zonedDateTime->nanoseconds()};
}

enum class TemporalDisambiguation;
enum class TemporalOffset;
enum class TemporalUnit;

/**
 * CreateTemporalZonedDateTime ( epochNanoseconds, timeZone, calendar [ ,
 * newTarget ] )
 */
ZonedDateTimeObject* CreateTemporalZonedDateTime(
    JSContext* cx, const Instant& instant, JS::Handle<JSObject*> timeZone,
    JS::Handle<JSObject*> calendar);

/**
 * AddZonedDateTime ( epochNanoseconds, timeZone, calendar, years, months,
 * weeks, days, hours, minutes, seconds, milliseconds, microseconds, nanoseconds
 * [ , options ] )
 */
bool AddZonedDateTime(JSContext* cx, const Instant& epochInstant,
                      JS::Handle<JSObject*> timeZone,
                      JS::Handle<JSObject*> calendar, const Duration& duration,
                      Instant* result);

/**
 * DifferenceZonedDateTime ( ns1, ns2, timeZone, calendar, largestUnit, options
 * )
 */
bool DifferenceZonedDateTime(JSContext* cx, const Instant& ns1,
                             const Instant& ns2, JS::Handle<JSObject*> timeZone,
                             JS::Handle<JSObject*> calendar,
                             TemporalUnit largestUnit, Duration* result);

struct NanosecondsAndDays final {
  JS::BigInt* days = nullptr;
  int64_t daysInt = 0;
  Instant nanoseconds;
  Instant dayLength;

  double daysNumber() const;

  void trace(JSTracer* trc);
};

/**
 * NanosecondsToDays ( nanoseconds, relativeTo )
 */
bool NanosecondsToDays(JSContext* cx, const Instant& nanoseconds,
                       JS::Handle<Wrapped<ZonedDateTimeObject*>> relativeTo,
                       JS::MutableHandle<NanosecondsAndDays> result);

enum class OffsetBehaviour { Option, Exact, Wall };

enum class MatchBehaviour { MatchExactly, MatchMinutes };

/**
 * InterpretISODateTimeOffset ( year, month, day, hour, minute, second,
 * millisecond, microsecond, nanosecond, offsetBehaviour, offsetNanoseconds,
 * timeZone, disambiguation, offsetOption, matchBehaviour )
 */
bool InterpretISODateTimeOffset(JSContext* cx, const PlainDateTime& dateTime,
                                OffsetBehaviour offsetBehaviour,
                                int64_t offsetNanoseconds,
                                JS::Handle<JSObject*> timeZone,
                                TemporalDisambiguation disambiguation,
                                TemporalOffset offsetOption,
                                MatchBehaviour matchBehaviour, Instant* result);

} /* namespace js::temporal */

namespace js {

template <typename Wrapper>
class WrappedPtrOperations<temporal::NanosecondsAndDays, Wrapper> {
  const auto& object() const {
    return static_cast<const Wrapper*>(this)->get();
  }

 public:
  double daysNumber() const { return object().daysNumber(); }

  JS::Handle<JS::BigInt*> days() const {
    return JS::Handle<JS::BigInt*>::fromMarkedLocation(&object().days);
  }

  int64_t daysInt() const { return object().daysInt; }

  temporal::Instant nanoseconds() const { return object().nanoseconds; }

  temporal::Instant dayLength() const { return object().dayLength; }
};

template <typename Wrapper>
class MutableWrappedPtrOperations<temporal::NanosecondsAndDays, Wrapper>
    : public WrappedPtrOperations<temporal::NanosecondsAndDays, Wrapper> {
  auto& object() { return static_cast<Wrapper*>(this)->get(); }

 public:
  void initialize(int64_t days, const temporal::Instant& nanoseconds,
                  const temporal::Instant& dayLength) {
    object().daysInt = days;
    object().nanoseconds = nanoseconds;
    object().dayLength = dayLength;
  }

  void initialize(JS::BigInt* days, const temporal::Instant& nanoseconds,
                  const temporal::Instant& dayLength) {
    object().days = days;
    object().nanoseconds = nanoseconds;
    object().dayLength = dayLength;
  }
};

} /* namespace js */

#endif /* builtin_temporal_ZonedDateTime_h */
