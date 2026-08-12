'use strict';

/**
 * Backward-compatible entry point for regenerating SpineRuntime's CJS glue.
 *
 * The former implementation embedded a gzip/base64 copy of the WASM binary.
 * All environments now use the same external `spine-runtime.wasm` through
 * cc.wasm, so delegate to the canonical external-glue generator.
 */

console.warn('[spine-runtime] gen-glue.js no longer embeds WASM; generating the shared external glue');
require('./gen-glue-file.js');
