/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#include "nsClipboardProxy.h"

#if defined(ACCESSIBILITY) && defined(XP_WIN)
#  include "mozilla/a11y/Compatibility.h"
#endif
#include "mozilla/ClipboardWriteRequestChild.h"
#include "mozilla/dom/ContentChild.h"
#include "mozilla/net/CookieJarSettings.h"
#include "mozilla/Maybe.h"
#include "mozilla/dom/BrowserChild.h"
#include "mozilla/dom/Document.h"
#include "mozilla/layers/LayersTypes.h"
#include "mozilla/Unused.h"
#include "nsArrayUtils.h"
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
                          nsIClipboardOwner* anOwner, int32_t aWhichClipboard) {
#if defined(ACCESSIBILITY) && defined(XP_WIN)
  a11y::Compatibility::SuppressA11yForClipboardCopy();
#endif

  ContentChild* child = ContentChild::GetSingleton();
  IPCTransferable ipcTransferable;
  nsContentUtils::TransferableToIPCTransferable(aTransferable, &ipcTransferable,
                                                false, nullptr);
  child->SendSetClipboard(std::move(ipcTransferable), aWhichClipboard);
  return NS_OK;
}

NS_IMETHODIMP nsClipboardProxy::AsyncSetData(
    int32_t aWhichClipboard, nsIAsyncSetClipboardDataCallback* aCallback,
    nsIAsyncSetClipboardData** _retval) {
  RefPtr<ClipboardWriteRequestChild> request =
      MakeRefPtr<ClipboardWriteRequestChild>(aCallback);
  ContentChild::GetSingleton()->SendPClipboardWriteRequestConstructor(
      request, aWhichClipboard);
  request.forget(_retval);
  return NS_OK;
}

class SendDoClipboardContentAnalysisSyncRunnable final : public Runnable {
 public:
  SendDoClipboardContentAnalysisSyncRunnable(
      CondVar& promiseDone,
      contentanalysis::MaybeContentAnalysisResult& promiseResult,
      layers::LayersId layersId, IPCTransferableData&& dataTransfer)
      : Runnable("SendDoClipboardContentAnalysisSyncRunnable"),
        mPromiseDone(promiseDone),
        mPromiseResult(promiseResult),
        mLayersId(layersId),
        mDataTransfer(dataTransfer) {}
  NS_IMETHOD Run() override {
    CondVar& localPromiseDone = mPromiseDone;
    contentanalysis::MaybeContentAnalysisResult& localPromiseResult =
        mPromiseResult;
    ContentChild::GetSingleton()
        ->GetContentAnalysisChild()
        ->SendDoClipboardContentAnalysis(mLayersId, std::move(mDataTransfer))
        ->Then(
            GetCurrentSerialEventTarget(), __func__,
            /* resolve */
            [&localPromiseDone, &localPromiseResult](
                contentanalysis::MaybeContentAnalysisResult result) {
              localPromiseResult = result;
              localPromiseDone.Notify();
            },
            /* reject */
            [&localPromiseDone,
             &localPromiseResult](mozilla::ipc::ResponseRejectReason aReason) {
              localPromiseResult.value = AsVariant(
                  contentanalysis::NoContentAnalysisResult::ERROR_OTHER);
              localPromiseDone.Notify();
            });
    return NS_OK;
  }

 private:
  ~SendDoClipboardContentAnalysisSyncRunnable() override = default;
  CondVar& mPromiseDone;
  contentanalysis::MaybeContentAnalysisResult& mPromiseResult;
  layers::LayersId mLayersId;
  IPCTransferableData& mDataTransfer;
};

