/* -*- Mode: C++; tab-width: 2; indent-tabs-mode: nil; c-basic-offset: 2 -*- */
/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#include "nsBaseClipboard.h"

#include "mozilla/Logging.h"

#include "nsIClipboardOwner.h"
#include "nsCOMPtr.h"
#include "nsError.h"
#include "nsXPCOM.h"
#include "ContentAnalysis.h"
#include "nsIContentAnalysis.h"
#include "nsGlobalWindowInner.h"
#include "mozilla/Components.h"
#include "mozilla/dom/AutoEntryScript.h"
#include "mozilla/dom/BrowserParent.h"
#include "mozilla/dom/CanonicalBrowsingContext.h"
#include "mozilla/dom/Document.h"
#include "mozilla/dom/PromiseNativeHandler.h"
#include "nsISupportsPrimitives.h"

using mozilla::GenericPromise;
using mozilla::LogLevel;

nsBaseClipboard::nsBaseClipboard() : mEmptyingForSetData(false) {}

nsBaseClipboard::~nsBaseClipboard() {
  EmptyClipboard(kSelectionClipboard);
  EmptyClipboard(kGlobalClipboard);
  EmptyClipboard(kFindClipboard);
}

NS_IMPL_ISUPPORTS(nsBaseClipboard, nsIClipboard)

/**
 * Sets the transferable object
 *
 */
NS_IMETHODIMP nsBaseClipboard::SetData(
    nsITransferable* aTransferable, nsIClipboardOwner* anOwner,
    int32_t aWhichClipboard,
    mozilla::Variant<mozilla::Nothing, mozilla::dom::Document*,
                     mozilla::dom::BrowserParent*>
        aSource) {
  NS_ASSERTION(aTransferable, "clipboard given a null transferable");

  CLIPBOARD_LOG("%s", __FUNCTION__);

  if (aTransferable == mTransferable && anOwner == mClipboardOwner) {
    CLIPBOARD_LOG("%s: skipping update.", __FUNCTION__);
    return NS_OK;
  }

  if (!nsIClipboard::IsClipboardTypeSupported(kSelectionClipboard) &&
      !nsIClipboard::IsClipboardTypeSupported(kFindClipboard) &&
      aWhichClipboard != kGlobalClipboard) {
    return NS_ERROR_FAILURE;
  }

  mEmptyingForSetData = true;
  if (NS_FAILED(EmptyClipboard(aWhichClipboard))) {
    CLIPBOARD_LOG("%s: emptying clipboard failed.", __FUNCTION__);
  }
  mEmptyingForSetData = false;

  mClipboardOwner = anOwner;
  mTransferable = aTransferable;

  nsresult rv = NS_ERROR_FAILURE;
  if (mTransferable) {
    mIgnoreEmptyNotification = true;
    rv = SetNativeClipboardData(aWhichClipboard,
                                aSource.is<mozilla::dom::BrowserParent*>()
                                    ? aSource.as<mozilla::dom::BrowserParent*>()
                                    : nullptr);
    ;
    mIgnoreEmptyNotification = false;
  }
  if (NS_FAILED(rv)) {
    CLIPBOARD_LOG("%s: setting native clipboard data failed.", __FUNCTION__);
  }

  return rv;
}

/**
 * Gets the transferable object
 *
 */
NS_IMETHODIMP nsBaseClipboard::GetData(nsITransferable* aTransferable,
                                       int32_t aWhichClipboard) {
  NS_ASSERTION(aTransferable, "clipboard given a null transferable");

  CLIPBOARD_LOG("%s", __FUNCTION__);

  if (!nsIClipboard::IsClipboardTypeSupported(kSelectionClipboard) &&
      !nsIClipboard::IsClipboardTypeSupported(kFindClipboard) &&
      aWhichClipboard != kGlobalClipboard) {
    return NS_ERROR_FAILURE;
  }

  if (aTransferable)
    return GetNativeClipboardData(aTransferable, aWhichClipboard);

  return NS_ERROR_FAILURE;
}

