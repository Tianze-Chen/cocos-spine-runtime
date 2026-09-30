'use strict';

/**
 * Simulator plugin support (editor-driven, build-time discovery).
 *
 * The editor never compiles the native simulator: simulator preview just
 * spawns the prebuilt exe from `<engine-native>/simulator/Release/`
 * (builtin `preview` extension, simulator.ts `runSimulator()`). The custom
 * engine's simulator CMakeLists accepts
 *
 *     -DSIMULATOR_PLUGIN_SCAN_DIRS="<dir>[;<dir>]"
 *
 * and discovers every `cc_plugin.json` under those roots through the same
 * generic chain as native device builds (no plugin names baked into the
 * engine). This module keeps that exe plugin-ready without any user-typed
 * cmake:
 *
 *   - on load(), a stamp inside the simulator build dir is compared against
 *     the desired state: this project's `extensions/` tree plus a content
 *     hash of every native tree the scan would compile;
 *   - when stale, the simulator is reconfigured and rebuilt in the background
 *     and the stamp refreshed. Scan roots accumulate per machine, so opening
 *     a previously covered project costs nothing;
 *   - a menu item (扩展 → "Spine: 重建模拟器") forces a rebuild.
 *
 * The same module also keeps the simulator's engine-JS side loadable. Preview
 * stages `<engine>/bin/native-preview` into `Release/src/cocos-js` and then
 * overwrites `Release/src/import-map.json` with the editor install's own copy
 * (builtin preview extension, simulator.ts) — neither is ever produced for a
 * custom engine automatically; the stock install's map lacks the custom
 * engine's feature units (e.g. `webassembly`), which leaves the simulator
 * blank. When that state is detected, the editor's own builder is requested
 * in the background:
 *
 *     Editor.Message.request('preview', 'build-simulator-engine-ts')
 *
 * — the message behind 开发者→编译模拟器引擎（只编译TS代码）; its live path
 * builds against the engine selected in this editor and rewrites both
 * `bin/native-preview` and the install's import-map. Nothing here compiles
 * or patches anything by hand.
 *
 * There is no "before preview" hook in the editor, so the first build after
 * installing the extension happens while the editor is already usable; if
 * simulator preview is clicked during that window it launches the previous
 * (plugin-less) exe. Requirements match native device builds: cmake, a
 * VS/Xcode toolchain and node on PATH (configure runs the engine's
 * plugins_parser.js through NODE_EXECUTABLE).
 *
 * Stamp reset (drops accumulated scan roots from other projects): delete
 * `<engine-native>/simulator/plugin-scan-stamp.json` and reload the editor.
 */

const fs = require('fs');
const os = require('os');
const path = require('path');
const crypto = require('crypto');
const { spawn } = require('child_process');

const LOG = process.env.SPINE_SIM_LOG || path.join(os.tmpdir(), 'spine-runtime-simulator.log');

// Relative to the engine's native/ directory (from 'query-engine-info').
const SIMULATOR_SOURCE = path.join('tools', 'simulator', 'frameworks', 'runtime-src');
const SIMULATOR_BUILD = 'simulator';
const STAMP_FILE = 'plugin-scan-stamp.json';
const STAMP_VERSION = 1;
// The engine-side opt-in this module drives. Its absence means a stock
// engine whose simulator cannot host plugins at all.
const ENGINE_MARKER = 'SIMULATOR_PLUGIN_SCAN_DIRS';
const TAIL_LINES = 15;

let inFlight = null;

function log (msg) {
    try {
        fs.appendFileSync(LOG, `${new Date().toISOString()} ${msg}\n`);
    } catch (e) {
        // ignore
    }
}

/** Canonical form for comparing/storing scan roots: forward slashes,
 *  lowercased on Windows (case-insensitive filesystem). */
function canonicalDir (dir) {
    const forward = path.normalize(dir).replace(/[\\/]+/g, '/');
    return process.platform === 'win32' ? forward.toLowerCase() : forward;
}

