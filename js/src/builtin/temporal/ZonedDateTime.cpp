/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2 -*-
 * vim: set ts=8 sts=2 et sw=2 tw=80:
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#include "builtin/temporal/ZonedDateTime.h"

#include "mozilla/Assertions.h"
#include "mozilla/CheckedInt.h"
#include "mozilla/FloatingPoint.h"
#include "mozilla/Likely.h"

#include <cstdlib>
#include <initializer_list>
#include <utility>

#include "jspubtd.h"
#include "NamespaceImports.h"

#include "builtin/temporal/Calendar.h"
#include "builtin/temporal/Duration.h"
#include "builtin/temporal/Instant.h"
#include "builtin/temporal/PlainDate.h"
#include "builtin/temporal/PlainDateTime.h"
#include "builtin/temporal/PlainMonthDay.h"
#include "builtin/temporal/PlainTime.h"
#include "builtin/temporal/PlainYearMonth.h"
#include "builtin/temporal/Temporal.h"
#include "builtin/temporal/TemporalFields.h"
#include "builtin/temporal/TemporalParser.h"
#include "builtin/temporal/TemporalRoundingMode.h"
#include "builtin/temporal/TemporalTypes.h"
#include "builtin/temporal/TemporalUnit.h"
#include "builtin/temporal/TimeZone.h"
#include "builtin/temporal/Wrapped.h"
#include "ds/IdValuePair.h"
#include "gc/AllocKind.h"
#include "gc/Barrier.h"
#include "js/AllocPolicy.h"
#include "js/CallArgs.h"
#include "js/CallNonGenericMethod.h"
#include "js/Class.h"
#include "js/ComparisonOperators.h"
#include "js/Conversions.h"
#include "js/ErrorReport.h"
#include "js/friend/ErrorMessages.h"
#include "js/GCVector.h"
#include "js/Id.h"
#include "js/Printer.h"
#include "js/PropertyDescriptor.h"
#include "js/PropertySpec.h"
#include "js/RootingAPI.h"
#include "js/TracingAPI.h"
#include "js/TypeDecls.h"
#include "js/Utility.h"
#include "js/Value.h"
#include "util/StringBuffer.h"
#include "vm/BigIntType.h"
#include "vm/Compartment.h"
#include "vm/GlobalObject.h"
#include "vm/JSAtomState.h"
#include "vm/JSObject.h"
#include "vm/PlainObject.h"
#include "vm/StringType.h"

#include "vm/JSContext-inl.h"
#include "vm/JSObject-inl.h"
#include "vm/NativeObject-inl.h"
#include "vm/ObjectOperations-inl.h"

using namespace js;
using namespace js::temporal;

static inline bool IsZonedDateTime(Handle<Value> v) {
  return v.isObject() && v.toObject().is<ZonedDateTimeObject>();
}

// Returns |RoundNumberToIncrement(offsetNanoseconds, 60 × 10^9, "halfExpand")|.
static int64_t RoundNanosecondsToMinutesIncrement(int64_t offsetNanoseconds) {
  MOZ_ASSERT(std::abs(offsetNanoseconds) < ToNanoseconds(TemporalUnit::Day));

  constexpr int64_t increment = ToNanoseconds(TemporalUnit::Minute);

  int64_t quotient = offsetNanoseconds / increment;
  int64_t remainder = offsetNanoseconds % increment;
  if (std::abs(remainder * 2) >= increment) {
    quotient += (offsetNanoseconds > 0 ? 1 : -1);
  }
  return quotient * increment;
}

/**
 * InterpretISODateTimeOffset ( year, month, day, hour, minute, second,
 * millisecond, microsecond, nanosecond, offsetBehaviour, offsetNanoseconds,
 * timeZone, disambiguation, offsetOption, matchBehaviour )
 */
bool js::temporal::InterpretISODateTimeOffset(
    JSContext* cx, const PlainDateTime& dateTime,
    OffsetBehaviour offsetBehaviour, int64_t offsetNanoseconds,
    Handle<JSObject*> timeZone, TemporalDisambiguation disambiguation,
    TemporalOffset offsetOption, MatchBehaviour matchBehaviour,
    Instant* result) {
  MOZ_ASSERT(std::abs(offsetNanoseconds) < ToNanoseconds(TemporalUnit::Day));

  // Step 1.
  Rooted<JSObject*> calendar(cx, GetISO8601Calendar(cx));
  if (!calendar) {
    return false;
  }

  // Step 2.
  Rooted<PlainDateTimeObject*> temporalDateTime(
      cx, CreateTemporalDateTime(cx, dateTime, calendar));
  if (!temporalDateTime) {
    return false;
  }

  // Step 3.
  if (offsetBehaviour == OffsetBehaviour::Wall ||
      offsetOption == TemporalOffset::Ignore) {
    // Steps 3.a-b.
    return GetInstantFor(cx, timeZone, temporalDateTime, disambiguation,
                         result);
  }

  // Step 4.
  if (offsetBehaviour == OffsetBehaviour::Exact ||
      offsetOption == TemporalOffset::Use) {
    // Step 4.a.
    auto epochNanoseconds = GetUTCEpochNanoseconds(dateTime);
    auto offsetNs = Instant::fromNanoseconds(offsetNanoseconds);

    // Step 4.b.
    epochNanoseconds = epochNanoseconds - offsetNs;

    // Step 4.c.
    if (!IsValidEpochInstant(epochNanoseconds)) {
      JS_ReportErrorNumberASCII(cx, GetErrorMessage, nullptr,
                                JSMSG_TEMPORAL_INSTANT_INVALID);
      return false;
    }

    // Step 4.d.
    *result = epochNanoseconds;
    return true;
  }

  // Step 5.
  MOZ_ASSERT(offsetBehaviour == OffsetBehaviour::Option);

  // Step 6.
  MOZ_ASSERT(offsetOption == TemporalOffset::Prefer ||
             offsetOption == TemporalOffset::Reject);

  // Step 7.
  Rooted<InstantVector> possibleInstants(cx, InstantVector(cx));
  if (!GetPossibleInstantsFor(cx, timeZone, temporalDateTime,
                              &possibleInstants)) {
    return false;
  }

  // Step 8.
  Rooted<Wrapped<InstantObject*>> candidate(cx);
  for (size_t i = 0; i < possibleInstants.length(); i++) {
    candidate = possibleInstants[i];

    // Step 8.a.
    int64_t candidateNanoseconds;
    if (!GetOffsetNanosecondsFor(cx, timeZone, candidate,
                                 &candidateNanoseconds)) {
      return false;
    }
    MOZ_ASSERT(std::abs(candidateNanoseconds) <
               ToNanoseconds(TemporalUnit::Day));

    // Step 8.b.
    if (candidateNanoseconds == offsetNanoseconds) {
      auto* unwrapped = candidate.unwrap(cx);
      if (!unwrapped) {
        return false;
      }

      *result = ToInstant(unwrapped);
      return true;
    }

    // Step 8.c.
    if (matchBehaviour == MatchBehaviour::MatchMinutes) {
      // Step 8.c.i.
      int64_t roundedCandidateNanoseconds =
          RoundNanosecondsToMinutesIncrement(candidateNanoseconds);

      // Step 8.c.ii.
      if (roundedCandidateNanoseconds == offsetNanoseconds) {
        auto* unwrapped = candidate.unwrap(cx);
        if (!unwrapped) {
          return false;
        }

        *result = ToInstant(unwrapped);
        return true;
      }
    }
  }

  // Step 9.
  if (offsetOption == TemporalOffset::Reject) {
    JS_ReportErrorNumberASCII(cx, GetErrorMessage, nullptr,
                              JSMSG_TEMPORAL_ZONED_DATE_TIME_NO_TIME_FOUND);
    return false;
  }

  // Step 10.
  auto instant = DisambiguatePossibleInstants(cx, possibleInstants, timeZone,
                                              temporalDateTime, disambiguation);
  if (!instant) {
    return false;
  }

  // Step 11.
  *result = ToInstant(&instant.unwrap());
  return true;
}

/**
 * ToTemporalZonedDateTime ( item [ , options ] )
 */
static Wrapped<ZonedDateTimeObject*> ToTemporalZonedDateTime(
    JSContext* cx, Handle<Value> item,
    Handle<JSObject*> maybeOptions = nullptr) {
  // Steps 1-2. (Not applicable in our implementation)

  // Step 3.
  auto offsetBehaviour = OffsetBehaviour::Option;

  // Step 4.
  auto matchBehaviour = MatchBehaviour::MatchExactly;

  // Step 7. (Reordered)
  int64_t offsetNanoseconds = 0;

  // Step 5.
  Rooted<JSObject*> calendar(cx);
  Rooted<JSObject*> timeZone(cx);
  PlainDateTime dateTime;
  auto disambiguation = TemporalDisambiguation::Compatible;
  auto offsetOption = TemporalOffset::Reject;
  if (item.isObject()) {
    Rooted<JSObject*> itemObj(cx, &item.toObject());

    // Step 5.a.
    if (itemObj->canUnwrapAs<ZonedDateTimeObject>()) {
      return itemObj;
    }

    // Step 5.b.
    calendar = GetTemporalCalendarWithISODefault(cx, itemObj);
    if (!calendar) {
      return nullptr;
    }

    // Step 5.c.
    JS::RootedVector<PropertyKey> fieldNames(cx);
    if (!CalendarFields(cx, calendar,
                        {CalendarField::Day, CalendarField::Hour,
                         CalendarField::Microsecond, CalendarField::Millisecond,
                         CalendarField::Minute, CalendarField::Month,
                         CalendarField::MonthCode, CalendarField::Nanosecond,
                         CalendarField::Second, CalendarField::Year},
                        &fieldNames)) {
      return nullptr;
    }

    // Steps 5.d-e.
    if (!AppendSorted(cx, fieldNames.get(),
                      {TemporalField::Offset, TemporalField::TimeZone})) {
      return nullptr;
    }

    // Step 5.f.
    Rooted<PlainObject*> fields(
        cx, PrepareTemporalFields(cx, itemObj, fieldNames,
                                  {TemporalField::TimeZone}));
    if (!fields) {
      return nullptr;
    }

    // Step 5.g.
    Rooted<Value> timeZoneValue(cx);
    if (!GetProperty(cx, fields, fields, cx->names().timeZone,
                     &timeZoneValue)) {
      return nullptr;
    }

    // Step 5.h.
    timeZone = ToTemporalTimeZone(cx, timeZoneValue);
    if (!timeZone) {
      return nullptr;
    }

    // Step 5.i.
    Rooted<Value> offsetValue(cx);
    if (!GetProperty(cx, fields, fields, cx->names().offset, &offsetValue)) {
      return nullptr;
    }

    // Step 5.j.
    MOZ_ASSERT(offsetValue.isString() || offsetValue.isUndefined());

    // Step 5.k.
    Rooted<JSString*> offsetString(cx);
    if (offsetValue.isString()) {
      offsetString = offsetValue.toString();
    } else {
      offsetBehaviour = OffsetBehaviour::Wall;
    }

    if (maybeOptions) {
      // Steps 5.l-m.
      if (!ToTemporalDisambiguation(cx, maybeOptions, &disambiguation)) {
        return nullptr;
      }

      // Step 5.n.
      if (!ToTemporalOffset(cx, maybeOptions, &offsetOption)) {
        return nullptr;
      }

      // Step 5.o.
      if (!InterpretTemporalDateTimeFields(cx, calendar, fields, maybeOptions,
                                           &dateTime)) {
        return nullptr;
      }
    } else {
      // Steps 5.l-n. (Not applicable)

      // Step 5.o.
      if (!InterpretTemporalDateTimeFields(cx, calendar, fields, &dateTime)) {
        return nullptr;
      }
    }

    // Step 8.
    if (offsetBehaviour == OffsetBehaviour::Option) {
      if (!ParseTimeZoneOffsetString(cx, offsetString, &offsetNanoseconds)) {
        return nullptr;
      }
    }
  } else {
    // Step 6.a.
    Rooted<JSString*> string(cx, JS::ToString(cx, item));
    if (!string) {
      return nullptr;
    }

    // Case 1: 19700101Z[+02:00]
    // { [[Z]]: true, [[OffsetString]]: undefined, [[Name]]: "+02:00" }
    //
    // Case 2: 19700101+00:00[+02:00]
    // { [[Z]]: false, [[OffsetString]]: "+00:00", [[Name]]: "+02:00" }
    //
    // Case 3: 19700101[+02:00]
    // { [[Z]]: false, [[OffsetString]]: undefined, [[Name]]: "+02:00" }
    //
    // Case 4: 19700101Z[Europe/Berlin]
    // { [[Z]]: true, [[OffsetString]]: undefined, [[Name]]: "Europe/Berlin" }
    //
    // Case 5: 19700101+00:00[Europe/Berlin]
    // { [[Z]]: false, [[OffsetString]]: "+00:00", [[Name]]: "Europe/Berlin" }
    //
    // Case 6: 19700101[Europe/Berlin]
    // { [[Z]]: false, [[OffsetString]]: undefined, [[Name]]: "Europe/Berlin" }

    // Steps 6.b-c.
    bool isUTC;
    bool hasOffset;
    int64_t timeZoneOffset;
    Rooted<JSString*> timeZoneString(cx);
    Rooted<JSString*> calendarString(cx);
    if (!ParseTemporalZonedDateTimeString(cx, string, &dateTime, &isUTC,
                                          &hasOffset, &timeZoneOffset,
                                          &timeZoneString, &calendarString)) {
      return nullptr;
    }

    // Step 6.d.
    MOZ_ASSERT(timeZoneString);

    // Step 6.f. (Not applicable in our implementation.)

    // Step 6.g.
    if (isUTC) {
      offsetBehaviour = OffsetBehaviour::Exact;
    }

    // Step 6.h.
    else if (!hasOffset) {
      offsetBehaviour = OffsetBehaviour::Wall;
    }

    // Steps 6.e and 6.i.
    timeZone = ToTemporalTimeZone(cx, timeZoneString);
    if (!timeZone) {
      return nullptr;
    }

    // Step 6.j.
    Rooted<Value> calendarValue(cx);
    if (calendarString) {
      calendarValue.setString(calendarString);
    }

    calendar = ToTemporalCalendarWithISODefault(cx, calendarValue);
    if (!calendar) {
      return nullptr;
    }

    // Step 6.k.
    matchBehaviour = MatchBehaviour::MatchMinutes;

    if (maybeOptions) {
      // Step 6.l.
      if (!ToTemporalDisambiguation(cx, maybeOptions, &disambiguation)) {
        return nullptr;
      }

      // Step 6.m.
      if (!ToTemporalOffset(cx, maybeOptions, &offsetOption)) {
        return nullptr;
      }

      // Step 6.n.
      TemporalOverflow ignored;
      if (!ToTemporalOverflow(cx, maybeOptions, &ignored)) {
        return nullptr;
      }
    }

    // Step 8.
    if (offsetBehaviour == OffsetBehaviour::Option) {
      MOZ_ASSERT(hasOffset);
      offsetNanoseconds = timeZoneOffset;
    }
  }

  // Step 9.
  Instant epochNanoseconds;
  if (!InterpretISODateTimeOffset(
          cx, dateTime, offsetBehaviour, offsetNanoseconds, timeZone,
          disambiguation, offsetOption, matchBehaviour, &epochNanoseconds)) {
    return nullptr;
  }

  // Step 10.
  return CreateTemporalZonedDateTime(cx, epochNanoseconds, timeZone, calendar);
}

/**
 * ToTemporalZonedDateTime ( item [ , options ] )
 */
static bool ToTemporalZonedDateTime(JSContext* cx, Handle<Value> item,
                                    Instant* result) {
  auto obj = ToTemporalZonedDateTime(cx, item);
  if (!obj) {
    return false;
  }

  *result = ToInstant(&obj.unwrap());
  return true;
}

/**
 * ToTemporalZonedDateTime ( item [ , options ] )
 */
static bool ToTemporalZonedDateTime(JSContext* cx, Handle<Value> item,
                                    Instant* instant,
                                    MutableHandle<JSObject*> timeZone,
                                    MutableHandle<JSObject*> calendar) {
  auto* obj = ToTemporalZonedDateTime(cx, item).unwrapOrNull();
  if (!obj) {
    return false;
  }

  *instant = ToInstant(obj);
  timeZone.set(obj->timeZone());
  calendar.set(obj->calendar());
  return cx->compartment()->wrap(cx, timeZone) &&
         cx->compartment()->wrap(cx, calendar);
}

/**
 * CreateTemporalZonedDateTime ( epochNanoseconds, timeZone, calendar [ ,
 * newTarget ] )
 */
