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
 *   3. Force our handler to the front of the dispatch order for `.json`/`.skel`
 *      (see prioritizeHandler below) — the engine's own built-in Spine importer
 *      (`spine-data`, engine-extensions) claims the exact same two extensions,
 *      and asset-db resolves ties by trying the LAST-registered handler first
 *      (asset-handler-manager.ts `_findImporterInRegisterInfo`, reverse loop).
 *      Registration order between a project extension and engine-extensions is
 *      not guaranteed, so relying on "we probably load later" is not reliable —
 *      observed in practice: with no reordering, `spine-data` won on some
 *      editor sessions despite this plugin registering `.skel` correctly.
 */

const HANDLER_INFO = {
    name: 'spine-skeleton',
    extnames: ['.json', '.skel'],
    handler: 'registerSpineSkeletonHandler',
};

// Moves this handler's entries to the end of extname2registerInfo[ext] for
// every extension it claims, so asset-db's reverse-priority resolver tries it
// before any other handler already registered for the same extname (including
// the engine's built-in spine-data importer). Safe to call repeatedly/on a
// list this handler isn't in yet — it's a no-op until `register()` has run.
function prioritizeHandler(mgr) {
    if (!mgr || !mgr.extname2registerInfo) return;
    for (const ext of HANDLER_INFO.extnames) {
        const list = mgr.extname2registerInfo[ext];
        if (!Array.isArray(list) || list.length === 0) continue;
        const ours = list.filter((info) => info && info.name === HANDLER_INFO.name);
        if (ours.length === 0) continue;
        const rest = list.filter((info) => !info || info.name !== HANDLER_INFO.name);
        mgr.extname2registerInfo[ext] = rest.concat(ours);
    }
}

// Wraps mgr.register so that ANY future registration (ours or anyone else's,
// e.g. engine-extensions registering its spine-data handler after we do) is
// immediately followed by re-asserting our priority. Idempotent: guarded so a
// second registerHandler() call (package re-enable) does not double-wrap.
function ensurePriorityMaintained(mgr) {
    prioritizeHandler(mgr);
    if (!mgr || mgr.__spineRuntimePatchedRegister) return;
    const originalRegister = mgr.register.bind(mgr);
    mgr.register = function patchedRegister(...args) {
        const result = originalRegister(...args);
        prioritizeHandler(mgr);
        return result;
    };
    mgr.__spineRuntimePatchedRegister = true;
}

// Reach the asset-db worker's handler manager. It is not on a documented public
// API, so probe globals first, then fall back to requiring it by path.
function findAssetHandlerManager() {
    const ed = globalThis.Editor;
    const g = globalThis;
    for (const k of ['assetHandlerManager', 'AssetDBManager', 'assetDBManager']) {
        if (g[k] && typeof g[k].register === 'function') return g[k];
    }
    const appPath = ed && ed.App && ed.App.path;
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
                    return mod.assetHandlerManager;
                }
            } catch (e) {
                // try the next candidate
            }
        }
    }
    return null;
}

function registerHandler() {
    const mgr = findAssetHandlerManager();
    if (!mgr) {
        console.warn('[spine-runtime] registerHandler: asset handler manager not reachable');
        return false;
    }
    mgr.register('spine-runtime', [HANDLER_INFO], false);
    ensurePriorityMaintained(mgr);
    console.debug(`[spine-runtime] registerHandler: registered + prioritized for ${HANDLER_INFO.extnames.join(', ')}`
        + ` (order now: ${HANDLER_INFO.extnames.map((e) => `${e}=[${(mgr.extname2registerInfo[e] || []).map((i) => i.name).join(',')}]`).join(' ')})`);
    return true;
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
