'use strict';

/**
 * Reuses Creator's built-in Animation Socket property editor for this
 * extension's SpineSocket class. Creator 3.8 only enables that editor for its
 * three built-in socket class names, so without this adapter path falls back
 * to a plain text input even when the component implements querySockets().
 */

const fs = require('fs');
const path = require('path');

const CUSTOM_SOCKET_TYPE = 'sp.spine.SpineSocket';
const BUILTIN_SOCKET_TYPE = 'sp.Skeleton.SpineSocket';

function loadDefaultClassInspector () {
    const candidates = [
        path.join(
            Editor.App.path,
            '..',
            'resources',
            '3d',
            'engine',
            'editor',
            'inspector',
            'components',
            'class.js',
        ),
        path.join(
            Editor.App.path,
            'resources',
            '3d',
            'engine',
            'editor',
            'inspector',
            'components',
            'class.js',
        ),
    ];

    for (const filename of candidates) {
        if (!fs.existsSync(filename)) continue;
        if (Editor.Module && Editor.Module.__protected__) {
            const module = Editor.Module.__protected__.requireFile(filename);
            if (module) return module;
        }
        return require(filename);
    }

    throw new Error('Unable to locate Creator default component inspector');
}

const defaultInspector = loadDefaultClassInspector();

function adaptSocketDump (dump) {
    const sockets = dump && dump.value && dump.value.sockets;
    if (!sockets || !Array.isArray(sockets.value)) return;

    for (const socket of sockets.value) {
        if (socket && socket.type === CUSTOM_SOCKET_TYPE) {
            // The built-in editor only reads/writes path and target. Changing
            // this dump-only discriminator selects that editor without
            // changing the serialized runtime class.
            socket.type = BUILTIN_SOCKET_TYPE;
        }
    }
}

exports.template = defaultInspector.template;
exports.$ = defaultInspector.$;
exports.close = defaultInspector.close;
exports.update = function update (dump) {
    adaptSocketDump(dump);
    return defaultInspector.update.call(this, dump);
};
