/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2 -*- */
/* vim: set ts=8 sts=2 et sw=2 tw=80: */
/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef mozilla_ContentAnalysisIPCTypes_h
#define mozilla_ContentAnalysisIPCTypes_h

#include "mozilla/Variant.h"
#include "nsIContentAnalysis.h"

namespace mozilla {
namespace contentanalysis {

enum class NoContentAnalysisResult : uint8_t {
  AGENT_NOT_PRESENT,
  ERROR_INVALID_JSON_RESPONSE,
  ERROR_OTHER,
};

using MaybeContentAnalysisResult = Variant<int32_t, NoContentAnalysisResult>;

bool ShouldAllowContent(MaybeContentAnalysisResult result) {
  if (result.is<NoContentAnalysisResult>()) {
    return result.as<NoContentAnalysisResult>() ==
           NoContentAnalysisResult::AGENT_NOT_PRESENT;
  }
  int32_t responseCode = result.as<int32_t>();
  return responseCode == nsIContentAnalysisResponse::ALLOW ||
         responseCode == nsIContentAnalysisResponse::REPORT_ONLY ||
         responseCode == nsIContentAnalysisResponse::WARN;
}

}  // namespace contentanalysis
}  // namespace mozilla

#endif  // mozilla_ContentAnalysisIPCTypes_h
