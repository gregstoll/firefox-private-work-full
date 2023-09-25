/* -*- Mode: C++; tab-width: 2; indent-tabs-mode: nil; c-basic-offset: 2 -*- */
/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#include "TextEditor.h"

#include "ContentAnalysis.h"
#include "EditorUtils.h"
#include "HTMLEditor.h"
#include "SelectionState.h"

#include "mozilla/ArrayUtils.h"
#include "mozilla/Components.h"
#include "mozilla/MouseEvents.h"
#include "mozilla/SpinEventLoopUntil.h"
#include "mozilla/dom/BrowserChild.h"
#include "mozilla/dom/BrowsingContextGroup.h"
#include "mozilla/dom/DataTransfer.h"
#include "mozilla/dom/Document.h"
#include "mozilla/dom/DocumentInlines.h"
#include "mozilla/dom/HTMLInputElement.h"
#include "mozilla/dom/PromiseNativeHandler.h"
#include "mozilla/dom/Selection.h"

#include "nsAString.h"
#include "nsClipboardProxy.h"
#include "nsCOMPtr.h"
#include "nsContentUtils.h"
#include "nsDebug.h"
#include "nsError.h"
#include "nsIClipboard.h"
#include "nsIContent.h"
#include "nsIDragService.h"
#include "nsIDragSession.h"
#include "nsIPrincipal.h"
#include "nsIFormControl.h"
#include "nsISupportsPrimitives.h"
#include "nsITransferable.h"
#include "nsIVariant.h"
#include "nsLiteralString.h"
#include "nsRange.h"
#include "nsServiceManagerUtils.h"
#include "nsString.h"
#include "nsXPCOM.h"
#include "nscore.h"

