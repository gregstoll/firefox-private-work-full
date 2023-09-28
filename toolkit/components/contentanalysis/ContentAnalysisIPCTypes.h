/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2 -*- */
/* vim: set ts=8 sts=2 et sw=2 tw=80: */
/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef mozilla_ContentAnalysisIPCTypes_h
#define mozilla_ContentAnalysisIPCTypes_h

#include "ipc/EnumSerializer.h"
#include "mozilla/Variant.h"
#include "nsIContentAnalysis.h"

namespace mozilla {
namespace contentanalysis {

enum class NoContentAnalysisResult : uint8_t {
  AGENT_NOT_PRESENT,
  NO_PARENT_BROWSER,
  CANCELED,
  ERROR_INVALID_JSON_RESPONSE,
  ERROR_COULD_NOT_GET_DATA,
  ERROR_OTHER,
  LAST_VALUE = ERROR_OTHER
};

struct MaybeContentAnalysisResult {
  MaybeContentAnalysisResult() : value(NoContentAnalysisResult::ERROR_OTHER) {}
  explicit MaybeContentAnalysisResult(int32_t response) : value(response) {}
  explicit MaybeContentAnalysisResult(NoContentAnalysisResult result)
      : value(result) {}
  MaybeContentAnalysisResult(const MaybeContentAnalysisResult&) = default;
  MaybeContentAnalysisResult(MaybeContentAnalysisResult&&) = default;
  MaybeContentAnalysisResult& operator=(const MaybeContentAnalysisResult&) =
      default;
  MaybeContentAnalysisResult& operator=(MaybeContentAnalysisResult&&) = default;

  static contentanalysis::MaybeContentAnalysisResult FromJSONResponse(
      const JS::Handle<JS::Value>& aValue, JSContext* aCx) {
    if (aValue.isObject()) {
      auto* obj = aValue.toObjectOrNull();
      JS::Handle<JSObject*> handle =
          JS::Handle<JSObject*>::fromMarkedLocation(&obj);
      JS::Rooted<JS::Value> actionValue(aCx);
      if (JS_GetProperty(aCx, handle, "action", &actionValue)) {
        if (actionValue.isNumber()) {
          double actionNumber = actionValue.toNumber();
          return contentanalysis::MaybeContentAnalysisResult(
              static_cast<int32_t>(actionNumber));
        }
      }
    }
    return contentanalysis::MaybeContentAnalysisResult(
        contentanalysis::NoContentAnalysisResult::ERROR_INVALID_JSON_RESPONSE);
  }

  bool ShouldAllowContent() const {
    if (value.is<NoContentAnalysisResult>()) {
      NoContentAnalysisResult result = value.as<NoContentAnalysisResult>();
      return result == NoContentAnalysisResult::AGENT_NOT_PRESENT ||
             result == NoContentAnalysisResult::NO_PARENT_BROWSER;
    }
    int32_t responseCode = value.as<int32_t>();
    return responseCode == nsIContentAnalysisResponse::ALLOW ||
           responseCode == nsIContentAnalysisResponse::REPORT_ONLY ||
           responseCode == nsIContentAnalysisResponse::WARN;
  }

  Variant<int32_t, NoContentAnalysisResult> value;
};

}  // namespace contentanalysis
}  // namespace mozilla

namespace IPC {
using namespace mozilla::contentanalysis;

template <>
struct ParamTraits<NoContentAnalysisResult>
    : public ContiguousEnumSerializerInclusive<
          NoContentAnalysisResult, static_cast<NoContentAnalysisResult>(0),
          NoContentAnalysisResult::LAST_VALUE> {};

template <>
struct ParamTraits<MaybeContentAnalysisResult> {
  typedef MaybeContentAnalysisResult paramType;

  static void Write(MessageWriter* aWriter, const paramType& aParam) {
    WriteParam(aWriter, aParam.value);
  }

  static bool Read(MessageReader* aReader, paramType* aResult) {
    return ReadParam(aReader, &(aResult->value));
  }
};

}  // namespace IPC

#endif  // mozilla_ContentAnalysisIPCTypes_h
