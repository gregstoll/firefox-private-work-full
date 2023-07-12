/* -*- Mode: C++; tab-width: 2; indent-tabs-mode: nil; c-basic-offset: 2 -*-
 *
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#include "nsCOMPtr.h"
#include "nsPIDOMWindow.h"
#include "nsIInterfaceRequestorUtils.h"
#include "nsIWidget.h"

#include "nsIStringBundle.h"
#include "nsString.h"
#include "nsCOMArray.h"
#include "nsIFile.h"
#include "nsEnumeratorUtils.h"
#include "mozilla/dom/Directory.h"
#include "mozilla/dom/Document.h"
#include "mozilla/dom/File.h"
#include "mozilla/dom/Promise.h"
#include "mozilla/dom/PromiseNativeHandler.h"
#include "mozilla/Components.h"
#include "WidgetUtils.h"
#include "nsSimpleEnumerator.h"
#include "nsThreadUtils.h"
#include "ContentAnalysis.h"
#include "nsIContentAnalysis.h"
#include "nsQueryObject.h"
#include "js/PropertyAndElement.h"
#include "mozilla/Atomics.h"
#include "nsGlobalWindowInner.h"
#include "ScopedNSSTypes.h"
#include "GMPUtils.h"

#include "nsBaseFilePicker.h"

using namespace mozilla::widget;
using namespace mozilla::dom;

#define FILEPICKER_TITLES "chrome://global/locale/filepicker.properties"
#define FILEPICKER_FILTERS "chrome://global/content/filepicker.properties"

namespace {

nsresult LocalFileToDirectoryOrBlob(nsPIDOMWindowInner* aWindow,
                                    bool aIsDirectory, nsIFile* aFile,
                                    nsISupports** aResult) {
  MOZ_ASSERT(aWindow);

  if (aIsDirectory) {
#ifdef DEBUG
    bool isDir;
    aFile->IsDirectory(&isDir);
    MOZ_ASSERT(isDir);
#endif

    RefPtr<Directory> directory = Directory::Create(aWindow->AsGlobal(), aFile);
    MOZ_ASSERT(directory);

    directory.forget(aResult);
    return NS_OK;
  }

  RefPtr<File> file = File::CreateFromFile(aWindow->AsGlobal(), aFile);
  if (NS_WARN_IF(!file)) {
    return NS_ERROR_FAILURE;
  }

  file.forget(aResult);
  return NS_OK;
}

}  // anonymous namespace

class ContentAnalysisPromiseListener : public PromiseNativeHandler {
 public:
   NS_DECL_ISUPPORTS
   ContentAnalysisPromiseListener(nsBaseFilePicker* aFilePicker,
                          nsIFilePickerShownCallback* aCallback, nsIFile* aFile, nsIFilePicker::ResultCode aResult)
      : mFilePicker(aFilePicker),
        mCallback(aCallback),
        mFile(aFile),
        mResult(aResult) {}

   virtual void ResolvedCallback(JSContext* aCx, JS::Handle<JS::Value> aValue,
     mozilla::ErrorResult& aRv) override {
      // TODO synchronization
      // TODO error checking
      if (aValue.isObject()) {
        auto* obj = aValue.toObjectOrNull();
        // TODO is this handle thing ok?
        JS::Handle<JSObject*> handle = JS::Handle<JSObject*>::fromMarkedLocation(&obj);
        JS::RootedValue actionValue(aCx);
        //JS_HasProperty(aCx, handle, "action", &found);
        if (JS_GetProperty(aCx, handle, "action", &actionValue)) {
          if (actionValue.isNumber()) {
            double actionNumber = actionValue.toNumber();
            if (actionNumber ==
                static_cast<double>(
                    nsIContentAnalysisResponse::BLOCK)) {
              mFilePicker->RemoveFile(mFile);
            }
          }
        }
      }
      if (mCallback) {
        mCallback->Done(mResult);
      }
   }
   
   virtual void RejectedCallback(JSContext* aCx, JS::Handle<JS::Value> aValue,
                                 mozilla::ErrorResult& aRv) override {
      // remove this entry
      // TODO synchronization
      // TODO error checking
      mFilePicker->RemoveFile(mFile);
      if (mCallback) {
        mCallback->Done(mResult);
      }
   }

 private:
   ~ContentAnalysisPromiseListener() = default;
  RefPtr<nsBaseFilePicker> mFilePicker;
  RefPtr<nsIFilePickerShownCallback> mCallback;
  RefPtr<nsIFile> mFile;
  nsIFilePicker::ResultCode mResult;
};

NS_IMPL_ISUPPORTS0(ContentAnalysisPromiseListener)

/**
 * A runnable to dispatch from the main thread to the main thread to display
 * the file picker while letting the showAsync method return right away.
 */
