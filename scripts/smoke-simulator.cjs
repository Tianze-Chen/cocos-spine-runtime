// Smoke: drive editor/simulator.js against this machine's real engine and
// test project with the Editor surface stubbed. Run 1 must perform a
// (no-op incremental) configure+build and write the stamp; run 2 must report
// 'fresh' without invoking cmake.
//
// Paths below are intentionally machine-local (same as smoke-wasm-yflip.mjs);
// adjust before reuse elsewhere.
'use strict';

const path = require('path');

global.Editor = {
    Message: {
        request: async (target, method) => {
            if (target === 'engine' && method === 'query-engine-info') {
                return { native: { path: 'D:\\code\\cocos4\\native' }, typescript: {} };
            }
            return null;
        },
    },
    Project: { path: 'D:\\projects\\spine' },
};

const sim = require(path.join(__dirname, '..', 'editor', 'simulator.js'));

(async () => {
    const first = await sim.ensureSimulatorPlugins();
    console.log('run 1:', JSON.stringify(first));
    if (first.status !== 'rebuilt' && first.status !== 'fresh') {
        throw new Error(`unexpected first-run status: ${first.status}`);
    }
    const second = await sim.ensureSimulatorPlugins();
    console.log('run 2:', JSON.stringify(second));
    if (second.status !== 'fresh') {
        throw new Error(`second run should be fresh, got: ${second.status}`);
    }
    console.log('smoke-simulator: OK');
})().catch((e) => {
    console.error('smoke-simulator: FAILED —', e && e.stack || e);
    process.exit(1);
});