class SendDoClipboardContentAnalysisAsyncRunnable final : public Runnable {
 public:
  SendDoClipboardContentAnalysisAsyncRunnable(
      RefPtr<mozilla::GenericPromise::Private> aPromise,
      IPCTransferableData&& aDataTransfer,
      layers::LayersId layersId)
      : Runnable("SendDoClipboardContentAnalysisAsyncRunnable"),
        mPromise(aPromise),
        mDataTransfer(std::move(aDataTransfer)),
        mLayersId(layersId) {}
  NS_IMETHOD Run() override {
    RefPtr<mozilla::GenericPromise::Private> localPromise = mPromise;
    ContentChild::GetSingleton()
        ->GetContentAnalysisChild()
        ->SendDoClipboardContentAnalysis(mLayersId, std::move(mDataTransfer))
        ->Then(
            GetCurrentSerialEventTarget(), __func__,
            /* resolve */
            [localPromise](
                contentanalysis::MaybeContentAnalysisResult result) {
                bool allowCopy = result.ShouldAllowContent();
                if (!allowCopy) {
                  // TODO - more specific?
                  localPromise->Reject(NS_ERROR_FAILURE, __func__);
                  return;
                }
                localPromise->Resolve(true, __func__);
            },
            /* reject */
            [localPromise](mozilla::ipc::ResponseRejectReason aReason) {
                localPromise->Reject(NS_ERROR_FAILURE, __func__);
            });
    return NS_OK;
  }

 private:
  ~SendDoClipboardContentAnalysisAsyncRunnable() override = default;
  RefPtr<mozilla::GenericPromise::Private> mPromise;
  IPCTransferableData mDataTransfer;
  layers::LayersId mLayersId;
};

