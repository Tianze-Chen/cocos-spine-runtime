'use strict';

/**
 * spine-skeleton asset handler — imports a spine .json (+ .atlas + .png) into an
 * `sp.spineData` asset.
 *
 * Registered via `contributions["asset-db"]["asset-handler"]` (the modern,
 * supported mechanism since 3.8.3; the old `importer` contribution is
 * deprecated). Pattern adapted from the editor's built-in spine handler.
 *
 * Requires the plugin runtime (sp.spineData / cc.UIMesh) to be available in the
 * target engine, and the project to load the mounted runtime scripts.
 */

const fs = require('fs');
const path = require('path');

// Load-time marker: lets us confirm (in the editor console / asset-db logs) that
// this handler module was actually required and registered by the asset-db.
console.debug('[spine-runtime] spine-skeleton asset handler module loaded');

const ATLAS_EXTS = ['.atlas', '.txt', '.atlas.txt', ''];

/**
 * Atlas-like files in a directory, in preference order. Only `.txt` files are
 * considered when no `.atlas` file exists (to avoid picking up readmes etc.).
 */
function listAtlasCandidates(dir) {
    const out = [];
    for (const f of fs.readdirSync(dir)) {
        if (f.endsWith('.atlas') && !f.endsWith('.atlas.txt')) out.push(path.join(dir, f));
    }
    for (const f of fs.readdirSync(dir)) {
        if (f.endsWith('.atlas.txt')) out.push(path.join(dir, f));
    }
    if (out.length === 0) {
        for (const f of fs.readdirSync(dir)) {
            if (f.endsWith('.txt') && !f.startsWith('.')) out.push(path.join(dir, f));
        }
    }
    return out;
}

/**
 * Search for the matching atlas file next to the skeleton json.
 *
 * First tries the same basename (official convention: `name.json` + `name.atlas`).
 * Falls back to "the only atlas-like file in the same directory" — some exports
 * name the skeleton differently from its atlas (e.g. `character-pro.json` +
 * `character.atlas`). Returns null when the match is ambiguous.
 */
function searchAtlas(skeletonPath) {
    const ext = path.extname(skeletonPath);
    const base = skeletonPath.substr(0, skeletonPath.length - ext.length);
    for (const suffix of ATLAS_EXTS) {
        const candidate = base + suffix;
        if (fs.existsSync(candidate)) {
            return candidate;
        }
    }
    const candidates = listAtlasCandidates(path.dirname(skeletonPath));
    return candidates.length === 1 ? candidates[0] : null;
}

/**
 * Collects the texture page uuids/names referenced by an atlas.
 */
class TextureParser {
    constructor(asset, atlasPath) {
        this.asset = asset;
        this.atlasPath = atlasPath;
        this.texturesUUID = [];
        this.textureNames = [];
        this.asset.depend(atlasPath);
    }

    load(line) {
        const name = path.basename(line);
        const base = path.dirname(this.atlasPath);
        const filePath = path.resolve(base, name);
        // resolve the texture asset through the asset-db. The handler runs in
        // the asset-db worker where @editor/asset-db is exposed as a global
        // (require('@editor/asset-db') fails through the editor module loader).
        const queryAsset = globalThis.AssetDB && globalThis.AssetDB.queryAsset;
        const asset = queryAsset ? queryAsset(filePath) : null;
        if (asset) {
            // the sprite-frame sub-asset uuid of a texture
            const uuid = asset.uuid + '@6c48a';
            this.asset.depend(uuid);
            this.texturesUUID.push(uuid);
            this.textureNames.push(line);
        } else if (!fs.existsSync(filePath)) {
            console.error(`[spine-runtime] Can not find texture "${line}" for atlas "${this.atlasPath}"`);
        } else {
            console.warn(`[spine-runtime] UUID not yet initialized for "${filePath}" (will retry on next import).`);
        }
        return null;
    }
}

/**
 * Parses the atlas text and returns texture uuids/names + the atlas text.
 */
