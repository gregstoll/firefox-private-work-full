/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2 -*- */
/* vim: set ts=8 sts=2 et sw=2 tw=80: */
/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef mozilla_ContentAnalysisParent_h
#define mozilla_ContentAnalysisParent_h

#include "nsIURI.h"
#include "mozilla/contentanalysis/ContentAnalysisIPCTypes.h"
#include "mozilla/contentanalysis/PContentAnalysisParent.h"
#include "mozilla/dom/BrowserParent.h"
#include "mozilla/layers/LayersTypes.h"

namespace mozilla {
namespace contentanalysis {
class ContentAnalysisParent final : public PContentAnalysisParent {
  NS_INLINE_DECL_REFCOUNTING_ONEVENTTARGET(ContentAnalysisParent, final)
  ContentAnalysisParent() {}

  mozilla::ipc::IPCResult RecvDoClipboardContentAnalysis(
      const layers::LayersId& aLayersId, const IPCTransferableData& aData,
      DoClipboardContentAnalysisResolver&& aResolver);

  mozilla::ipc::IPCResult RecvDoDragAndDropContentAnalysis(
      const MaybeDiscardedBrowsingContext& aBrowsingContext,
      DoClipboardContentAnalysisResolver&& aResolver);
 private:
  virtual ~ContentAnalysisParent() = default;
};

}  // namespace contentanalysis
}  // namespace mozilla

#endif  // mozilla_ContentAnalysisParent_h