static ZonedDateTimeObject* CreateTemporalZonedDateTime(
    JSContext* cx, const CallArgs& args, Handle<BigInt*> epochNanoseconds,
    Handle<JSObject*> timeZone, Handle<JSObject*> calendar) {
  // Step 1.
  MOZ_ASSERT(IsValidEpochNanoseconds(epochNanoseconds));

  // Steps 3-4.
  Rooted<JSObject*> proto(cx);
  if (!GetPrototypeFromBuiltinConstructor(cx, args, JSProto_ZonedDateTime,
                                          &proto)) {
    return nullptr;
  }

  auto* obj = NewObjectWithClassProto<ZonedDateTimeObject>(cx, proto);
  if (!obj) {
    return nullptr;
  }

  // Step 4.
  auto instant = ToInstant(epochNanoseconds);
  obj->setFixedSlot(ZonedDateTimeObject::SECONDS_SLOT,
                    NumberValue(instant.seconds));
  obj->setFixedSlot(ZonedDateTimeObject::NANOSECONDS_SLOT,
                    Int32Value(instant.nanoseconds));

  // Step 5.
  obj->setFixedSlot(ZonedDateTimeObject::TIMEZONE_SLOT, ObjectValue(*timeZone));

  // Step 6.
  obj->setFixedSlot(ZonedDateTimeObject::CALENDAR_SLOT, ObjectValue(*calendar));

  // Step 7.
  return obj;
}

/**
 * CreateTemporalZonedDateTime ( epochNanoseconds, timeZone, calendar [ ,
 * newTarget ] )
 */
ZonedDateTimeObject* js::temporal::CreateTemporalZonedDateTime(
    JSContext* cx, const Instant& instant, Handle<JSObject*> timeZone,
    Handle<JSObject*> calendar) {
  // Step 1.
  MOZ_ASSERT(IsValidEpochInstant(instant));

  // Steps 2-3.
  auto* obj = NewBuiltinClassInstance<ZonedDateTimeObject>(cx);
  if (!obj) {
    return nullptr;
  }

  // Step 4.
  obj->setFixedSlot(ZonedDateTimeObject::SECONDS_SLOT,
                    NumberValue(instant.seconds));
  obj->setFixedSlot(ZonedDateTimeObject::NANOSECONDS_SLOT,
                    Int32Value(instant.nanoseconds));

  // Step 5.
  obj->setFixedSlot(ZonedDateTimeObject::TIMEZONE_SLOT, ObjectValue(*timeZone));

  // Step 6.
  obj->setFixedSlot(ZonedDateTimeObject::CALENDAR_SLOT, ObjectValue(*calendar));

  // Step 7.
  return obj;
}

/**
 * TemporalZonedDateTimeToString ( zonedDateTime, precision, showCalendar,
 * showTimeZone, showOffset [ , increment, unit, roundingMode ] )
 */
static JSString* TemporalZonedDateTimeToString(
    JSContext* cx, Handle<ZonedDateTimeObject*> zonedDateTime,
    Precision precision, CalendarOption showCalendar,
    TimeZoneNameOption showTimeZone, ShowOffsetOption showOffset,
    Increment increment = Increment{1},
    TemporalUnit unit = TemporalUnit::Nanosecond,
    TemporalRoundingMode roundingMode = TemporalRoundingMode::Trunc) {
  JSStringBuilder result(cx);

  // Steps 1-3. (Not applicable in our implementation.)

  // Step 4.
  Instant ns;
  if (!RoundTemporalInstant(cx, ToInstant(zonedDateTime), increment, unit,
                            roundingMode, &ns)) {
    return nullptr;
  }

  // Step 5.
  Rooted<JSObject*> timeZone(cx, zonedDateTime->timeZone());

  // Step 6.
  Rooted<InstantObject*> instant(cx, CreateTemporalInstant(cx, ns));
  if (!instant) {
    return nullptr;
  }

  // Step 7.
  Rooted<CalendarObject*> isoCalendar(cx, GetISO8601Calendar(cx));
  if (!isoCalendar) {
    return nullptr;
  }

  // Step 8.
  PlainDateTime temporalDateTime;
  if (!js::temporal::GetPlainDateTimeFor(cx, timeZone, instant,
                                         &temporalDateTime)) {
    return nullptr;
  }

  // Step 9.
  JSString* dateTimeString = TemporalDateTimeToString(
      cx, temporalDateTime, isoCalendar, precision, CalendarOption::Never);
  if (!dateTimeString) {
    return nullptr;
  }
  if (!result.append(dateTimeString)) {
    return nullptr;
  }

  // Steps 10-11.
  if (showOffset != ShowOffsetOption::Never) {
    // Step 11.a.
    int64_t offsetNs;
    if (!GetOffsetNanosecondsFor(cx, timeZone, instant, &offsetNs)) {
      return nullptr;
    }
    MOZ_ASSERT(std::abs(offsetNs) < ToNanoseconds(TemporalUnit::Day));

    // Step 11.b.
    JSString* offsetString = FormatISOTimeZoneOffsetString(cx, offsetNs);
    if (!offsetString) {
      return nullptr;
    }
    if (!result.append(offsetString)) {
      return nullptr;
    }
  }

  // Steps 12-13.
  if (showTimeZone != TimeZoneNameOption::Never) {
    if (!result.append('[')) {
      return nullptr;
    }

    if (showTimeZone == TimeZoneNameOption::Critical) {
      if (!result.append('!')) {
        return nullptr;
      }
    }

    JSString* timeZoneString = TimeZoneToString(cx, timeZone);
    if (!timeZoneString) {
      return nullptr;
    }
    if (!result.append(timeZoneString)) {
      return nullptr;
    }

    if (!result.append(']')) {
      return nullptr;
    }
  }

  // Step 14.
  Rooted<JSObject*> calendar(cx, zonedDateTime->calendar());
  if (!MaybeFormatCalendarAnnotation(cx, result, calendar, showCalendar)) {
    return nullptr;
  }

  // Step 15.
  return result.finishString();
}

/**
 * AddZonedDateTime ( epochNanoseconds, timeZone, calendar, years, months,
 * weeks, days, hours, minutes, seconds, milliseconds, microseconds, nanoseconds
 * [ , options ] )
 */
static bool AddZonedDateTime(JSContext* cx, const Instant& epochNanoseconds,
                             Handle<JSObject*> timeZone,
                             Handle<JSObject*> calendar,
                             const Duration& duration,
                             Handle<JSObject*> maybeOptions, Instant* result) {
  MOZ_ASSERT(IsValidEpochInstant(epochNanoseconds));
  MOZ_ASSERT(IsValidDuration(duration.date()));
  MOZ_ASSERT(IsValidDuration(duration.time()));

  // Steps 1-2. (Not applicable)

  // Step 3.
  if (duration.years == 0 && duration.months == 0 && duration.weeks == 0 &&
      duration.days == 0) {
    // Step 3.a.
    return AddInstant(cx, epochNanoseconds, duration, result);
  }

  // Steps 4-5.
  PlainDateTime temporalDateTime;
  if (!GetPlainDateTimeFor(cx, timeZone, epochNanoseconds, &temporalDateTime)) {
    return false;
  }
  auto& [date, time] = temporalDateTime;

  // Step 6.
  Rooted<PlainDateObject*> datePart(cx, CreateTemporalDate(cx, date, calendar));
  if (!datePart) {
    return false;
  }

  // Step 7.
  Rooted<DurationObject*> dateDuration(
      cx, CreateTemporalDuration(cx, duration.date()));
  if (!dateDuration) {
    return false;
  }

  // Step 8.
  PlainDate addedDate;
  if (maybeOptions) {
    if (!CalendarDateAdd(cx, calendar, datePart, dateDuration, maybeOptions,
                         &addedDate)) {
      return false;
    }
  } else {
    if (!CalendarDateAdd(cx, calendar, datePart, dateDuration, &addedDate)) {
      return false;
    }
  }

  // Step 9.
  Rooted<PlainDateTimeObject*> intermediateDateTime(
      cx, CreateTemporalDateTime(cx, {addedDate, time}, calendar));
  if (!intermediateDateTime) {
    return false;
  }

  // Step 10.
  Instant intermediateInstant;
  if (!GetInstantFor(cx, timeZone, intermediateDateTime,
                     TemporalDisambiguation::Compatible,
                     &intermediateInstant)) {
    return false;
  }

  // Step 11.
  return AddInstant(cx, intermediateInstant, duration.time(), result);
}

/**
 * AddZonedDateTime ( epochNanoseconds, timeZone, calendar, years, months,
 * weeks, days, hours, minutes, seconds, milliseconds, microseconds, nanoseconds
 * [ , options ] )
 */
bool js::temporal::AddZonedDateTime(JSContext* cx, const Instant& epochInstant,
                                    Handle<JSObject*> timeZone,
                                    Handle<JSObject*> calendar,
                                    const Duration& duration, Instant* result) {
  return ::AddZonedDateTime(cx, epochInstant, timeZone, calendar, duration,
                            nullptr, result);
}

double NanosecondsAndDays::daysNumber() const {
  if (days) {
    return BigInt::numberValue(days);
  }
  return double(daysInt);
}

void NanosecondsAndDays::trace(JSTracer* trc) {
  if (days) {
    TraceRoot(trc, &days, "NanosecondsAndDays::days");
  }
}

/**
 * NanosecondsToDays ( nanoseconds, relativeTo )
 */
bool js::temporal::NanosecondsToDays(
    JSContext* cx, const Instant& nanoseconds,
    Handle<Wrapped<ZonedDateTimeObject*>> relativeTo,
    MutableHandle<NanosecondsAndDays> result) {
  MOZ_ASSERT(IsValidInstantDifference(nanoseconds));

  // Step 1.
  auto dayLengthNs = Instant::fromNanoseconds(ToNanoseconds(TemporalUnit::Day));

  // Step 2.
  if (nanoseconds == Instant{}) {
    result.initialize(int64_t(0), Instant{}, dayLengthNs);
    return true;
  }

  // Step 3.
  int32_t sign = nanoseconds < Instant{} ? -1 : 1;

  // Step 4. (Not applicable)

  // Step 5.
  auto* unwrappedRelativeTo = relativeTo.unwrap(cx);
  if (!unwrappedRelativeTo) {
    return false;
  }
  auto startNs = ToInstant(unwrappedRelativeTo);
  Rooted<JSObject*> timeZone(cx, unwrappedRelativeTo->timeZone());
  Rooted<JSObject*> calendar(cx, unwrappedRelativeTo->calendar());

  if (!cx->compartment()->wrap(cx, &timeZone)) {
    return false;
  }
  if (!cx->compartment()->wrap(cx, &calendar)) {
    return false;
  }

  // Steps 6-7.
  PlainDateTime startDateTime;
  if (!GetPlainDateTimeFor(cx, timeZone, startNs, &startDateTime)) {
    return false;
  }

  // Step 8.
  //
  // NB: This addition can't overflow, because we've checked that |nanoseconds|
  // can be represented as an Instant difference value.
  auto endNs = startNs + nanoseconds;

  // Step 9.
  if (!IsValidEpochInstant(endNs)) {
    JS_ReportErrorNumberASCII(cx, GetErrorMessage, nullptr,
                              JSMSG_TEMPORAL_INSTANT_INVALID);
    return false;
  }

  // Steps 10-11.
  PlainDateTime endDateTime;
  if (!GetPlainDateTimeFor(cx, timeZone, endNs, &endDateTime)) {
    return false;
  }

  // Step 12.
  Duration dateDifference;
  if (!DifferenceISODateTime(cx, startDateTime, endDateTime, calendar,
                             TemporalUnit::Day, &dateDifference)) {
    return false;
  }

  // Step 13.
  double days = dateDifference.days;

  // Step 14.
  Instant intermediateNs;
  if (!AddZonedDateTime(cx, startNs, timeZone, calendar, {0, 0, 0, days},
                        &intermediateNs)) {
    return false;
  }
  MOZ_ASSERT(IsValidEpochInstant(intermediateNs));

  // Sum up all days to subtract to avoid imprecise floating-point arithmetic.
  // Overflows can be safely ignored, because they take too long to happen.
  int64_t daysToSubtract = 0;

  // Step 15.
  if (sign > 0) {
    // Step 15.a.
    while (days > double(daysToSubtract) && intermediateNs > endNs) {
      // This loop can iterate indefinitely when given a specially crafted
      // time zone object, so we need to check for interrupts.
      if (!CheckForInterrupt(cx)) {
        return false;
      }

      // Step 15.a.i.
      daysToSubtract += 1;

      // Step 15.a.ii.
      double durationDays = days - double(daysToSubtract);
      if (!AddZonedDateTime(cx, startNs, timeZone, calendar,
                            {0, 0, 0, durationDays}, &intermediateNs)) {
        return false;
      }
      MOZ_ASSERT(IsValidEpochInstant(intermediateNs));
    }

    MOZ_ASSERT_IF(days > double(daysToSubtract), intermediateNs <= endNs);
  }

  MOZ_ASSERT_IF(days == double(daysToSubtract), intermediateNs == startNs);

  // Step 16.
  auto ns = endNs - intermediateNs;
  MOZ_ASSERT(IsValidInstantDifference(ns));

  // Sum up all days to add to avoid imprecise floating-point arithmetic.
  // Overflows can be safely ignored, because they take too long to happen.
  int64_t daysToAdd = -daysToSubtract;

  // Steps 17-18.
  while (true) {
    // This loop can iterate indefinitely when given a specially crafted time
    // zone object, so we need to check for interrupts.
    if (!CheckForInterrupt(cx)) {
      return false;
    }

    // Step 18.a.
    Instant oneDayFartherNs;
    if (!AddZonedDateTime(cx, intermediateNs, timeZone, calendar,
                          {0, 0, 0, double(sign)}, &oneDayFartherNs)) {
      return false;
    }
    MOZ_ASSERT(IsValidEpochInstant(oneDayFartherNs));

    // Step 18.b.
    dayLengthNs = oneDayFartherNs - intermediateNs;
    MOZ_ASSERT(IsValidInstantDifference(dayLengthNs));

    // First iteration:
    //
    // ns = endNs - intermediateNs
    // dayLengthNs = oneDayFartherNs - intermediateNs
    // diff = ns - dayLengthNs
    //      = (endNs - intermediateNs) - (oneDayFartherNs - intermediateNs)
    //      = endNs - intermediateNs - oneDayFartherNs + intermediateNs
    //      = endNs - oneDayFartherNs
    //
    // Second iteration:
    //
    // ns = diff'
    //    = endNs - oneDayFartherNs'
    // intermediateNs = oneDayFartherNs'
    // dayLengthNs = oneDayFartherNs - intermediateNs
    //             = oneDayFartherNs - oneDayFartherNs'
    // diff = ns - dayLengthNs
    //      = (endNs - oneDayFartherNs') - (oneDayFartherNs - oneDayFartherNs')
    //      = endNs - oneDayFartherNs' - oneDayFartherNs + oneDayFartherNs'
    //      = endNs - oneDayFartherNs
    //
    // Where |diff'| and |oneDayFartherNs'| denote the variables from the
    // previous iteration.
    //
    // This repeats for all following iterations.
    //
    // |endNs| and |oneDayFartherNs| are both valid epoch instant values, so the
    // difference is a valid epoch instant difference value, too.

    // Step 18.c.
    auto diff = ns - dayLengthNs;
    MOZ_ASSERT(IsValidInstantDifference(diff));
    MOZ_ASSERT(diff == (endNs - oneDayFartherNs));

    if (diff == Instant{} || ((diff < Instant{}) == (sign < 0))) {
      // Step 18.c.i.
      ns = diff;

      // Step 18.c.ii.
      intermediateNs = oneDayFartherNs;

      // Step 18.c.iii.
      daysToAdd += sign;
    } else {
      // Step 18.d.
      break;
    }
  }

  // Step 19.
  if (sign > 0) {
    bool totalDaysIsNegative;
    if (int64_t daysInt; mozilla::NumberEqualsInt64(days, &daysInt)) {
      // |daysInt + daysToAdd < 0| could overflow when |daysInt| is near the
      // int64 boundaries, so handle each case separately.
      totalDaysIsNegative = daysInt < 0
                                ? (daysToAdd < 0 || daysInt + daysToAdd < 0)
                                : (daysToAdd < 0 && daysInt + daysToAdd < 0);
    } else {
      // When |days| exceeds the int64 range any |daysToAdd| value can't
      // meaningfully affect the result, so only test for negative |days|.
      totalDaysIsNegative = days < 0;
    }

    if (totalDaysIsNegative) {
      JS_ReportErrorNumberASCII(cx, GetErrorMessage, nullptr,
                                JSMSG_TEMPORAL_ZONED_DATE_TIME_INCORRECT_SIGN,
                                "days");
      return false;
    }
  }

  // Step 20.
  if (sign < 0) {
    // |daysToAdd| can't be positive for |sign = -1|.
    MOZ_ASSERT(daysToAdd <= 0);

    bool totalDaysIsPositive;
    if (int64_t daysInt; mozilla::NumberEqualsInt64(days, &daysInt)) {
      // |daysInt + daysToAdd > 0| could overflow when |daysInt| is near the
      // int64 boundaries, so handle each case separately.
      totalDaysIsPositive = daysInt > 0 && daysInt + daysToAdd > 0;
    } else {
      // When |days| exceeds the int64 range any |daysToAdd| value can't
      // meaningfully affect the result, so only test for positive |days|.
      totalDaysIsPositive = days > 0;
    }

    if (totalDaysIsPositive) {
      JS_ReportErrorNumberASCII(cx, GetErrorMessage, nullptr,
                                JSMSG_TEMPORAL_ZONED_DATE_TIME_INCORRECT_SIGN,
                                "days");
      return false;
    }
  }

  MOZ_ASSERT(IsValidInstantDifference(dayLengthNs));
  MOZ_ASSERT(IsValidInstantDifference(ns));

  // FIXME: spec issue - rewrite steps 21-22 as:
  //
  // If sign = -1, then
  //   If nanoseconds > 0, throw a RangeError.
  // Else,
  //   Assert: nanoseconds ≥ 0.
  //
  // https://github.com/tc39/proposal-temporal/issues/2530

  // Steps 21-22.
  if (sign < 0) {
    if (ns > Instant{}) {
      JS_ReportErrorNumberASCII(cx, GetErrorMessage, nullptr,
                                JSMSG_TEMPORAL_ZONED_DATE_TIME_INCORRECT_SIGN,
                                "nanoseconds");
      return false;
    }
  } else {
    MOZ_ASSERT(ns >= Instant{});
  }

  // Step 23.
  MOZ_ASSERT(ns.abs() < dayLengthNs.abs());

  // Step 24.
  int64_t daysInt;
  if (mozilla::NumberEqualsInt64(days, &daysInt)) {
    auto daysChecked = mozilla::CheckedInt64(daysInt) + daysToAdd;
    if (daysChecked.isValid()) {
      result.initialize(daysChecked.value(), ns, dayLengthNs.abs());
      return true;
    }
  }

  // Total number of days is too large for int64_t, store it as BigInt.

  Rooted<BigInt*> daysBigInt(cx, BigInt::createFromDouble(cx, days));
  if (!daysBigInt) {
    return false;
  }

  Rooted<BigInt*> daysToAddBigInt(cx, BigInt::createFromInt64(cx, daysToAdd));
  if (!daysToAddBigInt) {
    return false;
  }

  daysBigInt = BigInt::add(cx, daysBigInt, daysToAddBigInt);
  if (!daysBigInt) {
    return false;
  }

  result.initialize(daysBigInt, ns, dayLengthNs.abs());
  return true;
}

