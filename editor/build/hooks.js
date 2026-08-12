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
