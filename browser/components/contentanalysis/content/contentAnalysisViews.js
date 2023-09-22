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

  _SLOW_DLP_NOTIFICATION_TIMEOUT_MS: 30 * 1000, // 30 sec

  _RESULT_NOTIFICATION_TIMEOUT_MS: 5 * 60 * 1000, // 5 min

  _RESULT_NOTIFICATION_FAST_TIMEOUT_MS: 60 * 1000, // 1 min

  /**
   * Registers for various messages/events that will indicate the potential
   * need for communicating something to the user.
   */
  initialize() {
    // Register only the first instance to get shutdown events.  We use this to
    // clear any open CA notifications, so it only needs to be added once.
    //    let thisPrototype = Object.getPrototypeOf(this);
    //    if (!('isObserving' in thisPrototype)) {
    //      Services.obs.addObserver(this, 'quit-application-requested');
    //      thisPrototype.isObserving = true;
    //    }

    this.initializeDownloadCA();
  },

  /**
   * Register for file download CA events.
   */
  async initializeDownloadCA() {
    let downloadsView = {
      _isCorrectWindow: aDownload => {
        // The correct window is the one associated with the download's
        // browsing context, or is the topmost window when the download is not
        // associated with one.
        return (
          (!aDownload.source.browsingContextId &&
            window === BrowserWindowTracker.getTopWindow()) ||
          (aDownload.source.browsingContextId &&
            BrowsingContext.get(aDownload.source.browsingContextId) &&
            BrowsingContext.get(aDownload.source.browsingContextId)
              .topChromeWindow === window)
        );
      },

      // nsIObserver
      observe: async (aSubj, aTopic, aData) => {
        switch (aTopic) {
          case "quit-application-requested":
            // console.log(`observe ${aTopic}`);
            let allDownloads = await (
              await Downloads.getList(Downloads.ALL)
            ).getAll();
            for (var download of allDownloads) {
              // console.log(`disconnecting ${download} has resultViewNotification as ${download.contentAnalysis.resultView.notification}`);
              this._clearDownloadViews(download);
            }
            Services.obs.removeObserver(
              downloadsView,
              "quit-application-requested"
            );
        }
      },

      onDownloadAdded: aDownload => {
        // ignored
      },

      onDownloadChanged: aDownload => {
        if (!downloadsView._isCorrectWindow(aDownload)) {
          return;
        }

        const SLOW_TIMEOUT_MS = 3000; // 3 sec

        // On contentAnalysis.RUNNING, start timer that, when it expires,
        // presents a "slow CA check" message.
        if (
          aDownload.contentAnalysis.state ==
            aDownload.contentAnalysis.RUNNING &&
          !aDownload.contentAnalysis.hasOwnProperty("busyView")
        ) {
          aDownload.contentAnalysis.busyView = {
            timer: setTimeout(() => {
              aDownload.contentAnalysis.busyView = {
                notification: this._showSlowCAMessage(
                  Ci.nsIContentAnalysisRequest.FILE_DOWNLOADED,
                  aDownload.source.url
                ),
              };
            }, SLOW_TIMEOUT_MS),
          };
          return;
        }

        // On ContentAnalysis.FINISHED, cancels timer or slow message UI,
        // if present, and possibly presents the CA verdict.
        if (
          aDownload.contentAnalysis.state ==
            aDownload.contentAnalysis.FINISHED &&
          !aDownload.contentAnalysis.hasOwnProperty("resultView")
        ) {
          this._disconnectFromView(aDownload.contentAnalysis.busyView);
          aDownload.contentAnalysis.resultView = {
            notification: this._showCAResult(
              Ci.nsIContentAnalysisRequest.FILE_DOWNLOADED,
              aDownload.source.url /* TODO: Better name */,
              aDownload.contentAnalysis.result
            ),
          };
        }
      },

      onDownloadRemoved: aDownload => {
        if (!downloadsView._isCorrectWindow(aDownload)) {
          return;
        }

        this._clearDownloadViews(aDownload);
      },
    };

    Services.obs.addObserver(downloadsView, "quit-application-requested");
    await (await Downloads.getList(Downloads.ALL)).addView(downloadsView);
  },

  _clearDownloadViews(aDownload) {
    if ("contentAnalysis.busyView" in aDownload) {
      this._disconnectFromView(aDownload.contentAnalysis.busyView);
    }

    if ("contentAnalysis.resultView" in aDownload) {
      this._disconnectFromView(aDownload.contentAnalysis.resultView);
    }
  },

  _disconnectFromView(caView) {
    // Cancels "slow operation" timer for the download, or any
    // notifications for it, if it exists.
    if (!caView) {
      return;
    }
    if ("timer" in caView) {
      // console.log('removing TIMER');
      clearTimeout(caView.timer);
    } else if ("notification" in caView) {
      // console.log('removing NOTIFICATION');
      caView.notification.close();
    }
  },

  _showMessage(aMessage, aTimeout = 0) {
    if (this._SHOW_DIALOGS) {
      window.alert(aMessage);
    }

    if (this._SHOW_NOTIFICATIONS) {
      const notification = new Notification("Content Analysis", {
        body: aMessage,
      });

      //      notification.addEventListener('click', () => { notification.close(); });
      if (aTimeout != 0) {
        setTimeout(() => {
          notification.close();
        }, aTimeout);
      }
      return notification;
    }

    return null;
  },

  /**
   * Show a message to the user to indicate that a CA request is taking
   * a long time.
   */
  _showSlowCAMessage(aOperation, aResourceName) {
    // TODO: Better message
    return this._showMessage(
      "The Content Analysis Tool is taking a looooong time to respond..."
    );
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
        timeoutMs = this._RESULT_NOTIFICATION_FAST_TIMEOUT_MS;
        break;
      case Ci.nsIContentAnalysisAcknowledgement.WARN:
        message = "CA responded with WARN";
        timeoutMs = this._RESULT_NOTIFICATION_TIMEOUT_MS;
        break;
      case Ci.nsIContentAnalysisAcknowledgement.BLOCK:
        message = "CA responded with BLOCK.  Transfer denied.";
        timeoutMs = this._RESULT_NOTIFICATION_TIMEOUT_MS;
        break;
      case Ci.nsIContentAnalysisAcknowledgement.ACTION_UNSPECIFIED:
        message =
          "An error occurred in communicating with the CA.  Transfer denied.";
        timeoutMs = this._RESULT_NOTIFICATION_TIMEOUT_MS;
        break;
    }

    if (message) {
      return this._showMessage(message, timeoutMs);
    }

    return null;
  },
};

ContentAnalysisViews.initialize();