/**
 * Content hash of everything the simulator build compiles from one scan
 * root: the `native/` tree of the root itself and of each extension
 * subdirectory (cc_plugin.json, cmake files, C++/JSB sources, vendored
 * third_party, prebuilt artifacts). `.git` and `build` directories are
 * skipped. The relpath is mixed in so renaming an extension invalidates
 * the stamp too.
 */
function hashScanRoot (root) {
    const hash = crypto.createHash('sha256');
    const walk = (dir, prefix) => {
        let entries;
        try {
            entries = fs.readdirSync(dir, { withFileTypes: true });
        } catch (e) {
            return;
        }
        entries.sort((a, b) => (a.name < b.name ? -1 : a.name > b.name ? 1 : 0));
        for (const entry of entries) {
            if (entry.name === '.git' || entry.name === 'build') continue;
            const rel = prefix + entry.name;
            const full = path.join(dir, entry.name);
            if (entry.isDirectory()) {
                hash.update(`d ${rel}\0`);
                walk(full, rel + '/');
            } else if (entry.isFile()) {
                hash.update(`f ${rel}\0`);
                try {
                    hash.update(fs.readFileSync(full));
                } catch (e) {
                    hash.update(`unreadable:${e.code}`);
                }
                hash.update('\0');
            }
        }
    };
    // `<root>/native` covers a repo-style root (the dev shortcut points the
    // scan straight at an extension checkout); `<root>/*/native` covers the
    // project layout where each extension is a subdirectory.
    walk(path.join(root, 'native'), 'native/');
    let entries;
    try {
        entries = fs.readdirSync(root, { withFileTypes: true });
    } catch (e) {
        entries = [];
    }
    for (const entry of entries.sort((a, b) => (a.name < b.name ? -1 : a.name > b.name ? 1 : 0))) {
        if (entry.isDirectory() && entry.name !== '.git' && entry.name !== 'build') {
            walk(path.join(root, entry.name, 'native'), `${entry.name}/native/`);
        }
    }
    return hash.digest('hex');
}

function stampPath (simBuild) {
    return path.join(simBuild, STAMP_FILE);
}

function readStamp (simBuild) {
    try {
        const stamp = JSON.parse(fs.readFileSync(stampPath(simBuild), 'utf8'));
        if (stamp && stamp.version === STAMP_VERSION && Array.isArray(stamp.scanDirs)
            && typeof stamp.nativeHash === 'string') {
            return stamp;
        }
    } catch (e) {
        // missing or unreadable -> treat as absent
    }
    return null;
}

function writeStamp (simBuild, scanDirs, nativeHash) {
    const stamp = {
        version: STAMP_VERSION,
        scanDirs,
        nativeHash,
        builtAt: new Date().toISOString(),
    };
    fs.mkdirSync(simBuild, { recursive: true });
    fs.writeFileSync(stampPath(simBuild), JSON.stringify(stamp, undefined, 2));
}

/** The exe the editor spawns for simulator preview (existence check only). */
function simulatorExe (simBuild) {
    return process.platform === 'win32'
        ? path.join(simBuild, 'Release', 'SimulatorApp-Win32.exe')
        : path.join(simBuild, 'Release', 'SimulatorApp-Mac.app');
}

/**
 * Run `cmake <args>`. Every output line goes to the log file; the resolve
 * value carries the exit code, a rolling tail for error reporting, and the
 * configure summary line when the parser reports the plugin count.
 */