class nsBaseFilePicker::AsyncShowFilePicker : public mozilla::Runnable {
 public:
  AsyncShowFilePicker(nsBaseFilePicker* aFilePicker,
                      nsIFilePickerShownCallback* aCallback)
      : mozilla::Runnable("AsyncShowFilePicker"),
        mFilePicker(aFilePicker),
        mCallback(aCallback) {}

  NS_IMETHOD Run() override {
    NS_ASSERTION(NS_IsMainThread(),
                 "AsyncShowFilePicker should be on the main thread!");

    // It's possible that some widget implementations require GUI operations
    // to be on the main thread, so that's why we're not dispatching to another
    // thread and calling back to the main after it's done.
    nsIFilePicker::ResultCode result = nsIFilePicker::returnCancel;
    nsresult rv = mFilePicker->Show(&result);
    if (NS_FAILED(rv)) {
      NS_ERROR("FilePicker's Show() implementation failed!");
    }
    // TODO - is this a race condition? Can running JS query this while we're waiting for a response?

    nsCOMPtr<nsIContentAnalysis> contentAnalysis =
        mozilla::components::nsIContentAnalysis::Service(&rv);
    NS_ENSURE_SUCCESS(rv, rv);
    bool contentAnalysisIsActive = false;
    rv = contentAnalysis->GetIsActive(&contentAnalysisIsActive);
    NS_ENSURE_SUCCESS(rv, rv);
    bool waitForPromise = false;
    mozilla::dom::Promise* promise = nullptr;
    if (contentAnalysisIsActive && result == nsIFilePicker::returnOK) {
      nsCOMPtr<nsIFile> file;
      // TODO handle directories
      //nsresult rv = mFilePicker->GetDomFileOrDirectory(getter_AddRefs(tmp));
      nsresult rv = mFilePicker->GetFile(getter_AddRefs(file));
      mozilla::PathString filePath = file->NativePath();
      RefPtr<ContentAnalysisPromiseListener> listener = new ContentAnalysisPromiseListener(
          mFilePicker, mCallback, file, result);

      nsCOMPtr<nsIContentAnalysis> contentAnalysis =
          mozilla::components::nsIContentAnalysis::Service(&rv);
      NS_ENSURE_SUCCESS(rv, rv);
      AutoEntryScript aes(nsGlobalWindowInner::Cast(mFilePicker->mInnerWindow), "call content analysis");

      // TODO handle error
      // TODO - is FILE_ATTACHED right?
      nsString uriString = mFilePicker->mDocumentURIString;
      mozilla::Digest digest;
      digest.Begin(SEC_OID_SHA256);
      PRFileDesc* fd = nullptr;
      rv = file->OpenNSPRFileDesc(PR_RDONLY | nsIFile::OS_READAHEAD, 0, &fd);
      NS_ENSURE_SUCCESS(rv, rv);
      // TODO - is this too small? Or is there a better way to do this?
      uint8_t buffer[4096];
      PRInt32 bytesRead;
      bytesRead = PR_Read(fd, buffer, sizeof(buffer)/sizeof(uint8_t));
      while (bytesRead != 0) {
        if (bytesRead == -1) {
          PR_Close(fd);
          return NS_ErrorAccordingToNSPR();
        }
        digest.Update(mozilla::Span<const uint8_t>(buffer, bytesRead));
        bytesRead = PR_Read(fd, buffer, sizeof(buffer)/sizeof(uint8_t));
      }
      PR_Close(fd);
      nsTArray<uint8_t> digestResults;
      rv = digest.End(digestResults);
      NS_ENSURE_SUCCESS(rv, rv);
      nsCString digestString = mozilla::ToHexString(digestResults);
      nsCOMPtr<nsIContentAnalysisRequest> contentAnalysisRequest(
          new mozilla::contentanalysis::ContentAnalysisRequest(
              nsIContentAnalysisRequest::FILE_ATTACHED, std::move(filePath),
              true, std::move(digestString), std::move(uriString)));
      rv = contentAnalysis->AnalyzeContentRequest(contentAnalysisRequest,
                                                  aes.cx(), &promise);
      // TODO - better handling
      if (NS_SUCCEEDED(rv)) {
        waitForPromise = true;
        promise->AppendNativeHandler(listener);
      }
    }

    if (!waitForPromise) {
      if (mCallback) {
        mCallback->Done(result);
      }
    }
    return NS_OK;
  }

