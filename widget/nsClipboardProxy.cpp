/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#if defined(ACCESSIBILITY) && defined(XP_WIN)
#  include "mozilla/a11y/Compatibility.h"
#endif
#include "mozilla/dom/ContentChild.h"
#include "mozilla/dom/BrowserChild.h"
#include "mozilla/dom/Document.h"
#include "mozilla/layers/LayersTypes.h"
#include "mozilla/Unused.h"
#include "nsArrayUtils.h"
#include "nsClipboardProxy.h"
#include "nsISupportsPrimitives.h"
#include "nsCOMPtr.h"
#include "nsComponentManagerUtils.h"
#include "nsXULAppAPI.h"
#include "nsContentUtils.h"
#include "PermissionMessageUtils.h"
#include "ContentAnalysis.h"
#include "nsIContentAnalysis.h"
#include "nsGlobalWindowInner.h"
#include "mozilla/Components.h"
#include "mozilla/dom/AutoEntryScript.h"
#include "mozilla/dom/BrowserParent.h"
#include "mozilla/dom/CanonicalBrowsingContext.h"
#include "mozilla/dom/PromiseNativeHandler.h"
#include "nsISupportsPrimitives.h"
#include "nsTransferable.h"

using namespace mozilla;
using namespace mozilla::dom;

NS_IMPL_ISUPPORTS(nsClipboardProxy, nsIClipboard, nsIClipboardProxy)

nsClipboardProxy::nsClipboardProxy() : mClipboardCaps(false, false, false) {}

NS_IMETHODIMP
nsClipboardProxy::SetData(nsITransferable* aTransferable,
                          nsIClipboardOwner* anOwner, int32_t aWhichClipboard,
                          Variant<Nothing, Document*, BrowserParent*> aSource) {
#if defined(ACCESSIBILITY) && defined(XP_WIN)
  a11y::Compatibility::SuppressA11yForClipboardCopy();
#endif

  ContentChild* child = ContentChild::GetSingleton();

  IPCDataTransfer ipcDataTransfer;
  nsContentUtils::TransferableToIPCTransferable(aTransferable, &ipcDataTransfer,
                                                false, nullptr);

  bool isPrivateData = aTransferable->GetIsPrivateData();
  nsCOMPtr<nsIPrincipal> requestingPrincipal =
      aTransferable->GetRequestingPrincipal();
  nsContentPolicyType contentPolicyType = aTransferable->GetContentPolicyType();
  nsCOMPtr<nsIReferrerInfo> referrerInfo = aTransferable->GetReferrerInfo();
  BrowserChild* browserChild = nullptr;
  if (aSource.is<Document*>()) {
    browserChild =
        BrowserChild::GetFrom(aSource.as<Document*>()->GetDocShell());
  }
  child->SendSetClipboard(std::move(ipcDataTransfer), isPrivateData,
                          requestingPrincipal, contentPolicyType, referrerInfo,
                          aWhichClipboard, browserChild);

  return NS_OK;
}

class SendDoClipboardContentAnalysisRunnable final : public Runnable {
 public:
  SendDoClipboardContentAnalysisRunnable(CondVar& promiseDone,
                                         std::atomic<int32_t>& promiseResult,
                                         layers::LayersId layersId,
                                         IPCDataTransfer&& dataTransfer)
      : Runnable("SendDoClipboardContentAnalysisRunnable"),
        mPromiseDone(promiseDone),
        mPromiseResult(promiseResult),
        mLayersId(layersId),
        mDataTransfer(dataTransfer) {}
  NS_IMETHOD Run() override {
    CondVar& localPromiseDone = mPromiseDone;
    std::atomic<int32_t>& localPromiseResult = mPromiseResult;
    ContentChild::GetSingleton()
        ->GetContentAnalysisChild()
        ->SendDoClipboardContentAnalysis(mLayersId, std::move(mDataTransfer))
        ->Then(
            GetCurrentSerialEventTarget(), __func__,
            /* resolve */
            [&localPromiseDone, &localPromiseResult](int32_t result) {
              localPromiseResult = result;
              localPromiseDone.Notify();
            },
            /* reject */
            [&localPromiseDone,
             &localPromiseResult](mozilla::ipc::ResponseRejectReason aReason) {
              localPromiseResult =
                  nsIContentAnalysisResponse::ACTION_UNSPECIFIED;
              localPromiseDone.Notify();
            });
    return NS_OK;
  }