/**
 * DifferenceZonedDateTime ( ns1, ns2, timeZone, calendar, largestUnit, options
 * )
 */
static bool DifferenceZonedDateTime(JSContext* cx, const Instant& ns1,
                                    const Instant& ns2,
                                    Handle<JSObject*> timeZone,
                                    Handle<JSObject*> calendar,
                                    TemporalUnit largestUnit,
                                    Handle<PlainObject*> maybeOptions,
                                    Duration* result) {
  MOZ_ASSERT(IsValidEpochInstant(ns1));
  MOZ_ASSERT(IsValidEpochInstant(ns2));

  // Steps 1-2. (Not applicable in our implementation.)

  // Steps 3.
  if (ns1 == ns2) {
    *result = {};
    return true;
  }

  // Steps 4-5.
  PlainDateTime startDateTime;
  if (!GetPlainDateTimeFor(cx, timeZone, ns1, &startDateTime)) {
    return false;
  }

  // Steps 6-7.
  PlainDateTime endDateTime;
  if (!GetPlainDateTimeFor(cx, timeZone, ns2, &endDateTime)) {
    return false;
  }

  // Step 8.
  Duration dateDifference;
  if (maybeOptions) {
    if (!DifferenceISODateTime(cx, startDateTime, endDateTime, calendar,
                               largestUnit, maybeOptions, &dateDifference)) {
      return false;
    }
  } else {
    if (!DifferenceISODateTime(cx, startDateTime, endDateTime, calendar,
                               largestUnit, &dateDifference)) {
      return false;
    }
  }

  // Step 9.
  Instant intermediateNs;
  if (!AddZonedDateTime(cx, ns1, timeZone, calendar,
                        {
                            dateDifference.years,
                            dateDifference.months,
                            dateDifference.weeks,
                        },
                        &intermediateNs)) {
    return false;
  }
  MOZ_ASSERT(IsValidEpochInstant(intermediateNs));

  // Step 10.
  auto timeRemainder = ns2 - intermediateNs;
  MOZ_ASSERT(IsValidInstantDifference(timeRemainder));

  // Step 11.
  Rooted<ZonedDateTimeObject*> intermediate(
      cx, CreateTemporalZonedDateTime(cx, intermediateNs, timeZone, calendar));
  if (!intermediate) {
    return false;
  }

  // Step 12.
  Rooted<NanosecondsAndDays> nanosAndDays(cx);
  if (!NanosecondsToDays(cx, timeRemainder, intermediate, &nanosAndDays)) {
    return false;
  }

  // Step 13.
  TimeDuration timeDifference;
  if (!BalanceDuration(cx, nanosAndDays.nanoseconds(), TemporalUnit::Hour,
                       &timeDifference)) {
    return false;
  }

  // Step 14.
  *result = {
      dateDifference.years,        dateDifference.months,
      dateDifference.weeks,        nanosAndDays.daysNumber(),
      timeDifference.hours,        timeDifference.minutes,
      timeDifference.seconds,      timeDifference.milliseconds,
      timeDifference.microseconds, timeDifference.nanoseconds,
  };
  MOZ_ASSERT(IsValidDuration(*result));
  return true;
}

/**
 * DifferenceZonedDateTime ( ns1, ns2, timeZone, calendar, largestUnit, options
 * )
 */
bool js::temporal::DifferenceZonedDateTime(JSContext* cx, const Instant& ns1,
                                           const Instant& ns2,
                                           Handle<JSObject*> timeZone,
                                           Handle<JSObject*> calendar,
                                           TemporalUnit largestUnit,
                                           Duration* result) {
  return ::DifferenceZonedDateTime(cx, ns1, ns2, timeZone, calendar,
                                   largestUnit, nullptr, result);
}

/**
 * TimeZoneEquals ( one, two )
 */
static bool TimeZoneEquals(JSContext* cx, Handle<JSObject*> one,
                           Handle<JSObject*> two, bool* equals) {
  // Step 1.
  if (one == two) {
    *equals = true;
    return true;
  }

  // Step 2.
  Rooted<JSString*> timeZoneOne(cx, TimeZoneToString(cx, one));
  if (!timeZoneOne) {
    return false;
  }

  // Step 3.
  JSString* timeZoneTwo = TimeZoneToString(cx, two);
  if (!timeZoneTwo) {
    return false;
  }

  // Steps 4-5.
  return EqualStrings(cx, timeZoneOne, timeZoneTwo, equals);
}

/**
 * TimeZoneEquals ( one, two )
 */
static bool TimeZoneEqualsOrThrow(JSContext* cx, Handle<JSObject*> one,
                                  Handle<JSObject*> two) {
  // Step 1.
  if (one == two) {
    return true;
  }

  // Step 2.
  Rooted<JSString*> timeZoneOne(cx, TimeZoneToString(cx, one));
  if (!timeZoneOne) {
    return false;
  }

  // Step 3.
  JSString* timeZoneTwo = TimeZoneToString(cx, two);
  if (!timeZoneTwo) {
    return false;
  }

  // Steps 4-5.
  bool equals;
  if (!EqualStrings(cx, timeZoneOne, timeZoneTwo, &equals)) {
    return false;
  }
  if (equals) {
    return true;
  }

  // Throw an error when the time zone identifiers don't match. Used when
  // unequal time zones throw a RangeError.
  if (auto charsOne = QuoteString(cx, timeZoneOne)) {
    if (auto charsTwo = QuoteString(cx, timeZoneTwo)) {
      JS_ReportErrorNumberUTF8(cx, GetErrorMessage, nullptr,
                               JSMSG_TEMPORAL_TIMEZONE_INCOMPATIBLE,
                               charsOne.get(), charsTwo.get());
    }
  }
  return false;
}

/**
 * RoundISODateTime ( year, month, day, hour, minute, second, millisecond,
 * microsecond, nanosecond, increment, unit, roundingMode [ , dayLength ] )
 */
static bool RoundISODateTime(JSContext* cx, const PlainDateTime& dateTime,
                             Increment increment, TemporalUnit unit,
                             TemporalRoundingMode roundingMode,
                             const Instant& dayLength, PlainDateTime* result) {
  MOZ_ASSERT(IsValidInstantDifference(dayLength));
  MOZ_ASSERT(dayLength > (Instant{}));

  const auto& [date, time] = dateTime;

  // Steps 1-2.
  MOZ_ASSERT(IsValidISODateTime(dateTime));
  MOZ_ASSERT(ISODateTimeWithinLimits(dateTime));

  // Step 3. (Not applicable in our implementation.)

  // Step 4.
  auto roundedTime = RoundTime(time, increment, unit, roundingMode, dayLength);

  // |dayLength| can be as small as 1, so the number of rounded days can be as
  // large as the number of nanoseconds in |time|.
  MOZ_ASSERT(0 <= roundedTime.days &&
             roundedTime.days < ToNanoseconds(TemporalUnit::Day));

  // Step 5.
  PlainDate balanceResult;
  if (!BalanceISODate(cx, date.year, date.month,
                      int64_t(date.day) + roundedTime.days, &balanceResult)) {
    return false;
  }

  // Step 6.
  *result = {balanceResult, roundedTime.time};
  return true;
}

/**
 * DifferenceTemporalZonedDateTime ( operation, zonedDateTime, other, options )
 */
static bool DifferenceTemporalZonedDateTime(JSContext* cx,
                                            TemporalDifference operation,
                                            const CallArgs& args) {
  Rooted<ZonedDateTimeObject*> zonedDateTime(
      cx, &args.thisv().toObject().as<ZonedDateTimeObject>());
  auto epochInstant = ToInstant(zonedDateTime);
  Rooted<JSObject*> timeZone(cx, zonedDateTime->timeZone());
  Rooted<JSObject*> calendar(cx, zonedDateTime->calendar());

  // Step 1. (Not applicable in our implementation.)

  // Step 2.
  Instant otherInstant;
  Rooted<JSObject*> otherTimeZone(cx);
  Rooted<JSObject*> otherCalendar(cx);
  if (!ToTemporalZonedDateTime(cx, args.get(0), &otherInstant, &otherTimeZone,
                               &otherCalendar)) {
    return false;
  }

  // Step 3.
  if (!CalendarEqualsOrThrow(cx, calendar, otherCalendar)) {
    return false;
  }

  // Steps 4-7.
  Rooted<PlainObject*> resolvedOptions(cx);
  DifferenceSettings settings;
  if (args.hasDefined(1)) {
    Rooted<JSObject*> options(
        cx, RequireObjectArg(cx, "options", ToName(operation), args[1]));
    if (!options) {
      return false;
    }

    // Step 4.
    resolvedOptions = NewPlainObjectWithProto(cx, nullptr);
    if (!resolvedOptions) {
      return false;
    }

    // Step 5.
    if (!CopyDataProperties(cx, resolvedOptions, options)) {
      return false;
    }

    // Step 6.
    if (!GetDifferenceSettings(
            cx, operation, resolvedOptions, TemporalUnitGroup::DateTime,
            TemporalUnit::Nanosecond, TemporalUnit::Hour, &settings)) {
      return false;
    }

    // Step 7.
    Rooted<Value> largestUnitValue(
        cx, StringValue(TemporalUnitToString(cx, settings.largestUnit)));
    if (!DefineDataProperty(cx, resolvedOptions, cx->names().largestUnit,
                            largestUnitValue)) {
      return false;
    }
  } else {
    // Steps 4-6.
    settings = {
        TemporalUnit::Nanosecond,
        TemporalUnit::Hour,
        TemporalRoundingMode::Trunc,
        Increment{1},
    };

    // Step 7. (Not applicable in our implementation.)
  }

  // Step 8.
  if (settings.largestUnit > TemporalUnit::Day) {
    MOZ_ASSERT(settings.smallestUnit >= settings.largestUnit);

    // Step 8.a.
    Duration difference;
    if (!DifferenceInstant(cx, epochInstant, otherInstant,
                           settings.roundingIncrement, settings.smallestUnit,
                           settings.largestUnit, settings.roundingMode,
                           &difference)) {
      return false;
    }

    // Step 8.b.
    if (operation == TemporalDifference::Since) {
      difference = difference.negate();
    }

    auto* result = CreateTemporalDuration(cx, difference);
    if (!result) {
      return false;
    }

    args.rval().setObject(*result);
    return true;
  }

  // FIXME: spec issue - move this step next to the calendar validation?
  // https://github.com/tc39/proposal-temporal/issues/2533

  // Step 9.
  if (!TimeZoneEqualsOrThrow(cx, timeZone, otherTimeZone)) {
    return false;
  }

  // Step 10.
  Duration difference;
  if (resolvedOptions) {
    if (!::DifferenceZonedDateTime(cx, epochInstant, otherInstant, timeZone,
                                   calendar, settings.largestUnit,
                                   resolvedOptions, &difference)) {
      return false;
    }
  } else {
    if (!::DifferenceZonedDateTime(cx, epochInstant, otherInstant, timeZone,
                                   calendar, settings.largestUnit, nullptr,
                                   &difference)) {
      return false;
    }
  }

  // Step 11.
  Duration roundResult;
  if (!RoundDuration(cx, difference, settings.roundingIncrement,
                     settings.smallestUnit, settings.roundingMode,
                     Handle<ZonedDateTimeObject*>(zonedDateTime),
                     &roundResult)) {
    return false;
  }

  // Step 12.
  Duration result;
  if (!AdjustRoundedDurationDays(cx, roundResult, settings.roundingIncrement,
                                 settings.smallestUnit, settings.roundingMode,
                                 zonedDateTime, &result)) {
    return false;
  }

  // Step 13.
  if (operation == TemporalDifference::Since) {
    result = result.negate();
  }

  auto* obj = CreateTemporalDuration(cx, result);
  if (!obj) {
    return false;
  }

  args.rval().setObject(*obj);
  return true;
}

enum class ZonedDateTimeDuration { Add, Subtract };

/**
 * AddDurationToOrSubtractDurationFromZonedDateTime ( operation, zonedDateTime,
 * temporalDurationLike, options )
 */
static bool AddDurationToOrSubtractDurationFromZonedDateTime(
    JSContext* cx, ZonedDateTimeDuration operation, const CallArgs& args) {
  auto* zonedDateTime = &args.thisv().toObject().as<ZonedDateTimeObject>();
  auto instant = ToInstant(zonedDateTime);

  // Step 1. (Not applicable in our implementation.)

  // Step 4. (Reorderd)
  Rooted<JSObject*> timeZone(cx, zonedDateTime->timeZone());

  // Step 5. (Reordered)
  Rooted<JSObject*> calendar(cx, zonedDateTime->calendar());

  // Step 2.
  Duration duration;
  if (!ToTemporalDurationRecord(cx, args.get(0), &duration)) {
    return false;
  }

  // Step 3.
  Rooted<JSObject*> options(cx);
  if (args.hasDefined(1)) {
    const char* name =
        operation == ZonedDateTimeDuration::Add ? "add" : "subtract";
    options = RequireObjectArg(cx, "options", name, args[1]);
  } else {
    options = NewPlainObjectWithProto(cx, nullptr);
  }
  if (!options) {
    return false;
  }

  // Step 6.
  if (operation == ZonedDateTimeDuration::Subtract) {
    duration = duration.negate();
  }

  Instant resultInstant;
  if (!::AddZonedDateTime(cx, instant, timeZone, calendar, duration, options,
                          &resultInstant)) {
    return false;
  }
  MOZ_ASSERT(IsValidEpochInstant(resultInstant));

  // Step 7.
  auto* result =
      CreateTemporalZonedDateTime(cx, resultInstant, timeZone, calendar);
  if (!result) {
    return false;
  }

  args.rval().setObject(*result);
  return true;
}

/**
 * Temporal.ZonedDateTime ( epochNanoseconds, timeZoneLike [ , calendarLike ] )
 */
