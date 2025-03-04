/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at <http://mozilla.org/MPL/2.0/>. */

import { showMenu } from "../../context-menu/menu";

import {
  getContext,
  isSourceOverridden,
  isSourceMapIgnoreListEnabled,
  isSourceOnSourceMapIgnoreList,
  getProjectDirectoryRoot,
  getSourcesTreeSources,
  getBlackBoxRanges,
} from "../../selectors";

import { setOverrideSource, removeOverrideSource } from "../sources";
import { loadSourceText } from "../sources/loadSourceText";
import { toggleBlackBox, blackBoxSources } from "../sources/blackbox";
import { setProjectDirectoryRoot, clearProjectDirectoryRoot } from "../ui";

import { shouldBlackbox } from "../../utils/source";
import { copyToTheClipboard } from "../../utils/clipboard";
import { saveAsLocalFile } from "../../utils/utils";

/**
 * Show the context menu of SourceTreeItem.
 *
 * @param {object} event
 *        The context-menu DOM event.
 * @param {object} item
 *        Source Tree Item object.
 */
export function showSourceTreeItemContextMenu(
  event,
  item,
  depth,
  setExpanded,
  itemName
) {
  return async ({ dispatch, getState }) => {
    const copySourceUri2Label = L10N.getStr("copySourceUri2");
    const copySourceUri2Key = L10N.getStr("copySourceUri2.accesskey");
    const setDirectoryRootLabel = L10N.getStr("setDirectoryRoot.label");
    const setDirectoryRootKey = L10N.getStr("setDirectoryRoot.accesskey");
    const removeDirectoryRootLabel = L10N.getStr("removeDirectoryRoot.label");

    const menuOptions = [];

    const state = getState();
    const cx = getContext(state);
    const isOverridden = isSourceOverridden(state, item.source);
    const isSourceOnIgnoreList =
      isSourceMapIgnoreListEnabled(state) &&
      isSourceOnSourceMapIgnoreList(state, item.source);
    const projectRoot = getProjectDirectoryRoot(state);

    if (item.type == "source") {
      const { source } = item;
      const copySourceUri2 = {
        id: "node-menu-copy-source",
        label: copySourceUri2Label,
        accesskey: copySourceUri2Key,
        disabled: false,
        click: () => copyToTheClipboard(source.url),
      };

      const ignoreStr = item.isBlackBoxed ? "unignore" : "ignore";
      const blackBoxMenuItem = {
        id: "node-menu-blackbox",
        label: L10N.getStr(`ignoreContextItem.${ignoreStr}`),
        accesskey: L10N.getStr(`ignoreContextItem.${ignoreStr}.accesskey`),
        disabled: isSourceOnIgnoreList || !shouldBlackbox(source),
        click: () => dispatch(toggleBlackBox(cx, source)),
      };
      const downloadFileItem = {
        id: "node-menu-download-file",
        label: L10N.getStr("downloadFile.label"),
        accesskey: L10N.getStr("downloadFile.accesskey"),
        disabled: false,
        click: () => saveLocalFile(cx, dispatch, source),
      };

      const overrideStr = !isOverridden ? "override" : "removeOverride";
      const overridesItem = {
        id: "node-menu-overrides",
        label: L10N.getStr(`overridesContextItem.${overrideStr}`),
        accesskey: L10N.getStr(`overridesContextItem.${overrideStr}.accesskey`),
        disabled: !!source.isHTML,
        click: () => handleLocalOverride(cx, dispatch, source, isOverridden),
      };

      menuOptions.push(
        copySourceUri2,
        blackBoxMenuItem,
        downloadFileItem,
        overridesItem
      );
    }

    // All other types other than source are folder-like
    if (item.type != "source") {
      addCollapseExpandAllOptions(menuOptions, item, setExpanded);

      if (projectRoot == item.uniquePath) {
        menuOptions.push({
          id: "node-remove-directory-root",
          label: removeDirectoryRootLabel,
          disabled: false,
          click: () => dispatch(clearProjectDirectoryRoot(cx)),
        });
      } else {
        menuOptions.push({
          id: "node-set-directory-root",
          label: setDirectoryRootLabel,
          accesskey: setDirectoryRootKey,
          disabled: false,
          click: () =>
            dispatch(setProjectDirectoryRoot(cx, item.uniquePath, itemName)),
        });
      }

      addBlackboxAllOption(cx, dispatch, state, menuOptions, item, depth);
    }

    showMenu(event, menuOptions);
  };
}

async function saveLocalFile(cx, dispatch, source) {
  if (!source) {
    return null;
  }

  const data = await dispatch(loadSourceText(cx, source));
  if (!data) {
    return null;
  }
  return saveAsLocalFile(data.value, source.displayURL.filename);
}

