/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#include "ContentAnalysisParent.h"
#include "ContentAnalysis.h"
#include "nsIContentAnalysis.h"
#include "nsISupportsImpl.h"
#include "nsITransferable.h"
#include "nsGlobalWindowInner.h"
#include "mozilla/Components.h"
#include "mozilla/contentanalysis/ContentAnalysisIPCTypes.h"
#include "mozilla/dom/AutoEntryScript.h"
#include "mozilla/dom/BrowserParent.h"
#include "mozilla/dom/CanonicalBrowsingContext.h"
#include "mozilla/dom/Document.h"
#include "mozilla/dom/PromiseNativeHandler.h"
#include "nsISupportsPrimitives.h"

namespace mozilla::contentanalysis {

namespace {
class ContentAnalysisPromiseListener
    : public mozilla::dom::PromiseNativeHandler {
  NS_DECL_ISUPPORTS
  ContentAnalysisPastePromiseListener(
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
  rv = trans->GetTransferData(kTextMime, getter_AddRefs(transferData));
  if (NS_FAILED(rv)) {
    aResolver(contentanalysis::MaybeContentAnalysisResult(
        NoContentAnalysisResult::ERROR_OTHER));
    return IPC_OK();
  }
  nsCOMPtr<nsISupportsString> textData = do_QueryInterface(transferData);
  // TODO - is it OK if this fails? Seems like this shouldn't fail
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
  nsCOMPtr<nsIContentAnalysisRequest> contentAnalysisRequest(
      new mozilla::contentanalysis::ContentAnalysisRequest(
          nsIContentAnalysisRequest::BULK_DATA_ENTRY, std::move(text), false,
          std::move(emptyDigest), std::move(documentURIString)));
  rv = contentAnalysis->AnalyzeContentRequest(contentAnalysisRequest, aes.cx(),
                                              &contentAnalysisPromise);
  if (NS_SUCCEEDED(rv)) {
    RefPtr<ContentAnalysisPastePromiseListener> listener =
        new ContentAnalysisPastePromiseListener(aResolver,
                                                contentAnalysisPromise);
    contentAnalysisPromise->AppendNativeHandler(listener);
  } else {
    aResolver(contentanalysis::MaybeContentAnalysisResult(
        NoContentAnalysisResult::ERROR_OTHER));
  }
  return IPC_OK();
}

mozilla::ipc::IPCResult ContentAnalysisParent::RecvDoDragAndDropContentAnalysis(
    PBrowserParent* aBrowserParent,
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
  mozilla::dom::BrowserParent* parent = mozilla::dom::BrowserParent::GetFrom(aBrowserParent);
  //RefPtr<nsIURI> currentURI = aBrowsingContext->Canonical()->GetCurrentURI();
  RefPtr<nsIURI> currentURI = parent->GetBrowsingContext()->GetCurrentURI();
  rv = currentURI->GetSpec(documentURICString);
  if (NS_FAILED(rv)) {
    aResolver(contentanalysis::MaybeContentAnalysisResult(
        NoContentAnalysisResult::ERROR_OTHER));
    return IPC_OK();
  }
  nsString documentURIString = NS_ConvertUTF8toUTF16(documentURICString);

  // TODO
  nsString emptyFilePath;
  nsCString emptyDigest;
  /* if (!aBrowsingContext->GetDocument()) {
    aResolver(contentanalysis::MaybeContentAnalysisResult(
        NoContentAnalysisResult::ERROR_OTHER));
    return IPC_OK();
  }*/
  mozilla::dom::AutoEntryScript aes(
      nsGlobalWindowInner::Cast(parent->GetOwnerElement()
                                    ->OwnerDoc()
                                    ->GetInnerWindow()),
      "content analysis on clipboard copy");
  nsCOMPtr<nsIContentAnalysisRequest> contentAnalysisRequest(
      new mozilla::contentanalysis::ContentAnalysisRequest(
          nsIContentAnalysisRequest::BULK_DATA_ENTRY, std::move(emptyFilePath), true,
          std::move(emptyDigest), std::move(documentURIString)));
  rv = contentAnalysis->AnalyzeContentRequest(
      contentAnalysisRequest, aes.cx(), &contentAnalysisPromise);
  if (NS_SUCCEEDED(rv)) {
    RefPtr<ContentAnalysisPromiseListener> listener =
        new ContentAnalysisPromiseListener(aResolver);
    contentAnalysisPromise->AppendNativeHandler(listener);
  } else {
    aResolver(contentanalysis::MaybeContentAnalysisResult(
        NoContentAnalysisResult::ERROR_OTHER));
  }
  return IPC_OK();
}

}  // namespace mozilla::contentanalysis