NS_IMETHODIMP
nsClipboardProxy::GetData(nsITransferable* aTransferable,
                          int32_t aWhichClipboard,
                          Variant<Nothing, dom::Document*, dom::BrowserParent*>
                              aSource) {
  nsTArray<nsCString> types;
  aTransferable->FlavorsTransferableCanImport(types);

  IPCTransferableData transferable;
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
                                                 &transferable);
  // Skip this possibly expensive stuff if content analysis is guaranteed to
  // not be active. Note that checking whether it is active for sure requires
  // being in the parent process since it has to be able to check the pipe.
  // But this will help us avoid calling into the parent process almost all
  // of the time.
  nsresult rv;
  nsCOMPtr<nsIContentAnalysis> contentAnalysis =
      components::nsIContentAnalysis::Service(&rv);
  bool contentAnalysisMightBeActive = false;
  NS_ENSURE_SUCCESS(rv, rv);
  rv = contentAnalysis->GetMightBeActive(&contentAnalysisMightBeActive);
  NS_ENSURE_SUCCESS(rv, rv);
  bool allowCopy = true;
  if (contentAnalysisMightBeActive) {
    // Turn off thread safety analysis because of the way we acquire a mutex
    // only if content analysis might be active
    MOZ_PUSH_IGNORE_THREAD_SAFETY
    Mutex promiseDoneMutex("nsClipboardProxy::GetData");
    // TODO there may be a more idiomatic way to do this than to use a CondVar
    // with an already-locked Mutex
    promiseDoneMutex.Lock();
    CondVar promiseDoneCondVar(promiseDoneMutex, "nsClipboardProxy::GetData");
    contentanalysis::MaybeContentAnalysisResult promiseResult;

    IPCTransferableData transferableCopy;
    rv = nsContentUtils::CloneIPCTransferable(transferable, transferableCopy);
    NS_ENSURE_SUCCESS(rv, rv);
    RefPtr<SendDoClipboardContentAnalysisSyncRunnable> runnable =
        new SendDoClipboardContentAnalysisSyncRunnable(promiseDoneCondVar,
                                                   promiseResult, layersId,
                                                   std::move(transferableCopy));
    auto contentAnalysisEventTarget =
        ContentChild::GetSingleton()->GetContentAnalysisEventTarget();
    contentAnalysisEventTarget->Dispatch(runnable.forget(),
                                         nsIEventTarget::DISPATCH_NORMAL);
    promiseDoneCondVar.Wait();
    // TODO - I am concerned here that there could be memory coherency issues
    // here, where reading the value of promiseResult might get optimized away
    // by the compiler or something. I was hoping to keep promiseResult in an
    // Atomic<>, but the type is now too complicated for that (it's not
    // trivially copyable, for one thing)
    allowCopy = promiseResult.ShouldAllowContent();
    // This is needed to avoid assertions when the mutex gets destroyed.
    promiseDoneMutex.Unlock();
    MOZ_POP_THREAD_SAFETY
  }

  if (!allowCopy) {
    return rv;
  }

  rv = nsContentUtils::IPCTransferableDataToTransferable(
      transferable, false /* aAddDataFlavor */, aTransferable,
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
    nsITransferable* aTransferable, int32_t aWhichClipboard,
    mozilla::Variant<mozilla::Nothing, mozilla::dom::Document*,
                     mozilla::dom::BrowserParent*>
        aSource) {
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
  BrowserChild* browserChild = nullptr;
  if (aSource.is<Document*>()) {
    browserChild =
        BrowserChild::GetFrom(aSource.as<Document*>()->GetDocShell());
  } else if (aSource.is<BrowserParent*>()) {
    browserChild = nullptr;
  }
  ContentChild::GetSingleton()
      ->SendGetClipboardAsync(flavors, aWhichClipboard, browserChild)
      ->Then(
          GetMainThreadSerialEventTarget(), __func__,
          /* resolve */
          [promise,
           transferable, browserChild](const IPCTransferableDataOrError& ipcTransferableDataOrError) {
            if (ipcTransferableDataOrError.type() ==
                IPCTransferableDataOrError::Tnsresult) {
              promise->Reject(ipcTransferableDataOrError.get_nsresult(), __func__);
              return;
            }
            const IPCTransferableData& dataTransfer = ipcTransferableDataOrError.get_IPCTransferableData();
            // TODO - in an ideal world we would probably only do this if the content analysis succeeds
            // (or if content analysis isn't active).
            // But doing this in the runnable means that the items in transferable get created on a different
            // thread than they get read later.
            // So always copy the results over, and rely on the result of the promise (whether it succeeds or fails)
            // for callers to know whether they can use those results.
            nsresult rv = nsContentUtils::IPCTransferableDataToTransferable(
              dataTransfer,
              false /* aAddDataFlavor */, transferable,
              false /* aFilterUnknownFlavors */);
            if (NS_FAILED(rv)) {
              promise->Reject(rv, __func__);
              return;
            }                       

            // Skip this possibly expensive stuff if content analysis is guaranteed to
            // not be active. Note that checking whether it is active for sure requires
            // being in the parent process since it has to be able to check the pipe.
            // But this will help us avoid calling into the parent process almost all
            // of the time.
            // TODO - consolidate this logic with SendDoClipboardContentAnalysisSyncRunnable?
            nsCOMPtr<nsIContentAnalysis> contentAnalysis =
                components::nsIContentAnalysis::Service(&rv);
            bool contentAnalysisMightBeActive = false;
            if (!NS_SUCCEEDED(rv)) {
              promise->Reject(rv, __func__);
              return;
            }
            rv = contentAnalysis->GetMightBeActive(&contentAnalysisMightBeActive);
            if (!NS_SUCCEEDED(rv)) {
              promise->Reject(rv, __func__);
              return;
            }
            if (contentAnalysisMightBeActive && browserChild) {
              IPCTransferableData dataTransferCopy1;
              rv = nsContentUtils::CloneIPCTransferable(dataTransfer, dataTransferCopy1);
              if (!NS_SUCCEEDED(rv)) {
                promise->Reject(rv, __func__);
                return;
              }
              RefPtr<SendDoClipboardContentAnalysisAsyncRunnable> runnable =
                  new SendDoClipboardContentAnalysisAsyncRunnable(
                      promise, std::move(dataTransferCopy1),
                      browserChild->GetLayersId());
              auto contentAnalysisEventTarget =
                  ContentChild::GetSingleton()->GetContentAnalysisEventTarget();
              contentAnalysisEventTarget->Dispatch(runnable.forget(),
                                                   nsIEventTarget::DISPATCH_NORMAL);
            } else {
              promise->Resolve(true, __func__);
            }
          },
          /* reject */
          [promise](mozilla::ipc::ResponseRejectReason aReason) {
            promise->Reject(NS_ERROR_FAILURE, __func__);
          });

  return promise.forget();
}
