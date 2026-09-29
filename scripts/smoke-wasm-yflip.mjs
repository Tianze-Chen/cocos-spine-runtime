// Smoke test: new wasm prebuilt + regenerated glue instantiate and the C++
// output-affine default is the constant y-flip (local-space vertices).
// Compare default output vs explicit identity transform output:
//   v_default.x == v_identity.x && v_default.y == -v_identity.y
import fs from 'node:fs';
import path from 'node:path';
import { pathToFileURL } from 'node:url';

const root = path.resolve(path.dirname(new URL(import.meta.url).pathname.replace(/^\/([A-Za-z]:)/, '$1')), '..');
// The glue is built for web/worker environments only; pose as a worker for the
// env sniff (wasm loading is bypassed via instantiateWasm below).
globalThis.WorkerGlobalScope = {};
const factory = (await import(pathToFileURL(path.join(root, 'runtime/bindings/spine-runtime.js')))).default;
const wasmPath = path.join(root, 'native/wasm/prebuilt/spine-runtime.wasm');

const mod = await factory({
    print: () => {},
    printErr: () => {},
    instantiateWasm: (info, receive) => {
        const bytes = fs.readFileSync(wasmPath);
        WebAssembly.instantiate(bytes, info).then((res) => { receive(res.instance, res.module); });
        return {};
    },
});

const json = fs.readFileSync(path.join(root, 'native/spine-adapter/poc/assets/spineboy-pro.json'), 'utf8');
const atlas = fs.readFileSync(path.join(root, 'native/spine-adapter/poc/assets/spineboy.atlas'), 'utf8');
const data = mod.createDataJson(json, atlas, ['spineboy.png'], 1);
if (!data) { throw new Error(`createDataJson failed: ${mod.lastError()}`); }
const rt = mod.createRuntime(data);
if (!rt) { throw new Error('createRuntime failed'); }
mod.runtimeSetAnimation(rt, 0, 'walk', true);

function extract () {
    mod.runtimeUpdate(rt, 0);
    const rd = mod.runtimeRenderData(rt);
    if (!rd || rd.vertexCount < 1) { throw new Error('no render data'); }
    const f32 = new Float32Array(mod.HEAPU8.buffer, rd.vPtr, rd.vertexCount * rd.vertexStrideBytes / 4);
    const out = [];
    for (let i = 0; i < rd.vertexCount; i++) {
        out.push(f32[i * rd.vertexStrideBytes / 4], f32[i * rd.vertexStrideBytes / 4 + 1]);
    }
    return out;
}

// Warm up the animation, then freeze: paused runtimeUpdate re-extracts the
// current pose without advancing, so both extractions below see the SAME pose
// and differ only by the output affine.
mod.runtimeUpdate(rt, 1 / 60);
mod.runtimeUpdate(rt, 1 / 60);
mod.runtimeSetPaused(rt, true);

// 1) default affine (create-time y-flip)
const def = extract();
// 2) explicit identity affine — the legacy per-frame call
mod.runtimeSetOutputTransform(rt, 1, 0, 0, 1, 0, 0);
const idt = extract();

let mismatch = 0;
for (let i = 0; i < def.length; i += 2) {
    if (Math.abs(def[i] - idt[i]) > 1e-4 || Math.abs(def[i + 1] + idt[i + 1]) > 1e-4) { mismatch++; }
}
const yNeg = def.filter((_, i) => i % 2 === 1).some((y) => y < 0);
console.log(`vertices=${def.length / 2} stride-checked, mismatches=${mismatch}, default-has-negative-y=${yNeg}`);
if (mismatch !== 0) { throw new Error('default affine is NOT the y-flip of identity'); }
if (!yNeg) { throw new Error('expected some negative y in flipped output'); }

mod.disposeRuntime(rt);
mod.disposeData(data);
console.log('SMOKE OK: wasm+glue instantiate; default output affine = constant y-flip (node-local space)');
