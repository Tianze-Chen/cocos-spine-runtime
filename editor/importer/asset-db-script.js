'use strict';

/**
 * Asset-DB script for the spine-runtime extension.
 *
 * The asset-db contribution's `script` field points here. The asset-db worker
 * loads this module (`requireFile`) and dispatches to `methods[name]`, where
 * `name` is the `handler` field of each entry in the `asset-handler`
 * contribution.
 *
 * The `asset-handler` contribution wiring alone is unreliable for project
 * extensions in 3.8.x: the package's enable event can fire before the asset-db
 * worker subscribes its own enable listener, so `assetHandlerManager.register()`
 * is never called for the package. `onPackageEnable` still loads this script
 * and calls `load()` — so we do the registration directly here:
 *
 *   1. Register a minimal `sp.spineData` class (extends Asset) so the worker's
 *      `cc.js` can resolve the asset type for the Assets panel.
 *   2. Register the `spine-skeleton` handler in the worker's handler manager,
 *      reaching it by absolute path through the editor's module loader.
 */

const HANDLER_INFO = {
    name: 'spine-skeleton',
    extnames: ['.json'],
    handler: 'registerSpineSkeletonHandler',
};

// Reach the asset-db worker's handler manager. It is not on a documented public
// API, so probe globals first, then fall back to requiring it by path.
function registerHandler() {
    const ed = globalThis.Editor;
    const appPath = ed && ed.App && ed.App.path;

    const g = globalThis;
    for (const k of ['assetHandlerManager', 'AssetDBManager', 'assetDBManager']) {
        if (g[k] && typeof g[k].register === 'function') {
            g[k].register('spine-runtime', [HANDLER_INFO], false);
            return true;
        }
    }
    if (ed && ed.Module && ed.Module.__protected__ && appPath) {
        // Editor.App.path already points into resources/app.asar.
        const candidates = [
            `${appPath}/builtin/asset-db/dist/worker/manager/asset-handler-manager.js`,
            `${appPath}/modules/builtin/asset-db/dist/worker/manager/asset-handler-manager.js`,
        ];
        for (const p of candidates) {
            try {
                const mod = ed.Module.__protected__.requireFile(p);
                if (mod && mod.assetHandlerManager && typeof mod.assetHandlerManager.register === 'function') {
                    mod.assetHandlerManager.register('spine-runtime', [HANDLER_INFO], false);
                    return true;
                }
            } catch (e) {
                // try the next candidate
            }
        }
    }
    return false;
}

// Register a minimal sp.spineData class so the asset-db worker can resolve the
// asset's type hierarchy (getExtendsFromCCType) without crashing the Assets
// panel. Guarded to the asset-db worker (globalThis.AssetDB exists there):
// the native build also compiles this extension script, but there the real
// SpineData class registers as a project script — a second registration would
// throw "A Class already exists with the same __cid__ : sp.spineData".
function registerAssetClass() {
    const adb = globalThis.AssetDB;
    if (!adb) return false; // not the asset-db worker — skip the minimal class
    const cc = (typeof globalThis.cc !== 'undefined') ? globalThis.cc
        : (adb.AssetDB ? require('cc') : null);
    if (!cc || !cc.js || typeof cc.js.getClassByName !== 'function') return false;
    if (cc.js.getClassByName('sp.spineData')) return true; // already registered
    const _decorator = cc._decorator || (cc.legacyCC && cc.legacyCC._decorator);
    if (!_decorator || typeof _decorator.ccclass !== 'function') return false;
    class SpineDataAssetDbPlaceholder extends cc.Asset {}
    Object.defineProperty(SpineDataAssetDbPlaceholder, '__spineRuntimeAssetDbPlaceholder', {
        value: true,
    });
    _decorator.ccclass('sp.spineData')(SpineDataAssetDbPlaceholder);
    return true;
}

exports.methods = {
    /**
     * Returns the spine-skeleton handler (sp.spineData import).
     */
    async registerSpineSkeletonHandler() {
        return require('./spine-skeleton.js');
    },
};

// onPackageEnable calls mod.load() when present. Register the handler and asset
// class directly to work around the enable-event timing gap described above.
exports.load = function load() {
    try {
        registerAssetClass();
        registerHandler();
    } catch (e) {
        console.warn(`[spine-runtime] asset-db-script.load() failed: ${e}`);
    }
};