class ContentAnalysisPastePromiseListener
    : public mozilla::dom::PromiseNativeHandler {
  NS_DECL_ISUPPORTS
  ContentAnalysisPastePromiseListener(
      RefPtr<GenericPromise::Private> aOuterPromise)
      : mOuterPromise(aOuterPromise) {}
  virtual void ResolvedCallback(JSContext* aCx, JS::Handle<JS::Value> aValue,
                                mozilla::ErrorResult& aRv) override {
    if (aValue.isObject()) {
      auto* obj = aValue.toObjectOrNull();
      // TODO is this handle thing ok?
      JS::Handle<JSObject*> handle =
          JS::Handle<JSObject*>::fromMarkedLocation(&obj);
      JS::RootedValue actionValue(aCx);
      // JS_HasProperty(aCx, handle, "action", &found);
      if (JS_GetProperty(aCx, handle, "action", &actionValue)) {
        if (actionValue.isNumber()) {
          double actionNumber = actionValue.toNumber();
          // TODO - handle WARN case too
          if (actionNumber ==
              static_cast<double>(nsIContentAnalysisResponse::ALLOW)) {
            mOuterPromise->Resolve(true, __func__);
            return;
          }
        }
      }
    }
    // TODO - indicate block to user
    // TODO - probably need a better error code
    mOuterPromise->Reject(NS_ERROR_PROXY_FORBIDDEN, __func__);
  }

  virtual void RejectedCallback(JSContext* aCx, JS::Handle<JS::Value> aValue,
                                mozilla::ErrorResult& aRv) override {
    // call to content analysis failed
    // TODO - indicate error to user
    // TODO - probably need a better error code
    mOuterPromise->Reject(NS_ERROR_PROXY_FORBIDDEN, __func__);
  }

 private:
  ~ContentAnalysisPastePromiseListener() = default;
  RefPtr<GenericPromise::Private> mOuterPromise;
};

NS_IMPL_ISUPPORTS0(ContentAnalysisPastePromiseListener)

RefPtr<GenericPromise> nsBaseClipboard::AsyncGetData(
    nsITransferable* aTransferable, int32_t aWhichClipboard,
    mozilla::Variant<mozilla::Nothing, mozilla::dom::Document*,
                     mozilla::dom::BrowserParent*>
        aSource) {
  // TODO - is this a race condition? Do we need to pass in a temporary
  // nsITransferable here and copy it to the real one only if content analysis
  // passes?
  nsresult rv = GetData(aTransferable, aWhichClipboard);
  if (NS_FAILED(rv)) {
    return GenericPromise::CreateAndReject(rv, __func__);
  }

  // GenericPromise::ChainTo
  mozilla::dom::Promise* contentAnalysisPromise = nullptr;
  mozilla::dom::BrowserParent* browser = nullptr;
  if (aSource.is<mozilla::dom::BrowserParent*>()) {
    browser = aSource.as<mozilla::dom::BrowserParent*>();
  }
  RefPtr<GenericPromise::Private> promiseToReturn = nullptr;
  if (browser) {
    nsCOMPtr<nsIContentAnalysis> contentAnalysis =
        mozilla::components::nsIContentAnalysis::Service(&rv);
    if (NS_FAILED(rv)) {
      return GenericPromise::CreateAndReject(rv, __func__);
    }
    bool contentAnalysisIsActive = false;
    rv = contentAnalysis->GetIsActive(&contentAnalysisIsActive);
    if (NS_FAILED(rv)) {
      return GenericPromise::CreateAndReject(rv, __func__);
    }
    if (contentAnalysisIsActive) {
      mozilla::dom::AutoEntryScript aes(
          nsGlobalWindowInner::Cast(
              browser->GetOwnerElement()->OwnerDoc()->GetInnerWindow()),
          "content analysis on clipboard copy");
      nsAutoCString documentURICString;
      RefPtr<nsIURI> currentURI =
          browser->GetBrowsingContext()->GetCurrentURI();
      rv = currentURI->GetSpec(documentURICString);
      if (NS_FAILED(rv)) {
        return GenericPromise::CreateAndReject(rv, __func__);
      }
      nsString documentURIString = NS_ConvertUTF8toUTF16(documentURICString);
      nsCOMPtr<nsISupports> transferData;
      // TODO - is it OK if this fails? Probably, if there's no text equivalent?
      rv = aTransferable->GetTransferData(kTextMime,
                                          getter_AddRefs(transferData));
      if (NS_FAILED(rv)) {
        return GenericPromise::CreateAndReject(rv, __func__);
      }
      nsCOMPtr<nsISupportsString> textData = do_QueryInterface(transferData);
      // TODO - is it OK if this fails? Seems like this shouldn't fail
      nsString text;
      if (textData) {
        rv = textData->GetData(text);
        if (NS_FAILED(rv)) {
          return GenericPromise::CreateAndReject(rv, __func__);
        }
      }

      nsCString emptyDigest;
      // TODO - is BULK_DATA_ENTRY right?
      nsCOMPtr<nsIContentAnalysisRequest> contentAnalysisRequest(
          new mozilla::contentanalysis::ContentAnalysisRequest(
              nsIContentAnalysisRequest::BULK_DATA_ENTRY, std::move(text),
              false, std::move(emptyDigest), std::move(documentURIString)));
      rv = contentAnalysis->AnalyzeContentRequest(
          contentAnalysisRequest, aes.cx(), &contentAnalysisPromise);
      // TODO handle error
      if (NS_SUCCEEDED(rv)) {
        promiseToReturn = new GenericPromise::Private(__func__);
        RefPtr<ContentAnalysisPastePromiseListener> listener =
            new ContentAnalysisPastePromiseListener(promiseToReturn);
        contentAnalysisPromise->AppendNativeHandler(listener);
      }
    }
  }
  if (promiseToReturn) {
    return promiseToReturn;
  } else {
    return GenericPromise::CreateAndResolve(true, __func__);
  }
}