function runCmake (args) {
    return new Promise((resolve) => {
        log(`$ cmake ${args.join(' ')}`);
        const child = spawn('cmake', args, { stdio: ['ignore', 'pipe', 'pipe'], windowsHide: true });
        const tail = [];
        let summary = '';
        const onChunk = (buf, stream) => {
            for (const line of buf.toString().split(/\r?\n/)) {
                if (!line) continue;
                log(`[cmake:${stream}] ${line}`);
                tail.push(line);
                if (tail.length > 200) tail.shift();
                if (/plugins?$|plugin dir found|record plugin/i.test(line)) {
                    summary = line.trim();
                }
            }
        };
        child.stdout.on('data', (buf) => onChunk(buf, 'out'));
        child.stderr.on('data', (buf) => onChunk(buf, 'err'));
        child.on('error', (err) => {
            // spawn itself failed (ENOENT etc.) — nothing ran at all.
            log(`cmake spawn failed: ${err.message}`);
            resolve({ code: -1, tail: [`cmake: ${err.message}`], summary });
        });
        child.on('close', (code) => resolve({ code: code === null ? -1 : code, tail, summary }));
    });
}

async function ensureSimulatorPlugins (options) {
    const force = !!(options && options.force);

    const info = await Editor.Message.request('engine', 'query-engine-info');
    const nativePath = info && info.native && info.native.path;
    if (!nativePath) {
        log('simulator plugin check skipped: engine native path is unavailable');
        return { status: 'no-engine' };
    }
    const simSrc = path.join(nativePath, SIMULATOR_SOURCE);
    const simBuild = path.join(nativePath, SIMULATOR_BUILD);

    // Stock engines have the simulator sources but not the scan hook; a
    // configure there would silently succeed without any plugin in the exe.
    let cmakeLists = '';
    try {
        cmakeLists = fs.readFileSync(path.join(simSrc, 'CMakeLists.txt'), 'utf8');
    } catch (e) {
        // fall through to the marker check below
    }
    if (!cmakeLists.includes(ENGINE_MARKER)) {
        console.warn('[spine-runtime] this engine\'s simulator does not support plugin scanning '
            + '(needs the custom engine); simulator preview runs without plugins');
        return { status: 'unsupported-engine' };
    }

    const scanRoot = path.join(Editor.Project.path, 'extensions');
    if (!fs.existsSync(scanRoot)) {
        log('simulator plugin check skipped: project has no extensions/ directory');
        return { status: 'no-extensions' };
    }
    const canonicalRoot = canonicalDir(scanRoot);

    const stamp = readStamp(simBuild);
    const accumulated = (stamp ? stamp.scanDirs : []).filter((dir) => fs.existsSync(dir));
    const desired = [...new Set([...accumulated, canonicalRoot])].sort();

    const nativeHash = desired.map(hashScanRoot).join('|');
    const reasons = [];
    if (force) reasons.push('forced');
    if (!fs.existsSync(simulatorExe(simBuild))) reasons.push('simulator exe missing');
    if (!stamp || stamp.nativeHash !== nativeHash) reasons.push('native sources changed');
    if (!accumulated.includes(canonicalRoot)) reasons.push('new scan root');
    if (reasons.length === 0) {
        log(`simulator plugins are fresh (${desired.length} scan root(s))`);
        return { status: 'fresh', scanDirs: desired };
    }
    log(`simulator rebuild triggered: ${reasons.join(', ')}`);

    console.log(`[spine-runtime] 更新模拟器原生插件 / updating simulator native plugins (${reasons.join(', ')})...`);
    const configureArgs = ['-S', simSrc, '-B', simBuild];
    // A fresh build dir needs the platform the editor expects the exe under;
    // an existing one locks its generator from the cache, so no -G/-A needed.
    if (!fs.existsSync(path.join(simBuild, 'CMakeCache.txt')) && process.platform === 'win32') {
        configureArgs.push('-A', 'x64');
    }
    configureArgs.push(`-DSIMULATOR_PLUGIN_SCAN_DIRS=${desired.join(';')}`);

    const configured = await runCmake(configureArgs);
    if (configured.code !== 0) {
        console.warn('[spine-runtime] simulator plugin configure failed; recent output:\n'
            + configured.tail.slice(-TAIL_LINES).join('\n'));
        return { status: 'configure-failed' };
    }
    const built = await runCmake(['--build', simBuild, '--config', 'Release']);
    if (built.code !== 0) {
        console.warn('[spine-runtime] simulator plugin build failed; recent output:\n'
            + built.tail.slice(-TAIL_LINES).join('\n'));
        return { status: 'build-failed' };
    }

    writeStamp(simBuild, desired, nativeHash);
    const summary = configured.summary || `scan roots: ${desired.length}`;
    console.log(`[spine-runtime] 模拟器插件支持已就绪 / simulator plugins ready — ${summary}`);
    return { status: 'rebuilt', scanDirs: desired, summary };
}