 private:
  ~SendDoClipboardContentAnalysisRunnable() override = default;
  CondVar& mPromiseDone;
  std::atomic<int32_t>& mPromiseResult;
  layers::LayersId mLayersId;
  IPCDataTransfer& mDataTransfer;
};

// Turn off thread safety analysis because of the way we acquire a mutex
// only if content analysis might be active
NS_IMETHODIMP
nsClipboardProxy::GetData(
    nsITransferable* aTransferable, int32_t aWhichClipboard,
    mozilla::Variant<mozilla::Nothing, mozilla::dom::Document*,
                     mozilla::dom::BrowserParent*>
        aSource) MOZ_NO_THREAD_SAFETY_ANALYSIS {
  nsTArray<nsCString> types;
  aTransferable->FlavorsTransferableCanImport(types);

  IPCDataTransfer dataTransfer;
  BrowserChild* browserChild = nullptr;
  if (aSource.is<Document*>()) {
    browserChild =
        BrowserChild::GetFrom(aSource.as<Document*>()->GetDocShell());
  }
  layers::LayersId layersId;
  if (browserChild) {
    layersId = browserChild->GetLayersId();
  }

  ContentChild::GetSingleton()->SendGetClipboard(types, aWhichClipboard,
                                                 &dataTransfer);
  // Skip this possibly expensive stuff if content analysis is guaranteed to
  // not be active. Note that checking whether it is active for sure requires
  // being in the parent process since it has to be able to check the pipe.
  // But this will help us avoid calling into the parent process almost all
  // of the time.
  nsresult rv;
  nsCOMPtr<nsIContentAnalysis> contentAnalysis =
      mozilla::components::nsIContentAnalysis::Service(&rv);
  bool contentAnalysisMightBeActive = false;
  NS_ENSURE_SUCCESS(rv, rv);
  rv = contentAnalysis->GetMightBeActive(&contentAnalysisMightBeActive);
  NS_ENSURE_SUCCESS(rv, rv);
  bool allowCopy = true;
  if (contentAnalysisMightBeActive) {
    Mutex promiseDoneMutex("nsClipboardProxy::GetData");
    // TODO there may be a more idiomatic way to do this than to use a CondVar
    // with an already-locked Mutex
    promiseDoneMutex.Lock();
    CondVar promiseDoneCondVar(promiseDoneMutex, "nsClipboardProxy::GetData");
    std::atomic<int32_t> promiseResult;

    // TODO - this is a very klunky way of making a copy of dataTransfer
    // (since we need it below)
    nsCOMPtr<nsITransferable> dataTransferTemp(
        do_CreateInstance("@mozilla.org/widget/transferable;1", &rv));
    NS_ENSURE_SUCCESS(rv, rv);
    dataTransferTemp->Init(nullptr);
    rv = nsContentUtils::IPCTransferableToTransferable(dataTransfer, false,
                                                       dataTransferTemp, false);
    NS_ENSURE_SUCCESS(rv, rv);
    IPCDataTransfer dataTransferCopy;
    nsContentUtils::TransferableToIPCTransferable(
        dataTransferTemp, &dataTransferCopy, true, nullptr);
    RefPtr<SendDoClipboardContentAnalysisRunnable> runnable =
        new SendDoClipboardContentAnalysisRunnable(promiseDoneCondVar,
                                                   promiseResult, layersId,
                                                   std::move(dataTransferCopy));
    auto contentAnalysisThread =
        ContentChild::GetSingleton()->GetContentAnalysisThread();
    contentAnalysisThread->Dispatch(runnable.forget(),
                                    nsIEventTarget::DISPATCH_NORMAL);
    promiseDoneCondVar.Wait();
    // TODO - check these conditions
    if (promiseResult == nsIContentAnalysisResponse::ACTION_UNSPECIFIED ||
        promiseResult == nsIContentAnalysisResponse::BLOCK) {
      allowCopy = false;
    }
    promiseDoneMutex.Unlock();
  }

  if (!allowCopy) {
    return rv;
  }

  rv = nsContentUtils::IPCTransferableToTransferable(
      dataTransfer, false /* aAddDataFlavor */, aTransferable,
      false /* aFilterUnknownFlavors */);
  return rv;
}

