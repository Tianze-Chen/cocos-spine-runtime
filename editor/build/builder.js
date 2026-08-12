'use strict';

/**
 * Build contribution for the spine-runtime extension.
 *
 * Registers build hooks (see ./hooks.js) for all platforms. The hooks are
 * responsible for placing the prebuilt `spine-runtime.wasm` into the build
 * output's `cocos-js/` so the runtime can load it through the engine's
 * packaged `pal/wasm` interface (web fetches bytes; mini-game delegates to
 * CCWebAssembly.instantiate with a file path).
 */

exports.configs = {
    '*': {
        hooks: './hooks',
    },
};
