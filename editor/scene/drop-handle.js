'use strict';

/**
 * sp.spineData scene drop-handle.
 *
 * The editor's built-in AnyHandler (acceptedTypes=[] = all types) is registered
 * first in the scene's dropHandlers array and no-ops for unknown/custom asset
 * types, so sp.spineData dropped into the scene never created a node. This scene
 * script reaches the scene's internal drag-drop handler registry and:
 *
 *   1. UNSHIFTS a sp.spineData handler to the FRONT of the dispatch order (so it
 *      wins before AnyHandler).
 *   2. Adds 'sp.spineData' to DragDropUtils.droppableAssetTypes (the drag
 *      acceptance gate, built independently of the handlers).
 *   3. onDrop builds the node + sp.spine component + bound skeletonData directly
 *      in the scene process.
 *
 * Runs via contributions.scene.script (load()/unload()).
 */

const fs = require('fs');
const path = require('path');

const LOG = process.env.SPINE_DROP_LOG || 'D:/projects/spine/drop-handle.log';

function log (msg) {
    try {
        fs.appendFileSync(LOG, `${new Date().toISOString()} ${msg}\n`);
    } catch (e) {
        // ignore — logging must never break the scene
    }
}

function tryReachModule (relPath) {
    const ed = globalThis.Editor;
    const appPath = ed && ed.App && ed.App.path;
    if (!appPath) { log('no Editor.App.path'); return null; }
    const c = path.join(appPath, relPath);
    try {
        if (ed.Module && ed.Module.__protected__) {
            const mod = ed.Module.__protected__.requireFile(c);
            if (mod) return mod;
        }
    } catch (e) {
        // try plain require
    }
    try {
        return require(c);
    } catch (e) {
        log(`cannot reach ${relPath}: ${e.message}`);
    }
    return null;
}

function tryReachHandlers () {
    return tryReachModule('builtin/scene/dist/script/3d/manager/asset/drag-drop/handlers/index')
        || tryReachModule('builtin/scene/dist/script/3d/manager/asset/drag-drop/handlers');
}

// Plain-object handler (fallback when BaseHandler isn't extendable).
function makePlainHandler () {
    return {
        _spineDrop: true,
        acceptedTypes: ['sp.spineData'],
        excludedTypes: [],
        canDrop (dragItems) {
            return Array.isArray(dragItems) && dragItems.every((d) => d && d.type === 'sp.spineData');
        },
        async onDragLeave () {},
        async onDragOver () {},
        async onDrop (event, dragItems) {
            const item = (dragItems && dragItems[0]) || (event && event.values && event.values[0]);
            await createNode(item);
        },
    };
}

// Add the sp.spine component to an EXISTING node (drop-onto-node).
async function addComponentToNode (node, uuid) {
    try {
        const cc = require('cc');
        const sfm = cce.SceneFacadeManager;
        const asset = await new Promise((res) => cc.assetManager.loadAny(uuid, (err, a) => res(err ? null : a)));
        if (!asset) { log('addComponentToNode: asset load failed'); return; }
        await sfm.createComponent({ uuid: node.uuid, component: 'sp.spine' });
        const compClass = cc.js.getClassByName ? cc.js.getClassByName('sp.spine') : null;
        const comp = compClass ? node.getComponent(compClass) : null;
        if (comp) {
            comp.spineData = asset;
            log(`addComponentToNode: added sp.spine to "${node.name}"`);
        } else {
            log('addComponentToNode: component not found after createComponent');
        }
        if (cce.Engine && cce.Engine.repaintInEditMode) cce.Engine.repaintInEditMode();
    } catch (e) {
        log(`addComponentToNode ERROR: ${(e && e.stack) || e}`);
    }
}

