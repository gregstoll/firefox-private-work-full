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
  ERROR_OTHER,
  LAST_VALUE = ERROR_OTHER
};

struct MaybeContentAnalysisResult {
  MaybeContentAnalysisResult() : value(NoContentAnalysisResult::ERROR_OTHER) {}
  explicit MaybeContentAnalysisResult(int32_t response) : value(response) {}
  explicit MaybeContentAnalysisResult(NoContentAnalysisResult result) : value(result) {}
  MaybeContentAnalysisResult(const MaybeContentAnalysisResult&) = default;
  MaybeContentAnalysisResult(MaybeContentAnalysisResult&&) = default;
  MaybeContentAnalysisResult& operator=(const MaybeContentAnalysisResult&) =
      default;
  MaybeContentAnalysisResult& operator=(MaybeContentAnalysisResult&&) = default;

  bool ShouldAllowContent() const {
    if (value.is<NoContentAnalysisResult>()) {
      NoContentAnalysisResult noResult = value.as<NoContentAnalysisResult>();
      return noResult == NoContentAnalysisResult::AGENT_NOT_PRESENT || noResult == NoContentAnalysisResult::NO_PARENT_BROWSER;
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
