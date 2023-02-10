/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2 -*- */
/* vim: set ts=8 sts=2 et sw=2 tw=80: */
/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#include "ContentAnalysis.h"
#include "content_analysis/sdk/analysis_client.h"

#include "base/process_util.h"
#include "mozilla/dom/Promise.h"
#include "mozilla/Logging.h"
#include "mozilla/ScopeExit.h"
#include "mozilla/StaticPrefs_browser.h"
#include "nsIGlobalObject.h"
#include "xpcpublic.h"

#include <algorithm>
#include <sstream>

#ifdef XP_WIN
#  include <windows.h>
#  define SECURITY_WIN32 1
#  include <security.h>
#endif  // XP_WIN

namespace {

#ifdef DLP_PER_USER
// TODO: Docs are confusing here.  The name should be unique to the user but
// must be the same for both the browser and the DLP?  How?
static const char* kPipeName = "path_user";
static bool kIsPerUser = true;
#else
static const char* kPipeName = "path_system";
static bool kIsPerUser = false;
#endif

static constexpr uint32_t kAnalysisTimeoutSecs = 30;  // 30 sec

nsresult MakePromise(JSContext* aCx, RefPtr<mozilla::dom::Promise>* aPromise) {
  nsIGlobalObject* go = xpc::CurrentNativeGlobal(aCx);
  if (NS_WARN_IF(!go)) {
    return NS_ERROR_UNEXPECTED;
  }
  mozilla::ErrorResult result;
  *aPromise = mozilla::dom::Promise::Create(go, result);
  if (NS_WARN_IF(result.Failed())) {
    return result.StealNSResult();
  }
  return NS_OK;
}

std::string GenerateRequestToken() {
  static uint32_t count = 0;
  std::stringstream stm;
  stm << std::hex << base::GetCurrentProcId() << "-" << count++;
  return stm.str();
}

}  // anonymous namespace

