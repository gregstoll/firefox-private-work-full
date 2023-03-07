/* -*- indent-tabs-mode: nil; js-indent-level: 2 -*- */
/* vim: set ts=2 et sw=2 tw=80: */
/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

/**
 * Contains elements of the Content Analysis UI, which are integrated into
 * various browser behaviors (uploading, downloading, printing, etc) that
 * require content analysis to be done.
 * The content analysis itself is done by the clients of this script, who
 * use nsIContentAnalysis to talk to the external CA system.
 */

"use strict";

ChromeUtils.defineESModuleGetters(this, {
  Downloads: "resource://gre/modules/Downloads.sys.mjs",
});

const { BrowserWindowTracker } = ChromeUtils.import(
  "resource:///modules/BrowserWindowTracker.jsm"
);

var ContentAnalysisViews = {
  _SHOW_NOTIFICATIONS: true,

  _SHOW_DIALOGS: false,

  _RESULT_NOTIFICATION_TIMEOUT_MS: 30 * 1000,

  /**
   * Registers for various messages/events that will indicate the potential
   * need for communicating something to the user. 
   */
  initialize() {
    this.initializeDownloadCA();
  },

  /**
   * Register for file download CA events.
   */
  async initializeDownloadCA() {
    let downloadsView = {

      /**
       * Maps download objects to a timer controlling the "slow operation"
       * display or null.
       */
      _caDownloadsTimers: new Map(),

      _isCorrectWindow: aDownload => {
        // The correct window is the one associated with the download's
        // browsing context, or is the topmost window when the download is not
        // associated with one.
        return ((!aDownload.source.browsingContextId &&
              window === BrowserWindowTracker.getTopWindow()) ||
            (aDownload.source.browsingContextId &&
              BrowsingContext.get(aDownload.source.browsingContextId).topChromeWindow === window));
      },

      onDownloadAdded: aDownload => {
        // ignored
      },

      onDownloadChanged: aDownload => {
        if (!downloadsView._isCorrectWindow(aDownload)) {
          return;
        }

        const SLOW_TIMEOUT_MS = 3000;  // 3 sec

        // On ContentAnalysisBegun, start timer that, when it expires,
        // presents a "slow CA check" message.
        // On ContentAnalysisResult, cancels timer or slow message UI,
        // if present, and possibly presents the CA verdict.
        if (aDownload.contentAnalysisBegun &&
            !downloadsView._caDownloadsTimers.has(aDownload)) {
          downloadsView._caDownloadsTimers.set(aDownload, setTimeout(() => {
            downloadsView._caDownloadsTimers.set(aDownload, 
              this._showSlowCAMessage(
                Ci.nsIContentAnalysisRequest.FILE_DOWNLOADED,
                aDownload.source.url));
          }, SLOW_TIMEOUT_MS));
          return;
        }

        // DLP TODO: This conditional is now wrong because it will trip when we
        // are showing the result notification.
        if (!aDownload.contentAnalysisBegun &&
            downloadsView._caDownloadsTimers.has(aDownload)) {
          this._disconnectFromView(downloadsView._caDownloadsTimers.get(aDownload));
          downloadsView._caDownloadsTimers.set(aDownload, this._showCAResult(
            Ci.nsIContentAnalysisRequest.FILE_DOWNLOADED,
            aDownload.source.url,  /* TODO: Better name */
            aDownload.contentAnalysisResult));
        }
      },

      onDownloadRemoved: aDownload => {
        if (!downloadsView._isCorrectWindow(aDownload)) {
          return;
        }

        this._disconnectFromView(downloadsView._caDownloadsTimers.get(aDownload));
        downloadsView._caDownloadsTimers.delete(aDownload);
      },
    };

    await (await Downloads.getList(Downloads.ALL)).addView(downloadsView);
  },

  _disconnectFromView(timerOrNotification) {
    // Cancels "slow operation" timer for the download, or any
    // notifications for it, if it exists.
    if (!timerOrNotification) {
      return;
    }
    if (typeof timerOrNotification === 'number') {
      // it's a timer
      clearTimeout(timer);
    } else {
      // it's a notification
      timerOrNotification.close();
    }
  },

  _showMessage(aMessage, aTimeout = 0) {
    if (this._SHOW_DIALOGS) {
      window.alert(aMessage);
    }

    if (this._SHOW_NOTIFICATIONS) {
      const notification = new Notification('Content Analysis', {
        body: aMessage,
      });

//      notification.addEventListener('click', () => { notification.close(); });
      if (aTimeout != 0) {
        setTimeout(() => { notification.close(); }, aTimeout);
      }
      return notification;
    }

    return null;
  },

  /**
   * Show a messagge to the user to indicate that a CA request is taking
   * a long time.
   */
  _showSlowCAMessage(aOperation, aResourceName) {
    // TODO: Better message
    return this._showMessage('The Content Analysis Tool is taking a looooong time to respond...');
  },

  /**
   * Show a message to the user to indicate the result of a CA request.
   */
  _showCAResult(aOperation, aResourceName, aCAResult) {
    // TODO: Better messages
    let message = null;
    let timeoutMs = 0;

    switch (aCAResult) {
      case Ci.nsIContentAnalysisAcknowledgement.ALLOW:
        // We don't need to show anything
        break;
      case Ci.nsIContentAnalysisAcknowledgement.REPORT_ONLY:
        message = "CA responded with REPORT_ONLY";
        timeoutMs = 15 * 1000;  // 15s
        break;
      case Ci.nsIContentAnalysisAcknowledgement.WARN:
        message = "CA responded with WARN";
        break;
      case Ci.nsIContentAnalysisAcknowledgement.BLOCK:
        message = "CA responded with BLOCK.  Transfer denied.";
        break;
      case Ci.nsIContentAnalysisAcknowledgement.ACTION_UNSPECIFIED:
        message = "An error occurred in communicating with the CA.  Transfer denied.";
        break;
    }

    if (message) {
      return this._showMessage(message, timeoutMs);
    }

    return null;
  },
};

ContentAnalysisViews.initialize();