 private:
  RefPtr<nsBaseFilePicker> mFilePicker;
  RefPtr<nsIFilePickerShownCallback> mCallback;
};

class nsBaseFilePickerEnumerator : public nsSimpleEnumerator {
 public:
  nsBaseFilePickerEnumerator(nsPIDOMWindowOuter* aParent,
                             nsISimpleEnumerator* iterator,
                             nsIFilePicker::Mode aMode)
      : mIterator(iterator),
        mParent(aParent->GetCurrentInnerWindow()),
        mMode(aMode) {}

  const nsID& DefaultInterface() override { return NS_GET_IID(nsIFile); }

  NS_IMETHOD
  GetNext(nsISupports** aResult) override {
    nsCOMPtr<nsISupports> tmp;
    nsresult rv = mIterator->GetNext(getter_AddRefs(tmp));
    NS_ENSURE_SUCCESS(rv, rv);

    if (!tmp) {
      return NS_OK;
    }

    nsCOMPtr<nsIFile> localFile = do_QueryInterface(tmp);
    if (!localFile) {
      return NS_ERROR_FAILURE;
    }

    if (!mParent) {
      return NS_ERROR_FAILURE;
    }

    return LocalFileToDirectoryOrBlob(
        mParent, mMode == nsIFilePicker::modeGetFolder, localFile, aResult);
  }

  NS_IMETHOD
  HasMoreElements(bool* aResult) override {
    return mIterator->HasMoreElements(aResult);
  }

 private:
  nsCOMPtr<nsISimpleEnumerator> mIterator;
  nsCOMPtr<nsPIDOMWindowInner> mParent;
  nsIFilePicker::Mode mMode;
};

nsBaseFilePicker::nsBaseFilePicker()
    : mAddToRecentDocs(true), mMode(nsIFilePicker::modeOpen) {}

nsBaseFilePicker::~nsBaseFilePicker() = default;

NS_IMETHODIMP nsBaseFilePicker::Init(mozIDOMWindowProxy* aParent,
                                     mozilla::dom::Document* aOwnerDoc,
                                     const nsAString& aTitle,
                                     nsIFilePicker::Mode aMode) {
  MOZ_ASSERT(aParent,
             "Null parent passed to filepicker, no file "
             "picker for you!");

  mParent = nsPIDOMWindowOuter::From(aParent);

  nsCOMPtr<nsIWidget> widget = WidgetUtils::DOMWindowToWidget(mParent);
  NS_ENSURE_TRUE(widget, NS_ERROR_FAILURE);
  mInnerWindow = aOwnerDoc->GetInnerWindow();
  nsAutoCString documentURIString;
  // TODO - is this right?
  nsresult rv = widget->GetInputContext().mURI->GetSpec(documentURIString);
  /* 
  nsAutoCString documentURIString;
  auto* uri = aOwnerDoc->GetDocumentURI();
  nsresult rv;
  rv = uri->GetSpec(documentURIString);*/
  NS_ENSURE_SUCCESS(rv, rv);
  mDocumentURIString = NS_ConvertUTF8toUTF16(documentURIString);

  mMode = aMode;
  InitNative(widget, aTitle);

  return NS_OK;
}

NS_IMETHODIMP
nsBaseFilePicker::Open(nsIFilePickerShownCallback* aCallback) {
  nsCOMPtr<nsIRunnable> filePickerEvent =
      new AsyncShowFilePicker(this, aCallback);
  return NS_DispatchToMainThread(filePickerEvent);
}

