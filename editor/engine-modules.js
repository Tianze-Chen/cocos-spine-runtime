'use strict';

/**
 * Engine feature requirements of the spine-runtime extension.
 *
 * Two engine features that the project's Feature Cropping panel
 * (项目设置 → 功能裁剪) is free to crop away are hard requirements here:
 *
 *   - `2d`          -> cc.UIMesh, the render component sp.spine extends
 *                      (engine: cocos/2d/components/ui-mesh.ts);
 *   - `webassembly` -> cc.wasm, the cross-platform WebAssembly loader the
 *                      runtime instantiates spine-runtime.wasm through
 *                      (engine: exports/webassembly.ts — custom engine only).
 *
 * The editor's own engine is always a full build, so a missing tick is
 * invisible while authoring and only breaks the packaged output. That is why
 * the state is repaired once when the extension loads and re-checked before
 * every build.
 *
 * The panel keeps two pieces of state per config and both have to agree:
 *
 *     modules.configs.<cfg>.cache.<feature> = { _value: boolean }  // checkbox
 *     modules.configs.<cfg>.includeModules  = [ <feature>, ... ]   // flattened
 *
 * `includeModules` holds the flattened result (UI group names such as `spine`
 * or `physics-2d` live only in `cache`), and it is what the builder reads.
 * Writing one without the other gets reverted the next time the panel saves.
 */

const fs = require('fs');
const path = require('path');

const REQUIRED_MODULES = ['2d', 'webassembly'];

const PROFILE_PACKAGE = 'engine';
const PROFILE_KEY = 'modules.configs';

/** `<project>/settings/v2/packages/engine.json` — the fallback when Editor.Profile is unavailable. */
function settingsFile () {
    return path.join(Editor.Project.path, 'settings', 'v2', 'packages', PROFILE_PACKAGE + '.json');
}

function readSettingsFile () {
    const file = settingsFile();
    if (!fs.existsSync(file)) return null;
    return JSON.parse(fs.readFileSync(file, 'utf8'));
}

/**
 * Read every module config of the project.
 *
 * Editor.Profile is preferred: the panel keeps an in-memory copy, so going
 * through the profile API avoids fighting it over the file on disk.
 */
async function readConfigs () {
    try {
        const configs = await Editor.Profile.getProject(PROFILE_PACKAGE, PROFILE_KEY);
        if (configs && typeof configs === 'object') return configs;
    } catch (e) {
        // fall through to the file
    }
    const data = readSettingsFile();
    const configs = data && data.modules && data.modules.configs;
    return (configs && typeof configs === 'object') ? configs : null;
}

async function writeConfigs (configs) {
    try {
        await Editor.Profile.setProject(PROFILE_PACKAGE, PROFILE_KEY, configs);
        return 'profile';
    } catch (e) {
        // Deep profile keys are not guaranteed across editor versions; the file
        // itself is the source of truth the builder reads, so patch it directly.
        const file = settingsFile();
        const data = readSettingsFile();
        if (!data || !data.modules) throw e;
        data.modules.configs = configs;
        fs.writeFileSync(file, JSON.stringify(data, null, 2) + '\n', 'utf8');
        return 'file';
    }
}

/** Features this extension needs that `config` does not currently enable. */
function missingModules (config) {
    const cache = (config && config.cache) || {};
    const included = new Set(Array.isArray(config && config.includeModules) ? config.includeModules : []);
    return REQUIRED_MODULES.filter((name) => {
        const checked = !!(cache[name] && cache[name]._value);
        return !checked || !included.has(name);
    });
}

/** Tick `missing` in both the checkbox cache and the flattened module list. */
function patchConfig (config, missing) {
    config.cache = config.cache || {};
    const includeModules = Array.isArray(config.includeModules) ? config.includeModules.slice() : [];
    for (const name of missing) {
        const entry = config.cache[name] || {};
        entry._value = true;
        config.cache[name] = entry;
        if (!includeModules.includes(name)) includeModules.push(name);
    }
    // The panel writes this list sorted; keep it that way to avoid diff noise.
    config.includeModules = includeModules.sort();
    return config;
}

/** Config key the build and the panel treat as the active one. */
async function globalConfigKey () {
    try {
        const key = await Editor.Profile.getProject(PROFILE_PACKAGE, 'modules.globalConfigKey');
        if (key) return key;
    } catch (e) {
        // fall through to the file
    }
    const data = readSettingsFile();
    return (data && data.modules && data.modules.globalConfigKey) || 'defaultConfig';
}

/**
 * Feature names the currently selected engine actually declares.
 *
 * Returns null when the engine's `cc.config.json` cannot be located, which is
 * the signal to skip the check rather than guess: adding a feature the engine
 * does not have would be worse than leaving the config alone.
 */
async function engineFeatures () {
    let info = null;
    try {
        info = await Editor.Message.request('engine', 'query-engine-info');
    } catch (e) {
        return null;
    }
    const roots = [];
    for (const key of ['typescript', 'javascript', 'engine']) {
        if (info && info[key] && info[key].path) roots.push(info[key].path);
    }
    if (info && info.path) roots.push(info.path);
    // `native.path` is `<engine>/native` for a source engine checkout.
    if (info && info.native && info.native.path) roots.push(path.dirname(info.native.path));

    for (const root of roots) {
        const file = path.join(root, 'cc.config.json');
        try {
            if (!fs.existsSync(file)) continue;
            const config = JSON.parse(fs.readFileSync(file, 'utf8'));
            if (config && config.features) return new Set(Object.keys(config.features));
        } catch (e) {
            // try the next candidate
        }
    }
    return null;
}

module.exports = {
    REQUIRED_MODULES,
    settingsFile,
    readConfigs,
    writeConfigs,
    missingModules,
    patchConfig,
    globalConfigKey,
    engineFeatures,
};
