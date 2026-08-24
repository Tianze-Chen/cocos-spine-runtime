'use strict';

/**
 * Extension main process entry.
 *
 * The editor's "asset dropped onto a node" message (contributions.inspector.drop
 * -> message "spine-runtime:inspector-drop") is delivered here. Component
 * creation must happen in the scene process, so forward to the scene script.
 */

const fs = require('fs');
const os = require('os');
const path = require('path');
const engineModules = require('./editor/engine-modules');
const LOG = process.env.SPINE_MAIN_LOG || path.join(os.tmpdir(), 'spine-runtime-main-drop.log');
const WASM_SOURCE = path.join(__dirname, 'native', 'wasm', 'prebuilt', 'spine-runtime.wasm');
// Remembers that the user declined the module repair, so the prompt is asked
// once per project instead of on every editor start.
const DECLINED_KEY = 'engineModulesPromptDeclined';

function log (msg) {
    try {
        fs.appendFileSync(LOG, `${new Date().toISOString()} ${msg}\n`);
    } catch (e) {
        // ignore
    }
}

async function syncEditorWasm () {
    const info = await Editor.Message.request('engine', 'query-engine-info');
    const nativePath = info && info.native && info.native.path;
    if (!nativePath) {
        throw new Error('engine native path is unavailable');
    }
    if (!fs.existsSync(WASM_SOURCE)) {
        throw new Error(`WASM source is missing: ${WASM_SOURCE}`);
    }
    const destination = path.join(nativePath, 'external', 'spine-runtime.wasm');
    fs.mkdirSync(path.dirname(destination), { recursive: true });
    fs.copyFileSync(WASM_SOURCE, destination);
    log(`synced editor WASM: ${destination}`);
}

/**
 * Repair the project's engine feature selection if it crops something the
 * runtime needs (see editor/engine-modules.js for what and why).
 *
 * The user is asked first: this writes their project settings and only takes
 * effect after an editor restart, so doing it silently would be surprising and
 * hard to trace back here.
 */
async function ensureEngineModules () {
    const configs = await engineModules.readConfigs();
    if (!configs) {
        log('engine module check skipped: project module configs are unavailable');
        return;
    }

    const features = await engineModules.engineFeatures();
    if (!features) {
        log('engine module check skipped: could not read the engine cc.config.json');
        return;
    }
    // Never add a feature the selected engine does not declare — on an engine
    // without the `webassembly` export that would only produce a broken config.
    const known = engineModules.REQUIRED_MODULES.filter((name) => features.has(name));
    const unknown = engineModules.REQUIRED_MODULES.filter((name) => !features.has(name));
    if (unknown.length > 0) {
        log(`engine does not declare: ${unknown.join(', ')}`);
        console.warn(`[spine-runtime] the selected engine has no ${unknown.join(', ')} feature; `
            + 'this extension needs the custom engine that exports cc.wasm and cc.UIMesh');
    }

    const broken = [];
    for (const [key, config] of Object.entries(configs)) {
        const missing = engineModules.missingModules(config).filter((name) => known.includes(name));
        if (missing.length > 0) broken.push({ key, config, missing });
    }
    if (broken.length === 0) return;

    const summary = broken.map(({ key, missing }) => `${key}: ${missing.join(', ')}`).join('; ');
    log(`engine modules missing -> ${summary}`);

    let declined = false;
    try {
        declined = await Editor.Profile.getConfig('spine-runtime', DECLINED_KEY, 'project');
    } catch (e) {
        // treat an unreadable flag as "not declined"
    }
    if (declined) {
        console.warn(`[spine-runtime] engine modules still missing (${summary}); `
            + 'enable them in 项目设置 → 功能裁剪');
        return;
    }

    const names = [...new Set(broken.flatMap(({ missing }) => missing))].join(', ');
    const result = await Editor.Dialog.warn(
        `[spine-runtime] 缺少必需的引擎模块 / missing required engine modules: ${names}`,
        {
            detail: '这些模块被“功能裁剪”排除了。编辑器内的引擎是全模块构建，所以现在看不出问题，'
                + '但构建出的包会在运行时报错。是否自动写入项目配置？\n'
                + `受影响的配置 / affected configs: ${summary}\n`
                + '也可以手动勾选：项目设置 → 功能裁剪。',
            buttons: ['自动写入项目配置 / Fix', '以后再说 / Later'],
            default: 0,
            cancel: 1,
        },
    );
    if (!result || result.response !== 0) {
        log('user declined the engine module repair');
        try {
            await Editor.Profile.setConfig('spine-runtime', DECLINED_KEY, true, 'project');
        } catch (e) {
            log(`could not persist the declined flag: ${e && e.message}`);
        }
        return;
    }

    for (const { config, missing } of broken) {
        engineModules.patchConfig(config, missing);
    }
    const via = await engineModules.writeConfigs(configs);
    log(`patched engine modules via ${via}: ${summary}`);
    await Editor.Dialog.info(
        `[spine-runtime] 已开启 ${names}，请重启编辑器后生效 / enabled ${names}, restart the editor to apply`,
        { detail: `写入位置 / written to: ${engineModules.settingsFile()}` },
    );
}

exports.load = async function load () {
    try {
        await syncEditorWasm();
    } catch (e) {
        log(`sync editor WASM failed: ${e && e.message}`);
        console.warn(`[spine-runtime] could not prepare editor WASM: ${e && e.message}`);
    }
    // Deliberately not awaited: this may open a modal, and package loading must
    // not sit behind a dialog during editor startup.
    ensureEngineModules().catch((e) => {
        log(`ensureEngineModules failed: ${e && e.message}`);
        console.warn(`[spine-runtime] engine module check failed: ${e && e.message}`);
    });
};

exports.methods = {
    /**
     * `contributions.inspector.drop.node` handler.
     *
     * The Inspector's node panel (engine `editor/inspector/contributions/node.js`)
     * dispatches the drop as
     *
     *     Editor.Message.request(pkg, message, dropItem, dumps, uuidList)
     *
     * wrapped in scene `begin-recording` / `end-recording`, so everything we do
     * here lands in a single undo step. `dropItem` is one entry of the drag's
     * `additional` list: `{ type: 'sp.spineData', value: <asset uuid> }`.
     *
     * Component creation must happen in the scene process, so forward one call
     * per selected node to the scene script.
     */
    async inspectorDrop (dropItem, dumps, uuidList) {
        const assetUuid = String((dropItem && dropItem.value) || '').replace(/@[\w]+$/, '');
        const nodeUuids = (Array.isArray(uuidList) && uuidList.length > 0)
            ? uuidList.filter(Boolean)
            : (Array.isArray(dumps) ? dumps.map((dump) => dump && dump.uuid && dump.uuid.value).filter(Boolean) : []);
        log(`MAIN inspectorDrop asset=${assetUuid} nodes=${JSON.stringify(nodeUuids)}`);
        if (!assetUuid || nodeUuids.length === 0) return;

        for (const nodeUuid of nodeUuids) {
            try {
                const res = await Editor.Message.request('scene', 'execute-scene-script', {
                    name: 'spine-runtime',
                    method: 'inspectorDrop',
                    args: [nodeUuid, assetUuid],
                });
                log(`MAIN execute-scene-script (${nodeUuid}) returned: ${JSON.stringify(res)}`);
            } catch (e) {
                log(`MAIN execute-scene-script (${nodeUuid}) FAILED: ${e && e.message}`);
                console.warn(`[spine-runtime] could not add sp.spine to ${nodeUuid}: ${e && e.message}`);
            }
        }
    },
};