static bool ZonedDateTimeConstructor(JSContext* cx, unsigned argc, Value* vp) {
  CallArgs args = CallArgsFromVp(argc, vp);

  // Step 1.
  if (!ThrowIfNotConstructing(cx, args, "Temporal.ZonedDateTime")) {
    return false;
  }

  // Step 2.
  Rooted<BigInt*> epochNanoseconds(cx, js::ToBigInt(cx, args.get(0)));
  if (!epochNanoseconds) {
    return false;
  }

  // Step 3.
  if (!IsValidEpochNanoseconds(epochNanoseconds)) {
    JS_ReportErrorNumberASCII(cx, GetErrorMessage, nullptr,
                              JSMSG_TEMPORAL_INSTANT_INVALID);
    return false;
  }

  // Step 4.
  Rooted<JSObject*> timeZone(cx, ToTemporalTimeZone(cx, args.get(1)));
  if (!timeZone) {
    return false;
  }

  // Step 5.
  Rooted<JSObject*> calendar(cx,
                             ToTemporalCalendarWithISODefault(cx, args.get(2)));
  if (!calendar) {
    return false;
  }

  // Step 6.
  auto* obj = CreateTemporalZonedDateTime(cx, args, epochNanoseconds, timeZone,
                                          calendar);
  if (!obj) {
    return false;
  }

  args.rval().setObject(*obj);
  return true;
}

/**
 * Temporal.ZonedDateTime.from ( item [ , options ] )
 */
static bool ZonedDateTime_from(JSContext* cx, unsigned argc, Value* vp) {
  CallArgs args = CallArgsFromVp(argc, vp);

  // Step 1.
  Rooted<JSObject*> options(cx);
  if (args.hasDefined(1)) {
    options = RequireObjectArg(cx, "options", "from", args[1]);
    if (!options) {
      return false;
    }
  }

  // Step 2.
  if (args.get(0).isObject()) {
    JSObject* item = &args[0].toObject();
    if (auto* zonedDateTime = item->maybeUnwrapIf<ZonedDateTimeObject>()) {
      auto epochInstant = ToInstant(zonedDateTime);
      Rooted<JSObject*> timeZone(cx, zonedDateTime->timeZone());
      Rooted<JSObject*> calendar(cx, zonedDateTime->calendar());

      if (!cx->compartment()->wrap(cx, &timeZone)) {
        return false;
      }
      if (!cx->compartment()->wrap(cx, &calendar)) {
        return false;
      }

      if (options) {
        // Steps 2.a-b.
        TemporalDisambiguation ignoredDisambiguation;
        if (!ToTemporalDisambiguation(cx, options, &ignoredDisambiguation)) {
          return false;
        }

        // Step 2.c.
        TemporalOffset ignoredOffset;
        if (!ToTemporalOffset(cx, options, &ignoredOffset)) {
          return false;
        }

        // Step 2.d.
        TemporalOverflow ignoredOverflow;
        if (!ToTemporalOverflow(cx, options, &ignoredOverflow)) {
          return false;
        }
      }

      // Step 2.e.
      auto* result =
          CreateTemporalZonedDateTime(cx, epochInstant, timeZone, calendar);
      if (!result) {
        return false;
      }

      args.rval().setObject(*result);
      return true;
    }
  }

  // Step 3.
  auto result = ToTemporalZonedDateTime(cx, args.get(0), options);
  if (!result) {
    return false;
  }

  args.rval().setObject(*result);
  return true;
}

/**
 * Temporal.ZonedDateTime.compare ( one, two )
 */
static bool ZonedDateTime_compare(JSContext* cx, unsigned argc, Value* vp) {
  CallArgs args = CallArgsFromVp(argc, vp);

  // Step 1.
  Instant one;
  if (!ToTemporalZonedDateTime(cx, args.get(0), &one)) {
    return false;
  }

  // Step 2.
  Instant two;
  if (!ToTemporalZonedDateTime(cx, args.get(1), &two)) {
    return false;
  }

  // Step 3.
  args.rval().setInt32(one > two ? 1 : one < two ? -1 : 0);
  return true;
}

/**
 * get Temporal.ZonedDateTime.prototype.calendar
 */
static bool ZonedDateTime_calendar(JSContext* cx, const CallArgs& args) {
  // Step 3.
  auto* zonedDateTime = &args.thisv().toObject().as<ZonedDateTimeObject>();
  args.rval().setObject(*zonedDateTime->calendar());
  return true;
}

/**
 * get Temporal.ZonedDateTime.prototype.calendar
 */
static bool ZonedDateTime_calendar(JSContext* cx, unsigned argc, Value* vp) {
  // Steps 1-2.
  CallArgs args = CallArgsFromVp(argc, vp);
  return CallNonGenericMethod<IsZonedDateTime, ZonedDateTime_calendar>(cx,
                                                                       args);
}

/**
 * get Temporal.ZonedDateTime.prototype.timeZone
 */
static bool ZonedDateTime_timeZone(JSContext* cx, const CallArgs& args) {
  // Step 3.
  auto* zonedDateTime = &args.thisv().toObject().as<ZonedDateTimeObject>();
  args.rval().setObject(*zonedDateTime->timeZone());
  return true;
}

/**
 * get Temporal.ZonedDateTime.prototype.timeZone
 */
static bool ZonedDateTime_timeZone(JSContext* cx, unsigned argc, Value* vp) {
  // Steps 1-2.
  CallArgs args = CallArgsFromVp(argc, vp);
  return CallNonGenericMethod<IsZonedDateTime, ZonedDateTime_timeZone>(cx,
                                                                       args);
}

/**
 * get Temporal.ZonedDateTime.prototype.year
 */
static bool ZonedDateTime_year(JSContext* cx, const CallArgs& args) {
  auto* zonedDateTime = &args.thisv().toObject().as<ZonedDateTimeObject>();
  auto instant = ToInstant(zonedDateTime);
  Rooted<JSObject*> timeZone(cx, zonedDateTime->timeZone());
  Rooted<JSObject*> calendar(cx, zonedDateTime->calendar());

  // Steps 3-6.
  auto* dateTime = GetPlainDateTimeFor(cx, timeZone, instant, calendar);
  if (!dateTime) {
    return false;
  }
  Rooted<Value> temporalDateTime(cx, ObjectValue(*dateTime));

  // Step 7.
  return CalendarYear(cx, calendar, temporalDateTime, args.rval());
}

/**
 * get Temporal.ZonedDateTime.prototype.year
 */
static bool ZonedDateTime_year(JSContext* cx, unsigned argc, Value* vp) {
  // Steps 1-2.
  CallArgs args = CallArgsFromVp(argc, vp);
  return CallNonGenericMethod<IsZonedDateTime, ZonedDateTime_year>(cx, args);
}

/**
 * get Temporal.ZonedDateTime.prototype.month
 */
static bool ZonedDateTime_month(JSContext* cx, const CallArgs& args) {
  auto* zonedDateTime = &args.thisv().toObject().as<ZonedDateTimeObject>();
  auto instant = ToInstant(zonedDateTime);
  Rooted<JSObject*> timeZone(cx, zonedDateTime->timeZone());
  Rooted<JSObject*> calendar(cx, zonedDateTime->calendar());

  // Steps 3-6.
  auto* dateTime = GetPlainDateTimeFor(cx, timeZone, instant, calendar);
  if (!dateTime) {
    return false;
  }
  Rooted<Value> temporalDateTime(cx, ObjectValue(*dateTime));

  // Step 7.
  return CalendarMonth(cx, calendar, temporalDateTime, args.rval());
}

/**
 * get Temporal.ZonedDateTime.prototype.month
 */
static bool ZonedDateTime_month(JSContext* cx, unsigned argc, Value* vp) {
  // Steps 1-2.
  CallArgs args = CallArgsFromVp(argc, vp);
  return CallNonGenericMethod<IsZonedDateTime, ZonedDateTime_month>(cx, args);
}

/**
 * get Temporal.ZonedDateTime.prototype.monthCode
 */
static bool ZonedDateTime_monthCode(JSContext* cx, const CallArgs& args) {
  auto* zonedDateTime = &args.thisv().toObject().as<ZonedDateTimeObject>();
  auto instant = ToInstant(zonedDateTime);
  Rooted<JSObject*> timeZone(cx, zonedDateTime->timeZone());
  Rooted<JSObject*> calendar(cx, zonedDateTime->calendar());

  // Steps 3-6.
  auto* dateTime = GetPlainDateTimeFor(cx, timeZone, instant, calendar);
  if (!dateTime) {
    return false;
  }
  Rooted<Value> temporalDateTime(cx, ObjectValue(*dateTime));

  // Step 7.
  return CalendarMonthCode(cx, calendar, temporalDateTime, args.rval());
}

/**
 * get Temporal.ZonedDateTime.prototype.monthCode
 */
static bool ZonedDateTime_monthCode(JSContext* cx, unsigned argc, Value* vp) {
  // Steps 1-2.
  CallArgs args = CallArgsFromVp(argc, vp);
  return CallNonGenericMethod<IsZonedDateTime, ZonedDateTime_monthCode>(cx,
                                                                        args);
}

/**
 * get Temporal.ZonedDateTime.prototype.day
 */
static bool ZonedDateTime_day(JSContext* cx, const CallArgs& args) {
  auto* zonedDateTime = &args.thisv().toObject().as<ZonedDateTimeObject>();
  auto instant = ToInstant(zonedDateTime);
  Rooted<JSObject*> timeZone(cx, zonedDateTime->timeZone());
  Rooted<JSObject*> calendar(cx, zonedDateTime->calendar());

  // Steps 3-6.
  auto* dateTime = GetPlainDateTimeFor(cx, timeZone, instant, calendar);
  if (!dateTime) {
    return false;
  }
  Rooted<Value> temporalDateTime(cx, ObjectValue(*dateTime));

  // Step 7.
  return CalendarDay(cx, calendar, temporalDateTime, args.rval());
}

/**
 * get Temporal.ZonedDateTime.prototype.day
 */
static bool ZonedDateTime_day(JSContext* cx, unsigned argc, Value* vp) {
  // Steps 1-2.
  CallArgs args = CallArgsFromVp(argc, vp);
  return CallNonGenericMethod<IsZonedDateTime, ZonedDateTime_day>(cx, args);
}

/**
 * get Temporal.ZonedDateTime.prototype.hour
 */
static bool ZonedDateTime_hour(JSContext* cx, const CallArgs& args) {
  auto* zonedDateTime = &args.thisv().toObject().as<ZonedDateTimeObject>();
  auto instant = ToInstant(zonedDateTime);
  Rooted<JSObject*> timeZone(cx, zonedDateTime->timeZone());

  // Steps 3-6.
  PlainDateTime dateTime;
  if (!GetPlainDateTimeFor(cx, timeZone, instant, &dateTime)) {
    return false;
  }

  // Step 7.
  args.rval().setInt32(dateTime.time.hour);
  return true;
}

/**
 * get Temporal.ZonedDateTime.prototype.hour
 */
static bool ZonedDateTime_hour(JSContext* cx, unsigned argc, Value* vp) {
  // Steps 1-2.
  CallArgs args = CallArgsFromVp(argc, vp);
  return CallNonGenericMethod<IsZonedDateTime, ZonedDateTime_hour>(cx, args);
}

/**
 * get Temporal.ZonedDateTime.prototype.minute
 */
static bool ZonedDateTime_minute(JSContext* cx, const CallArgs& args) {
  auto* zonedDateTime = &args.thisv().toObject().as<ZonedDateTimeObject>();
  auto instant = ToInstant(zonedDateTime);
  Rooted<JSObject*> timeZone(cx, zonedDateTime->timeZone());

  // Steps 3-6.
  PlainDateTime dateTime;
  if (!GetPlainDateTimeFor(cx, timeZone, instant, &dateTime)) {
    return false;
  }

  // Step 7.
  args.rval().setInt32(dateTime.time.minute);
  return true;
}

/**
 * get Temporal.ZonedDateTime.prototype.minute
 */
static bool ZonedDateTime_minute(JSContext* cx, unsigned argc, Value* vp) {
  // Steps 1-2.
  CallArgs args = CallArgsFromVp(argc, vp);
  return CallNonGenericMethod<IsZonedDateTime, ZonedDateTime_minute>(cx, args);
}

/**
 * get Temporal.ZonedDateTime.prototype.second
 */
static bool ZonedDateTime_second(JSContext* cx, const CallArgs& args) {
  auto* zonedDateTime = &args.thisv().toObject().as<ZonedDateTimeObject>();
  auto instant = ToInstant(zonedDateTime);
  Rooted<JSObject*> timeZone(cx, zonedDateTime->timeZone());

  // Steps 3-6.
  PlainDateTime dateTime;
  if (!GetPlainDateTimeFor(cx, timeZone, instant, &dateTime)) {
    return false;
  }

  // Step 7.
  args.rval().setInt32(dateTime.time.second);
  return true;
}

/**
 * get Temporal.ZonedDateTime.prototype.second
 */
static bool ZonedDateTime_second(JSContext* cx, unsigned argc, Value* vp) {
  // Steps 1-2.
  CallArgs args = CallArgsFromVp(argc, vp);
  return CallNonGenericMethod<IsZonedDateTime, ZonedDateTime_second>(cx, args);
}

/**
 * get Temporal.ZonedDateTime.prototype.millisecond
 */
static bool ZonedDateTime_millisecond(JSContext* cx, const CallArgs& args) {
  auto* zonedDateTime = &args.thisv().toObject().as<ZonedDateTimeObject>();
  auto instant = ToInstant(zonedDateTime);
  Rooted<JSObject*> timeZone(cx, zonedDateTime->timeZone());

  // Steps 3-6.
  PlainDateTime dateTime;
  if (!GetPlainDateTimeFor(cx, timeZone, instant, &dateTime)) {
    return false;
  }

  // Step 7.
  args.rval().setInt32(dateTime.time.millisecond);
  return true;
}

/**
 * get Temporal.ZonedDateTime.prototype.millisecond
 */
static bool ZonedDateTime_millisecond(JSContext* cx, unsigned argc, Value* vp) {
  // Steps 1-2.
  CallArgs args = CallArgsFromVp(argc, vp);
  return CallNonGenericMethod<IsZonedDateTime, ZonedDateTime_millisecond>(cx,
                                                                          args);
}

/**
 * get Temporal.ZonedDateTime.prototype.microsecond
 */
static bool ZonedDateTime_microsecond(JSContext* cx, const CallArgs& args) {
  auto* zonedDateTime = &args.thisv().toObject().as<ZonedDateTimeObject>();
  auto instant = ToInstant(zonedDateTime);
  Rooted<JSObject*> timeZone(cx, zonedDateTime->timeZone());

  // Steps 3-6.
  PlainDateTime dateTime;
  if (!GetPlainDateTimeFor(cx, timeZone, instant, &dateTime)) {
    return false;
  }

  // Step 7.
  args.rval().setInt32(dateTime.time.microsecond);
  return true;
}

/**
 * get Temporal.ZonedDateTime.prototype.microsecond
 */
static bool ZonedDateTime_microsecond(JSContext* cx, unsigned argc, Value* vp) {
  // Steps 1-2.
  CallArgs args = CallArgsFromVp(argc, vp);
  return CallNonGenericMethod<IsZonedDateTime, ZonedDateTime_microsecond>(cx,
                                                                          args);
}

/**
 * get Temporal.ZonedDateTime.prototype.nanosecond
 */
static bool ZonedDateTime_nanosecond(JSContext* cx, const CallArgs& args) {
  auto* zonedDateTime = &args.thisv().toObject().as<ZonedDateTimeObject>();
  auto instant = ToInstant(zonedDateTime);
  Rooted<JSObject*> timeZone(cx, zonedDateTime->timeZone());

  // Steps 3-6.
  PlainDateTime dateTime;
  if (!GetPlainDateTimeFor(cx, timeZone, instant, &dateTime)) {
    return false;
  }

  // Step 7.
  args.rval().setInt32(dateTime.time.nanosecond);
  return true;
}

/**
 * get Temporal.ZonedDateTime.prototype.nanosecond
 */
static bool ZonedDateTime_nanosecond(JSContext* cx, unsigned argc, Value* vp) {
  // Steps 1-2.
  CallArgs args = CallArgsFromVp(argc, vp);
  return CallNonGenericMethod<IsZonedDateTime, ZonedDateTime_nanosecond>(cx,
                                                                         args);
}

/**
 * get Temporal.ZonedDateTime.prototype.epochSeconds
 */
static bool ZonedDateTime_epochSeconds(JSContext* cx, const CallArgs& args) {
  auto* zonedDateTime = &args.thisv().toObject().as<ZonedDateTimeObject>();

  // Step 3.
  auto instant = ToInstant(zonedDateTime);

  // Steps 4-5.
  args.rval().setNumber(instant.seconds);
  return true;
}

