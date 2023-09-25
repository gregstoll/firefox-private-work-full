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

var { XPCOMUtils } = ChromeUtils.importESModule(
  "resource://gre/modules/XPCOMUtils.sys.mjs"
);

XPCOMUtils.defineLazyServiceGetter(
  this,
  "gContentAnalysis",
  "@mozilla.org/contentanalysis;1",
  Ci.nsIContentAnalysis
);

var ContentAnalysisViews = {
  _SHOW_NOTIFICATIONS: true,

  _SHOW_DIALOGS: false,

  _SLOW_DLP_NOTIFICATION_TIMEOUT_MS: 5 * 1000, // 5 sec

  _RESULT_NOTIFICATION_TIMEOUT_MS: 5 * 60 * 1000, // 5 min

  _RESULT_NOTIFICATION_FAST_TIMEOUT_MS: 60 * 1000, // 1 min

  _CA_SILENCE_NOTIFICATIONS: "browser.contentanalysis.silent_notifications",

  haveCleanedUp: false,

  dlpBusyViews: new WeakMap(),

  requestTokenToBrowserAndResourceName: new Map(),

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

    ChromeUtils.defineLazyGetter(this, "l10n", function () {
      return new Localization(
        ["toolkit/contentanalysis/contentanalysis.ftl"],
        true
      );
    });
  },

  // TODO - I guess we're stuck with having to do this because these are defined
  // in the SDK? But yuck.
  responseResultToAcknowledgementResult(responseResult) {
    switch (responseResult) {
      case Ci.nsIContentAnalysisResponse.REPORT_ONLY:
        return Ci.nsIContentAnalysisAcknowledgement.REPORT_ONLY;
      case Ci.nsIContentAnalysisResponse.WARN:
        return Ci.nsIContentAnalysisAcknowledgement.WARN;
      case Ci.nsIContentAnalysisResponse.BLOCK:
        return Ci.nsIContentAnalysisAcknowledgement.BLOCK;
      case Ci.nsIContentAnalysisResponse.ALLOW:
        return Ci.nsIContentAnalysisAcknowledgement.ALLOW;
      case Ci.nsIContentAnalysisResponse.ACTION_UNSPECIFIED:
        return Ci.nsIContentAnalysisAcknowledgement.ACTION_UNSPECIFIED;
      default:
        // TODO - assert or warn here?
        return Ci.nsIContentAnalysisAcknowledgement.ACTION_UNSPECIFIED;
    }
  },

  /**
   * Register UI for file download CA events.
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
            if (!ContentAnalysisViews.haveCleanedUp) {
              ContentAnalysisViews.haveCleanedUp = true;
              let allDownloads = await (
                await Downloads.getList(Downloads.ALL)
              ).getAll();
              for (var download of allDownloads) {
                this._clearDownloadViews(download);
              }
              Services.obs.removeObserver(
                downloadsView,
                "quit-application-requested"
              );
              Services.obs.removeObserver(downloadsView, "dlp-request-made");
              Services.obs.removeObserver(downloadsView, "dlp-response");
            }
            break;
          case "dlp-request-made":
            {
              const operation =
                aSubj?.QueryInterface(Ci.nsIContentAnalysisRequest)
                  ?.analysisType ??
                Ci.nsIContentAnalysisRequest.ANALYSIS_CONNECTOR_UNSPECIFIED;
              // For operations that block browser interaction, show the "slow content analysis"
              // dialog faster
              let slowTimeoutMs = this._shouldShowBlockingNotification(
                operation
              )
                ? 250
                : 3000;
              let browser = window.gBrowser.selectedBrowser;

              // Start timer that, when it expires,
              // presents a "slow CA check" message.
              if (!this.dlpBusyViews.has(browser)) {
                let request = aSubj.QueryInterface(
                  Ci.nsIContentAnalysisRequest
                );
                let resourceName = await this._getResourceNameFromRequest(
                  request
                );
                this.requestTokenToBrowserAndResourceName.set(
                  request.requestToken,
                  { browser, resourceName }
                );
                let browsingContext = browser.browsingContext;
                this.dlpBusyViews.set(browser, {
                  timer: setTimeout(() => {
                    this.dlpBusyViews.set(browser, {
                      notification: this._showSlowCAMessage(
                        operation,
                        request,
                        resourceName,
                        browsingContext
                      ),
                    });
                  }, slowTimeoutMs),
                });
              }
            }
            break;
          case "dlp-response":
            let request = aSubj?.QueryInterface(Ci.nsIContentAnalysisResponse);
            // Cancels timer or slow message UI,
            // if present, and possibly presents the CA verdict.
            if (
              !request ||
              !this.requestTokenToBrowserAndResourceName.has(
                request.requestToken
              )
            ) {
              // Maybe this was cancelled or something.
              return;
            }
            let browserAndResourceName =
              this.requestTokenToBrowserAndResourceName.get(
                request.requestToken
              );
            this.requestTokenToBrowserAndResourceName.delete(
              request.requestToken
            );
            if (this.dlpBusyViews.has(browserAndResourceName.browser)) {
              this._disconnectFromView(
                this.dlpBusyViews.get(browserAndResourceName.browser)
              );
              this.dlpBusyViews.delete(browserAndResourceName.browser);
            }
            const responseResult =
              request?.action ??
              Ci.nsIContentAnalysisResponse.ACTION_UNSPECIFIED;
            this.resultView = {
              notification: this._showCAResult(
                Ci.nsIContentAnalysisRequest
                  .FILE_DOWNLOADED /* TODO fix this type */,
                browserAndResourceName.resourceName,
                this.responseResultToAcknowledgementResult(responseResult)
              ),
            };
            break;
        }
      },

      onDownloadAdded: aDownload => {
        // ignored
      },

      onDownloadChanged: aDownload => {
        if (!downloadsView._isCorrectWindow(aDownload)) {
          return;
        }

        // On contentAnalysis.RUNNING, start timer that, when it expires,
        // presents a "slow CA check" message.
        if (
          aDownload.contentAnalysis.state ===
            aDownload.contentAnalysis.RUNNING &&
          !aDownload.contentAnalysis.hasOwnProperty("busyView")
        ) {
          let browsingContext = window.gBrowser.selectedBrowser.browsingContext;
          aDownload.contentAnalysis.busyView = {
            timer: setTimeout(() => {
              aDownload.contentAnalysis.busyView = {
                notification: this._showSlowCAMessage(
                  Ci.nsIContentAnalysisRequest.FILE_DOWNLOADED,
                  null,
                  aDownload.source.url,
                  browsingContext
                ),
              };
            }, this._SLOW_DLP_NOTIFICATION_TIMEOUT_MS),
          };
          return;
        }

        // On ContentAnalysis.FINISHED, cancels timer or slow message UI,
        // if present, and possibly presents the CA verdict.
        if (
          aDownload.contentAnalysis.state ===
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

        downloadsView._clearDownloadViews(aDownload);
      },

      _clearDownloadViews(aDownload) {
        // Cancels "slow operation" timer for the download, or any
        // result notifications, if they exist.
        if ("busyView" in aDownload.contentAnalysis) {
          this._disconnectFromView(aDownload.contentAnalysis.busyView);
          delete aDownload.contentAnalysis.busyView;
        }

        if ("resultView" in aDownload.contentAnalysis) {
          this._disconnectFromView(aDownload.contentAnalysis.resultView);
          delete aDownload.contentAnalysis.resultView;
        }
      },
    };

    Services.obs.addObserver(downloadsView, "quit-application-requested");
    Services.obs.addObserver(downloadsView, "dlp-request-made");
    Services.obs.addObserver(downloadsView, "dlp-response");
    let downloadList = await Downloads.getList(Downloads.ALL);
    await downloadList.addView(downloadsView);
    window.addEventListener("unload", async () => {
      if (!ContentAnalysisViews.haveCleanedUp) {
        ContentAnalysisViews.haveCleanedUp = true;
        Services.obs.removeObserver(
          downloadsView,
          "quit-application-requested"
        );
        Services.obs.removeObserver(downloadsView, "dlp-request-made");
        Services.obs.removeObserver(downloadsView, "dlp-response");
        await downloadList.removeView(downloadsView);
      }
    });
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
    if (!caView) {
      return;
    }
    if ("timer" in caView) {
      clearTimeout(caView.timer);
    } else if ("notification" in caView) {
      if ("close" in caView.notification) {
        // native notification
        caView.notification.close();
      } else if ("dialogBrowsingContext" in caView.notification) {
        // in-browser notification
        let browser =
          caView.notification.dialogBrowsingContext.top.embedderElement;
        let win = browser.ownerGlobal;
        let dialogBox = win.gBrowser.getTabDialogBox(browser);
        // Don't close any content-modal dialogs, because we could be doing
        // content analysis on something like a prompt() call.
        dialogBox.getTabDialogManager().abortDialogs();
      } else {
        console.error(
          "Unexpected content analysis notification - can't close it!"
        );
      }
    }
  },

  _showMessage(aMessage, aTimeout = 0) {
    if (this._SHOW_DIALOGS) {
      window.alert(aMessage);
    }

    if (this._SHOW_NOTIFICATIONS) {
      const notification = new Notification("Content Analysis", {
        body: aMessage,
        silent: Services.prefs.getBoolPref(this._CA_SILENCE_NOTIFICATIONS),
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

  _shouldShowBlockingNotification(aOperation) {
    return !(
      aOperation == Ci.nsIContentAnalysisRequest.FILE_DOWNLOADED ||
      aOperation == Ci.nsIContentAnalysisRequest.PRINT
    );
  },

  async _getResourceNameFromRequest(aRequest) {
    if (
      aRequest.operationTypeForDisplay ==
      Ci.nsIContentAnalysisRequest.OPERATION_CUSTOMDISPLAYSTRING
    ) {
      return aRequest.operationDisplayString;
    }
    let key;
    switch (aRequest.operationTypeForDisplay) {
      case Ci.nsIContentAnalysisRequest.OPERATION_CLIPBOARD:
        key = "contentanalysis-operationtype-clipboard";
        break;
      case Ci.nsIContentAnalysisRequest.OPERATION_DROPPEDTEXT:
        key = "contentanalysis-operationtype-droppedtext";
        break;
    }
    if (!key) {
      console.error(
        "Unknown operationTypeForDisplay: " + aRequest.operationTypeForDisplay
      );
      return "";
    }
    return this.l10n.formatValue(key);
  },

  /**
   * Show a message to the user to indicate that a CA request is taking
   * a long time.
   */
  _showSlowCAMessage(aOperation, aRequest, aResourceName, aBrowsingContext) {
    // TODO: Better message
    if (!this._shouldShowBlockingNotification(aOperation)) {
      return this._showMessage(
        "The Content Analysis Tool is taking a looooong time to respond for resource " +
          aResourceName
      );
    }

    if (!aRequest) {
      console.error(
        "Showing in-browser Content Analysis notification but no request was passed"
      );
    }

    let promise = Services.prompt.asyncConfirmEx(
      aBrowsingContext,
      Ci.nsIPromptService.MODAL_TYPE_TAB,
      "Content Analysis in progress",
      "Content Analysis is analyzing: " + aResourceName,
      Ci.nsIPromptService.BUTTON_POS_0 *
        Ci.nsIPromptService.BUTTON_TITLE_CANCEL +
        Ci.nsIPromptService.SHOW_SPINNER,
      null,
      null,
      null,
      null,
      false
    );
    let browser = window.gBrowser.selectedBrowser;
    promise
      .then(() => {
        gContentAnalysis.CancelContentAnalysisRequest(aRequest.requestToken);
        if (this.dlpBusyViews.has(browser)) {
          this._disconnectFromView(this.dlpBusyViews.get(browser));
          this.dlpBusyViews.delete(browser);
          this.requestTokenToBrowserAndResourceName.delete(
            aRequest.requestToken
          );
        }
      })
      .catch(() => {
        // needed to avoid crashing the tab when we programmatically close the dialog
      });
    return {
      requestToken: aRequest.requestToken,
      dialogBrowsingContext: aBrowsingContext,
    };
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
        message = "CA responded with REPORT_ONLY for resource " + aResourceName;
        timeoutMs = this._RESULT_NOTIFICATION_FAST_TIMEOUT_MS;
        break;
      case Ci.nsIContentAnalysisAcknowledgement.WARN:
        message = "CA responded with WARN for resource " + aResourceName;
        timeoutMs = this._RESULT_NOTIFICATION_TIMEOUT_MS;
        break;
      case Ci.nsIContentAnalysisAcknowledgement.BLOCK:
        message =
          "CA responded with BLOCK. Transfer denied for resource " +
          aResourceName;
        timeoutMs = this._RESULT_NOTIFICATION_TIMEOUT_MS;
        break;
      case Ci.nsIContentAnalysisAcknowledgement.ACTION_UNSPECIFIED:
        message =
          "An error occurred in communicating with the CA. Transfer denied for resource " +
          aResourceName;
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
