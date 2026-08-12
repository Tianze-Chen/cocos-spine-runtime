'use strict';

/**
 * Extension main process entry.
 *
 * The editor's "asset dropped onto a node" message (contributions.inspector.drop
 * -> message "spine-runtime:inspector-drop") is delivered here. Component
 * creation must happen in the scene process, so forward to the scene script.
 */

const fs = require('fs');
const path = require('path');
const LOG = process.env.SPINE_MAIN_LOG || 'D:/projects/spine/main-drop.log';
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
    async inspectorDrop (args) {
        log(`MAIN inspectorDrop args: ${JSON.stringify(args)}`);
        try {
            const res = await Editor.Message.request('scene', 'execute-scene-script', {
                name: 'spine-runtime',
                method: 'inspectorDrop',
                args: [args],
            });
            log(`MAIN execute-scene-script returned: ${JSON.stringify(res)}`);
            return res;
        } catch (e) {
            log(`MAIN execute-scene-script FAILED: ${e.message}`);
            throw e;
        }
    },
};
