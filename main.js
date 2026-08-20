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
const LOG = process.env.SPINE_MAIN_LOG || path.join(os.tmpdir(), 'spine-runtime-main-drop.log');
const WASM_SOURCE = path.join(__dirname, 'native', 'wasm', 'prebuilt', 'spine-runtime.wasm');

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

exports.load = async function load () {
    try {
        await syncEditorWasm();
    } catch (e) {
        log(`sync editor WASM failed: ${e && e.message}`);
        console.warn(`[spine-runtime] could not prepare editor WASM: ${e && e.message}`);
    }
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