/**
 * get Temporal.ZonedDateTime.prototype.epochSeconds
 */
static bool ZonedDateTime_epochSeconds(JSContext* cx, unsigned argc,
                                       Value* vp) {
  // Steps 1-2.
  CallArgs args = CallArgsFromVp(argc, vp);
  return CallNonGenericMethod<IsZonedDateTime, ZonedDateTime_epochSeconds>(
      cx, args);
}

/**
 * get Temporal.ZonedDateTime.prototype.epochMilliseconds
 */
static bool ZonedDateTime_epochMilliseconds(JSContext* cx,
                                            const CallArgs& args) {
  auto* zonedDateTime = &args.thisv().toObject().as<ZonedDateTimeObject>();

  // Step 3.
  auto instant = ToInstant(zonedDateTime);

  // Steps 4-5.
  args.rval().setNumber(instant.floorToMilliseconds());
  return true;
}

/**
 * get Temporal.ZonedDateTime.prototype.epochMilliseconds
 */
static bool ZonedDateTime_epochMilliseconds(JSContext* cx, unsigned argc,
                                            Value* vp) {
  // Steps 1-2.
  CallArgs args = CallArgsFromVp(argc, vp);
  return CallNonGenericMethod<IsZonedDateTime, ZonedDateTime_epochMilliseconds>(
      cx, args);
}

/**
 * get Temporal.ZonedDateTime.prototype.epochMicroseconds
 */
static bool ZonedDateTime_epochMicroseconds(JSContext* cx,
                                            const CallArgs& args) {
  auto* zonedDateTime = &args.thisv().toObject().as<ZonedDateTimeObject>();

  // Step 3.
  auto instant = ToInstant(zonedDateTime);

  // Step 4.
  auto* microseconds =
      BigInt::createFromInt64(cx, instant.floorToMicroseconds());
  if (!microseconds) {
    return false;
  }

  // Step 5.
  args.rval().setBigInt(microseconds);
  return true;
}

/**
 * get Temporal.ZonedDateTime.prototype.epochMicroseconds
 */
static bool ZonedDateTime_epochMicroseconds(JSContext* cx, unsigned argc,
                                            Value* vp) {
  // Steps 1-2.
  CallArgs args = CallArgsFromVp(argc, vp);
  return CallNonGenericMethod<IsZonedDateTime, ZonedDateTime_epochMicroseconds>(
      cx, args);
}

/**
 * get Temporal.ZonedDateTime.prototype.epochNanoseconds
 */
static bool ZonedDateTime_epochNanoseconds(JSContext* cx,
                                           const CallArgs& args) {
  auto* zonedDateTime = &args.thisv().toObject().as<ZonedDateTimeObject>();

  // Step 3.
  auto* nanoseconds = ToEpochNanoseconds(cx, ToInstant(zonedDateTime));
  if (!nanoseconds) {
    return false;
  }

  args.rval().setBigInt(nanoseconds);
  return true;
}

/**
 * get Temporal.ZonedDateTime.prototype.epochNanoseconds
 */
static bool ZonedDateTime_epochNanoseconds(JSContext* cx, unsigned argc,
                                           Value* vp) {
  // Steps 1-2.
  CallArgs args = CallArgsFromVp(argc, vp);
  return CallNonGenericMethod<IsZonedDateTime, ZonedDateTime_epochNanoseconds>(
      cx, args);
}

/**
 * get Temporal.ZonedDateTime.prototype.dayOfWeek
 */
static bool ZonedDateTime_dayOfWeek(JSContext* cx, const CallArgs& args) {
  auto* zonedDateTime = &args.thisv().toObject().as<ZonedDateTimeObject>();
  auto instant = ToInstant(zonedDateTime);
  Rooted<JSObject*> timeZone(cx, zonedDateTime->timeZone());
  Rooted<JSObject*> calendar(cx, zonedDateTime->calendar());

  // Steps 3-6.
  auto* dateTime = GetPlainDateTimeFor(cx, timeZone, instant, calendar);
  if (!dateTime) {
    return false;
  }
  Rooted<Value> temporalDateTime(cx, ObjectValue(*dateTime));

  // Step 7.
  return CalendarDayOfWeek(cx, calendar, temporalDateTime, args.rval());
}

/**
 * get Temporal.ZonedDateTime.prototype.dayOfWeek
 */
static bool ZonedDateTime_dayOfWeek(JSContext* cx, unsigned argc, Value* vp) {
  // Steps 1-2.
  CallArgs args = CallArgsFromVp(argc, vp);
  return CallNonGenericMethod<IsZonedDateTime, ZonedDateTime_dayOfWeek>(cx,
                                                                        args);
}

/**
 * get Temporal.ZonedDateTime.prototype.dayOfYear
 */
static bool ZonedDateTime_dayOfYear(JSContext* cx, const CallArgs& args) {
  auto* zonedDateTime = &args.thisv().toObject().as<ZonedDateTimeObject>();
  auto instant = ToInstant(zonedDateTime);
  Rooted<JSObject*> timeZone(cx, zonedDateTime->timeZone());
  Rooted<JSObject*> calendar(cx, zonedDateTime->calendar());

  // Steps 3-6.
  auto* dateTime = GetPlainDateTimeFor(cx, timeZone, instant, calendar);
  if (!dateTime) {
    return false;
  }
  Rooted<Value> temporalDateTime(cx, ObjectValue(*dateTime));

  // Step 7.
  return CalendarDayOfYear(cx, calendar, temporalDateTime, args.rval());
}

/**
 * get Temporal.ZonedDateTime.prototype.dayOfYear
 */
static bool ZonedDateTime_dayOfYear(JSContext* cx, unsigned argc, Value* vp) {
  // Steps 1-2.
  CallArgs args = CallArgsFromVp(argc, vp);
  return CallNonGenericMethod<IsZonedDateTime, ZonedDateTime_dayOfYear>(cx,
                                                                        args);
}

/**
 * get Temporal.ZonedDateTime.prototype.weekOfYear
 */
static bool ZonedDateTime_weekOfYear(JSContext* cx, const CallArgs& args) {
  auto* zonedDateTime = &args.thisv().toObject().as<ZonedDateTimeObject>();
  auto instant = ToInstant(zonedDateTime);
  Rooted<JSObject*> timeZone(cx, zonedDateTime->timeZone());
  Rooted<JSObject*> calendar(cx, zonedDateTime->calendar());

  // Steps 3-6.
  auto* dateTime = GetPlainDateTimeFor(cx, timeZone, instant, calendar);
  if (!dateTime) {
    return false;
  }
  Rooted<Value> temporalDateTime(cx, ObjectValue(*dateTime));

  // Step 7.
  return CalendarWeekOfYear(cx, calendar, temporalDateTime, args.rval());
}

/**
 * get Temporal.ZonedDateTime.prototype.weekOfYear
 */
static bool ZonedDateTime_weekOfYear(JSContext* cx, unsigned argc, Value* vp) {
  // Steps 1-2.
  CallArgs args = CallArgsFromVp(argc, vp);
  return CallNonGenericMethod<IsZonedDateTime, ZonedDateTime_weekOfYear>(cx,
                                                                         args);
}

/**
 * get Temporal.ZonedDateTime.prototype.yearOfWeek
 */
static bool ZonedDateTime_yearOfWeek(JSContext* cx, const CallArgs& args) {
  auto* zonedDateTime = &args.thisv().toObject().as<ZonedDateTimeObject>();
  auto instant = ToInstant(zonedDateTime);
  Rooted<JSObject*> timeZone(cx, zonedDateTime->timeZone());
  Rooted<JSObject*> calendar(cx, zonedDateTime->calendar());

  // Steps 3-6.
  auto* dateTime = GetPlainDateTimeFor(cx, timeZone, instant, calendar);
  if (!dateTime) {
    return false;
  }
  Rooted<Value> temporalDateTime(cx, ObjectValue(*dateTime));

  // Step 7.
  return CalendarYearOfWeek(cx, calendar, temporalDateTime, args.rval());
}

/**
 * get Temporal.ZonedDateTime.prototype.yearOfWeek
 */
static bool ZonedDateTime_yearOfWeek(JSContext* cx, unsigned argc, Value* vp) {
  // Steps 1-2.
  CallArgs args = CallArgsFromVp(argc, vp);
  return CallNonGenericMethod<IsZonedDateTime, ZonedDateTime_yearOfWeek>(cx,
                                                                         args);
}

/**
 * get Temporal.ZonedDateTime.prototype.hoursInDay
 */
static bool ZonedDateTime_hoursInDay(JSContext* cx, const CallArgs& args) {
  auto* zonedDateTime = &args.thisv().toObject().as<ZonedDateTimeObject>();
  auto epochInstant = ToInstant(zonedDateTime);

  // Step 3.
  Rooted<JSObject*> timeZone(cx, zonedDateTime->timeZone());

  // Step 5.
  Rooted<CalendarObject*> isoCalendar(cx, GetISO8601Calendar(cx));
  if (!isoCalendar) {
    return false;
  }

  // Steps 4 and 6.
  PlainDateTime temporalDateTime;
  if (!GetPlainDateTimeFor(cx, timeZone, epochInstant, &temporalDateTime)) {
    return false;
  }

  // Steps 7-9.
  const auto& date = temporalDateTime.date;

  // Step 10.
  Rooted<PlainDateTimeObject*> today(
      cx, CreateTemporalDateTime(cx, {date, {}}, isoCalendar));
  if (!today) {
    return false;
  }

  // Step 11.
  PlainDate tomorrowFields =
      BalanceISODate(date.year, date.month, date.day + 1);

  // Step 12.
  Rooted<PlainDateTimeObject*> tomorrow(
      cx, CreateTemporalDateTime(cx, {tomorrowFields, {}}, isoCalendar));
  if (!tomorrow) {
    return false;
  }

  // Step 13.
  Instant todayInstant;
  if (!GetInstantFor(cx, timeZone, today, TemporalDisambiguation::Compatible,
                     &todayInstant)) {
    return false;
  }

  // Step 14.
  Instant tomorrowInstant;
  if (!GetInstantFor(cx, timeZone, tomorrow, TemporalDisambiguation::Compatible,
                     &tomorrowInstant)) {
    return false;
  }

  // Step 15.
  auto diffNs = tomorrowInstant - todayInstant;
  MOZ_ASSERT(IsValidInstantDifference(diffNs));

  // Step 16.
  constexpr int32_t secPerHour = 60 * 60;
  constexpr int64_t nsPerSec = ToNanoseconds(TemporalUnit::Second);
  constexpr double nsPerHour = ToNanoseconds(TemporalUnit::Hour);

  int64_t hours = diffNs.seconds / secPerHour;
  int64_t seconds = diffNs.seconds % secPerHour;
  int64_t nanoseconds = seconds * nsPerSec + diffNs.nanoseconds;

  double result = double(hours) + double(nanoseconds) / nsPerHour;
  args.rval().setNumber(result);
  return true;
}

/**
 * get Temporal.ZonedDateTime.prototype.hoursInDay
 */
static bool ZonedDateTime_hoursInDay(JSContext* cx, unsigned argc, Value* vp) {
  // Steps 1-2.
  CallArgs args = CallArgsFromVp(argc, vp);
  return CallNonGenericMethod<IsZonedDateTime, ZonedDateTime_hoursInDay>(cx,
                                                                         args);
}

/**
 * get Temporal.ZonedDateTime.prototype.daysInWeek
 */
static bool ZonedDateTime_daysInWeek(JSContext* cx, const CallArgs& args) {
  auto* zonedDateTime = &args.thisv().toObject().as<ZonedDateTimeObject>();
  auto instant = ToInstant(zonedDateTime);
  Rooted<JSObject*> timeZone(cx, zonedDateTime->timeZone());
  Rooted<JSObject*> calendar(cx, zonedDateTime->calendar());

  // Steps 3-6.
  auto* dateTime = GetPlainDateTimeFor(cx, timeZone, instant, calendar);
  if (!dateTime) {
    return false;
  }
  Rooted<Value> temporalDateTime(cx, ObjectValue(*dateTime));

  // Step 7.
  return CalendarDaysInWeek(cx, calendar, temporalDateTime, args.rval());
}

/**
 * get Temporal.ZonedDateTime.prototype.daysInWeek
 */
static bool ZonedDateTime_daysInWeek(JSContext* cx, unsigned argc, Value* vp) {
  // Steps 1-2.
  CallArgs args = CallArgsFromVp(argc, vp);
  return CallNonGenericMethod<IsZonedDateTime, ZonedDateTime_daysInWeek>(cx,
                                                                         args);
}

/**
 * get Temporal.ZonedDateTime.prototype.daysInMonth
 */
static bool ZonedDateTime_daysInMonth(JSContext* cx, const CallArgs& args) {
  auto* zonedDateTime = &args.thisv().toObject().as<ZonedDateTimeObject>();
  auto instant = ToInstant(zonedDateTime);
  Rooted<JSObject*> timeZone(cx, zonedDateTime->timeZone());
  Rooted<JSObject*> calendar(cx, zonedDateTime->calendar());

  // Steps 3-6.
  auto* dateTime = GetPlainDateTimeFor(cx, timeZone, instant, calendar);
  if (!dateTime) {
    return false;
  }
  Rooted<Value> temporalDateTime(cx, ObjectValue(*dateTime));

  // Step 7.
  return CalendarDaysInMonth(cx, calendar, temporalDateTime, args.rval());
}

/**
 * get Temporal.ZonedDateTime.prototype.daysInMonth
 */
static bool ZonedDateTime_daysInMonth(JSContext* cx, unsigned argc, Value* vp) {
  // Steps 1-2.
  CallArgs args = CallArgsFromVp(argc, vp);
  return CallNonGenericMethod<IsZonedDateTime, ZonedDateTime_daysInMonth>(cx,
                                                                          args);
}

/**
 * get Temporal.ZonedDateTime.prototype.daysInYear
 */
static bool ZonedDateTime_daysInYear(JSContext* cx, const CallArgs& args) {
  auto* zonedDateTime = &args.thisv().toObject().as<ZonedDateTimeObject>();
  auto instant = ToInstant(zonedDateTime);
  Rooted<JSObject*> timeZone(cx, zonedDateTime->timeZone());
  Rooted<JSObject*> calendar(cx, zonedDateTime->calendar());

  // Steps 3-6.
  auto* dateTime = GetPlainDateTimeFor(cx, timeZone, instant, calendar);
  if (!dateTime) {
    return false;
  }
  Rooted<Value> temporalDateTime(cx, ObjectValue(*dateTime));

  // Step 7.
  return CalendarDaysInYear(cx, calendar, temporalDateTime, args.rval());
}

/**
 * get Temporal.ZonedDateTime.prototype.daysInYear
 */
static bool ZonedDateTime_daysInYear(JSContext* cx, unsigned argc, Value* vp) {
  // Steps 1-2.
  CallArgs args = CallArgsFromVp(argc, vp);
  return CallNonGenericMethod<IsZonedDateTime, ZonedDateTime_daysInYear>(cx,
                                                                         args);
}

/**
 * get Temporal.ZonedDateTime.prototype.monthsInYear
 */
static bool ZonedDateTime_monthsInYear(JSContext* cx, const CallArgs& args) {
  auto* zonedDateTime = &args.thisv().toObject().as<ZonedDateTimeObject>();
  auto instant = ToInstant(zonedDateTime);
  Rooted<JSObject*> timeZone(cx, zonedDateTime->timeZone());
  Rooted<JSObject*> calendar(cx, zonedDateTime->calendar());

  // Steps 3-6.
  auto* dateTime = GetPlainDateTimeFor(cx, timeZone, instant, calendar);
  if (!dateTime) {
    return false;
  }
  Rooted<Value> temporalDateTime(cx, ObjectValue(*dateTime));

  // Step 7.
  return CalendarMonthsInYear(cx, calendar, temporalDateTime, args.rval());
}

/**
 * get Temporal.ZonedDateTime.prototype.monthsInYear
 */
static bool ZonedDateTime_monthsInYear(JSContext* cx, unsigned argc,
                                       Value* vp) {
  // Steps 1-2.
  CallArgs args = CallArgsFromVp(argc, vp);
  return CallNonGenericMethod<IsZonedDateTime, ZonedDateTime_monthsInYear>(
      cx, args);
}

/**
 * get Temporal.ZonedDateTime.prototype.inLeapYear
 */