NS_IMETHODIMP
nsClipboardProxy::EmptyClipboard(int32_t aWhichClipboard) {
  ContentChild::GetSingleton()->SendEmptyClipboard(aWhichClipboard);
  return NS_OK;
}

NS_IMETHODIMP
nsClipboardProxy::HasDataMatchingFlavors(const nsTArray<nsCString>& aFlavorList,
                                         int32_t aWhichClipboard,
                                         bool* aHasType) {
  *aHasType = false;

  ContentChild::GetSingleton()->SendClipboardHasType(aFlavorList,
                                                     aWhichClipboard, aHasType);

  return NS_OK;
}

NS_IMETHODIMP
nsClipboardProxy::IsClipboardTypeSupported(int32_t aWhichClipboard,
                                           bool* aIsSupported) {
  switch (aWhichClipboard) {
    case kGlobalClipboard:
      // We always support the global clipboard.
      *aIsSupported = true;
      return NS_OK;
    case kSelectionClipboard:
      *aIsSupported = mClipboardCaps.supportsSelectionClipboard();
      return NS_OK;
    case kFindClipboard:
      *aIsSupported = mClipboardCaps.supportsFindClipboard();
      return NS_OK;
    case kSelectionCache:
      *aIsSupported = mClipboardCaps.supportsSelectionCache();
      return NS_OK;
  }

  *aIsSupported = false;
  return NS_OK;
}

void nsClipboardProxy::SetCapabilities(
    const ClipboardCapabilities& aClipboardCaps) {
  mClipboardCaps = aClipboardCaps;
}

RefPtr<DataFlavorsPromise> nsClipboardProxy::AsyncHasDataMatchingFlavors(
    const nsTArray<nsCString>& aFlavorList, int32_t aWhichClipboard) {
  auto promise = MakeRefPtr<DataFlavorsPromise::Private>(__func__);
  ContentChild::GetSingleton()
      ->SendClipboardHasTypesAsync(aFlavorList, aWhichClipboard)
      ->Then(
          GetMainThreadSerialEventTarget(), __func__,
          /* resolve */
          [promise](nsTArray<nsCString> types) {
            promise->Resolve(std::move(types), __func__);
          },
          /* reject */
          [promise](mozilla::ipc::ResponseRejectReason aReason) {
            promise->Reject(NS_ERROR_FAILURE, __func__);
          });

  return promise.forget();
}

RefPtr<GenericPromise> nsClipboardProxy::AsyncGetData(
    nsITransferable* aTransferable, int32_t aWhichClipboard) {
  if (!aTransferable) {
    return GenericPromise::CreateAndReject(NS_ERROR_FAILURE, __func__);
  }

  // Get a list of flavors this transferable can import
  nsTArray<nsCString> flavors;
  nsresult rv = aTransferable->FlavorsTransferableCanImport(flavors);
  if (NS_FAILED(rv)) {
    return GenericPromise::CreateAndReject(rv, __func__);
  }

  nsCOMPtr<nsITransferable> transferable(aTransferable);
  auto promise = MakeRefPtr<GenericPromise::Private>(__func__);
  ContentChild::GetSingleton()
      ->SendGetClipboardAsync(flavors, aWhichClipboard)
      ->Then(
          GetMainThreadSerialEventTarget(), __func__,
          /* resolve */
          [promise,
           transferable](const IPCDataTransferOrError& ipcDataTransferOrError) {
            if (ipcDataTransferOrError.type() ==
                IPCDataTransferOrError::Tnsresult) {
              promise->Reject(ipcDataTransferOrError.get_nsresult(), __func__);
              return;
            }

            nsresult rv = nsContentUtils::IPCTransferableToTransferable(
                ipcDataTransferOrError.get_IPCDataTransfer(),
                false /* aAddDataFlavor */, transferable,
                false /* aFilterUnknownFlavors */);
            if (NS_FAILED(rv)) {
              promise->Reject(rv, __func__);
              return;
            }

            promise->Resolve(true, __func__);
          },
          /* reject */
          [promise](mozilla::ipc::ResponseRejectReason aReason) {
            promise->Reject(NS_ERROR_FAILURE, __func__);
          });

  return promise.forget();
}
