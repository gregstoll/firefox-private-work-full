/**
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/.
 */

"use strict";

const EXPORTED_SYMBOLS = [
  "ContentAnalysisViewsService",
  "ContentAnalysisViewsStubService",
];

/**
 * The nsIAboutNewTabService is accessed by the AboutRedirector anytime
 * about:home, about:newtab or about:welcome are requested. The primary
 * job of an nsIAboutNewTabService is to tell the AboutRedirector what
 * resources to actually load for those requests.
 *
 * The nsIAboutNewTabService is not involved when the user has overridden
 * the default about:home or about:newtab pages.
 *
 * There are two implementations of this service - one for the parent
 * process, and one for content processes. Each one has some secondary
 * responsibilties that are process-specific.
 *
 * The need for two implementations is an unfortunate consequence of how
 * document loading and process redirection for about: pages currently
 * works in Gecko. The commonalities between the two implementations has
 * been put into an abstract base class.
 */

const { XPCOMUtils } = ChromeUtils.importESModule(
  "resource://gre/modules/XPCOMUtils.sys.mjs"
);
const { AppConstants } = ChromeUtils.importESModule(
  "resource://gre/modules/AppConstants.sys.mjs"
);

const lazy = {};

XPCOMUtils.defineLazyServiceGetters(lazy, {
  AlertsService: ["@mozilla.org/alerts-service;1", "nsIAlertsService"],
});

/**
 * BEWARE: Do not add variables for holding state in the global scope.
 * Any state variables should be properties of the appropriate class
 * below. This is to avoid confusion where the state is set in one process,
 * but not in another.
 *
 * Constants are fine in the global scope.
 */

class ContentAnalysisViewsService {
  constructor() {
    this.classID = Components.ID("{2fe1d4b3-2849-4faa-a36a-5f0184755d61}");
    this.QueryInterface = ChromeUtils.generateQI([
      "nsIContentAnalysisViews",
    ]);
  }

  showMessage(message) {
    let alert = Cc["@mozilla.org/alert-notification;1"].createInstance(
      Ci.nsIAlertNotification
    );
    let { tag } = message;
    let systemPrincipal = Services.scriptSecurityManager.getSystemPrincipal();
    alert.init(
      tag,
      "", /* image_url */
      "title",
      message,
      true /* aTextClickable */,
      "", /* content.data, */
      null /* aDir */,
      null /* aLang */,
      null /* aData */,
      systemPrincipal,
      null /* aInPrivateBrowsing */,
      false /* content.requireInteraction */
    );

    lazy.AlertsService.showAlert(alert);
  }
}

function ContentAnalysisViewsStubService() {
  return new ContentAnalysisViewsService();
}