static bool ZonedDateTime_inLeapYear(JSContext* cx, const CallArgs& args) {
  auto* zonedDateTime = &args.thisv().toObject().as<ZonedDateTimeObject>();
  auto instant = ToInstant(zonedDateTime);
  Rooted<JSObject*> timeZone(cx, zonedDateTime->timeZone());
  Rooted<JSObject*> calendar(cx, zonedDateTime->calendar());

  // Steps 3-6.
  auto* dateTime = GetPlainDateTimeFor(cx, timeZone, instant, calendar);
  if (!dateTime) {
    return false;
  }
  Rooted<Value> temporalDateTime(cx, ObjectValue(*dateTime));

  // Step 7.
  return CalendarInLeapYear(cx, calendar, temporalDateTime, args.rval());
}

/**
 * get Temporal.ZonedDateTime.prototype.inLeapYear
 */
static bool ZonedDateTime_inLeapYear(JSContext* cx, unsigned argc, Value* vp) {
  // Steps 1-2.
  CallArgs args = CallArgsFromVp(argc, vp);
  return CallNonGenericMethod<IsZonedDateTime, ZonedDateTime_inLeapYear>(cx,
                                                                         args);
}

/**
 * get Temporal.ZonedDateTime.prototype.offsetNanoseconds
 */
static bool ZonedDateTime_offsetNanoseconds(JSContext* cx,
                                            const CallArgs& args) {
  auto* zonedDateTime = &args.thisv().toObject().as<ZonedDateTimeObject>();

  // Step 3.
  Rooted<JSObject*> timeZone(cx, zonedDateTime->timeZone());

  // Step 4.
  auto instant = ToInstant(zonedDateTime);

  // Step 5.
  int64_t offsetNanoseconds;
  if (!GetOffsetNanosecondsFor(cx, timeZone, instant, &offsetNanoseconds)) {
    return false;
  }
  MOZ_ASSERT(std::abs(offsetNanoseconds) < ToNanoseconds(TemporalUnit::Day));

  args.rval().setNumber(offsetNanoseconds);
  return true;
}

/**
 * get Temporal.ZonedDateTime.prototype.offsetNanoseconds
 */
static bool ZonedDateTime_offsetNanoseconds(JSContext* cx, unsigned argc,
                                            Value* vp) {
  // Steps 1-2.
  CallArgs args = CallArgsFromVp(argc, vp);
  return CallNonGenericMethod<IsZonedDateTime, ZonedDateTime_offsetNanoseconds>(
      cx, args);
}

/**
 * get Temporal.ZonedDateTime.prototype.offset
 */
static bool ZonedDateTime_offset(JSContext* cx, const CallArgs& args) {
  auto* zonedDateTime = &args.thisv().toObject().as<ZonedDateTimeObject>();
  auto epochInstant = ToInstant(zonedDateTime);

  // Step 3.
  Rooted<JSObject*> timeZone(cx, zonedDateTime->timeZone());

  // Step 4.
  Rooted<InstantObject*> instant(cx, CreateTemporalInstant(cx, epochInstant));
  if (!instant) {
    return false;
  }

  // Step 5.
  JSString* str = GetOffsetStringFor(cx, timeZone, instant);
  if (!str) {
    return false;
  }

  args.rval().setString(str);
  return true;
}

/**
 * get Temporal.ZonedDateTime.prototype.offset
 */
static bool ZonedDateTime_offset(JSContext* cx, unsigned argc, Value* vp) {
  // Steps 1-2.
  CallArgs args = CallArgsFromVp(argc, vp);
  return CallNonGenericMethod<IsZonedDateTime, ZonedDateTime_offset>(cx, args);
}

/**
 * Temporal.ZonedDateTime.prototype.with ( temporalZonedDateTimeLike [ , options
 * ] )
 */
static bool ZonedDateTime_with(JSContext* cx, const CallArgs& args) {
  Rooted<ZonedDateTimeObject*> zonedDateTime(
      cx, &args.thisv().toObject().as<ZonedDateTimeObject>());

  // Step 3.
  Rooted<JSObject*> temporalZonedDateTimeLike(
      cx,
      RequireObjectArg(cx, "temporalZonedDateTimeLike", "with", args.get(0)));
  if (!temporalZonedDateTimeLike) {
    return false;
  }

  // Step 4.
  if (!RejectObjectWithCalendarOrTimeZone(cx, temporalZonedDateTimeLike)) {
    return false;
  }

  // Step 5.
  Rooted<JSObject*> options(cx);
  if (args.hasDefined(1)) {
    options = RequireObjectArg(cx, "options", "with", args[1]);
  } else {
    options = NewPlainObjectWithProto(cx, nullptr);
  }
  if (!options) {
    return false;
  }

  // Step 6.
  Rooted<JSObject*> calendar(cx, zonedDateTime->calendar());

  // Step 7.
  JS::RootedVector<PropertyKey> fieldNames(cx);
  if (!CalendarFields(cx, calendar,
                      {CalendarField::Day, CalendarField::Hour,
                       CalendarField::Microsecond, CalendarField::Millisecond,
                       CalendarField::Minute, CalendarField::Month,
                       CalendarField::MonthCode, CalendarField::Nanosecond,
                       CalendarField::Second, CalendarField::Year},
                      &fieldNames)) {
    return false;
  }

  // FIXME: spec issue - "offset" can already be part of |fieldNames|. Consider
  // using MergeLists(fieldNames, «"offset"») here.
  // https://github.com/tc39/proposal-temporal/issues/2532

  // Step 8.
  if (!AppendSorted(cx, fieldNames.get(), {TemporalField::Offset})) {
    return false;
  }

  // Step 9.
  Rooted<PlainObject*> fields(
      cx, PrepareTemporalFields(cx, zonedDateTime, fieldNames,
                                {TemporalField::Offset}));
  if (!fields) {
    return false;
  }

  // Step 10.
  Rooted<PlainObject*> partialZonedDateTime(
      cx,
      PreparePartialTemporalFields(cx, temporalZonedDateTimeLike, fieldNames));
  if (!partialZonedDateTime) {
    return false;
  }

  // Step 11.
  Rooted<JSObject*> mergedFields(
      cx, CalendarMergeFields(cx, calendar, fields, partialZonedDateTime));
  if (!mergedFields) {
    return false;
  }

  // Step 12.
  fields = PrepareTemporalFields(cx, mergedFields, fieldNames,
                                 {TemporalField::Offset});
  if (!fields) {
    return false;
  }

  // Step 13-14.
  auto disambiguation = TemporalDisambiguation::Compatible;
  if (!ToTemporalDisambiguation(cx, options, &disambiguation)) {
    return false;
  }

  // Step 15.
  auto offset = TemporalOffset::Prefer;
  if (!ToTemporalOffset(cx, options, &offset)) {
    return false;
  }

  // Step 16.
  PlainDateTime dateTimeResult;
  if (!InterpretTemporalDateTimeFields(cx, calendar, fields, options,
                                       &dateTimeResult)) {
    return false;
  }

  // Step 17.
  Rooted<Value> offsetString(cx);
  if (!GetProperty(cx, fields, fields, cx->names().offset, &offsetString)) {
    return false;
  }

  // Step 18.
  MOZ_ASSERT(offsetString.isString());

  // Steps 19-21.
  Rooted<JSString*> offsetStr(cx, offsetString.toString());
  int64_t offsetNanoseconds;
  if (!ParseTimeZoneOffsetString(cx, offsetStr, &offsetNanoseconds)) {
    return false;
  }

  // Step 21.
  Rooted<JSObject*> timeZone(cx, zonedDateTime->timeZone());

  // Step 22.
  Instant epochNanoseconds;
  if (!InterpretISODateTimeOffset(cx, dateTimeResult, OffsetBehaviour::Option,
                                  offsetNanoseconds, timeZone, disambiguation,
                                  offset, MatchBehaviour::MatchExactly,
                                  &epochNanoseconds)) {
    return false;
  }

  // Step 23.
  auto* result =
      CreateTemporalZonedDateTime(cx, epochNanoseconds, timeZone, calendar);
  if (!result) {
    return false;
  }

  args.rval().setObject(*result);
  return true;
}

/**
 * Temporal.ZonedDateTime.prototype.with ( temporalZonedDateTimeLike [ , options
 * ] )
 */
static bool ZonedDateTime_with(JSContext* cx, unsigned argc, Value* vp) {
  // Steps 1-2.
  CallArgs args = CallArgsFromVp(argc, vp);
  return CallNonGenericMethod<IsZonedDateTime, ZonedDateTime_with>(cx, args);
}

/**
 * Temporal.ZonedDateTime.prototype.withPlainTime ( [ plainTimeLike ] )
 */
static bool ZonedDateTime_withPlainTime(JSContext* cx, const CallArgs& args) {
  auto* zonedDateTime = &args.thisv().toObject().as<ZonedDateTimeObject>();
  auto epochInstant = ToInstant(zonedDateTime);

  // Step 5. (Reordered)
  Rooted<JSObject*> timeZone(cx, zonedDateTime->timeZone());

  // Step 7. (Reordered)
  Rooted<JSObject*> calendar(cx, zonedDateTime->calendar());

  // Steps 3-4.
  PlainTime time = {};
  if (args.hasDefined(0)) {
    if (!ToTemporalTime(cx, args[0], &time)) {
      return false;
    }
  }

  // Steps 6 and 8.
  PlainDateTime plainDateTime;
  if (!GetPlainDateTimeFor(cx, timeZone, epochInstant, &plainDateTime)) {
    return false;
  }

  // Step 9.
  Rooted<PlainDateTimeObject*> resultPlainDateTime(
      cx, CreateTemporalDateTime(cx, {plainDateTime.date, time}, calendar));
  if (!resultPlainDateTime) {
    return false;
  }

  // Step 10.
  Instant instant;
  if (!GetInstantFor(cx, timeZone, resultPlainDateTime,
                     TemporalDisambiguation::Compatible, &instant)) {
    return false;
  }

  // Step 11.
  auto* result = CreateTemporalZonedDateTime(cx, instant, timeZone, calendar);
  if (!result) {
    return false;
  }

  args.rval().setObject(*result);
  return true;
}

/**
 * Temporal.ZonedDateTime.prototype.withPlainTime ( [ plainTimeLike ] )
 */
static bool ZonedDateTime_withPlainTime(JSContext* cx, unsigned argc,
                                        Value* vp) {
  // Steps 1-2.
  CallArgs args = CallArgsFromVp(argc, vp);
  return CallNonGenericMethod<IsZonedDateTime, ZonedDateTime_withPlainTime>(
      cx, args);
}

/**
 * Temporal.ZonedDateTime.prototype.withPlainDate ( plainDateLike )
 */
static bool ZonedDateTime_withPlainDate(JSContext* cx, const CallArgs& args) {
  auto* zonedDateTime = &args.thisv().toObject().as<ZonedDateTimeObject>();
  auto epochInstant = ToInstant(zonedDateTime);
  Rooted<JSObject*> calendar(cx, zonedDateTime->calendar());

  // Step 4. (Reordered)
  Rooted<JSObject*> timeZone(cx, zonedDateTime->timeZone());

  // Step 3.
  PlainDate date;
  Rooted<JSObject*> plainDateCalendar(cx);
  if (!ToTemporalDate(cx, args.get(0), &date, &plainDateCalendar)) {
    return false;
  }

  // Steps 5-6.
  PlainDateTime plainDateTime;
  if (!GetPlainDateTimeFor(cx, timeZone, epochInstant, &plainDateTime)) {
    return false;
  }

  // Step 7.
  calendar = ConsolidateCalendars(cx, calendar, plainDateCalendar);
  if (!calendar) {
    return false;
  }

  // Step 8.
  Rooted<PlainDateTimeObject*> resultPlainDateTime(
      cx, CreateTemporalDateTime(cx, {date, plainDateTime.time}, calendar));
  if (!resultPlainDateTime) {
    return false;
  }

  // Step 9.
  Instant instant;
  if (!GetInstantFor(cx, timeZone, resultPlainDateTime,
                     TemporalDisambiguation::Compatible, &instant)) {
    return false;
  }

  // Step 10.
  auto* result = CreateTemporalZonedDateTime(cx, instant, timeZone, calendar);
  if (!result) {
    return false;
  }

  args.rval().setObject(*result);
  return true;
}

/**
 * Temporal.ZonedDateTime.prototype.withPlainDate ( plainDateLike )
 */
static bool ZonedDateTime_withPlainDate(JSContext* cx, unsigned argc,
                                        Value* vp) {
  // Steps 1-2.
  CallArgs args = CallArgsFromVp(argc, vp);
  return CallNonGenericMethod<IsZonedDateTime, ZonedDateTime_withPlainDate>(
      cx, args);
}

/**
 * Temporal.ZonedDateTime.prototype.withTimeZone ( timeZoneLike )
 */
static bool ZonedDateTime_withTimeZone(JSContext* cx, const CallArgs& args) {
  auto* zonedDateTime = &args.thisv().toObject().as<ZonedDateTimeObject>();
  auto epochNanoseconds = ToInstant(zonedDateTime);
  Rooted<JSObject*> calendar(cx, zonedDateTime->calendar());

  // Step 3.
  Rooted<JSObject*> timeZone(cx, ToTemporalTimeZone(cx, args.get(0)));
  if (!timeZone) {
    return false;
  }

  // Step 4.
  auto* result =
      CreateTemporalZonedDateTime(cx, epochNanoseconds, timeZone, calendar);
  if (!result) {
    return false;
  }

  args.rval().setObject(*result);
  return true;
}

/**
 * Temporal.ZonedDateTime.prototype.withTimeZone ( timeZoneLike )
 */
static bool ZonedDateTime_withTimeZone(JSContext* cx, unsigned argc,
                                       Value* vp) {
  // Steps 1-2.
  CallArgs args = CallArgsFromVp(argc, vp);
  return CallNonGenericMethod<IsZonedDateTime, ZonedDateTime_withTimeZone>(
      cx, args);
}

/**
 * Temporal.ZonedDateTime.prototype.withCalendar ( calendarLike )
 */
static bool ZonedDateTime_withCalendar(JSContext* cx, const CallArgs& args) {
  auto* zonedDateTime = &args.thisv().toObject().as<ZonedDateTimeObject>();
  auto epochNanoseconds = ToInstant(zonedDateTime);
  Rooted<JSObject*> timeZone(cx, zonedDateTime->timeZone());

  // Step 3.
  Rooted<JSObject*> calendar(cx, ToTemporalCalendar(cx, args.get(0)));
  if (!calendar) {
    return false;
  }

  // Step 4.
  auto* result =
      CreateTemporalZonedDateTime(cx, epochNanoseconds, timeZone, calendar);
  if (!result) {
    return false;
  }

  args.rval().setObject(*result);
  return true;
}

/**
 * Temporal.ZonedDateTime.prototype.withCalendar ( calendarLike )
 */
static bool ZonedDateTime_withCalendar(JSContext* cx, unsigned argc,
                                       Value* vp) {
  // Steps 1-2.
  CallArgs args = CallArgsFromVp(argc, vp);
  return CallNonGenericMethod<IsZonedDateTime, ZonedDateTime_withCalendar>(
      cx, args);
}

/**
 * Temporal.ZonedDateTime.prototype.add ( temporalDurationLike [ , options ] )
 */
static bool ZonedDateTime_add(JSContext* cx, const CallArgs& args) {
  return AddDurationToOrSubtractDurationFromZonedDateTime(
      cx, ZonedDateTimeDuration::Add, args);
}

/**
 * Temporal.ZonedDateTime.prototype.add ( temporalDurationLike [ , options ] )
 */
static bool ZonedDateTime_add(JSContext* cx, unsigned argc, Value* vp) {
  // Steps 1-2.
  CallArgs args = CallArgsFromVp(argc, vp);
  return CallNonGenericMethod<IsZonedDateTime, ZonedDateTime_add>(cx, args);
}

/**
 * Temporal.ZonedDateTime.prototype.subtract ( temporalDurationLike [ , options
 * ] )
 */
static bool ZonedDateTime_subtract(JSContext* cx, const CallArgs& args) {
  return AddDurationToOrSubtractDurationFromZonedDateTime(
      cx, ZonedDateTimeDuration::Subtract, args);
}

/**
 * Temporal.ZonedDateTime.prototype.subtract ( temporalDurationLike [ , options
 * ] )
 */
static bool ZonedDateTime_subtract(JSContext* cx, unsigned argc, Value* vp) {
  // Steps 1-2.
  CallArgs args = CallArgsFromVp(argc, vp);
  return CallNonGenericMethod<IsZonedDateTime, ZonedDateTime_subtract>(cx,
                                                                       args);
}

/**
 * Temporal.ZonedDateTime.prototype.until ( other [ , options ] )
 */
static bool ZonedDateTime_until(JSContext* cx, const CallArgs& args) {
  // Step 3.
  return DifferenceTemporalZonedDateTime(cx, TemporalDifference::Until, args);
}

/**
 * Temporal.ZonedDateTime.prototype.until ( other [ , options ] )
 */
