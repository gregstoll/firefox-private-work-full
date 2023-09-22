/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2 -*- */
/* vim: set ts=8 sts=2 et sw=2 tw=80: */
/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#include "mozilla/dom/DragEvent.h"

#include "mozilla/Components.h"
#include "mozilla/dom/BlobImpl.h"
#include "mozilla/dom/BrowserChild.h"
#include "mozilla/dom/MouseEventBinding.h"
#include "mozilla/MouseEvents.h"
#include "mozilla/SpinEventLoopUntil.h"

#include "ContentAnalysis.h"
#include "nsContentUtils.h"
#include "nsIContentAnalysis.h"

#include "prtime.h"

namespace mozilla::dom {

using ContentAnalysisPermissionResult = mozilla::Result<bool, nsresult>;

static ContentAnalysisPermissionResult CheckContentAnalysisPermission(
    nsCOMPtr<DataTransfer> aDataTransfer, RefPtr<nsPresContext> aPresContext) {
  if (!aPresContext || !aPresContext->GetDocShell()) {
    return true;
  }

  BrowserChild* browserChild =
      BrowserChild::GetFrom(aPresContext->GetDocShell());
  if (!browserChild) {
    return true;
  }

  // Check content of drop events to verify that they are permitted by
  // content analysis.
  nsCOMPtr<nsIDragSession> dragSession = nsContentUtils::GetDragSession();
  if (NS_WARN_IF(!dragSession)) {
    return true;
  }

  nsCOMPtr<nsIPrincipal> principal;
  dragSession->GetTriggeringPrincipal(getter_AddRefs(principal));
  if (!principal) {
    principal = nsContentUtils::GetSystemPrincipal();
  }
  MOZ_ASSERT(principal);

  nsresult rv = NS_OK;
  nsCOMPtr<nsIContentAnalysis> contentAnalysis =
      mozilla::components::nsIContentAnalysis::Service(&rv);
  NS_ENSURE_SUCCESS(rv, true);

  bool contentAnalysisMightBeActive = false;
  rv = contentAnalysis->GetMightBeActive(&contentAnalysisMightBeActive);
  NS_ENSURE_SUCCESS(rv, Err(rv));

  if (!contentAnalysisMightBeActive) {
    return true;
  }

  // Hold a strong reference during the event loop below.
  RefPtr<const DataTransferItemList> itemList = aDataTransfer->Items();

  nsTArray<nsString> filePaths;
  // These items are grouped together by Index() - every item with the same
  // Index() is a different representation of the same underlying data.
  // So we only need to check one of them.
  Maybe<uint32_t> lastCheckedStringIndex = Nothing();
  for (uint32_t i = 0; i < itemList->Length(); ++i) {
    bool found;
    DataTransferItem* item = itemList->IndexedGetter(i, found);
    MOZ_ASSERT(found);
    if (item->Kind() == DataTransferItem::KIND_STRING) {
      // Skip mozilla-internal context around HTML
      nsString type;
      item->GetType(type);
      if (type.EqualsASCII(kHTMLContext) || type.EqualsASCII(kHTMLInfo)) {
        continue;
      }
      if (Some(item->Index()) == lastCheckedStringIndex) {
        // Already checked a representation of this underlying data
        continue;
      }
      ErrorResult errorResult;
      nsCOMPtr<nsIVariant> data = item->Data(principal, errorResult);
      if (errorResult.Failed()) {
        NS_WARNING("Failed to get data from dragged KIND_STRING");
        return Err(errorResult.StealNSResult());
      }
      if (!data) {
        // due to principal?
        continue;
      }

      nsAutoString stringData;
      nsresult rv = data->GetAsAString(stringData);
      NS_ENSURE_SUCCESS(rv, Err(rv));

      Maybe<bool> result;
      browserChild->SendDoDragAndDropTextContentAnalysis(std::move(stringData))
          ->Then(
              GetCurrentSerialEventTarget(), __func__,
              /* resolve */
              [&result](
                  const contentanalysis::MaybeContentAnalysisResult& aResult) {
                result = Some(aResult.ShouldAllowContent());
              },
              /* reject */
              [&result](mozilla::ipc::ResponseRejectReason aReason) {
                result = Some(false);
              });

      SpinEventLoopUntil("SendDoDragAndDropTextContentAnalysis"_ns,
                         [&result]() -> bool { return result.isSome(); });

      if (!(*result)) {
        // Rejected by content analysis
        return false;
      }
      lastCheckedStringIndex = Some(item->Index());
    } else if (item->Kind() == DataTransferItem::KIND_FILE) {
      nsString path;
      ErrorResult errorResult;
      nsCOMPtr<nsIVariant> data = item->Data(principal, errorResult);
      if (errorResult.Failed()) {
        NS_WARNING("Failed to get data from dragged KIND_FILE");
        return Err(errorResult.StealNSResult());
      }
      nsCOMPtr<nsISupports> supports;
      errorResult = data->GetAsISupports(getter_AddRefs(supports));
      MOZ_ASSERT(!errorResult.Failed() && supports,
                 "File objects should be stored as nsISupports variants");
      if (nsCOMPtr<BlobImpl> blobImpl = do_QueryInterface(supports)) {
        MOZ_ASSERT(blobImpl->IsFile());
        blobImpl->GetMozFullPath(path, SystemCallerGuarantee(), errorResult);
        if (errorResult.Failed()) {
          NS_WARNING("Failed to get path from dragged KIND_FILE blob");
          return Err(errorResult.StealNSResult());
        }
      } else if (nsCOMPtr<nsIFile> ifile = do_QueryInterface(supports)) {
        ifile->GetPath(path);
      }
      if (!path.IsEmpty()) {
        filePaths.EmplaceBack(std::move(path));
      }
    }
  }

  if (!filePaths.IsEmpty()) {
    Maybe<bool> result;
    browserChild->SendDoDragAndDropFilesContentAnalysis(std::move(filePaths))
        ->Then(
            GetCurrentSerialEventTarget(), __func__,
            /* resolve */
            [&result](
                const contentanalysis::MaybeContentAnalysisResult& aResult) {
              result = Some(aResult.ShouldAllowContent());
            },
            /* reject */
            [&result](mozilla::ipc::ResponseRejectReason aReason) {
              result = Some(false);
            });

    SpinEventLoopUntil("SendDoDragAndDropFilesContentAnalysis"_ns,
                       [&result]() -> bool { return result.isSome(); });

    if (!(*result)) {
      // Rejected by content analysis
      return false;
    }
  }

  return true;
}

DragEvent::DragEvent(EventTarget* aOwner, nsPresContext* aPresContext,
                     WidgetDragEvent* aEvent)
    : MouseEvent(
          aOwner, aPresContext,
          aEvent ? aEvent : new WidgetDragEvent(false, eVoidEvent, nullptr)) {
  if (aEvent) {
    mEventIsInternal = false;
  } else {
    mEventIsInternal = true;
    mEvent->mRefPoint = LayoutDeviceIntPoint(0, 0);
    mEvent->AsMouseEvent()->mInputSource =
        MouseEvent_Binding::MOZ_SOURCE_UNKNOWN;
  }
}

void DragEvent::InitDragEvent(const nsAString& aType, bool aCanBubble,
                              bool aCancelable, nsGlobalWindowInner* aView,
                              int32_t aDetail, int32_t aScreenX,
                              int32_t aScreenY, int32_t aClientX,
                              int32_t aClientY, bool aCtrlKey, bool aAltKey,
                              bool aShiftKey, bool aMetaKey, uint16_t aButton,
                              EventTarget* aRelatedTarget,
                              DataTransfer* aDataTransfer) {
  NS_ENSURE_TRUE_VOID(!mEvent->mFlags.mIsBeingDispatched);

  MouseEvent::InitMouseEvent(aType, aCanBubble, aCancelable, aView, aDetail,
                             aScreenX, aScreenY, aClientX, aClientY, aCtrlKey,
                             aAltKey, aShiftKey, aMetaKey, aButton,
                             aRelatedTarget);
  if (mEventIsInternal) {
    mEvent->AsDragEvent()->mDataTransfer = aDataTransfer;
  }
}

DataTransfer* DragEvent::GetDataTransfer() {
  // the dataTransfer field of the event caches the DataTransfer associated
  // with the drag. It is initialized when an attempt is made to retrieve it
  // rather that when the event is created to avoid duplicating the data when
  // no listener ever uses it.
  if (!mEvent || mEvent->mClass != eDragEventClass) {
    NS_WARNING("Tried to get dataTransfer from non-drag event!");
    return nullptr;
  }

  WidgetDragEvent* dragEvent = mEvent->AsDragEvent();
  // for synthetic events, just use the supplied data transfer object even if
  // null
  if (!mEventIsInternal && !dragEvent->mDataTransfer) {
    MOZ_ASSERT(!mDragSession);
    mDragSession = nsContentUtils::GetDragSession();
    nsresult rv = nsContentUtils::SetDataTransferInEvent(dragEvent);
    NS_ENSURE_SUCCESS(rv, nullptr);

    if (dragEvent->mMessage == eDrop) {
      ContentAnalysisPermissionResult permission =
          CheckContentAnalysisPermission(do_AddRef(dragEvent->mDataTransfer),
                                         mPresContext);
      if (!permission.unwrapOr(false)) {
        // Content analysis rejected the drop or there was an error so we
        // reject.
        dragEvent->mDataTransfer->ClearAll();
      }
    }
  }

  return dragEvent->mDataTransfer;
}

// static
already_AddRefed<DragEvent> DragEvent::Constructor(
    const GlobalObject& aGlobal, const nsAString& aType,
    const DragEventInit& aParam) {
  nsCOMPtr<EventTarget> t = do_QueryInterface(aGlobal.GetAsSupports());
  RefPtr<DragEvent> e = new DragEvent(t, nullptr, nullptr);
  bool trusted = e->Init(t);
  e->InitDragEvent(aType, aParam.mBubbles, aParam.mCancelable, aParam.mView,
                   aParam.mDetail, aParam.mScreenX, aParam.mScreenY,
                   aParam.mClientX, aParam.mClientY, aParam.mCtrlKey,
                   aParam.mAltKey, aParam.mShiftKey, aParam.mMetaKey,
                   aParam.mButton, aParam.mRelatedTarget, aParam.mDataTransfer);
  e->InitializeExtraMouseEventDictionaryMembers(aParam);
  e->SetTrusted(trusted);
  e->SetComposed(aParam.mComposed);
  return e.forget();
}

}  // namespace mozilla::dom

using namespace mozilla;
using namespace mozilla::dom;

already_AddRefed<DragEvent> NS_NewDOMDragEvent(EventTarget* aOwner,
                                               nsPresContext* aPresContext,
                                               WidgetDragEvent* aEvent) {
  RefPtr<DragEvent> event = new DragEvent(aOwner, aPresContext, aEvent);
  return event.forget();
}