NS_IMETHODIMP nsBaseClipboard::EmptyClipboard(int32_t aWhichClipboard) {
  CLIPBOARD_LOG("%s: clipboard=%i", __FUNCTION__, aWhichClipboard);

  if (!nsIClipboard::IsClipboardTypeSupported(kSelectionClipboard) &&
      !nsIClipboard::IsClipboardTypeSupported(kFindClipboard) &&
      aWhichClipboard != kGlobalClipboard) {
    return NS_ERROR_FAILURE;
  }

  if (mIgnoreEmptyNotification) {
    MOZ_DIAGNOSTIC_ASSERT(false, "How did we get here?");
    return NS_OK;
  }

  ClearClipboardCache();
  return NS_OK;
}

NS_IMETHODIMP
nsBaseClipboard::HasDataMatchingFlavors(const nsTArray<nsCString>& aFlavorList,
                                        int32_t aWhichClipboard,
                                        bool* outResult) {
  *outResult = true;  // say we always do.
  return NS_OK;
}

RefPtr<DataFlavorsPromise> nsBaseClipboard::AsyncHasDataMatchingFlavors(
    const nsTArray<nsCString>& aFlavorList, int32_t aWhichClipboard) {
  nsTArray<nsCString> results;
  for (const auto& flavor : aFlavorList) {
    bool hasMatchingFlavor = false;
    nsresult rv = HasDataMatchingFlavors(AutoTArray<nsCString, 1>{flavor},
                                         aWhichClipboard, &hasMatchingFlavor);
    if (NS_SUCCEEDED(rv) && hasMatchingFlavor) {
      results.AppendElement(flavor);
    }
  }

  return DataFlavorsPromise::CreateAndResolve(std::move(results), __func__);
}

NS_IMETHODIMP
nsBaseClipboard::IsClipboardTypeSupported(int32_t aWhichClipboard,
                                          bool* _retval) {
  NS_ENSURE_ARG_POINTER(_retval);
  // We support global clipboard by default.
  *_retval = kGlobalClipboard == aWhichClipboard;
  return NS_OK;
}

void nsBaseClipboard::ClearClipboardCache() {
  if (mClipboardOwner) {
    mClipboardOwner->LosingOwnership(mTransferable);
    mClipboardOwner = nullptr;
  }
  mTransferable = nullptr;
}