static bool ZonedDateTime_until(JSContext* cx, unsigned argc, Value* vp) {
  // Steps 1-2.
  CallArgs args = CallArgsFromVp(argc, vp);
  return CallNonGenericMethod<IsZonedDateTime, ZonedDateTime_until>(cx, args);
}

/**
 * Temporal.ZonedDateTime.prototype.since ( other [ , options ] )
 */
static bool ZonedDateTime_since(JSContext* cx, const CallArgs& args) {
  // Step 3.
  return DifferenceTemporalZonedDateTime(cx, TemporalDifference::Since, args);
}

/**
 * Temporal.ZonedDateTime.prototype.since ( other [ , options ] )
 */
static bool ZonedDateTime_since(JSContext* cx, unsigned argc, Value* vp) {
  // Steps 1-2.
  CallArgs args = CallArgsFromVp(argc, vp);
  return CallNonGenericMethod<IsZonedDateTime, ZonedDateTime_since>(cx, args);
}

/**
 * Temporal.ZonedDateTime.prototype.round ( roundTo )
 */
static bool ZonedDateTime_round(JSContext* cx, const CallArgs& args) {
  auto* zonedDateTime = &args.thisv().toObject().as<ZonedDateTimeObject>();
  auto epochInstant = ToInstant(zonedDateTime);

  // Step 13. (Reorderd)
  Rooted<JSObject*> timeZone(cx, zonedDateTime->timeZone());

  // Step 15. (Reordered)
  Rooted<JSObject*> calendar(cx, zonedDateTime->calendar());

  // Steps 3-12.
  auto smallestUnit = TemporalUnit::Auto;
  auto roundingMode = TemporalRoundingMode::HalfExpand;
  auto roundingIncrement = Increment{1};
  if (args.get(0).isString()) {
    // Step 4. (Not applicable in our implementation.)

    // Step 9.
    Rooted<JSString*> paramString(cx, args[0].toString());
    if (!GetTemporalUnit(cx, paramString, TemporalUnitKey::SmallestUnit,
                         TemporalUnitGroup::DayTime, &smallestUnit)) {
      return false;
    }

    // Steps 6-8 and 10-12. (Implicit)
  } else {
    // Steps 3 and 5.a
    Rooted<JSObject*> roundTo(
        cx, RequireObjectArg(cx, "roundTo", "round", args.get(0)));
    if (!roundTo) {
      return false;
    }

    // Steps 6-7.
    if (!ToTemporalRoundingIncrement(cx, roundTo, &roundingIncrement)) {
      return false;
    }

    // Step 8.
    if (!ToTemporalRoundingMode(cx, roundTo, &roundingMode)) {
      return false;
    }

    // Step 9.
    if (!GetTemporalUnit(cx, roundTo, TemporalUnitKey::SmallestUnit,
                         TemporalUnitGroup::DayTime, &smallestUnit)) {
      return false;
    }

    if (smallestUnit == TemporalUnit::Auto) {
      JS_ReportErrorNumberASCII(cx, GetErrorMessage, nullptr,
                                JSMSG_TEMPORAL_MISSING_OPTION, "smallestUnit");
      return false;
    }

    MOZ_ASSERT(TemporalUnit::Day <= smallestUnit &&
               smallestUnit <= TemporalUnit::Nanosecond);

    // Steps 10-11.
    auto maximum = Increment{1};
    bool inclusive = true;
    if (smallestUnit > TemporalUnit::Day) {
      maximum = MaximumTemporalDurationRoundingIncrement(smallestUnit);
      inclusive = false;
    }

    // Step 12.
    if (!ValidateTemporalRoundingIncrement(cx, roundingIncrement, maximum,
                                           inclusive)) {
      return false;
    }
  }

  // Step 14.
  Rooted<InstantObject*> instant(cx, CreateTemporalInstant(cx, epochInstant));
  if (!instant) {
    return false;
  }

  // Step 16.
  PlainDateTime temporalDateTime;
  if (!temporal::GetPlainDateTimeFor(cx, timeZone, instant,
                                     &temporalDateTime)) {
    return false;
  }

  // Step 17.
  Rooted<CalendarObject*> isoCalendar(cx, GetISO8601Calendar(cx));
  if (!isoCalendar) {
    return false;
  }

  // Step 18.
  Rooted<PlainDateTimeObject*> dtStart(
      cx, CreateTemporalDateTime(cx, {temporalDateTime.date}, isoCalendar));
  if (!dtStart) {
    return false;
  }

  // Steps 19-20.
  Instant startNs;
  if (!GetInstantFor(cx, timeZone, dtStart, TemporalDisambiguation::Compatible,
                     &startNs)) {
    return false;
  }

  // Step 21.
  Instant endNs;
  if (!AddZonedDateTime(cx, startNs, timeZone, calendar, {0, 0, 0, 1},
                        &endNs)) {
    return false;
  }
  MOZ_ASSERT(IsValidEpochInstant(endNs));

  // Step 22.
  auto dayLengthNs = endNs - startNs;
  MOZ_ASSERT(IsValidInstantDifference(dayLengthNs));

  // Step 23.
  if (dayLengthNs <= Instant{}) {
    JS_ReportErrorNumberASCII(
        cx, GetErrorMessage, nullptr,
        JSMSG_TEMPORAL_ZONED_DATE_TIME_NON_POSITIVE_DAY_LENGTH);
    return false;
  }

  // Step 25.
  int64_t offsetNanoseconds;
  if (!GetOffsetNanosecondsFor(cx, timeZone, instant, &offsetNanoseconds)) {
    return false;
  }
  MOZ_ASSERT(std::abs(offsetNanoseconds) < ToNanoseconds(TemporalUnit::Day));

  // Step 24. (Reordered to throw exceptions in correct order.)
  //
  // Per spec, out-of-range date values throw a RangeError when
  // CreateTemporalDateTime in InterpretISODateTimeOffset is called. This
  // implementation throws the RangeError in RoundISODateTime, therefore steps
  // 24 and 25 have to be switched.
  PlainDateTime roundResult;
  if (!RoundISODateTime(cx, temporalDateTime, roundingIncrement, smallestUnit,
                        roundingMode, dayLengthNs, &roundResult)) {
    return false;
  }

  // Step 26.
  Instant epochNanoseconds;
  if (!InterpretISODateTimeOffset(
          cx, roundResult, OffsetBehaviour::Option, offsetNanoseconds, timeZone,
          TemporalDisambiguation::Compatible, TemporalOffset::Prefer,
          MatchBehaviour::MatchExactly, &epochNanoseconds)) {
    return false;
  }

  // Step 27.
  auto* result =
      CreateTemporalZonedDateTime(cx, epochNanoseconds, timeZone, calendar);
  if (!result) {
    return false;
  }

  args.rval().setObject(*result);
  return true;
}

/**
 * Temporal.ZonedDateTime.prototype.round ( roundTo )
 */
static bool ZonedDateTime_round(JSContext* cx, unsigned argc, Value* vp) {
  // Steps 1-2.
  CallArgs args = CallArgsFromVp(argc, vp);
  return CallNonGenericMethod<IsZonedDateTime, ZonedDateTime_round>(cx, args);
}

/**
 * Temporal.ZonedDateTime.prototype.equals ( other )
 */
static bool ZonedDateTime_equals(JSContext* cx, const CallArgs& args) {
  auto* zonedDateTime = &args.thisv().toObject().as<ZonedDateTimeObject>();
  auto epochNanoseconds = ToInstant(zonedDateTime);
  Rooted<JSObject*> timeZone(cx, zonedDateTime->timeZone());
  Rooted<JSObject*> calendar(cx, zonedDateTime->calendar());

  // Step 3.
  Instant otherEpochNanoseconds;
  Rooted<JSObject*> otherTimeZone(cx);
  Rooted<JSObject*> otherCalendar(cx);
  if (!ToTemporalZonedDateTime(cx, args.get(0), &otherEpochNanoseconds,
                               &otherTimeZone, &otherCalendar)) {
    return false;
  }

  // Steps 4-6.
  bool equals = epochNanoseconds == otherEpochNanoseconds;
  if (equals) {
    if (!TimeZoneEquals(cx, timeZone, otherTimeZone, &equals)) {
      return false;
    }
  }
  if (equals) {
    if (!CalendarEquals(cx, calendar, otherCalendar, &equals)) {
      return false;
    }
  }

  args.rval().setBoolean(equals);
  return true;
}

/**
 * Temporal.ZonedDateTime.prototype.equals ( other )
 */
static bool ZonedDateTime_equals(JSContext* cx, unsigned argc, Value* vp) {
  // Steps 1-2.
  CallArgs args = CallArgsFromVp(argc, vp);
  return CallNonGenericMethod<IsZonedDateTime, ZonedDateTime_equals>(cx, args);
}

/**
 * Temporal.ZonedDateTime.prototype.toString ( [ options ] )
 */
static bool ZonedDateTime_toString(JSContext* cx, const CallArgs& args) {
  Rooted<ZonedDateTimeObject*> zonedDateTime(
      cx, &args.thisv().toObject().as<ZonedDateTimeObject>());

  SecondsStringPrecision precision = {Precision::Auto(),
                                      TemporalUnit::Nanosecond, Increment{1}};
  auto roundingMode = TemporalRoundingMode::Trunc;
  auto showCalendar = CalendarOption::Auto;
  auto showTimeZone = TimeZoneNameOption::Auto;
  auto showOffset = ShowOffsetOption::Auto;
  if (args.hasDefined(0)) {
    // Step 3.
    Rooted<JSObject*> options(
        cx, RequireObjectArg(cx, "options", "toString", args[0]));
    if (!options) {
      return false;
    }

    // Steps 4-5.
    if (!ToCalendarNameOption(cx, options, &showCalendar)) {
      return false;
    }

    // Step 6.
    auto digits = Precision::Auto();
    if (!ToFractionalSecondDigits(cx, options, &digits)) {
      return false;
    }

    // Step 7.
    if (!ToShowOffsetOption(cx, options, &showOffset)) {
      return false;
    }

    // Step 8.
    if (!ToTemporalRoundingMode(cx, options, &roundingMode)) {
      return false;
    }

    // Step 9.
    auto smallestUnit = TemporalUnit::Auto;
    if (!GetTemporalUnit(cx, options, TemporalUnitKey::SmallestUnit,
                         TemporalUnitGroup::Time, &smallestUnit)) {
      return false;
    }

    // Step 10.
    if (smallestUnit == TemporalUnit::Hour) {
      JS_ReportErrorNumberASCII(cx, GetErrorMessage, nullptr,
                                JSMSG_TEMPORAL_INVALID_UNIT_OPTION, "hour",
                                "smallestUnit");
      return false;
    }

    // Step 11.
    if (!ToTimeZoneNameOption(cx, options, &showTimeZone)) {
      return false;
    }

    // Step 12.
    precision = ToSecondsStringPrecision(smallestUnit, digits);
  }

  // Step 13.
  JSString* str = TemporalZonedDateTimeToString(
      cx, zonedDateTime, precision.precision, showCalendar, showTimeZone,
      showOffset, precision.increment, precision.unit, roundingMode);
  if (!str) {
    return false;
  }

  args.rval().setString(str);
  return true;
}

/**
 * Temporal.ZonedDateTime.prototype.toString ( [ options ] )
 */
static bool ZonedDateTime_toString(JSContext* cx, unsigned argc, Value* vp) {
  // Steps 1-2.
  CallArgs args = CallArgsFromVp(argc, vp);
  return CallNonGenericMethod<IsZonedDateTime, ZonedDateTime_toString>(cx,
                                                                       args);
}

/**
 * Temporal.ZonedDateTime.prototype.toLocaleString ( [ locales [ , options ] ] )
 */
static bool ZonedDateTime_toLocaleString(JSContext* cx, const CallArgs& args) {
  Rooted<ZonedDateTimeObject*> zonedDateTime(
      cx, &args.thisv().toObject().as<ZonedDateTimeObject>());

  // Step 3.
  JSString* str = TemporalZonedDateTimeToString(
      cx, zonedDateTime, Precision::Auto(), CalendarOption::Auto,
      TimeZoneNameOption::Auto, ShowOffsetOption::Auto);
  if (!str) {
    return false;
  }

  args.rval().setString(str);
  return true;
}

/**
 * Temporal.ZonedDateTime.prototype.toLocaleString ( [ locales [ , options ] ] )
 */
static bool ZonedDateTime_toLocaleString(JSContext* cx, unsigned argc,
                                         Value* vp) {
  // Steps 1-2.
  CallArgs args = CallArgsFromVp(argc, vp);
  return CallNonGenericMethod<IsZonedDateTime, ZonedDateTime_toLocaleString>(
      cx, args);
}

/**
 * Temporal.ZonedDateTime.prototype.toJSON ( )
 */
static bool ZonedDateTime_toJSON(JSContext* cx, const CallArgs& args) {
  Rooted<ZonedDateTimeObject*> zonedDateTime(
      cx, &args.thisv().toObject().as<ZonedDateTimeObject>());

  // Step 3.
  JSString* str = TemporalZonedDateTimeToString(
      cx, zonedDateTime, Precision::Auto(), CalendarOption::Auto,
      TimeZoneNameOption::Auto, ShowOffsetOption::Auto);
  if (!str) {
    return false;
  }

  args.rval().setString(str);
  return true;
}

/**
 * Temporal.ZonedDateTime.prototype.toJSON ( )
 */
static bool ZonedDateTime_toJSON(JSContext* cx, unsigned argc, Value* vp) {
  // Steps 1-2.
  CallArgs args = CallArgsFromVp(argc, vp);
  return CallNonGenericMethod<IsZonedDateTime, ZonedDateTime_toJSON>(cx, args);
}

/**
 * Temporal.ZonedDateTime.prototype.valueOf ( )
 */
static bool ZonedDateTime_valueOf(JSContext* cx, unsigned argc, Value* vp) {
  JS_ReportErrorNumberASCII(cx, GetErrorMessage, nullptr, JSMSG_CANT_CONVERT_TO,
                            "ZonedDateTime", "primitive type");
  return false;
}

/**
 * Temporal.ZonedDateTime.prototype.startOfDay ( )
 */
static bool ZonedDateTime_startOfDay(JSContext* cx, const CallArgs& args) {
  auto* zonedDateTime = &args.thisv().toObject().as<ZonedDateTimeObject>();
  auto instant = ToInstant(zonedDateTime);

  // Step 3.
  Rooted<JSObject*> timeZone(cx, zonedDateTime->timeZone());

  // Step 4.
  Rooted<JSObject*> calendar(cx, zonedDateTime->calendar());

  // Steps 5-6.
  PlainDateTime temporalDateTime;
  if (!GetPlainDateTimeFor(cx, timeZone, instant, &temporalDateTime)) {
    return false;
  }

  // Step 7.
  Rooted<PlainDateTimeObject*> startDateTime(
      cx, CreateTemporalDateTime(cx, {temporalDateTime.date, {}}, calendar));
  if (!startDateTime) {
    return false;
  }

  // Step 8.
  Instant startInstant;
  if (!GetInstantFor(cx, timeZone, startDateTime,
                     TemporalDisambiguation::Compatible, &startInstant)) {
    return false;
  }

  // Step 9.
  auto* result =
      CreateTemporalZonedDateTime(cx, startInstant, timeZone, calendar);
  if (!result) {
    return false;
  }

  args.rval().setObject(*result);
  return true;
}

/**
 * Temporal.ZonedDateTime.prototype.startOfDay ( )
 */
static bool ZonedDateTime_startOfDay(JSContext* cx, unsigned argc, Value* vp) {
  // Steps 1-2.
  CallArgs args = CallArgsFromVp(argc, vp);
  return CallNonGenericMethod<IsZonedDateTime, ZonedDateTime_startOfDay>(cx,
                                                                         args);
}

/**
 * Temporal.ZonedDateTime.prototype.toInstant ( )
 */
static bool ZonedDateTime_toInstant(JSContext* cx, const CallArgs& args) {
  auto* zonedDateTime = &args.thisv().toObject().as<ZonedDateTimeObject>();
  auto instant = ToInstant(zonedDateTime);

  // Step 3.
  auto* result = CreateTemporalInstant(cx, instant);
  if (!result) {
    return false;
  }

  args.rval().setObject(*result);
  return true;
}

/**
 * Temporal.ZonedDateTime.prototype.toInstant ( )
 */
static bool ZonedDateTime_toInstant(JSContext* cx, unsigned argc, Value* vp) {
  // Steps 1-2.
  CallArgs args = CallArgsFromVp(argc, vp);
  return CallNonGenericMethod<IsZonedDateTime, ZonedDateTime_toInstant>(cx,
                                                                        args);
}

/**
 * Temporal.ZonedDateTime.prototype.toPlainDate ( )
 */
