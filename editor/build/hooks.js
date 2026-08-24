'use strict';

/**
 * Build hooks for the spine-runtime extension.
 *
 * onAfterBuild copies the prebuilt spine-runtime.wasm into the build output's
 * `cocos-js/` directory. That is exactly where the engine's `pal/wasm`
 * interface expects a `.wasm` file to live:
 *
 *   - web:       pal/wasm fetches it relative to import.meta.url (inside cocos-js/);
 *   - mini-game: pal/wasm resolves `cocos-js/<name>` and hands the path to
 *                CCWebAssembly.instantiate (WXWebAssembly only accepts a file
 *                path, never raw bytes — the reason the old base64-embedded
 *                approach failed on WeChat);
 *   - native:    skipped — the JSB binding is used instead (no .wasm needed).
 *
 * The runtime's `bindings/index.ts` passes the bare name `spine-runtime.wasm`
 * to `cc.wasm.instantiateWasm`, matching the destination file name here.
 */

const fs = require('fs');
const path = require('path');
const engineModules = require('../engine-modules');

const WASM_SOURCE = path.join(__dirname, '..', '..', 'native', 'wasm', 'prebuilt', 'spine-runtime.wasm');
const WASM_NAME = 'spine-runtime.wasm';

// Platforms that use the JSB binding rather than wasm — no .wasm file needed.
const NATIVE_PLATFORMS = new Set([
    'android', 'ios', 'mac', 'windows', 'linux',
    'ohos', 'harmonyos-next', 'open-harmony', 'google-play',
]);

function log(msg) {
    // eslint-disable-next-line no-console
    console.log(`[spine-runtime] ${msg}`);
}

function shouldCopy(platform) {
    return !platform || !NATIVE_PLATFORMS.has(platform);
}

function findCocosJsDir(result) {
    const bases = [];
    if (result.dest) bases.push(result.dest);
    if (result.paths && result.paths.output) bases.push(result.paths.output);
    for (const base of bases) {
        const candidate = path.join(base, 'cocos-js');
        try {
            if (fs.existsSync(candidate) && fs.statSync(candidate).isDirectory()) {
                return candidate;
            }
        } catch (e) {
            // ignore
        }
    }
    return null;
}

/**
 * Fail the build when the project crops an engine feature the runtime needs.
 *
 * This is the only place the mistake can still be caught: the editor's engine is
 * a full build, so an unticked `webassembly` is invisible while authoring and
 * would only surface as a runtime error inside the packaged game.
 *
 * `2d` is required on every platform (sp.spine extends cc.UIMesh); `cc.wasm` is
 * only used on the web/mini-game path, native goes through the JSB binding.
 */
exports.onBeforeBuild = async function (options) {
    const platform = options && options.platform;
    const native = !shouldCopy(platform);

    const configs = await engineModules.readConfigs();
    if (!configs) {
        log('engine module check skipped: project module configs are unavailable');
        return;
    }
    // The build options do not carry the module list, so check the config the
    // project marks as global — the same one the panel edits by default.
    const key = await engineModules.globalConfigKey();
    const config = configs[key] || configs.defaultConfig;
    if (!config) {
        log(`engine module check skipped: no module config named "${key}"`);
        return;
    }

    let missing = engineModules.missingModules(config);
    if (native) {
        missing = missing.filter((name) => name !== 'webassembly');
    }
    if (missing.length === 0) {
        log(`engine modules ok (config "${key}")`);
        return;
    }

    throw new Error(`[spine-runtime] 项目缺少必需的引擎模块 / missing required engine modules: `
        + `${missing.join(', ')} (功能裁剪配置 / module config "${key}").\n`
        + `请在 项目设置 → 功能裁剪 中勾选它们后重新构建 / enable them in `
        + `Project Settings → Feature Cropping and build again.\n`
        + `配置文件 / config file: ${engineModules.settingsFile()}`);
};

exports.onAfterBuild = async function (options, result) {
    try {
        if (!shouldCopy(options && options.platform)) {
            log(`skip (native platform: ${options && options.platform})`);
            return;
        }
        if (!fs.existsSync(WASM_SOURCE)) {
            log(`skip (missing wasm source: ${WASM_SOURCE})`);
            return;
        }
        const cocosJsDir = findCocosJsDir(result);
        if (!cocosJsDir) {
            log('skip (could not resolve cocos-js dir)');
            return;
        }
        fs.mkdirSync(cocosJsDir, { recursive: true });
        const dest = path.join(cocosJsDir, WASM_NAME);
        fs.copyFileSync(WASM_SOURCE, dest);
        log(`copied ${WASM_NAME} -> ${dest}`);
    } catch (e) {
        // A failed copy must not break the whole build; log and continue.
        log(`onAfterBuild error: ${e && e.message}`);
    }
};
