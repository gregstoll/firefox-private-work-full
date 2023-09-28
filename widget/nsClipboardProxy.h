/* -*- Mode: c++; c-basic-offset: 2; tab-width: 20; indent-tabs-mode: nil; -*-
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef NS_CLIPBOARD_PROXY_H
#define NS_CLIPBOARD_PROXY_H

#include "mozilla/dom/PContent.h"
#include "nsIClipboard.h"

#define NS_CLIPBOARDPROXY_IID                        \
  {                                                  \
    0xa64c82da, 0x7326, 0x4681, {                    \
      0xa0, 0x95, 0x81, 0x2c, 0xc9, 0x86, 0xe6, 0xde \
    }                                                \
  }

// Hack for ContentChild to be able to know that we're an nsClipboardProxy.
class nsIClipboardProxy : public nsIClipboard {
 protected:
  typedef mozilla::dom::ClipboardCapabilities ClipboardCapabilities;

 public:
  NS_DECLARE_STATIC_IID_ACCESSOR(NS_CLIPBOARDPROXY_IID)

  virtual void SetCapabilities(const ClipboardCapabilities& aClipboardCaps) = 0;

  // Like GetData but allows for consultation with content analysis via
  // BrowserChild.
  virtual nsresult GetDataWithBrowserCheck(
      nsITransferable* aTransferable, int32_t aWhichClipboard,
      mozilla::dom::BrowserChild* aBrowserChild) = 0;

  // Like AsyncGetData but allows for consultation with content analysis via
  // BrowserChild.
  virtual RefPtr<mozilla::GenericPromise> AsyncGetDataWithBrowserCheck(
      nsITransferable* aTransferable, int32_t aWhichClipboard,
      mozilla::dom::BrowserChild* aBrowserChild) = 0;
};

NS_DEFINE_STATIC_IID_ACCESSOR(nsIClipboardProxy, NS_CLIPBOARDPROXY_IID)

class nsClipboardProxy final : public nsIClipboardProxy {
 public:
  NS_DECL_ISUPPORTS
  NS_DECL_NSICLIPBOARD

  nsClipboardProxy();

  static nsIClipboardProxy* FromClipboard(nsIClipboard& clipboard) {
    bool isProxy = false;
    MOZ_ASSERT(NS_SUCCEEDED(clipboard.GetIsProxy(&isProxy)));
    return isProxy ? static_cast<nsIClipboardProxy*>(&clipboard) : nullptr;
  }

  virtual void SetCapabilities(
      const ClipboardCapabilities& aClipboardCaps) override;

  nsresult GetDataWithBrowserCheck(
      nsITransferable* aTransferable, int32_t aWhichClipboard,
      mozilla::dom::BrowserChild* aBrowserChild) override;

  RefPtr<mozilla::GenericPromise> AsyncGetDataWithBrowserCheck(
      nsITransferable* aTransferable, int32_t aWhichClipboard,
      mozilla::dom::BrowserChild* aBrowserChild) override;

 private:
  ~nsClipboardProxy() = default;

  ClipboardCapabilities mClipboardCaps;
};

#endif
