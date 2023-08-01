/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2 -*- */
/* vim: set ts=8 sts=2 et sw=2 tw=80: */
/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */
#ifndef mozilla_contentanalysis_h
#define mozilla_contentanalysis_h

#include "mozilla/DataMutex.h"
#include "nsIContentAnalysis.h"
#include "nsString.h"

#include <string>

namespace content_analysis::sdk {
class Client;
class ContentAnalysisResponse;
}  // namespace content_analysis::sdk

namespace mozilla {
namespace contentanalysis {

class ContentAnalysis : public nsIContentAnalysis {
 public:
  NS_DECL_THREADSAFE_ISUPPORTS
  NS_DECL_NSICONTENTANALYSIS

  nsresult RunAcknowledgeTask(
      nsIContentAnalysisAcknowledgement* aAcknowledgement,
      const std::string& aRequestToken);

  ContentAnalysis() = default;

 private:
  virtual ~ContentAnalysis();
  nsresult EnsureContentAnalysisClient();
  nsresult RunAnalyzeRequestTask(RefPtr<nsIContentAnalysisRequest> aRequest,
                                 RefPtr<mozilla::dom::Promise> aPromise);

  static StaticDataMutex<UniquePtr<content_analysis::sdk::Client>> sCaClient;
};

class ContentAnalysisResponse : public nsIContentAnalysisResponse {
 public:
  NS_DECL_THREADSAFE_ISUPPORTS
  NS_DECL_NSICONTENTANALYSISRESPONSE

  static RefPtr<ContentAnalysisResponse> FromProtobuf(
      content_analysis::sdk::ContentAnalysisResponse&& aResponse);

  void SetOwner(RefPtr<ContentAnalysis> aOwner);

 private:
  virtual ~ContentAnalysisResponse() = default;
  explicit ContentAnalysisResponse(
      content_analysis::sdk::ContentAnalysisResponse&& aResponse);

  // See nsIContentAnalysisResponse for values
  unsigned long mAction;

  std::string mRequestToken;

  // ContentAnalysis (or, more precisely, it's Client object) must outlive
  // the transaction.
  RefPtr<ContentAnalysis> mOwner;
};

}  // namespace contentanalysis
}  // namespace mozilla

#endif  // mozilla_contentanalysis_h
