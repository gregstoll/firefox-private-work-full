/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2 -*- */
/* vim: set ts=8 sts=2 et sw=2 tw=80: */
/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */
#ifndef mozilla_contentanalysis_h
#define mozilla_contentanalysis_h

#include "mozilla/DataMutex.h"
#include "mozilla/Mutex.h"
#include "nsIContentAnalysis.h"
#include "nsProxyRelease.h"
#include "nsString.h"
#include "nsProxyRelease.h"
#include "nsTHashMap.h"

#include <string>

namespace content_analysis::sdk {
class Client;
class ContentAnalysisResponse;
}  // namespace content_analysis::sdk

namespace mozilla {
namespace contentanalysis {

class ContentAnalysisRequest : public nsIContentAnalysisRequest {
 public:
  NS_DECL_THREADSAFE_ISUPPORTS
  NS_DECL_NSICONTENTANALYSISREQUEST

  ContentAnalysisRequest(unsigned long aAnalysisType, nsAString&& aString,
                         bool aStringIsFilePath, nsACString&& aSha256Digest,
                         nsAString&& aUrl, unsigned long aResourceNameType);

 private:
  virtual ~ContentAnalysisRequest() = default;

  // See nsIContentAnalysisRequest for values
  unsigned long mAnalysisType;

  // Text content to analyze.  Only one of textContent or filePath is defined.
  nsString mTextContent;

  // Name of file to analyze.  Only one of textContent or filePath is defined.
  nsString mFilePath;

  // The URL containing the file download/upload or to which web content is
  // being uploaded.
  nsString mUrl;

  // Sha256 digest of file.
  nsCString mSha256Digest;

  // URLs involved in the download.
  nsTArray<RefPtr<nsIClientDownloadResource>> mResources;

  // Email address of user.
  nsString mEmail;

  // Unique identifier for this request
  nsCString mRequestToken;

  // Type of text to display, see nsIContentAnalysisRequest for values
  unsigned long mOperationTypeForDisplay;

  // String to display if mOperationTypeForDisplay is
  // OPERATION_CUSTOMDISPLAYSTRING
  nsString mOperationDisplayString;
};

class ContentAnalysis : public nsIContentAnalysis {
 public:
  NS_DECL_THREADSAFE_ISUPPORTS
  NS_DECL_NSICONTENTANALYSIS

  nsresult RunAcknowledgeTask(
      nsIContentAnalysisAcknowledgement* aAcknowledgement,
      const nsCString& aRequestToken);

  ContentAnalysis();

 private:
  virtual ~ContentAnalysis();
  nsresult EnsureContentAnalysisClient();
  nsresult RunAnalyzeRequestTask(RefPtr<nsIContentAnalysisRequest> aRequest,
                                 RefPtr<mozilla::dom::Promise> aPromise);

  static StaticDataMutex<UniquePtr<content_analysis::sdk::Client>> sCaClient;
  // Whether sCaClient has been created. This is convenient for checking
  // this without having to acquire the sCaClient mutex.
  static std::atomic<bool> sCaClientCreated;

  DataMutex<nsTHashMap<nsCString, nsMainThreadPtrHandle<dom::Promise>>>
      mPromiseMap;
};

class ContentAnalysisResponse : public nsIContentAnalysisResponse {
 public:
  NS_DECL_THREADSAFE_ISUPPORTS
  NS_DECL_NSICONTENTANALYSISRESPONSE

  static RefPtr<ContentAnalysisResponse> FromProtobuf(
      content_analysis::sdk::ContentAnalysisResponse&& aResponse);
  static RefPtr<ContentAnalysisResponse> FromAction(
      unsigned long aAction, const nsACString& aRequestToken);

  void SetOwner(RefPtr<ContentAnalysis> aOwner);

 private:
  virtual ~ContentAnalysisResponse() = default;
  explicit ContentAnalysisResponse(
      content_analysis::sdk::ContentAnalysisResponse&& aResponse);
  ContentAnalysisResponse(unsigned long aAction,
                          const nsACString& aRequestToken);

  // See nsIContentAnalysisResponse for values
  unsigned long mAction;

  // Identifier for the corresponding nsIContentAnalysisRequest
  nsCString mRequestToken;

  // ContentAnalysis (or, more precisely, it's Client object) must outlive
  // the transaction.
  RefPtr<ContentAnalysis> mOwner;
};

}  // namespace contentanalysis
}  // namespace mozilla

#endif  // mozilla_contentanalysis_h