/** Single-flight wrapper: concurrent callers (load + menu) share one run. */
function ensure (options) {
    if (!inFlight) {
        inFlight = ensureSimulatorPlugins(options)
            .catch((e) => {
                log(`ensureSimulatorPlugins failed: ${e && e.stack || e}`);
                console.warn(`[spine-runtime] simulator plugin check failed: ${e && e.message}`);
                return { status: 'error' };
            })
            .finally(() => { inFlight = null; });
    }
    return inFlight;
}

// ---------------------------------------------------------------------------
// Simulator engine TS build (bin/native-preview + the editor install's
// simulator import-map), requested from the editor's own builder.
// ---------------------------------------------------------------------------

// Relative to the engine root ('query-engine-info' typescript.path).
const NATIVE_PREVIEW_DIR = path.join('bin', 'native-preview');
// Rollup chunk names end in an 8-char hash; feature-unit files don't.
const CHUNK_HASH_RE = /-[A-Za-z0-9_-]{8}\.js$/;
const CC_FU_PREFIX = 'cce:/internal/x/cc-fu/';

/**
 * The import-map the editor restages into the simulator on every preview
 * click: the builtin `preview` extension's unpacked static copy inside the
 * editor install — the same file its builder (and 开发者 menu) rewrites.
 * Empty string when this editor run has no such file (e.g. a source
 * checkout); callers then fall back to the staged copy.
 */
function editorInstallImportMap () {
    try {
        return path.join(path.dirname(process.execPath), 'resources', 'app.asar.unpacked',
            'builtin', 'preview', 'static', 'simulator', 'import-map.json');
    } catch (e) {
        return '';
    }
}

/** Feature-unit files at the top of bin/native-preview (hash chunks skipped). */
function nativePreviewUnits (dir) {
    let entries;
    try {
        entries = fs.readdirSync(dir, { withFileTypes: true });
    } catch (e) {
        return [];
    }
    return entries
        .filter((entry) => entry.isFile() && /\.js$/.test(entry.name) && !CHUNK_HASH_RE.test(entry.name))
        .map((entry) => entry.name.replace(/\.js$/, ''));
}

/**
 * Feature units the staged cc.js stub asks the loader to resolve (the exact
 * list the simulator must be able to load); empty when never staged.
 */
function stagedStubUnits (nativePath) {
    try {
        const head = fs.readFileSync(
            path.join(nativePath, SIMULATOR_BUILD, 'Release', 'src', 'cocos-js', 'cc.js'), 'utf8',
        ).slice(0, 8192);
        const list = head.match(/System\.register\(\[([^\]]*)\]/);
        return list ? JSON.parse(`[${list[1]}]`) : [];
    } catch (e) {
        return [];
    }
}

/**
 * Detect the blank-simulator state (missing engine build or an import-map
 * that cannot resolve the engine's feature units) and, when found, run the
 * editor's own simulator-engine-TS builder in the background. Checks are
 * read-only; the builder rewrites bin/native-preview and the install's
 * import-map, so a successful run leaves every check green.
 */