function makeHandler () {
    // Prefer extending BaseHandler so onDrop can raycast the drop target and,
    // when the drop lands on an existing node, add the component instead of
    // creating a new node.
    try {
        const baseMod = tryReachModule('builtin/scene/dist/script/3d/manager/asset/drag-drop/handlers/base-handler');
        const BH = baseMod && (baseMod.BaseHandler || baseMod.default);
        if (typeof BH === 'function') {
            class SpineDropHandler extends BH {
                constructor () {
                    super();
                    this._spineDrop = true;
                    this.acceptedTypes = ['sp.spineData'];
                    this.excludedTypes = [];
                }
                canDrop (dragItems) {
                    return Array.isArray(dragItems) && dragItems.every((d) => d && d.type === 'sp.spineData');
                }
                async onDragLeave () {}
                async onDragOver () {}
                async onDrop (event, dragItems) {
                    const item = (dragItems && dragItems[0]) || (event && event.values && event.values[0]);
                    if (!item || !item.value) return;
                    try {
                        const nodes = this.getRaycastResultNodes
                            ? this.getRaycastResultNodes(event.clientX, event.clientY)
                            : null;
                        if (nodes && nodes.length > 0) {
                            log(`onDrop raycast nodes: ${nodes.map((n) => `${n.name}(${n.uuid})`).join(' | ')}`);
                            // Skip the scene root / Canvas (background); prefer
                            // the most specific node the drop landed on.
                            const specific = nodes.find((n) => n && n.uuid && !n.isScene && n.name !== 'Canvas' && n.name !== 'Scene');
                            const target = specific || nodes[nodes.length - 1];
                            if (target) {
                                await addComponentToNode(target, item.value);
                                return;
                            }
                        }
                    } catch (e) {
                        log(`onDrop raycast failed (creating node instead): ${e.message}`);
                    }
                    await createNode(item);
                }
            }
            log('drop handler extends BaseHandler (drop-onto-node enabled)');
            return new SpineDropHandler();
        }
    } catch (e) {
        log(`BaseHandler extend failed: ${e.message}`);
    }
    return makePlainHandler();
}

async function buildNode (uuid) {
    const cc = require('cc');
    const sfm = cce.SceneFacadeManager;
    if (!sfm || typeof sfm.createNode !== 'function') {
        log('buildNode: cce.SceneFacadeManager.createNode not available');
        return null;
    }
    const asset = await new Promise((res) => {
        cc.assetManager.loadAny(uuid, (err, a) => res(err ? null : a));
    });
    if (!asset) { log('buildNode: asset load failed'); return null; }

    // Create the node through the editor's scene facade so it is integrated
    // with the hierarchy, undo and scene save (raw scene.addChild is not).
    const scene = cc.director.getScene();
    // Prefer a Canvas as the 2D parent (matches how 2D assets drop); fall back
    // to the scene root.
    let parentUuid;
    if (scene) {
        const canvasComp = scene.getComponentInChildren(cc.Canvas);
        const canvasNode = canvasComp ? canvasComp.node : (scene.getChildByName && scene.getChildByName('Canvas'));
        parentUuid = (canvasNode && canvasNode.uuid) || scene.uuid;
    }
    log(`buildNode: parent=${parentUuid}`);
    const created = await sfm.createNode({ name: asset.name || 'spine', parent: parentUuid, snapshot: true });
    const nodeUuid = typeof created === 'string' ? created : (created && created.uuid);
    log(`buildNode: facade.createNode -> ${JSON.stringify(created)}`);
    if (!nodeUuid) return null;

    try {
        await sfm.createComponent({ uuid: nodeUuid, component: 'sp.spine' });
        log('buildNode: createComponent sp.spine ok');
    } catch (e) {
        log(`buildNode: createComponent failed: ${e.message}`);
    }

    const node = cce.Node.query ? cce.Node.query(nodeUuid) : null;
    if (node) {
        const parentInfo = node._parent ? `${node._parent.name}(${node._parent.uuid})` : 'ROOT/NONE';
        log(`buildNode: node.parent = ${parentInfo}, pos=(${node.position.x},${node.position.y})`);
        // The facade may have ignored `parent` and created under the scene root.
        // 2D UI renders only under the Canvas, so re-parent explicitly.
        if (parentUuid && (!node._parent || node._parent.uuid !== parentUuid)) {
            try {
                await sfm.setNodeParent({ parent: parentUuid, uuids: [nodeUuid] });
                log(`buildNode: re-parented under ${node._parent ? node._parent.name : parentUuid}`);
            } catch (e) {
                log(`buildNode: setNodeParent failed: ${e.message}`);
            }
        }
        const compClass = cc.js.getClassByName ? cc.js.getClassByName('sp.spine') : null;
        const comp = compClass ? node.getComponent(compClass) : null;
        if (comp) {
            comp.spineData = asset;
            log('buildNode: skeletonData bound on facade-created node');
        } else {
            log('buildNode: sp.spine component not found on created node');
        }
    } else {
        log('buildNode: cce.Node.query returned null');
    }

    if (cce.Engine && cce.Engine.repaintInEditMode) cce.Engine.repaintInEditMode();
    return node;
}