function parserAtlas(asset, atlasPath) {
    const textureParser = new TextureParser(asset, atlasPath);
    const content = fs.readFileSync(atlasPath, 'utf8');
    const lines = content.split('\n');
    let page = null;
    for (const line of lines) {
        const trimmed = line.trim();
        if (trimmed.length === 0) {
            page = null;
        } else if (!page) {
            page = trimmed;
            textureParser.load(page);
        }
    }
    return {
        texturesUUID: textureParser.texturesUUID,
        textureNames: textureParser.textureNames,
        atlasText: content,
    };
}

/**
 * The serialized `sp.spineData` payload. Field names match the runtime class:
 * _atlasText / _skeletonJsonStr / textures / textureNames / scale.
 */
function buildPayload(asset, jsonText, atlasText, textureUUIDs, textureNames, scale) {
    return {
        __type__: 'sp.spineData',
        _name: asset.basename || '',
        _objFlags: 0,
        _native: '',
        _atlasText: atlasText,
        _skeletonJsonStr: jsonText,
        textures: textureUUIDs.map((uuid) => ({ __uuid__: uuid })),
        textureNames: textureNames,
        scale: scale,
    };
}

/**
 * Detects whether a json text is a spine skeleton.
 */
function isSpineSkeleton(asset) {
    const assetpath = asset.source;
    if (assetpath.endsWith('.skel')) {
        return true;
    }
    let json;
    const text = fs.readFileSync(assetpath, 'utf8');
    const fastTest = text.slice(0, 30);
    const maybe =
        fastTest.indexOf('slots') > 0 ||
        fastTest.indexOf('skins') > 0 ||
        fastTest.indexOf('events') > 0 ||
        fastTest.indexOf('animations') > 0 ||
        fastTest.indexOf('bones') > 0 ||
        fastTest.indexOf('skeleton') > 0 ||
        fastTest.indexOf('"ik"') > 0;
    if (!maybe) {
        return false;
    }
    try {
        json = JSON.parse(text);
    } catch (e) {
        return false;
    }
    return Array.isArray(json.bones);
}

/**
 * The asset handler.
 */
module.exports = {
    // handler name (referenced in the asset-handler contribution)
    name: 'spine-skeleton',

    // runtime asset class this handler produces
    assetType: 'sp.spineData',

    displayName: 'sp.spineData',
    description: 'Spine skeleton (sp.spineData, spine-cpp 4.3)',

    // user-configurable options (shown in the meta inspector)
    userDataConfig: {
        default: {
            scale: {
                displayName: 'Scale',
                type: 'number',
                default: 1,
            },
        },
    },

    async validate(asset) {
        return isSpineSkeleton(asset);
    },

    importer: {
        version: '1.0.0',

        /**
         * The actual import flow. Returns true on success.
         * @param {import('@editor/asset-db').Asset} asset
         */
        async import(asset) {
            const fspath = asset.source;

            // Binary .skel is not supported by the facade JSON loader yet.
            if (fspath.endsWith('.skel')) {
                throw new Error('[spine-runtime] binary .skel is not supported yet; use .json');
            }

            const jsonText = await fs.promises.readFile(fspath, { encoding: 'utf8' });
            let json;
            try {
                json = JSON.parse(jsonText);
            } catch (e) {
                console.error(e);
                return false;
            }

            const scale = (asset.userData && asset.userData.scale) || 1;

            // Find + parse the matching atlas.
            const atlasPath = searchAtlas(fspath);
            if (!atlasPath) {
                throw new Error(`[spine-runtime] The atlas with the same name is not found for ${fspath}`);
            }
            const atlas = parserAtlas(asset, atlasPath);

            // Build the serialized sp.spineData payload and save it to the library.
            const payload = buildPayload(asset, jsonText, atlas.atlasText, atlas.texturesUUID, atlas.textureNames, scale);
            await asset.saveToLibrary('.json', JSON.stringify(payload));

            // Record dependencies so dependent assets refresh on change.
            const depends = [atlasPath, ...atlas.texturesUUID];
            asset.setData('depends', depends);

            return true;
        },
    },
};
