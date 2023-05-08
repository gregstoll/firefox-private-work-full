#include "ContentAnalysisParent.h"
#include "ContentAnalysis.h"
#include "nsIContentAnalysis.h"
#include "nsISupportsImpl.h"
#include "nsITransferable.h"
#include "nsGlobalWindowInner.h"
#include "mozilla/Components.h"
#include "mozilla/dom/AutoEntryScript.h"
#include "mozilla/dom/BrowserParent.h"
#include "mozilla/dom/CanonicalBrowsingContext.h"
#include "mozilla/dom/Document.h"
#include "mozilla/dom/PromiseNativeHandler.h"
#include "nsISupportsPrimitives.h"

namespace mozilla::contentanalysis {

namespace {
class ContentAnalysisPastePromiseListener
    : public mozilla::dom::PromiseNativeHandler {
  NS_DECL_ISUPPORTS
  ContentAnalysisPastePromiseListener(
      PContentAnalysisParent::DoClipboardContentAnalysisResolver aResolver)
      : mResolver(aResolver) {}
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
          mResolver(static_cast<int32_t>(actionNumber));
          return;
        }
      }
    }
    // TODO - probably need a better error code?
    mResolver(
        static_cast<int32_t>(nsIContentAnalysisResponse::ACTION_UNSPECIFIED));
  }

  virtual void RejectedCallback(JSContext* aCx, JS::Handle<JS::Value> aValue,
                                mozilla::ErrorResult& aRv) override {
    // call to content analysis failed
    // TODO - indicate error to user
    // TODO - probably need a better error code
    mResolver(
        static_cast<int32_t>(nsIContentAnalysisResponse::ACTION_UNSPECIFIED));
  }

 private:
  ~ContentAnalysisPastePromiseListener() = default;
  PContentAnalysisParent::DoClipboardContentAnalysisResolver mResolver;
};
}  // namespace

NS_IMPL_ISUPPORTS0(ContentAnalysisPastePromiseListener)

mozilla::ipc::IPCResult ContentAnalysisParent::RecvDoClipboardContentAnalysis(
    const layers::LayersId& layersId, const IPCTransferableData& aData,
    DoClipboardContentAnalysisResolver&& aResolver) {
  nsresult rv;
  mozilla::dom::Promise* contentAnalysisPromise = nullptr;
  mozilla::dom::BrowserParent* browser = nullptr;
  browser = mozilla::dom::BrowserParent::GetBrowserParentFromLayersId(layersId);
  if (browser) {
    nsCOMPtr<nsIContentAnalysis> contentAnalysis =
        mozilla::components::nsIContentAnalysis::Service(&rv);
    if (NS_FAILED(rv)) {
      // TODO - is this right? (and others)
      // TODO - probably need to call aResolver here
      return IPC_OK();
    }
    bool contentAnalysisIsActive = false;
    rv = contentAnalysis->GetIsActive(&contentAnalysisIsActive);
    if (NS_FAILED(rv)) {
      return IPC_OK();
    }
    if (MOZ_UNLIKELY(contentAnalysisIsActive)) {
      mozilla::dom::AutoEntryScript aes(
          nsGlobalWindowInner::Cast(
              browser->GetOwnerElement()->OwnerDoc()->GetInnerWindow()),
          "content analysis on clipboard copy");
      nsAutoCString documentURICString;
      RefPtr<nsIURI> currentURI =
          browser->GetBrowsingContext()->GetCurrentURI();
      rv = currentURI->GetSpec(documentURICString);
      if (NS_FAILED(rv)) {
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
        return IPC_OK();
      }
      rv = trans->GetTransferData(kTextMime, getter_AddRefs(transferData));
      if (NS_FAILED(rv)) {
        return IPC_OK();
      }
      nsCOMPtr<nsISupportsString> textData = do_QueryInterface(transferData);
      // TODO - is it OK if this fails? Seems like this shouldn't fail
      nsString text;
      if (MOZ_LIKELY(textData)) {
        rv = textData->GetData(text);
        if (NS_FAILED(rv)) {
          return IPC_OK();
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
        RefPtr<ContentAnalysisPastePromiseListener> listener =
            new ContentAnalysisPastePromiseListener(aResolver);
        contentAnalysisPromise->AppendNativeHandler(listener);

        // TODO - is this ok?
        // Just use the global of the Promise itself as the callee global.
        // JS::Rooted<JSObject*> global(aes.cx(),
        // contentAnalysisPromise->PromiseObj()); global =
        // JS::GetNonCCWObjectGlobal(global); contentAnalysisPromise->Then(
        //  aes.cx(), global,
        //  /* GetMainThreadSerialEventTarget(), __func__,*/
        //  /* resolve */
        //  [aResolver](const int32_t result) { aResolver(result); },
        //  /* reject */
        //  // TODO - is this OK?
        //  [aResolver](nsresult rv) {
        //  aResolver(static_cast<int32_t>(nsIContentAnalysisResponse::ACTION_UNSPECIFIED));
        //  });
      }
    }
  }
  return IPC_OK();
}
}  // namespace mozilla::contentanalysis