namespace mozilla {
namespace contentanalysis {

LazyLogModule gContentAnalysisLog("contentanalysis");
#define LOGD(...) MOZ_LOG(gContentAnalysisLog, LogLevel::Debug, (__VA_ARGS__))

/* static */
StaticDataMutex<UniquePtr<content_analysis::sdk::Client>>
    ContentAnalysis::sCaClient("ContentAnalysisClient");

ContentAnalysis* gInstance;

nsresult ContentAnalysis::EnsureContentAnalysisClient() {
  auto caClientRef = sCaClient.Lock();
  auto& caClient = caClientRef.ref();
  if (caClient) {
    return NS_OK;
  }

  caClient.reset(
      content_analysis::sdk::Client::Create({kPipeName, kIsPerUser}).release());
  LOGD("Content analysis is %s", caClient ? "connected" : "not available");
  return caClient ? NS_OK : NS_ERROR_NOT_AVAILABLE;
}

static nsresult ConvertToProtobuf(
    nsIClientDownloadResource* aIn,
    content_analysis::sdk::ClientDownloadRequest_Resource* aOut) {
  nsString url;
  nsresult rv = aIn->GetUrl(url);
  NS_ENSURE_SUCCESS(rv, rv);
  aOut->set_url(NS_ConvertUTF16toUTF8(url).get());

  uint32_t resourceType;
  rv = aIn->GetType(&resourceType);
  NS_ENSURE_SUCCESS(rv, rv);
  aOut->set_type(
      static_cast<content_analysis::sdk::ClientDownloadRequest_ResourceType>(
          resourceType));

  return NS_OK;
}

ContentAnalysisRequest::ContentAnalysisRequest(unsigned long aAnalysisType,
                                               nsAString&& aString,
                                               bool aStringIsFilePath)
    : mAnalysisType(aAnalysisType) {
  if (aStringIsFilePath) {
    mFilePath = aString;
  } else {
    mTextContent = aString;
  }
}

static nsresult ConvertToProtobuf(
    nsIContentAnalysisRequest* aIn,
    content_analysis::sdk::ContentAnalysisRequest* aOut) {
  aOut->set_expires_at(time(nullptr) + kAnalysisTimeoutSecs);  // TODO:

  uint32_t analysisType;
  nsresult rv = aIn->GetAnalysisType(&analysisType);
  NS_ENSURE_SUCCESS(rv, rv);
  auto connector =
      static_cast<content_analysis::sdk::AnalysisConnector>(analysisType);
  aOut->set_analysis_connector(connector);

  std::string requestToken = GenerateRequestToken();
  aOut->set_request_token(requestToken);

  const std::string tag = "dlp";  // TODO:
  *aOut->add_tags() = tag;

  auto* requestData = aOut->mutable_request_data();

  nsString url;
  rv = aIn->GetUrl(url);
  NS_ENSURE_SUCCESS(rv, rv);
  if (!url.IsEmpty()) {
    requestData->set_url(NS_ConvertUTF16toUTF8(url).get());
  }

  nsString email;
  rv = aIn->GetEmail(email);
  NS_ENSURE_SUCCESS(rv, rv);
  if (!email.IsEmpty()) {
    requestData->set_email(NS_ConvertUTF16toUTF8(email).get());
  }

  nsCString sha256Digest;
  rv = aIn->GetSha256Digest(sha256Digest);
  NS_ENSURE_SUCCESS(rv, rv);
  if (!sha256Digest.IsEmpty()) {
    requestData->set_digest(sha256Digest.get());
  }

  nsString filePath;
  rv = aIn->GetFilePath(filePath);
  NS_ENSURE_SUCCESS(rv, rv);
  if (!filePath.IsEmpty()) {
    std::string filePathStr = NS_ConvertUTF16toUTF8(filePath).get();
    aOut->set_file_path(filePathStr);
    auto filename = filePathStr.substr(filePathStr.find_last_of("/\\") + 1);
    if (!filename.empty()) {
      requestData->set_filename(filename);
    }
  } else {
    nsString textContent;
    rv = aIn->GetTextContent(textContent);
    NS_ENSURE_SUCCESS(rv, rv);
    MOZ_ASSERT(!textContent.IsEmpty());
    aOut->set_text_content(NS_ConvertUTF16toUTF8(textContent).get());
  }

#ifdef XP_WIN
  ULONG userLen = 0;
  GetUserNameExW(NameSamCompatible, nullptr, &userLen);
  if (GetLastError() == ERROR_MORE_DATA && userLen > 0) {
    auto user = mozilla::MakeUnique<wchar_t[]>(userLen);
    if (GetUserNameExW(NameSamCompatible, user.get(), &userLen)) {
      auto* clientMetadata = aOut->mutable_client_metadata();
      auto* browser = clientMetadata->mutable_browser();
      browser->set_machine_user(NS_ConvertUTF16toUTF8(user.get()).get());
    }
  }
#endif

  nsTArray<RefPtr<nsIClientDownloadResource>> resources;
  rv = aIn->GetResources(resources);
  NS_ENSURE_SUCCESS(rv, rv);
  if (!resources.IsEmpty()) {
    auto* pbClientDownloadRequest = requestData->mutable_csd();
    for (auto& nsResource : resources) {
      auto* resource = static_cast<ClientDownloadResource*>(nsResource.get());
      rv =
          ConvertToProtobuf(resource, pbClientDownloadRequest->add_resources());
      NS_ENSURE_SUCCESS(rv, rv);
    }
  }

  return NS_OK;
}

ContentAnalysisResponse::ContentAnalysisResponse(
    content_analysis::sdk::ContentAnalysisResponse&& aResponse) {
  mAction = nsIContentAnalysisResponse::ACTION_UNSPECIFIED;
  for (auto result : aResponse.results()) {
    if (!result.has_status() ||
        result.status() !=
            content_analysis::sdk::ContentAnalysisResponse::Result::SUCCESS) {
      mAction = nsIContentAnalysisResponse::ACTION_UNSPECIFIED;
      return;
    }
    // The action values increase with severity, so the max is the most severe.
    for (auto rule : result.triggered_rules()) {
      mAction = std::max(mAction, static_cast<unsigned long>(rule.action()));
    }
  }

  // If no rules blocked then we should allow.
  if (mAction == nsIContentAnalysisResponse::ACTION_UNSPECIFIED) {
    mAction = nsIContentAnalysisResponse::ALLOW;
  }

  mRequestToken = aResponse.request_token().c_str();
}

/* static */
RefPtr<ContentAnalysisResponse> ContentAnalysisResponse::FromProtobuf(
    content_analysis::sdk::ContentAnalysisResponse&& aResponse) {
  auto ret = RefPtr<ContentAnalysisResponse>(
      new ContentAnalysisResponse(std::move(aResponse)));

  using PBContentAnalysisResponse =
      content_analysis::sdk::ContentAnalysisResponse;
  if (ret->mAction ==
      PBContentAnalysisResponse::Result::TriggeredRule::ACTION_UNSPECIFIED) {
    return nullptr;
  }

  return ret;
}

static nsresult ConvertToProtobuf(
    nsIContentAnalysisAcknowledgement* aIn, const std::string& aRequestToken,
    content_analysis::sdk::ContentAnalysisAcknowledgement* aOut) {
  aOut->set_request_token(aRequestToken);

  uint32_t result;
  nsresult rv = aIn->GetResult(&result);
  NS_ENSURE_SUCCESS(rv, rv);
  aOut->set_status(
      static_cast<content_analysis::sdk::ContentAnalysisAcknowledgement_Status>(
          result));

  uint32_t finalAction;
  rv = aIn->GetFinalAction(&finalAction);
  NS_ENSURE_SUCCESS(rv, rv);
  aOut->set_final_action(
      static_cast<
          content_analysis::sdk::ContentAnalysisAcknowledgement_FinalAction>(
          finalAction));

  return NS_OK;
}

NS_IMETHODIMP
ContentAnalysisRequest::GetAnalysisType(uint32_t* aAnalysisType) {
  *aAnalysisType = mAnalysisType;
  return NS_OK;
}

NS_IMETHODIMP
ContentAnalysisRequest::GetTextContent(nsAString& aTextContent) {
  aTextContent = mTextContent;
  return NS_OK;
}

NS_IMETHODIMP
ContentAnalysisRequest::GetFilePath(nsAString& aFilePath) {
  aFilePath = mFilePath;
  return NS_OK;
}

NS_IMETHODIMP
ContentAnalysisRequest::GetUrl(nsAString& aUrl) {
  aUrl = mUrl;
  return NS_OK;
}

NS_IMETHODIMP
ContentAnalysisRequest::GetEmail(nsAString& aEmail) {
  aEmail = mEmail;
  return NS_OK;
}

NS_IMETHODIMP
ContentAnalysisRequest::GetSha256Digest(nsACString& aSha256Digest) {
  aSha256Digest = mSha256Digest;
  return NS_OK;
}

NS_IMETHODIMP
ContentAnalysisRequest::GetResources(
    nsTArray<RefPtr<nsIClientDownloadResource>>& aResources) {
  aResources = mResources.Clone();
  return NS_OK;
}

NS_IMETHODIMP
ContentAnalysisResponse::GetAction(uint32_t* aAction) {
  *aAction = mAction;
  return NS_OK;
}

NS_IMETHODIMP
ContentAnalysisAcknowledgement::GetResult(uint32_t* aResult) {
  *aResult = mResult;
  return NS_OK;
}

NS_IMETHODIMP
ContentAnalysisAcknowledgement::GetFinalAction(uint32_t* aFinalAction) {
  *aFinalAction = mFinalAction;
  return NS_OK;
}

void ContentAnalysisResponse::SetOwner(RefPtr<ContentAnalysis> aOwner) {
  mOwner = aOwner;
}

// TODO: Finish refcount wiring
NS_IMPL_ISUPPORTS(ClientDownloadResource, nsIClientDownloadResource);
NS_IMPL_ISUPPORTS(ContentAnalysisRequest, nsIContentAnalysisRequest);
NS_IMPL_ISUPPORTS(ContentAnalysisResponse, nsIContentAnalysisResponse);
NS_IMPL_ISUPPORTS(ContentAnalysisAcknowledgement,
                  nsIContentAnalysisAcknowledgement);
NS_IMPL_ISUPPORTS(ContentAnalysis, nsIContentAnalysis);

ContentAnalysis::~ContentAnalysis() {
  auto caClientRef = sCaClient.Lock();
  auto& caClient = caClientRef.ref();
  caClient = nullptr;
}

NS_IMETHODIMP
ContentAnalysis::GetIsActive(bool* aIsActive) {
  *aIsActive = false;
  if (!StaticPrefs::browser_contentanalysis_enabled()) {
    return NS_OK;
  }

  nsresult rv = EnsureContentAnalysisClient();
  *aIsActive = SUCCEEDED(rv);
  LOGD("Local DLP Content Analysis is %sactive", *aIsActive ? "" : "not ");
  return NS_OK;
}

nsresult ContentAnalysis::RunAnalyzeRequestTask(
    RefPtr<nsIContentAnalysisRequest> aRequest,
    RefPtr<mozilla::dom::Promise> aPromise) {
  nsresult rv = NS_ERROR_FAILURE;
  auto promiseCopy = aPromise;
  auto se = MakeScopeExit([&] {
    if (!SUCCEEDED(rv)) {
      LOGD("RunAnalyzeRequestTask failed");
      promiseCopy->MaybeReject(rv);
    }
  });

  // The Client object from the SDK must be kept live as long as there are
  // active transactions.
  RefPtr<ContentAnalysis> owner = this;

  content_analysis::sdk::ContentAnalysisRequest pbRequest;
  rv = ConvertToProtobuf(aRequest, &pbRequest);
  NS_ENSURE_SUCCESS(rv, rv);

  // The content analysis connection is synchronous so run in the background.
  nsMainThreadPtrHandle<dom::Promise> promiseHolder(
      new nsMainThreadPtrHolder<dom::Promise>("content analysis promise",
                                              aPromise));
  rv = NS_DispatchBackgroundTask(
      NS_NewRunnableFunction(
          "RunAnalyzeRequestTask",
          [pbRequest = std::move(pbRequest),
           promiseHolder = std::move(promiseHolder), owner] {
            nsresult rv = NS_ERROR_FAILURE;
            content_analysis::sdk::ContentAnalysisResponse pbResponse;

            auto resolveOnMainThread = MakeScopeExit([&] {
              NS_DispatchToMainThread(NS_NewRunnableFunction(
                  "ResolveOnMainThread",
                  [rv, owner, promiseHolder = std::move(promiseHolder),
                   pbResponse = std::move(pbResponse)]() mutable {
                    if (SUCCEEDED(rv)) {
                      LOGD("Content analysis client transaction succeeded");
                      RefPtr<ContentAnalysisResponse> response =
                          ContentAnalysisResponse::FromProtobuf(
                              std::move(pbResponse));
                      response->SetOwner(owner);
                      promiseHolder.get()->MaybeResolve(std::move(response));
                    } else {
                      promiseHolder.get()->MaybeReject(rv);
                    }
                  }));
            });

            auto caClientRef = sCaClient.Lock();
            auto& caClient = caClientRef.ref();
            if (!caClient) {
              LOGD("RunAnalyzeRequestTask failed to get client");
              rv = NS_ERROR_NOT_AVAILABLE;
              return;
            }

            // Run request, then dispatch back to main thread to resolve
            // aPromise
            int err = caClient->Send(pbRequest, &pbResponse);
            if (err != 0) {
              LOGD("RunAnalyzeRequestTask client transaction failed");
              rv = NS_ERROR_FAILURE;
              return;
            }

            LOGD("Content analysis client transaction succeeded");
            rv = NS_OK;
          }),
      NS_DISPATCH_EVENT_MAY_BLOCK);

  return rv;
}

NS_IMETHODIMP
ContentAnalysis::AnalyzeContentRequest(nsIContentAnalysisRequest* aRequest,
                                       JSContext* aCx,
                                       mozilla::dom::Promise** aPromise) {
  NS_ENSURE_ARG(aRequest);

  bool isActive;
  nsresult rv = GetIsActive(&isActive);
  NS_ENSURE_SUCCESS(rv, rv);
  if (!isActive) {
    return NS_ERROR_NOT_AVAILABLE;
  }

  RefPtr<mozilla::dom::Promise> promise;
  rv = MakePromise(aCx, &promise);
  NS_ENSURE_SUCCESS(rv, rv);

  rv = RunAnalyzeRequestTask(aRequest, promise);
  if (SUCCEEDED(rv)) {
    promise.forget(aPromise);
  }
  return rv;
}

NS_IMETHODIMP
ContentAnalysisResponse::Acknowledge(
    nsIContentAnalysisAcknowledgement* aAcknowledgement) {
  MOZ_ASSERT(mOwner);
  return mOwner->RunAcknowledgeTask(aAcknowledgement, mRequestToken);
};

nsresult ContentAnalysis::RunAcknowledgeTask(
    nsIContentAnalysisAcknowledgement* aAcknowledgement,
    const std::string& aRequestToken) {
  bool isActive;
  nsresult rv = GetIsActive(&isActive);
  NS_ENSURE_SUCCESS(rv, rv);
  if (!isActive) {
    return NS_ERROR_NOT_AVAILABLE;
  }

  content_analysis::sdk::ContentAnalysisAcknowledgement pbAck;
  rv = ConvertToProtobuf(aAcknowledgement, aRequestToken, &pbAck);
  NS_ENSURE_SUCCESS(rv, rv);

  // The Client object from the SDK must be kept live as long as there are
  // active transactions.
  RefPtr<ContentAnalysis> owner = this;

  // The content analysis connection is synchronous so run in the background.
  LOGD("RunAcknowledgeTask dispatching acknowledge task");
  return NS_DispatchBackgroundTask(NS_NewRunnableFunction(
      "RunAcknowledgeTask", [owner, pbAck = std::move(pbAck)] {
        auto caClientRef = sCaClient.Lock();
        auto& caClient = caClientRef.ref();
        if (!caClient) {
          LOGD("RunAcknowledgeTask failed to get the client");
          return;
        }

        DebugOnly<int> err = caClient->Acknowledge(pbAck);
        MOZ_ASSERT(err == 0);
        LOGD("RunAcknowledgeTask sent transaction acknowledgement");
      }));
}

}  // namespace contentanalysis
}  // namespace mozilla