async function ensureSimulatorEngineTS (options) {
    const force = !!(options && options.force);

    const info = await Editor.Message.request('engine', 'query-engine-info');
    const enginePath = info && info.typescript && info.typescript.path;
    const nativePath = info && info.native && info.native.path;
    if (!enginePath || !nativePath) {
        log('simulator engine TS check skipped: engine paths are unavailable');
        return { status: 'no-engine' };
    }

    const nativePreview = path.join(enginePath, NATIVE_PREVIEW_DIR);
    const units = nativePreviewUnits(nativePreview);

    // What the simulator will actually be asked to load: prefer the staged
    // cc.js stub's dependency list (exact); fall back to the unit files the
    // engine build produced.
    let required = stagedStubUnits(nativePath);
    let requiredFrom = 'staged cc.js stub';
    if (required.length === 0) {
        required = units;
        requiredFrom = 'bin/native-preview';
    }

    const reasons = [];
    if (force) reasons.push('forced');
    if (units.length === 0) reasons.push('bin/native-preview is missing or empty');

    // The map the NEXT preview click will stage: the install's own copy when
    // this editor has one, otherwise whatever was staged last.
    let importMap = null;
    let importMapPath = '';
    const candidates = [
        editorInstallImportMap(),
        path.join(nativePath, SIMULATOR_BUILD, 'Release', 'src', 'import-map.json'),
    ];
    for (const candidate of candidates) {
        if (candidate && fs.existsSync(candidate)) {
            importMapPath = candidate;
            try {
                importMap = JSON.parse(fs.readFileSync(candidate, 'utf8'));
            } catch (e) {
                importMap = { corrupt: true };
            }
            break;
        }
    }
    if (importMap && importMap.corrupt) {
        reasons.push(`import-map is unreadable (${importMapPath})`);
    } else if (importMap) {
        const imports = importMap.imports || {};
        const missing = required.filter((unit) => !imports[CC_FU_PREFIX + unit]);
        if (missing.length > 0) {
            reasons.push(`import-map misses ${missing.join(', ')} (${requiredFrom}, checked ${importMapPath})`);
        }
    } else if (required.length > 0) {
        log('simulator import-map not found anywhere; checking files only');
    }
    const absent = required.filter((unit) => !fs.existsSync(path.join(nativePreview, `${unit}.js`)));
    if (absent.length > 0) {
        reasons.push(`bin/native-preview misses ${absent.join(', ')}`);
    }

    if (reasons.length === 0) {
        log('simulator engine TS is fresh');
        return { status: 'fresh' };
    }
    log(`simulator engine TS rebuild triggered: ${reasons.join(', ')}`);
    console.log('[spine-runtime] 编译模拟器引擎TS / building the simulator engine TS '
        + '(开发者→编译模拟器引擎 的后台调用 / the editor\'s own builder, in the background)...');
    try {
        await Editor.Message.request('preview', 'build-simulator-engine-ts');
    } catch (e) {
        log(`build-simulator-engine-ts failed: ${e && e.stack || e}`);
        console.warn('[spine-runtime] simulator engine TS build failed — 可用 开发者→编译模拟器引擎'
            + `（只编译TS代码）重试 / retry via the 开发者 menu: ${e && e.message}`);
        return { status: 'build-failed' };
    }
    console.log('[spine-runtime] 模拟器引擎TS已就绪 / simulator engine TS ready');
    return { status: 'rebuilt' };
}

/** Single-flight wrapper: concurrent callers (load + menu) share one run. */
let tsInFlight = null;
function ensureEngineTS (options) {
    if (!tsInFlight) {
        tsInFlight = ensureSimulatorEngineTS(options)
            .catch((e) => {
                log(`ensureSimulatorEngineTS failed: ${e && e.stack || e}`);
                console.warn(`[spine-runtime] simulator engine TS check failed: ${e && e.message}`);
                return { status: 'error' };
            })
            .finally(() => { tsInFlight = null; });
    }
    return tsInFlight;
}

module.exports = {
    ensureSimulatorPlugins: ensure,
    ensureSimulatorEngineTS: ensureEngineTS,
    // exposed for the smoke script / tests
    hashScanRoot,
    canonicalDir,
    readStamp,
    writeStamp,
    stampPath,
};