NS_IMETHODIMP
nsBaseFilePicker::AppendFilters(int32_t aFilterMask) {
  nsCOMPtr<nsIStringBundleService> stringService =
      mozilla::components::StringBundle::Service();
  if (!stringService) return NS_ERROR_FAILURE;

  nsCOMPtr<nsIStringBundle> titleBundle, filterBundle;

  nsresult rv = stringService->CreateBundle(FILEPICKER_TITLES,
                                            getter_AddRefs(titleBundle));
  if (NS_FAILED(rv)) return NS_ERROR_FAILURE;

  rv = stringService->CreateBundle(FILEPICKER_FILTERS,
                                   getter_AddRefs(filterBundle));
  if (NS_FAILED(rv)) return NS_ERROR_FAILURE;

  nsAutoString title;
  nsAutoString filter;

  if (aFilterMask & filterAll) {
    titleBundle->GetStringFromName("allTitle", title);
    filterBundle->GetStringFromName("allFilter", filter);
    AppendFilter(title, filter);
  }
  if (aFilterMask & filterHTML) {
    titleBundle->GetStringFromName("htmlTitle", title);
    filterBundle->GetStringFromName("htmlFilter", filter);
    AppendFilter(title, filter);
  }
  if (aFilterMask & filterText) {
    titleBundle->GetStringFromName("textTitle", title);
    filterBundle->GetStringFromName("textFilter", filter);
    AppendFilter(title, filter);
  }
  if (aFilterMask & filterImages) {
    titleBundle->GetStringFromName("imageTitle", title);
    filterBundle->GetStringFromName("imageFilter", filter);
    AppendFilter(title, filter);
  }
  if (aFilterMask & filterAudio) {
    titleBundle->GetStringFromName("audioTitle", title);
    filterBundle->GetStringFromName("audioFilter", filter);
    AppendFilter(title, filter);
  }
  if (aFilterMask & filterVideo) {
    titleBundle->GetStringFromName("videoTitle", title);
    filterBundle->GetStringFromName("videoFilter", filter);
    AppendFilter(title, filter);
  }
  if (aFilterMask & filterXML) {
    titleBundle->GetStringFromName("xmlTitle", title);
    filterBundle->GetStringFromName("xmlFilter", filter);
    AppendFilter(title, filter);
  }
  if (aFilterMask & filterXUL) {
    titleBundle->GetStringFromName("xulTitle", title);
    filterBundle->GetStringFromName("xulFilter", filter);
    AppendFilter(title, filter);
  }
  if (aFilterMask & filterApps) {
    titleBundle->GetStringFromName("appsTitle", title);
    // Pass the magic string "..apps" to the platform filepicker, which it
    // should recognize and do the correct platform behavior for.
    AppendFilter(title, u"..apps"_ns);
  }
  return NS_OK;
}

NS_IMETHODIMP nsBaseFilePicker::AppendRawFilter(const nsAString& aFilter) {
  mRawFilters.AppendElement(aFilter);
  return NS_OK;
}

NS_IMETHODIMP nsBaseFilePicker::GetCapture(
    nsIFilePicker::CaptureTarget* aCapture) {
  *aCapture = nsIFilePicker::CaptureTarget::captureNone;
  return NS_OK;
}

NS_IMETHODIMP nsBaseFilePicker::SetCapture(
    nsIFilePicker::CaptureTarget aCapture) {
  return NS_OK;
}

// Set the filter index
NS_IMETHODIMP nsBaseFilePicker::GetFilterIndex(int32_t* aFilterIndex) {
  *aFilterIndex = 0;
  return NS_OK;
}

NS_IMETHODIMP nsBaseFilePicker::SetFilterIndex(int32_t aFilterIndex) {
  return NS_OK;
}

NS_IMETHODIMP nsBaseFilePicker::GetFiles(nsISimpleEnumerator** aFiles) {
  NS_ENSURE_ARG_POINTER(aFiles);
  nsCOMArray<nsIFile> files;
  nsresult rv;

  // if we get into the base class, the platform
  // doesn't implement GetFiles() yet.
  // so we fake it.
  nsCOMPtr<nsIFile> file;
  rv = GetFile(getter_AddRefs(file));
  NS_ENSURE_SUCCESS(rv, rv);

  files.AppendObject(file);

  return NS_NewArrayEnumerator(aFiles, files, NS_GET_IID(nsIFile));
}