async function createNode (dragItem) {
    try {
        if (!dragItem || !dragItem.value) { log('createNode: no drag item value'); return; }
        const uuid = String(dragItem.value).replace(/@[\w]+$/, '');
        const node = await buildNode(uuid);
        log(node ? `createNode: created "${node.name}"` : 'createNode: buildNode returned null');
    } catch (e) {
        log(`createNode ERROR: ${(e && e.stack) || e}`);
    }
}

// Wrap the scene's createNodeByAsset so the Hierarchy panel (and any asset-drop
// message path that funnels through `create-node` -> createNodeByAsset) creates
// a node for sp.spineData too.
function wrapCreateNodeByAsset () {
    const mod = tryReachModule('builtin/scene/dist/script/3d/manager/node/create');
    if (!mod || typeof mod.createNodeByAsset !== 'function') {
        log('createNodeByAsset not reachable — hierarchy drop will not work');
        return;
    }
    const orig = mod.createNodeByAsset.bind(mod);
    mod.createNodeByAsset = async function (info) {
        log(`createNodeByAsset called: ${JSON.stringify({ type: info && info.type, uuid: info && info.uuid, assetUuid: info && info.assetUuid })}`);
        if (info && (info.type === 'sp.spineData' || info.assetUuid)) {
            // Only intercept when the caller actually targets a spine asset.
            const isSpine = info.type === 'sp.spineData';
            if (isSpine) {
                const node = await buildNode(info.uuid || info.assetUuid);
                if (node) return { node, canvasRequired: false };
            }
        }
        return orig(info);
    };
    log('createNodeByAsset wrapped for sp.spineData');
}

exports.load = function load () {
    log('--- load ---');
    // 1) Put our handler first so it dispatches before the built-in AnyHandler.
    const mod = tryReachHandlers();
    if (mod) {
        globalThis.__dropHandlersMod = mod;
        if (Array.isArray(mod.dropHandlers)) {
            if (!mod.dropHandlers.some((h) => h && h._spineDrop)) {
                mod.dropHandlers.unshift(makeHandler());
                log('sp.spineData drop handler registered (front of dispatch)');
            }
        }
    }
    // 2) Add sp.spineData to the drag acceptance gate. Only push — never rebuild
    //    the list, which would drop the editor's own droppable asset types.
    const utils = tryReachModule('builtin/scene/dist/script/utils/drag-drop-utils');
    if (utils && Array.isArray(utils.droppableAssetTypes) && !utils.droppableAssetTypes.includes('sp.spineData')) {
        utils.droppableAssetTypes.push('sp.spineData');
        log('sp.spineData added to droppableAssetTypes');
    }
    // 3) Wrap createNodeByAsset so the Hierarchy panel's drop also works.
    wrapCreateNodeByAsset();

    // 4) Diagnostic: the Hierarchy's ui-drag-area `droppable` gate is fed by the
    //    tree's droppableTypes. Probe whether it derives from the scene's
    //    NodeManager.creatableAssetTypes so we know what to extend.
    try {
        const nm = globalThis.cce && cce.Node;
        if (nm) {
            const cat = nm.creatableAssetTypes;
            log(`creatableAssetTypes: ${Array.isArray(cat) ? JSON.stringify(cat) : typeof cat}`);
            if (Array.isArray(cat) && !cat.includes('sp.spineData')) {
                cat.push('sp.spineData');
                log('creatableAssetTypes += sp.spineData');
            }
        } else {
            log('cce.Node not available for creatableAssetTypes');
        }
    } catch (e) {
        log(`creatableAssetTypes probe threw: ${e.message}`);
    }
};

exports.unload = function unload () {
    log('--- unload ---');
};

exports.methods = {
    // Handle the editor's "asset dropped onto a node" message
    // (contributions.inspector.drop.node -> message). Payload shape is logged.
    async inspectorDrop (args) {
        log(`inspectorDrop args: ${JSON.stringify(args)}`);
        const uuid = (args && (args.assetUuid || args.uuid)) || '';
        const node = args && (args.node || args.nodeUuid);
        log(`inspectorDrop: node=${JSON.stringify(node)} asset=${uuid}`);
        if (!uuid || !node) return;
        // node may be a uuid string or a node object
        const nodeUuid = typeof node === 'string' ? node : node.uuid;
        const nn = cce.Node.query ? cce.Node.query(nodeUuid) : null;
        if (nn) {
            await addComponentToNode(nn, uuid);
        } else {
            log(`inspectorDrop: node not found via cce.Node.query (${nodeUuid})`);
        }
    },
};