static bool ZonedDateTime_toPlainDate(JSContext* cx, const CallArgs& args) {
  auto* zonedDateTime = &args.thisv().toObject().as<ZonedDateTimeObject>();
  auto instant = ToInstant(zonedDateTime);
  Rooted<JSObject*> timeZone(cx, zonedDateTime->timeZone());

  // Step 5.
  Rooted<JSObject*> calendar(cx, zonedDateTime->calendar());

  // Steps 3-4 and 6.
  PlainDateTime temporalDateTime;
  if (!GetPlainDateTimeFor(cx, timeZone, instant, &temporalDateTime)) {
    return false;
  }

  // Step 7.
  auto* result = CreateTemporalDate(cx, temporalDateTime.date, calendar);
  if (!result) {
    return false;
  }

  args.rval().setObject(*result);
  return true;
}

/**
 * Temporal.ZonedDateTime.prototype.toPlainDate ( )
 */
static bool ZonedDateTime_toPlainDate(JSContext* cx, unsigned argc, Value* vp) {
  // Steps 1-2.
  CallArgs args = CallArgsFromVp(argc, vp);
  return CallNonGenericMethod<IsZonedDateTime, ZonedDateTime_toPlainDate>(cx,
                                                                          args);
}

/**
 * Temporal.ZonedDateTime.prototype.toPlainTime ( )
 */
static bool ZonedDateTime_toPlainTime(JSContext* cx, const CallArgs& args) {
  auto* zonedDateTime = &args.thisv().toObject().as<ZonedDateTimeObject>();
  auto instant = ToInstant(zonedDateTime);
  Rooted<JSObject*> timeZone(cx, zonedDateTime->timeZone());
  Rooted<JSObject*> calendar(cx, zonedDateTime->calendar());

  // Steps 3-5.
  PlainDateTime temporalDateTime;
  if (!GetPlainDateTimeFor(cx, timeZone, instant, &temporalDateTime)) {
    return false;
  }

  // Step 6.
  auto* result = CreateTemporalTime(cx, temporalDateTime.time);
  if (!result) {
    return false;
  }

  args.rval().setObject(*result);
  return true;
}

/**
 * Temporal.ZonedDateTime.prototype.toPlainTime ( )
 */
static bool ZonedDateTime_toPlainTime(JSContext* cx, unsigned argc, Value* vp) {
  // Steps 1-2.
  CallArgs args = CallArgsFromVp(argc, vp);
  return CallNonGenericMethod<IsZonedDateTime, ZonedDateTime_toPlainTime>(cx,
                                                                          args);
}

/**
 * Temporal.ZonedDateTime.prototype.toPlainDateTime ( )
 */
static bool ZonedDateTime_toPlainDateTime(JSContext* cx, const CallArgs& args) {
  auto* zonedDateTime = &args.thisv().toObject().as<ZonedDateTimeObject>();
  auto instant = ToInstant(zonedDateTime);
  Rooted<JSObject*> timeZone(cx, zonedDateTime->timeZone());
  Rooted<JSObject*> calendar(cx, zonedDateTime->calendar());

  // Steps 3-5.
  auto* result = GetPlainDateTimeFor(cx, timeZone, instant, calendar);
  if (!result) {
    return false;
  }

  args.rval().setObject(*result);
  return true;
}

/**
 * Temporal.ZonedDateTime.prototype.toPlainDateTime ( )
 */
static bool ZonedDateTime_toPlainDateTime(JSContext* cx, unsigned argc,
                                          Value* vp) {
  // Steps 1-2.
  CallArgs args = CallArgsFromVp(argc, vp);
  return CallNonGenericMethod<IsZonedDateTime, ZonedDateTime_toPlainDateTime>(
      cx, args);
}

/**
 * Temporal.ZonedDateTime.prototype.toPlainYearMonth ( )
 */
static bool ZonedDateTime_toPlainYearMonth(JSContext* cx,
                                           const CallArgs& args) {
  auto* zonedDateTime = &args.thisv().toObject().as<ZonedDateTimeObject>();
  auto instant = ToInstant(zonedDateTime);
  Rooted<JSObject*> timeZone(cx, zonedDateTime->timeZone());

  // Step 5.
  Rooted<JSObject*> calendar(cx, zonedDateTime->calendar());

  // Steps 3-4 and 6.
  Rooted<PlainDateTimeObject*> temporalDateTime(
      cx, GetPlainDateTimeFor(cx, timeZone, instant, calendar));
  if (!temporalDateTime) {
    return false;
  }

  // Step 7.
  JS::RootedVector<PropertyKey> fieldNames(cx);
  if (!CalendarFields(cx, calendar,
                      {CalendarField::MonthCode, CalendarField::Year},
                      &fieldNames)) {
    return false;
  }

  // Step 8.
  Rooted<PlainObject*> fields(
      cx, PrepareTemporalFields(cx, temporalDateTime, fieldNames));
  if (!fields) {
    return false;
  }

  // Steps 9-10.
  auto result = CalendarYearMonthFromFields(cx, calendar, fields);
  if (!result) {
    return false;
  }

  args.rval().setObject(*result);
  return true;
}

/**
 * Temporal.ZonedDateTime.prototype.toPlainYearMonth ( )
 */
static bool ZonedDateTime_toPlainYearMonth(JSContext* cx, unsigned argc,
                                           Value* vp) {
  // Steps 1-2.
  CallArgs args = CallArgsFromVp(argc, vp);
  return CallNonGenericMethod<IsZonedDateTime, ZonedDateTime_toPlainYearMonth>(
      cx, args);
}

/**
 * Temporal.ZonedDateTime.prototype.toPlainMonthDay ( )
 */
static bool ZonedDateTime_toPlainMonthDay(JSContext* cx, const CallArgs& args) {
  auto* zonedDateTime = &args.thisv().toObject().as<ZonedDateTimeObject>();
  auto instant = ToInstant(zonedDateTime);
  Rooted<JSObject*> timeZone(cx, zonedDateTime->timeZone());

  // Step 5.
  Rooted<JSObject*> calendar(cx, zonedDateTime->calendar());

  // Steps 3-4 and 6.
  Rooted<PlainDateTimeObject*> temporalDateTime(
      cx, GetPlainDateTimeFor(cx, timeZone, instant, calendar));
  if (!temporalDateTime) {
    return false;
  }

  // Step 7.
  JS::RootedVector<PropertyKey> fieldNames(cx);
  if (!CalendarFields(cx, calendar,
                      {CalendarField::Day, CalendarField::MonthCode},
                      &fieldNames)) {
    return false;
  }

  // Step 8.
  Rooted<PlainObject*> fields(
      cx, PrepareTemporalFields(cx, temporalDateTime, fieldNames));
  if (!fields) {
    return false;
  }

  // Steps 9-10.
  auto result = CalendarMonthDayFromFields(cx, calendar, fields);
  if (!result) {
    return false;
  }

  args.rval().setObject(*result);
  return true;
}

/**
 * Temporal.ZonedDateTime.prototype.toPlainMonthDay ( )
 */
static bool ZonedDateTime_toPlainMonthDay(JSContext* cx, unsigned argc,
                                          Value* vp) {
  // Steps 1-2.
  CallArgs args = CallArgsFromVp(argc, vp);
  return CallNonGenericMethod<IsZonedDateTime, ZonedDateTime_toPlainMonthDay>(
      cx, args);
}

/**
 * Temporal.ZonedDateTime.prototype.getISOFields ( )
 */
static bool ZonedDateTime_getISOFields(JSContext* cx, const CallArgs& args) {
  auto* zonedDateTime = &args.thisv().toObject().as<ZonedDateTimeObject>();
  auto epochInstant = ToInstant(zonedDateTime);

  // Step 3.
  Rooted<IdValueVector> fields(cx, IdValueVector(cx));

  // Step 4.
  Rooted<JSObject*> timeZone(cx, zonedDateTime->timeZone());

  // Step 6. (Reordered)
  Rooted<JSObject*> calendar(cx, zonedDateTime->calendar());

  // Step 5.
  Rooted<InstantObject*> instant(cx, CreateTemporalInstant(cx, epochInstant));
  if (!instant) {
    return false;
  }

  // Step 7.
  PlainDateTime temporalDateTime;
  if (!js::temporal::GetPlainDateTimeFor(cx, timeZone, instant,
                                         &temporalDateTime)) {
    return false;
  }

  // Step 8.
  Rooted<JSString*> offset(cx, GetOffsetStringFor(cx, timeZone, instant));
  if (!offset) {
    return false;
  }

  // Step 9.
  if (!fields.emplaceBack(NameToId(cx->names().calendar),
                          ObjectValue(*calendar))) {
    return false;
  }

  // Step 10.
  if (!fields.emplaceBack(NameToId(cx->names().isoDay),
                          Int32Value(temporalDateTime.date.day))) {
    return false;
  }

  // Step 11.
  if (!fields.emplaceBack(NameToId(cx->names().isoHour),
                          Int32Value(temporalDateTime.time.hour))) {
    return false;
  }

  // Step 12.
  if (!fields.emplaceBack(NameToId(cx->names().isoMicrosecond),
                          Int32Value(temporalDateTime.time.microsecond))) {
    return false;
  }

  // Step 13.
  if (!fields.emplaceBack(NameToId(cx->names().isoMillisecond),
                          Int32Value(temporalDateTime.time.millisecond))) {
    return false;
  }

  // Step 14.
  if (!fields.emplaceBack(NameToId(cx->names().isoMinute),
                          Int32Value(temporalDateTime.time.minute))) {
    return false;
  }

  // Step 15.
  if (!fields.emplaceBack(NameToId(cx->names().isoMonth),
                          Int32Value(temporalDateTime.date.month))) {
    return false;
  }

  // Step 16.
  if (!fields.emplaceBack(NameToId(cx->names().isoNanosecond),
                          Int32Value(temporalDateTime.time.nanosecond))) {
    return false;
  }

  // Step 17.
  if (!fields.emplaceBack(NameToId(cx->names().isoSecond),
                          Int32Value(temporalDateTime.time.second))) {
    return false;
  }

  // Step 18.
  if (!fields.emplaceBack(NameToId(cx->names().isoYear),
                          Int32Value(temporalDateTime.date.year))) {
    return false;
  }

  // Step 19.
  if (!fields.emplaceBack(NameToId(cx->names().offset), StringValue(offset))) {
    return false;
  }

  // Step 20.
  if (!fields.emplaceBack(NameToId(cx->names().timeZone),
                          ObjectValue(*timeZone))) {
    return false;
  }

  // Step 21.
  auto* obj =
      NewPlainObjectWithUniqueNames(cx, fields.begin(), fields.length());
  if (!obj) {
    return false;
  }

  args.rval().setObject(*obj);
  return true;
}

/**
 * Temporal.ZonedDateTime.prototype.getISOFields ( )
 */
static bool ZonedDateTime_getISOFields(JSContext* cx, unsigned argc,
                                       Value* vp) {
  // Steps 1-2.
  CallArgs args = CallArgsFromVp(argc, vp);
  return CallNonGenericMethod<IsZonedDateTime, ZonedDateTime_getISOFields>(
      cx, args);
}

const JSClass ZonedDateTimeObject::class_ = {
    "Temporal.ZonedDateTime",
    JSCLASS_HAS_RESERVED_SLOTS(ZonedDateTimeObject::SLOT_COUNT) |
        JSCLASS_HAS_CACHED_PROTO(JSProto_ZonedDateTime),
    JS_NULL_CLASS_OPS,
    &ZonedDateTimeObject::classSpec_,
};

const JSClass& ZonedDateTimeObject::protoClass_ = PlainObject::class_;

static const JSFunctionSpec ZonedDateTime_methods[] = {
    JS_FN("from", ZonedDateTime_from, 1, 0),
    JS_FN("compare", ZonedDateTime_compare, 2, 0),
    JS_FS_END,
};

static const JSFunctionSpec ZonedDateTime_prototype_methods[] = {
    JS_FN("with", ZonedDateTime_with, 1, 0),
    JS_FN("withPlainTime", ZonedDateTime_withPlainTime, 0, 0),
    JS_FN("withPlainDate", ZonedDateTime_withPlainDate, 1, 0),
    JS_FN("withTimeZone", ZonedDateTime_withTimeZone, 1, 0),
    JS_FN("withCalendar", ZonedDateTime_withCalendar, 1, 0),
    JS_FN("add", ZonedDateTime_add, 1, 0),
    JS_FN("subtract", ZonedDateTime_subtract, 1, 0),
    JS_FN("until", ZonedDateTime_until, 1, 0),
    JS_FN("since", ZonedDateTime_since, 1, 0),
    JS_FN("round", ZonedDateTime_round, 1, 0),
    JS_FN("equals", ZonedDateTime_equals, 1, 0),
    JS_FN("toString", ZonedDateTime_toString, 0, 0),
    JS_FN("toLocaleString", ZonedDateTime_toLocaleString, 0, 0),
    JS_FN("toJSON", ZonedDateTime_toJSON, 0, 0),
    JS_FN("valueOf", ZonedDateTime_valueOf, 0, 0),
    JS_FN("startOfDay", ZonedDateTime_startOfDay, 0, 0),
    JS_FN("toInstant", ZonedDateTime_toInstant, 0, 0),
    JS_FN("toPlainDate", ZonedDateTime_toPlainDate, 0, 0),
    JS_FN("toPlainTime", ZonedDateTime_toPlainTime, 0, 0),
    JS_FN("toPlainDateTime", ZonedDateTime_toPlainDateTime, 0, 0),
    JS_FN("toPlainYearMonth", ZonedDateTime_toPlainYearMonth, 0, 0),
    JS_FN("toPlainMonthDay", ZonedDateTime_toPlainMonthDay, 0, 0),
    JS_FN("getISOFields", ZonedDateTime_getISOFields, 0, 0),
    JS_FS_END,
};

static const JSPropertySpec ZonedDateTime_prototype_properties[] = {
    JS_PSG("calendar", ZonedDateTime_calendar, 0),
    JS_PSG("timeZone", ZonedDateTime_timeZone, 0),
    JS_PSG("year", ZonedDateTime_year, 0),
    JS_PSG("month", ZonedDateTime_month, 0),
    JS_PSG("monthCode", ZonedDateTime_monthCode, 0),
    JS_PSG("day", ZonedDateTime_day, 0),
    JS_PSG("hour", ZonedDateTime_hour, 0),
    JS_PSG("minute", ZonedDateTime_minute, 0),
    JS_PSG("second", ZonedDateTime_second, 0),
    JS_PSG("millisecond", ZonedDateTime_millisecond, 0),
    JS_PSG("microsecond", ZonedDateTime_microsecond, 0),
    JS_PSG("nanosecond", ZonedDateTime_nanosecond, 0),
    JS_PSG("epochSeconds", ZonedDateTime_epochSeconds, 0),
    JS_PSG("epochMilliseconds", ZonedDateTime_epochMilliseconds, 0),
    JS_PSG("epochMicroseconds", ZonedDateTime_epochMicroseconds, 0),
    JS_PSG("epochNanoseconds", ZonedDateTime_epochNanoseconds, 0),
    JS_PSG("dayOfWeek", ZonedDateTime_dayOfWeek, 0),
    JS_PSG("dayOfYear", ZonedDateTime_dayOfYear, 0),
    JS_PSG("weekOfYear", ZonedDateTime_weekOfYear, 0),
    JS_PSG("yearOfWeek", ZonedDateTime_yearOfWeek, 0),
    JS_PSG("hoursInDay", ZonedDateTime_hoursInDay, 0),
    JS_PSG("daysInWeek", ZonedDateTime_daysInWeek, 0),
    JS_PSG("daysInMonth", ZonedDateTime_daysInMonth, 0),
    JS_PSG("daysInYear", ZonedDateTime_daysInYear, 0),
    JS_PSG("monthsInYear", ZonedDateTime_monthsInYear, 0),
    JS_PSG("inLeapYear", ZonedDateTime_inLeapYear, 0),
    JS_PSG("offsetNanoseconds", ZonedDateTime_offsetNanoseconds, 0),
    JS_PSG("offset", ZonedDateTime_offset, 0),
    JS_STRING_SYM_PS(toStringTag, "Temporal.ZonedDateTime", JSPROP_READONLY),
    JS_PS_END,
};

const ClassSpec ZonedDateTimeObject::classSpec_ = {
    GenericCreateConstructor<ZonedDateTimeConstructor, 2,
                             gc::AllocKind::FUNCTION>,
    GenericCreatePrototype<ZonedDateTimeObject>,
    ZonedDateTime_methods,
    nullptr,
    ZonedDateTime_prototype_methods,
    ZonedDateTime_prototype_properties,
    nullptr,
    ClassSpec::DontDefineConstructor,
};
