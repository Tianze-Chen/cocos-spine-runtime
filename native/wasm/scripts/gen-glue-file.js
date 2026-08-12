'use strict';

/**
 * Regenerates the single CJS glue used by every WASM environment. The `.wasm`
 * is a SEPARATE file and instantiation is delegated through the
 * `Module.instantiateWasm` hook (set by `bindings/index.ts` to the engine's
 * `cc.wasm.instantiateWasm`). Native targets never use this glue; they require
 * the JSB binding.
 *
 *   1. removes `import.meta.url` usages (illegal in a CJS-parsed file),
 *   2. neutralizes the emscripten environment guards (so it also runs on
 *      mini-game runtimes / editor scene worker, where env detection differs),
 *   3. switches `export default` to `module.exports`.
 *
 * The raw ESM glue would otherwise locate its .wasm via import.meta.url and
 * WebAssembly.instantiateStreaming; those are dead once the instantiateWasm hook
 * is supplied, and are stripped so the file is valid CJS.
 *
 * Usage: node native/wasm/scripts/gen-glue-file.js [emscripten-output-directory]
 * Output: runtime/bindings/spine-runtime.js (overwrites in place)
 */

const fs = require('fs');
const path = require('path');

const WASM_ROOT = path.resolve(__dirname, '..');
const ROOT = path.resolve(WASM_ROOT, '..', '..');
const DEFAULT_INPUT_DIR = path.join(WASM_ROOT, 'prebuilt');
const INPUT_DIR = process.argv[2] ? path.resolve(ROOT, process.argv[2]) : DEFAULT_INPUT_DIR;
const GLUE_IN = path.join(INPUT_DIR, 'spine-runtime.js');
const GLUE_OUT = path.join(ROOT, 'runtime', 'bindings', 'spine-runtime.js');

if (!fs.existsSync(GLUE_IN)) {
    console.error(`glue not found: ${GLUE_IN}`);
    process.exit(1);
}

let glue = fs.readFileSync(GLUE_IN, 'utf8');

// 1. Remove import.meta.url usages (dead once instantiateWasm is injected, but
//    import.meta is illegal in a CJS-parsed file).
glue = glue.replace(/var _scriptName\s*=\s*import\.meta\.url;/, 'var _scriptName="";');
glue = glue.replace(/new URL\(['"]spine-runtime\.wasm['"],\s*import\.meta\.url\)\.href/, '"spine-runtime.wasm"');

// 2. Neutralize the emscripten environment guards so the glue can also run in
//    mini-game runtimes (whose env detection may not expose `window`) and in the
//    editor's scene worker (Node-like, no `window`). The wasm is always loaded
//    through the instantiateWasm hook, so the env-detection readAsync branch and
//    these advisory asserts are never reached.
glue = glue.replace(
    /if \(currentNodeVersion < TARGET_NOT_SUPPORTED\) \{/,
    'if (false && currentNodeVersion < TARGET_NOT_SUPPORTED) {',
);
glue = glue.replace(
    /if \(!\(globalThis\.window \|\| globalThis\.WorkerGlobalScope\)\) throw new Error\('not compiled for this environment/,
    'if (false && !(globalThis.window || globalThis.WorkerGlobalScope)) throw new Error(\'not compiled for this environment',
);
// The second, hard Node-version guard uses the raw sentinel literal.
glue = glue.replace(
    /if \(currentNodeVersion < 2147483647\) \{/,
    'if (false && currentNodeVersion < 2147483647) {',
);
// The web-only build has no Node readAsync branch — the env-detection fallback
// throws unconditionally in Node. instantiateWasm handles the wasm, so readAsync
// is never invoked; neutralize the throw.
glue = glue.replace(
    /throw new Error\('environment detection error'\);/,
    "/* environment detection error neutralized (readAsync unused; wasm via instantiateWasm) */",
);
// The build-time env asserts (shell.js section) abort on Node/worker/shell.
// assert(cond, msg) aborts when cond is FALSE, so force cond to true.
glue = glue.replace(
    /assert\(!ENVIRONMENT_IS_WORKER, 'worker environment detected but not enabled at build time/,
    "assert(true, 'worker environment detected but not enabled at build time",
);
glue = glue.replace(
    /assert\(!ENVIRONMENT_IS_NODE, 'node environment detected but not enabled at build time/,
    "assert(true, 'node environment detected but not enabled at build time",
);
glue = glue.replace(
    /assert\(!ENVIRONMENT_IS_SHELL, 'shell environment detected but not enabled at build time/,
    "assert(true, 'shell environment detected but not enabled at build time",
);

// 3. CJS export.
glue = glue.replace(/export default Spine(Runtime|Facade);/, 'module.exports=Spine$1;');
if (glue.includes('export default')) {
    console.error('still contains export default; aborting');
    process.exit(1);
}

fs.writeFileSync(GLUE_OUT, glue);
console.log(`wrote ${GLUE_OUT} (${fs.statSync(GLUE_OUT).size} bytes, wasm NOT embedded — loaded separately via instantiateWasm hook)`);