namespace mozilla {

using namespace dom;

nsresult TextEditor::InsertTextFromTransferable(
    nsITransferable* aTransferable) {
  MOZ_ASSERT(IsEditActionDataAvailable());
  MOZ_ASSERT(IsTextEditor());

  nsAutoCString bestFlavor;
  nsCOMPtr<nsISupports> genericDataObj;
  nsresult rv = aTransferable->GetAnyTransferData(
      bestFlavor, getter_AddRefs(genericDataObj));
  NS_WARNING_ASSERTION(
      NS_SUCCEEDED(rv),
      "nsITransferable::GetAnyDataTransferData() failed, but ignored");
  if (NS_SUCCEEDED(rv) && (bestFlavor.EqualsLiteral(kTextMime) ||
                           bestFlavor.EqualsLiteral(kMozTextInternal))) {
    AutoTransactionsConserveSelection dontChangeMySelection(*this);

    nsAutoString stuffToPaste;
    if (nsCOMPtr<nsISupportsString> text = do_QueryInterface(genericDataObj)) {
      text->GetData(stuffToPaste);
    }
    MOZ_ASSERT(GetEditAction() == EditAction::ePaste);
    // Use native line breaks for compatibility with Chrome.
    // XXX Although, somebody has already converted native line breaks to
    //     XP line breaks.
    UpdateEditActionData(stuffToPaste);

    nsresult rv = MaybeDispatchBeforeInputEvent();
    if (NS_FAILED(rv)) {
      NS_WARNING_ASSERTION(
          rv == NS_ERROR_EDITOR_ACTION_CANCELED,
          "EditorBase::MaybeDispatchBeforeInputEvent() failed");
      return rv;
    }

    if (!stuffToPaste.IsEmpty()) {
      // Sanitize possible carriage returns in the string to be inserted
      nsContentUtils::PlatformToDOMLineBreaks(stuffToPaste);

      AutoPlaceholderBatch treatAsOneTransaction(
          *this, ScrollSelectionIntoView::Yes, __FUNCTION__);
      nsresult rv =
          InsertTextAsSubAction(stuffToPaste, SelectionHandling::Delete);
      if (NS_FAILED(rv)) {
        NS_WARNING("EditorBase::InsertTextAsSubAction() failed");
        return rv;
      }
    }
  }

  // Try to scroll the selection into view if the paste/drop succeeded
  rv = ScrollSelectionFocusIntoView();
  NS_WARNING_ASSERTION(NS_SUCCEEDED(rv),
                       "EditorBase::ScrollSelectionFocusIntoView() failed");
  return rv;
}

nsresult TextEditor::InsertDroppedDataTransferAsAction(
    AutoEditActionDataSetter& aEditActionData, DataTransfer& aDataTransfer,
    const EditorDOMPoint& aDroppedAt, nsIPrincipal* aSourcePrincipal) {
  MOZ_ASSERT(aEditActionData.GetEditAction() == EditAction::eDrop);
  MOZ_ASSERT(GetEditAction() == EditAction::eDrop);
  MOZ_ASSERT(aDroppedAt.IsSet());
  MOZ_ASSERT(aDataTransfer.MozItemCount() > 0);

  uint32_t numItems = aDataTransfer.MozItemCount();
  AutoTArray<nsString, 5> textArray;
  textArray.SetCapacity(numItems);
  uint32_t textLength = 0;
  for (uint32_t i = 0; i < numItems; ++i) {
    nsCOMPtr<nsIVariant> data;
    aDataTransfer.GetDataAtNoSecurityCheck(u"text/plain"_ns, i,
                                           getter_AddRefs(data));
    if (!data) {
      continue;
    }
    // Use nsString to avoid copying its storage to textArray.
    nsString insertText;
    data->GetAsAString(insertText);
    if (insertText.IsEmpty()) {
      continue;
    }
    textArray.AppendElement(insertText);
    textLength += insertText.Length();
  }
  // Use nsString to avoid copying its storage to aEditActionData.
  nsString data;
  data.SetCapacity(textLength);
  // Join the text array from end to start because we insert each items
  // in the aDataTransfer at same point from start to end.  Although I
  // don't know whether this is intentional behavior.
  for (nsString& text : Reversed(textArray)) {
    data.Append(text);
  }
  // Use native line breaks for compatibility with Chrome.
  // XXX Although, somebody has already converted native line breaks to
  //     XP line breaks.
  aEditActionData.SetData(data);

  nsresult rv = aEditActionData.MaybeDispatchBeforeInputEvent();
  if (NS_FAILED(rv)) {
    NS_WARNING_ASSERTION(rv == NS_ERROR_EDITOR_ACTION_CANCELED,
                         "MaybeDispatchBeforeInputEvent() failed");
    return rv;
  }

  // Then, insert the text.  Note that we shouldn't need to walk the array
  // anymore because nobody should listen to mutation events of anonymous
  // text node in <input>/<textarea>.
  nsContentUtils::PlatformToDOMLineBreaks(data);
  rv = InsertTextAt(data, aDroppedAt, DeleteSelectedContent::No);
  if (NS_WARN_IF(Destroyed())) {
    return NS_ERROR_EDITOR_DESTROYED;
  }
  NS_WARNING_ASSERTION(NS_SUCCEEDED(rv),
                       "EditorBase::InsertTextAt(DeleteSelectedContent::No) "
                       "failed, but ignored");
  return rv;
}

void GetRequestingUrlFromDocument(EditorBase::Document* document,
                                  nsString& requestingUrl) {
  dom::Element* requestingUrlElement =
      document->GetElementById(u"requestingUrl"_ns);
  if (requestingUrlElement) {
    dom::HTMLInputElement* requestingUrlInputElement =
        dom::HTMLInputElement::FromNode(requestingUrlElement);
    if (requestingUrlInputElement) {
      requestingUrlInputElement->GetValue(requestingUrl,
                                          nsINode::CallerType::System);
    }
  }
}

namespace {
class ContentAnalysisPromiseListener
    : public mozilla::dom::PromiseNativeHandler {
  NS_DECL_ISUPPORTS

  ContentAnalysisPromiseListener(Maybe<bool>* aShouldAllowContent,
                                 mozilla::dom::Promise* aContentAnalysisPromise)
      : mShouldAllowContent(aShouldAllowContent),
        mContentAnalysisPromise(aContentAnalysisPromise) {}
  virtual void ResolvedCallback(JSContext* aCx, JS::Handle<JS::Value> aValue,
                                mozilla::ErrorResult& aRv) override {
    contentanalysis::MaybeContentAnalysisResult result =
        contentanalysis::MaybeContentAnalysisResult::FromJSONResponse(aValue,
                                                                      aCx);
    *mShouldAllowContent = Some(result.ShouldAllowContent());
    mContentAnalysisPromise->Release();
  }

  virtual void RejectedCallback(JSContext* aCx, JS::Handle<JS::Value> aValue,
                                mozilla::ErrorResult& aRv) override {
    // call to content analysis failed
    *mShouldAllowContent = Some(false);
    mContentAnalysisPromise->Release();
  }

 private:
  ~ContentAnalysisPromiseListener() = default;
  Maybe<bool>* mShouldAllowContent;
  mozilla::dom::Promise* mContentAnalysisPromise;
};
NS_IMPL_ISUPPORTS0(ContentAnalysisPromiseListener)
}  // namespace

nsresult TextEditor::HandlePaste(AutoEditActionDataSetter& aEditActionData,
                                 int32_t aClipboardType) {
  if (NS_WARN_IF(!GetDocument())) {
    return NS_OK;
  }

  // The data will be initialized in InsertTextFromTransferable() if we're not
  // an HTMLEditor.  Therefore, we cannot dispatch "beforeinput" here.

  // Get Clipboard Service
  nsresult rv;
  nsCOMPtr<nsIClipboard> clipboard =
      do_GetService("@mozilla.org/widget/clipboard;1", &rv);
  if (NS_FAILED(rv)) {
    NS_WARNING("Failed to get nsIClipboard service");
    return rv;
  }

  // Get the nsITransferable interface for getting the data from the clipboard
  Result<nsCOMPtr<nsITransferable>, nsresult> maybeTransferable =
      EditorUtils::CreateTransferableForPlainText(*GetDocument());
  if (maybeTransferable.isErr()) {
    NS_WARNING("EditorUtils::CreateTransferableForPlainText() failed");
    return maybeTransferable.unwrapErr();
  }
  nsCOMPtr<nsITransferable> transferable(maybeTransferable.unwrap());
  if (NS_WARN_IF(!transferable)) {
    NS_WARNING(
        "EditorUtils::CreateTransferableForPlainText() returned nullptr, but "
        "ignored");
    return NS_OK;  // XXX Why?
  }
  // Get the Data from the clipboard.
  auto* browserChild = BrowserChild::GetFrom(GetDocument()->GetDocShell());
  nsCOMPtr<nsIClipboardProxy> clipboardProxy = do_QueryInterface(clipboard);
  if (browserChild && clipboardProxy) {
    rv = clipboardProxy->GetDataWithBrowserCheck(transferable, aClipboardType,
                                                 browserChild);
  } else {
    nsString url;
    rv = GetDocument()->GetURL(url);
    if (NS_FAILED(rv)) {
      NS_WARNING("Failed to get URL for document");
      return rv;
    }
    bool didContentAnalysis = false;
    if (url.Equals(u"chrome://global/content/commonDialog.xhtml")) {
      // This could be a prompt() dialog, may need to do content analysis here
      nsString requestingUrl;
      GetRequestingUrlFromDocument(GetDocument(), requestingUrl);
      if (!requestingUrl.IsEmpty()) {
        mozilla::dom::Promise* contentAnalysisPromise = nullptr;
        nsCOMPtr<nsIContentAnalysis> contentAnalysis =
            mozilla::components::nsIContentAnalysis::Service(&rv);
        if (NS_FAILED(rv)) {
          NS_WARNING("Failed to get nsIContentAnalysis service");
          return rv;
        }

        bool contentAnalysisIsActive = false;
        rv = contentAnalysis->GetIsActive(&contentAnalysisIsActive);
        if (NS_FAILED(rv)) {
          NS_WARNING("Failed to get whether content analysis is active");
          return rv;
        }
        if (MOZ_UNLIKELY(contentAnalysisIsActive)) {
          mozilla::dom::AutoEntryScript aes(
              nsGlobalWindowInner::Cast(GetDocument()->GetInnerWindow()),
              "content analysis on clipboard copy");

          rv = clipboard->GetData(transferable, aClipboardType);
          if (NS_FAILED(rv)) {
            NS_WARNING("nsIClipboard::GetData() failed, but ignored");
            return NS_OK;  // XXX Why?
          }

          nsCOMPtr<nsISupports> transferData;
          rv = transferable->GetTransferData(kTextMime,
                                             getter_AddRefs(transferData));
          if (NS_SUCCEEDED(rv)) {
            nsCOMPtr<nsISupportsString> textData =
                do_QueryInterface(transferData);
            nsString text;
            if (MOZ_LIKELY(textData)) {
              rv = textData->GetData(text);
              if (NS_FAILED(rv)) {
                NS_WARNING("Failed to get text from clipboard");
                return rv;
              }
            }

            nsCString emptyDigest;
            nsCOMPtr<nsIContentAnalysisRequest> contentAnalysisRequest =
                new mozilla::contentanalysis::ContentAnalysisRequest(
                    nsIContentAnalysisRequest::BULK_DATA_ENTRY, std::move(text),
                    false, std::move(emptyDigest), std::move(requestingUrl),
                    nsIContentAnalysisRequest::OPERATION_CLIPBOARD);

            rv = contentAnalysis->AnalyzeContentRequest(
                contentAnalysisRequest, true, aes.cx(),
                &contentAnalysisPromise);
            didContentAnalysis = true;
            if (NS_SUCCEEDED(rv)) {
              didContentAnalysis = true;
              Maybe<bool> shouldAllowContent;
              RefPtr<ContentAnalysisPromiseListener> listener =
                  new ContentAnalysisPromiseListener(&shouldAllowContent,
                                                     contentAnalysisPromise);
              contentAnalysisPromise->AppendNativeHandler(listener);
              SpinEventLoopUntil("TextEditor::HandlePaste"_ns,
                                 [&shouldAllowContent]() -> bool {
                                   return shouldAllowContent.isSome();
                                 });
              if (!*shouldAllowContent) {
                // block the paste action because of content analysis
                return NS_OK;
              }
            }
          }
        }
      }
    }
    if (!didContentAnalysis) {
      rv = clipboard->GetData(transferable, aClipboardType);
    }
  }

  if (NS_FAILED(rv)) {
    NS_WARNING("nsIClipboard::GetData() failed, but ignored");
    return NS_OK;  // XXX Why?
  }
  // XXX Why don't we check this first?
  if (!IsModifiable()) {
    return NS_OK;
  }
  rv = InsertTextFromTransferable(transferable);
  NS_WARNING_ASSERTION(NS_SUCCEEDED(rv),
                       "TextEditor::InsertTextFromTransferable() failed");
  return rv;
}

nsresult TextEditor::HandlePasteTransferable(
    AutoEditActionDataSetter& aEditActionData, nsITransferable& aTransferable) {
  if (!IsModifiable()) {
    return NS_OK;
  }

  // FYI: The data of beforeinput will be initialized in
  // InsertTextFromTransferable().  Therefore, here does not touch
  // aEditActionData.
  nsresult rv = InsertTextFromTransferable(&aTransferable);
  NS_WARNING_ASSERTION(NS_SUCCEEDED(rv),
                       "TextEditor::InsertTextFromTransferable() failed");
  return rv;
}

bool TextEditor::CanPaste(int32_t aClipboardType) const {
  if (AreClipboardCommandsUnconditionallyEnabled()) {
    return true;
  }

  // can't paste if readonly
  if (!IsModifiable()) {
    return false;
  }

  nsresult rv;
  nsCOMPtr<nsIClipboard> clipboard(
      do_GetService("@mozilla.org/widget/clipboard;1", &rv));
  if (NS_FAILED(rv)) {
    NS_WARNING("Failed to get nsIClipboard service");
    return false;
  }

  // the flavors that we can deal with
  AutoTArray<nsCString, 1> textEditorFlavors = {nsDependentCString(kTextMime)};

  bool haveFlavors;
  rv = clipboard->HasDataMatchingFlavors(textEditorFlavors, aClipboardType,
                                         &haveFlavors);
  NS_WARNING_ASSERTION(NS_SUCCEEDED(rv),
                       "nsIClipboard::HasDataMatchingFlavors() failed");
  return NS_SUCCEEDED(rv) && haveFlavors;
}

bool TextEditor::CanPasteTransferable(nsITransferable* aTransferable) {
  // can't paste if readonly
  if (!IsModifiable()) {
    return false;
  }

  // If |aTransferable| is null, assume that a paste will succeed.
  if (!aTransferable) {
    return true;
  }

  nsCOMPtr<nsISupports> data;
  nsresult rv = aTransferable->GetTransferData(kTextMime, getter_AddRefs(data));
  NS_WARNING_ASSERTION(NS_SUCCEEDED(rv),
                       "nsITransferable::GetTransferData(kTextMime) failed");
  return NS_SUCCEEDED(rv) && data;
}

}  // namespace mozilla
