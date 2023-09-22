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

NS_IMETHODIMP
nsClipboardProxy::GetData(nsITransferable* aTransferable,
                          int32_t aWhichClipboard) {
  return GetDataWithBrowserCheck(aTransferable, aWhichClipboard, nullptr);
}

nsresult nsClipboardProxy::GetDataWithBrowserCheck(
    nsITransferable* aTransferable, int32_t aWhichClipboard,
    RefPtr<mozilla::dom::BrowserChild> aBrowserChild) {
  nsTArray<nsCString> types;
  aTransferable->FlavorsTransferableCanImport(types);

  IPCTransferableData transferable;
  ContentChild::GetSingleton()->SendGetClipboard(types, aWhichClipboard,
                                                 &transferable);

  // Allow unless content analysis says not to.
  bool allowCopy = true;
  if (aBrowserChild) {
    allowCopy =
        aBrowserChild->CheckClipboardWithContentAnalysisSync(transferable);
  }
  if (!allowCopy) {
    return NS_ERROR_CONTENT_BLOCKED;
  }

  nsresult rv = nsContentUtils::IPCTransferableDataToTransferable(
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
    nsITransferable* aTransferable, int32_t aWhichClipboard) {
  return AsyncGetDataWithBrowserCheck(aTransferable, aWhichClipboard, nullptr);
}

RefPtr<GenericPromise> nsClipboardProxy::AsyncGetDataWithBrowserCheck(
    nsITransferable* aTransferable, int32_t aWhichClipboard,
    RefPtr<mozilla::dom::BrowserChild> aBrowserChild) {
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
      ->SendGetClipboardAsync(flavors, aWhichClipboard, aBrowserChild)
      ->Then(
          GetMainThreadSerialEventTarget(), __func__,
          /* resolve */
          [promise, transferable, aBrowserChild](
              const IPCTransferableDataOrError& ipcTransferableDataOrError) {
            if (ipcTransferableDataOrError.type() ==
                IPCTransferableDataOrError::Tnsresult) {
              promise->Reject(ipcTransferableDataOrError.get_nsresult(),
                              __func__);
              return;
            }
            const IPCTransferableData& dataTransfer =
                ipcTransferableDataOrError.get_IPCTransferableData();
            // TODO - in an ideal world we would probably only do this if the
            // content analysis succeeds (or if content analysis isn't active).
            // But doing this in the runnable means that the items in
            // transferable get created on a different thread than they get read
            // later. So always copy the results over, and rely on the result of
            // the promise (whether it succeeds or fails) for callers to know
            // whether they can use those results.
            nsresult rv = nsContentUtils::IPCTransferableDataToTransferable(
                dataTransfer, false /* aAddDataFlavor */, transferable,
                false /* aFilterUnknownFlavors */);
            if (NS_FAILED(rv)) {
              promise->Reject(rv, __func__);
              return;
            }

            // Allow unless content analysis says not to.
            if (aBrowserChild) {
              aBrowserChild->CheckClipboardWithContentAnalysis(dataTransfer,
                                                               promise);
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