async function handleLocalOverride(cx, dispatch, source, isOverridden) {
  if (!isOverridden) {
    const localPath = await saveLocalFile(cx, dispatch, source);
    if (localPath) {
      dispatch(setOverrideSource(cx, source, localPath));
    }
  } else {
    dispatch(removeOverrideSource(cx, source));
  }
}

function addBlackboxAllOption(cx, dispatch, state, menuOptions, item, depth) {
  const {
    sourcesInside,
    sourcesOutside,
    allInsideBlackBoxed,
    allOutsideBlackBoxed,
  } = getBlackBoxSourcesGroups(state, item);
  const projectRoot = getProjectDirectoryRoot(state);

  let blackBoxInsideMenuItemLabel;
  let blackBoxOutsideMenuItemLabel;
  if (depth === 0 || (depth === 1 && projectRoot === "")) {
    blackBoxInsideMenuItemLabel = allInsideBlackBoxed
      ? L10N.getStr("unignoreAllInGroup.label")
      : L10N.getStr("ignoreAllInGroup.label");
    if (sourcesOutside.length) {
      blackBoxOutsideMenuItemLabel = allOutsideBlackBoxed
        ? L10N.getStr("unignoreAllOutsideGroup.label")
        : L10N.getStr("ignoreAllOutsideGroup.label");
    }
  } else {
    blackBoxInsideMenuItemLabel = allInsideBlackBoxed
      ? L10N.getStr("unignoreAllInDir.label")
      : L10N.getStr("ignoreAllInDir.label");
    if (sourcesOutside.length) {
      blackBoxOutsideMenuItemLabel = allOutsideBlackBoxed
        ? L10N.getStr("unignoreAllOutsideDir.label")
        : L10N.getStr("ignoreAllOutsideDir.label");
    }
  }

  const blackBoxInsideMenuItem = {
    id: allInsideBlackBoxed
      ? "node-unblackbox-all-inside"
      : "node-blackbox-all-inside",
    label: blackBoxInsideMenuItemLabel,
    disabled: false,
    click: () =>
      dispatch(blackBoxSources(cx, sourcesInside, !allInsideBlackBoxed)),
  };

  if (sourcesOutside.length) {
    menuOptions.push({
      id: "node-blackbox-all",
      label: L10N.getStr("ignoreAll.label"),
      submenu: [
        blackBoxInsideMenuItem,
        {
          id: allOutsideBlackBoxed
            ? "node-unblackbox-all-outside"
            : "node-blackbox-all-outside",
          label: blackBoxOutsideMenuItemLabel,
          disabled: false,
          click: () =>
            dispatch(
              blackBoxSources(cx, sourcesOutside, !allOutsideBlackBoxed)
            ),
        },
      ],
    });
  } else {
    menuOptions.push(blackBoxInsideMenuItem);
  }
}

function addCollapseExpandAllOptions(menuOptions, item, setExpanded) {
  menuOptions.push({
    id: "node-menu-collapse-all",
    label: L10N.getStr("collapseAll.label"),
    disabled: false,
    click: () => setExpanded(item, false, true),
  });

  menuOptions.push({
    id: "node-menu-expand-all",
    label: L10N.getStr("expandAll.label"),
    disabled: false,
    click: () => setExpanded(item, true, true),
  });
}

/**
 * Computes 4 lists:
 *  - `sourcesInside`: the list of all Source Items that are
 *    children of the current item (can be thread/group/directory).
 *    This include any nested level of children.
 *  - `sourcesOutside`: all other Source Items.
 *    i.e. all sources that are in any other folder of any group/thread.
 *  - `allInsideBlackBoxed`, all sources of `sourcesInside` which are currently
 *    blackboxed.
 *  - `allOutsideBlackBoxed`, all sources of `sourcesOutside` which are currently
 *    blackboxed.
 */
function getBlackBoxSourcesGroups(state, item) {
  const allSources = [];
  function collectAllSources(list, _item) {
    if (_item.children) {
      _item.children.forEach(i => collectAllSources(list, i));
    }
    if (_item.type == "source") {
      list.push(_item.source);
    }
  }

  const rootItems = getSourcesTreeSources(state);
  const blackBoxRanges = getBlackBoxRanges(state);

  for (const rootItem of rootItems) {
    collectAllSources(allSources, rootItem);
  }

  const sourcesInside = [];
  collectAllSources(sourcesInside, item);

  const sourcesOutside = allSources.filter(
    source => !sourcesInside.includes(source)
  );
  const allInsideBlackBoxed = sourcesInside.every(
    source => blackBoxRanges[source.url]
  );
  const allOutsideBlackBoxed = sourcesOutside.every(
    source => blackBoxRanges[source.url]
  );

  return {
    sourcesInside,
    sourcesOutside,
    allInsideBlackBoxed,
    allOutsideBlackBoxed,
  };
}
