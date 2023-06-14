/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#include "ContentAnalysisParent.h"
#include "ContentAnalysis.h"
#include "ErrorList.h"
#include "nsIContentAnalysis.h"
#include "nsISupportsImpl.h"
#include "nsITransferable.h"
#include "nsGlobalWindowInner.h"
#include "mozilla/Components.h"
#include "mozilla/contentanalysis/ContentAnalysisIPCTypes.h"
#include "mozilla/dom/AutoEntryScript.h"
#include "mozilla/dom/BlobImpl.h"
#include "mozilla/dom/BrowserParent.h"
#include "mozilla/dom/CanonicalBrowsingContext.h"
#include "mozilla/dom/Document.h"
#include "mozilla/dom/PromiseNativeHandler.h"
#include "nsIFile.h"
#include "nsISupportsPrimitives.h"
#include "GMPUtils.h"
#include "ScopedNSSTypes.h"

namespace mozilla::contentanalysis {

namespace {
class ContentAnalysisPromiseListener
    : public mozilla::dom::PromiseNativeHandler {
  NS_DECL_ISUPPORTS
  ContentAnalysisPromiseListener(
      PContentAnalysisParent::DoClipboardContentAnalysisResolver aResolver,
      mozilla::dom::Promise* aContentAnalysisPromise)
      : mResolver(aResolver),
        mContentAnalysisPromise(aContentAnalysisPromise) {}
  virtual void ResolvedCallback(JSContext* aCx, JS::Handle<JS::Value> aValue,
                                mozilla::ErrorResult& aRv) override {
    if (aValue.isObject()) {
      auto* obj = aValue.toObjectOrNull();
      JS::Handle<JSObject*> handle =
          JS::Handle<JSObject*>::fromMarkedLocation(&obj);
      JS::RootedValue actionValue(aCx);
      if (JS_GetProperty(aCx, handle, "action", &actionValue)) {
        if (actionValue.isNumber()) {
          double actionNumber = actionValue.toNumber();
          mResolver(contentanalysis::MaybeContentAnalysisResult(
              static_cast<int32_t>(actionNumber)));
          mContentAnalysisPromise->Release();
          return;
        }
      }
    }
    mResolver(contentanalysis::MaybeContentAnalysisResult(
        NoContentAnalysisResult::ERROR_INVALID_JSON_RESPONSE));
    mContentAnalysisPromise->Release();
  }

  virtual void RejectedCallback(JSContext* aCx, JS::Handle<JS::Value> aValue,
                                mozilla::ErrorResult& aRv) override {
    // call to content analysis failed
    mResolver(contentanalysis::MaybeContentAnalysisResult(
        NoContentAnalysisResult::ERROR_OTHER));
    mContentAnalysisPromise->Release();
  }

 private:
  ~ContentAnalysisPromiseListener() = default;
  PContentAnalysisParent::DoClipboardContentAnalysisResolver mResolver;
  mozilla::dom::Promise* mContentAnalysisPromise;
};
}  // namespace

NS_IMPL_ISUPPORTS0(ContentAnalysisPromiseListener)

static nsresult GetFileDigest(const nsString& filePath, nsCString& digestString) {
  nsresult rv;
  mozilla::Digest digest;
  digest.Begin(SEC_OID_SHA256);
  PRFileDesc* fd = nullptr;
  nsCOMPtr<nsIFile> file =
      do_CreateInstance("@mozilla.org/file/local;1", &rv);
  NS_ENSURE_SUCCESS(rv, rv);
  rv = file->InitWithPath(filePath);
  NS_ENSURE_SUCCESS(rv, rv);
  rv =
      file->OpenNSPRFileDesc(PR_RDONLY | nsIFile::OS_READAHEAD, 0, &fd);
  NS_ENSURE_SUCCESS(rv, rv);
  // TODO - is this too small? Or is there a better way to do this?
  uint8_t buffer[4096];
  PRInt32 bytesRead;
  bytesRead = PR_Read(fd, buffer, sizeof(buffer) / sizeof(uint8_t));
  while (bytesRead != 0) {
    if (bytesRead == -1) {
      PR_Close(fd);
      // TODO?
      return NS_ERROR_DOM_FILE_NOT_READABLE_ERR;
    }
    digest.Update(mozilla::Span<const uint8_t>(buffer, bytesRead));
    bytesRead = PR_Read(fd, buffer, sizeof(buffer) / sizeof(uint8_t));
  }
  PR_Close(fd);
  nsTArray<uint8_t> digestResults;
  rv = digest.End(digestResults);
  NS_ENSURE_SUCCESS(rv, rv);
  digestString = mozilla::ToHexString(digestResults);
  return NS_OK;
}

mozilla::ipc::IPCResult ContentAnalysisParent::RecvDoClipboardContentAnalysis(
    const layers::LayersId& layersId, const IPCTransferableData& aData,
    DoClipboardContentAnalysisResolver&& aResolver) {
  nsresult rv;
  mozilla::dom::Promise* contentAnalysisPromise = nullptr;
  mozilla::dom::BrowserParent* browser = nullptr;
  browser = mozilla::dom::BrowserParent::GetBrowserParentFromLayersId(layersId);
  if (!browser) {
    // not eligible for content analysis
    aResolver(contentanalysis::MaybeContentAnalysisResult(
        NoContentAnalysisResult::NO_PARENT_BROWSER));
    return IPC_OK();
  }
  nsCOMPtr<nsIContentAnalysis> contentAnalysis =
      mozilla::components::nsIContentAnalysis::Service(&rv);
  if (NS_FAILED(rv)) {
    aResolver(contentanalysis::MaybeContentAnalysisResult(
        NoContentAnalysisResult::ERROR_OTHER));
    return IPC_OK();
  }
  bool contentAnalysisIsActive = false;
  rv = contentAnalysis->GetIsActive(&contentAnalysisIsActive);
  if (NS_FAILED(rv)) {
    aResolver(contentanalysis::MaybeContentAnalysisResult(
        NoContentAnalysisResult::AGENT_NOT_PRESENT));
    return IPC_OK();
  }
  if (MOZ_LIKELY(!contentAnalysisIsActive)) {
    aResolver(contentanalysis::MaybeContentAnalysisResult(
        NoContentAnalysisResult::AGENT_NOT_PRESENT));
    return IPC_OK();
  }
  mozilla::dom::AutoEntryScript aes(
      nsGlobalWindowInner::Cast(
          browser->GetOwnerElement()->OwnerDoc()->GetInnerWindow()),
      "content analysis on clipboard copy");
  nsAutoCString documentURICString;
  RefPtr<nsIURI> currentURI = browser->GetBrowsingContext()->GetCurrentURI();
  rv = currentURI->GetSpec(documentURICString);
  if (NS_FAILED(rv)) {
    aResolver(contentanalysis::MaybeContentAnalysisResult(
        NoContentAnalysisResult::ERROR_OTHER));
    return IPC_OK();
  }
  nsString documentURIString = NS_ConvertUTF8toUTF16(documentURICString);
  nsCOMPtr<nsISupports> transferData;
  // TODO - is it OK if this fails? Probably, if there's no text
  // equivalent?
  nsCOMPtr<nsITransferable> trans =
      do_CreateInstance("@mozilla.org/widget/transferable;1", &rv);
  NS_ENSURE_SUCCESS(rv, IPC_OK());
  trans->Init(nullptr);

  rv = nsContentUtils::IPCTransferableDataToTransferable(aData, false, trans,
                                                         false);
  if (NS_FAILED(rv)) {
    aResolver(contentanalysis::MaybeContentAnalysisResult(
        NoContentAnalysisResult::ERROR_OTHER));
    return IPC_OK();
  }
  nsCOMPtr<nsIContentAnalysisRequest> contentAnalysisRequest;
  rv = trans->GetTransferData(kTextMime, getter_AddRefs(transferData));
  if (NS_SUCCEEDED(rv)) {
    nsCOMPtr<nsISupportsString> textData = do_QueryInterface(transferData);
    nsString text;
    if (MOZ_LIKELY(textData)) {
      rv = textData->GetData(text);
      if (NS_FAILED(rv)) {
        aResolver(contentanalysis::MaybeContentAnalysisResult(
            NoContentAnalysisResult::ERROR_OTHER));
        return IPC_OK();
      }
    }

    nsCString emptyDigest;
    contentAnalysisRequest = new mozilla::contentanalysis::ContentAnalysisRequest(
        nsIContentAnalysisRequest::BULK_DATA_ENTRY, std::move(text), false,
        std::move(emptyDigest), std::move(documentURIString));
  } else {
    rv = trans->GetTransferData(kFileMime, getter_AddRefs(transferData));
    if (NS_SUCCEEDED(rv)) {
      nsCOMPtr<mozilla::dom::BlobImpl> blob = do_QueryInterface(transferData);
      if (blob) {
        nsString filePath;
        ErrorResult result;
        blob->GetMozFullPathInternal(filePath, result);
        if (NS_WARN_IF(result.Failed())) {
          rv = result.StealNSResult();
        } else {
          nsCString digestString;
          if (NS_FAILED(GetFileDigest(filePath, digestString))) {
            aResolver(contentanalysis::MaybeContentAnalysisResult(
                NoContentAnalysisResult::ERROR_OTHER));
            return IPC_OK();
          }
          contentAnalysisRequest = new mozilla::contentanalysis::ContentAnalysisRequest(
              nsIContentAnalysisRequest::BULK_DATA_ENTRY, std::move(filePath), true,
              std::move(digestString), std::move(documentURIString));
        }
      }
    }
  }
  if (!contentAnalysisRequest) {
    // TODO - something like ERROR_COULD_NOT_GET_DATA?
    aResolver(contentanalysis::MaybeContentAnalysisResult(
        NoContentAnalysisResult::ERROR_OTHER));
    return IPC_OK();
  }
  rv = contentAnalysis->AnalyzeContentRequest(
      contentAnalysisRequest, aes.cx(), &contentAnalysisPromise);
  if (NS_SUCCEEDED(rv)) {
    RefPtr<ContentAnalysisPromiseListener> listener =
        new ContentAnalysisPromiseListener(aResolver,
                                                contentAnalysisPromise);
    contentAnalysisPromise->AppendNativeHandler(listener);
  } else {
    aResolver(contentanalysis::MaybeContentAnalysisResult(
        NoContentAnalysisResult::ERROR_OTHER));
  }
  return IPC_OK();
}

mozilla::ipc::IPCResult ContentAnalysisParent::RecvDoDragAndDropContentAnalysis(
    const layers::LayersId& aLayersId,
    nsTArray<nsString>&& aFilePaths,
    DoClipboardContentAnalysisResolver&& aResolver) {
  nsresult rv;
  mozilla::dom::Promise* contentAnalysisPromise = nullptr;
  nsCOMPtr<nsIContentAnalysis> contentAnalysis =
      mozilla::components::nsIContentAnalysis::Service(&rv);
  if (NS_FAILED(rv)) {
    aResolver(contentanalysis::MaybeContentAnalysisResult(
        NoContentAnalysisResult::ERROR_OTHER));
    return IPC_OK();
  }
  bool contentAnalysisIsActive = false;
  rv = contentAnalysis->GetIsActive(&contentAnalysisIsActive);
  if (NS_FAILED(rv)) {
    aResolver(contentanalysis::MaybeContentAnalysisResult(
        NoContentAnalysisResult::AGENT_NOT_PRESENT));
    return IPC_OK();
  }
  if (MOZ_LIKELY(!contentAnalysisIsActive)) {
    aResolver(contentanalysis::MaybeContentAnalysisResult(
        NoContentAnalysisResult::AGENT_NOT_PRESENT));
    return IPC_OK();
  }
  nsAutoCString documentURICString;
  mozilla::dom::BrowserParent* parent = mozilla::dom::BrowserParent::GetBrowserParentFromLayersId(aLayersId);
  RefPtr<nsIURI> currentURI = parent->GetBrowsingContext()->GetCurrentURI();
  rv = currentURI->GetSpec(documentURICString);
  if (NS_FAILED(rv)) {
    aResolver(contentanalysis::MaybeContentAnalysisResult(
        NoContentAnalysisResult::ERROR_OTHER));
    return IPC_OK();
  }
  nsString documentURIString = NS_ConvertUTF8toUTF16(documentURICString);

  // TODO - just handles one file
  nsString filePath = aFilePaths[0];
  nsCString digestString;
  if (NS_FAILED(GetFileDigest(filePath, digestString))) {
    aResolver(contentanalysis::MaybeContentAnalysisResult(
        NoContentAnalysisResult::ERROR_OTHER));
    return IPC_OK();
  }

  mozilla::dom::AutoEntryScript aes(
      nsGlobalWindowInner::Cast(parent->GetOwnerElement()
                                    ->OwnerDoc()
                                    ->GetInnerWindow()),
      "content analysis on clipboard copy");
  // TODO - is BULK_DATA_ENTRY right?
  nsCOMPtr<nsIContentAnalysisRequest> contentAnalysisRequest(
      new mozilla::contentanalysis::ContentAnalysisRequest(
          nsIContentAnalysisRequest::BULK_DATA_ENTRY, std::move(filePath), true,
          std::move(digestString), std::move(documentURIString)));
  rv = contentAnalysis->AnalyzeContentRequest(
      contentAnalysisRequest, aes.cx(), &contentAnalysisPromise);
  if (NS_SUCCEEDED(rv)) {
    RefPtr<ContentAnalysisPromiseListener> listener =
        new ContentAnalysisPromiseListener(aResolver, contentAnalysisPromise);
    contentAnalysisPromise->AppendNativeHandler(listener);
  } else {
    aResolver(contentanalysis::MaybeContentAnalysisResult(
        NoContentAnalysisResult::ERROR_OTHER));
  }
  return IPC_OK();
}

}  // namespace mozilla::contentanalysis