// Set the display directory
NS_IMETHODIMP nsBaseFilePicker::SetDisplayDirectory(nsIFile* aDirectory) {
  // if displaySpecialDirectory has been previously called, let's abort this
  // operation.
  if (!mDisplaySpecialDirectory.IsEmpty()) {
    return NS_OK;
  }

  if (!aDirectory) {
    mDisplayDirectory = nullptr;
    return NS_OK;
  }
  nsCOMPtr<nsIFile> directory;
  nsresult rv = aDirectory->Clone(getter_AddRefs(directory));
  if (NS_FAILED(rv)) return rv;
  mDisplayDirectory = directory;
  return NS_OK;
}

// Get the display directory
NS_IMETHODIMP nsBaseFilePicker::GetDisplayDirectory(nsIFile** aDirectory) {
  *aDirectory = nullptr;

  // if displaySpecialDirectory has been previously called, let's abort this
  // operation.
  if (!mDisplaySpecialDirectory.IsEmpty()) {
    return NS_OK;
  }

  if (!mDisplayDirectory) return NS_OK;
  nsCOMPtr<nsIFile> directory;
  nsresult rv = mDisplayDirectory->Clone(getter_AddRefs(directory));
  if (NS_FAILED(rv)) {
    return rv;
  }
  directory.forget(aDirectory);
  return NS_OK;
}

// Set the display special directory
NS_IMETHODIMP nsBaseFilePicker::SetDisplaySpecialDirectory(
    const nsAString& aDirectory) {
  // if displayDirectory has been previously called, let's abort this operation.
  if (mDisplayDirectory && mDisplaySpecialDirectory.IsEmpty()) {
    return NS_OK;
  }

  mDisplaySpecialDirectory = aDirectory;
  if (mDisplaySpecialDirectory.IsEmpty()) {
    mDisplayDirectory = nullptr;
    return NS_OK;
  }

  return NS_GetSpecialDirectory(
      NS_ConvertUTF16toUTF8(mDisplaySpecialDirectory).get(),
      getter_AddRefs(mDisplayDirectory));
}

// Get the display special directory
NS_IMETHODIMP nsBaseFilePicker::GetDisplaySpecialDirectory(
    nsAString& aDirectory) {
  aDirectory = mDisplaySpecialDirectory;
  return NS_OK;
}

NS_IMETHODIMP
nsBaseFilePicker::GetAddToRecentDocs(bool* aFlag) {
  *aFlag = mAddToRecentDocs;
  return NS_OK;
}

NS_IMETHODIMP
nsBaseFilePicker::SetAddToRecentDocs(bool aFlag) {
  mAddToRecentDocs = aFlag;
  return NS_OK;
}

NS_IMETHODIMP
nsBaseFilePicker::GetMode(nsIFilePicker::Mode* aMode) {
  *aMode = mMode;
  return NS_OK;
}

NS_IMETHODIMP
nsBaseFilePicker::SetOkButtonLabel(const nsAString& aLabel) {
  mOkButtonLabel = aLabel;
  return NS_OK;
}

NS_IMETHODIMP
nsBaseFilePicker::GetOkButtonLabel(nsAString& aLabel) {
  aLabel = mOkButtonLabel;
  return NS_OK;
}

NS_IMETHODIMP
nsBaseFilePicker::GetDomFileOrDirectory(nsISupports** aValue) {
  nsCOMPtr<nsIFile> localFile;
  nsresult rv = GetFile(getter_AddRefs(localFile));
  NS_ENSURE_SUCCESS(rv, rv);

  if (!localFile) {
    *aValue = nullptr;
    return NS_OK;
  }

  auto* innerParent = mParent ? mParent->GetCurrentInnerWindow() : nullptr;

  if (!innerParent) {
    return NS_ERROR_FAILURE;
  }

  return LocalFileToDirectoryOrBlob(
      innerParent, mMode == nsIFilePicker::modeGetFolder, localFile, aValue);
}

NS_IMETHODIMP
nsBaseFilePicker::GetDomFileOrDirectoryEnumerator(
    nsISimpleEnumerator** aValue) {
  nsCOMPtr<nsISimpleEnumerator> iter;
  nsresult rv = GetFiles(getter_AddRefs(iter));
  NS_ENSURE_SUCCESS(rv, rv);

  RefPtr<nsBaseFilePickerEnumerator> retIter =
      new nsBaseFilePickerEnumerator(mParent, iter, mMode);

  retIter.forget(aValue);
  return NS_OK;
}
