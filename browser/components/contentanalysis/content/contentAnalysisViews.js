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

      onDownloadAdded: aDownload => {
        // ignored
      },

      onDownloadChanged: aDownload => {
        // Ensure that this is the view for the window associated with the
        // download, or is the topmost window when the download is not
        // associated with one.
        if ((!aDownload.source.browsingContextId &&
              window !== BrowserWindowTracker.getTopWindow()) ||
            (aDownload.source.browsingContextId &&
              BrowsingContext.get(aDownload.source.browsingContextId).topChromeWindow !== window)) {
          return;
        }

        const SLOW_TIMEOUT_MS = 3000;

        // On ContentAnalysisBegun, start timer that, when it expires,
        // presents a "slow CA check" message.
        // On ContentAnalysisResult, cancels timer or slow message UI,
        // if present, and possibly presents the CA verdict.
        if (aDownload.contentAnalysisBegun &&
            !downloadsView._caDownloadsTimers.has(aDownload)) {
          downloadsView._caDownloadsTimers.set(aDownload, setTimeout(() => {
            downloadsView._caDownloadsTimers.set(aDownload, null);
            this._showSlowCAMessage(
              Ci.nsIContentAnalysisRequest.FILE_DOWNLOADED,
              aDownload.source.url);
          }, SLOW_TIMEOUT_MS));
          return;
        }
        if (!aDownload.contentAnalysisBegun &&
            downloadsView._caDownloadsTimers.has(aDownload)) {
          downloadsView.onDownloadRemoved(aDownload);
          this._showCAResult(
            Ci.nsIContentAnalysisRequest.FILE_DOWNLOADED,
            aDownload.source.url,  /* TODO: Better name */
            aDownload.contentAnalysisResult);
        }
      },

      onDownloadRemoved: aDownload => {
        // Cancels "slow operation" timer for the download, if it exists.
        let timer = downloadsView._caDownloadsTimers.get(aDownload);
        downloadsView._caDownloadsTimers.delete(aDownload);
        if (!timer) {
          return;
        }
        clearTimeout(timer);
      },
    };

    await (await Downloads.getList(Downloads.ALL)).addView(downloadsView);
  },

  /**
   * Show a messagge to the user to indicate that a CA request is taking
   * a long time.
   */
  _showSlowCAMessage(aOperation, aResourceName) {
    // TODO: Better message
    window.alert("The Content Analysis Tool is taking a looooong time to respond...");
  },

  _showCAResult(aOperation, aResourceName, aCAResult) {
    // NB: ACTION_UNSPECIFIED indicates an unexpected error occurred during
    // CA consultation.
    // TODO: Better message
    switch (aCAResult) {
      case Ci.nsIContentAnalysisAcknowledgement.ALLOW:
        // We don't need to show anything
        break;
      case Ci.nsIContentAnalysisAcknowledgement.REPORT_ONLY:
        window.alert("CA responded with REPORT_ONLY");
        break;
      case Ci.nsIContentAnalysisAcknowledgement.WARN:
        window.alert("CA responded with WARN");
        break;
      case Ci.nsIContentAnalysisAcknowledgement.BLOCK:
        window.alert("CA responded with BLOCK.  Transfer denied.");
        break;
      case Ci.nsIContentAnalysisAcknowledgement.ACTION_UNSPECIFIED:
        window.alert("An error occurred in communicating with the CA.  Transfer denied.");
        break;
    }
  },
};

ContentAnalysisViews.initialize();
