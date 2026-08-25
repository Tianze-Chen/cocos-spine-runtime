# 从零制作一个 Cocos Creator 插件 —— 以 sp.spine 为例

> 本文以本仓库(`sp.spine` + `sp.spineData`,Cocos Creator 3.8.8)为真实样例,拆解一个「带原生运行时」的插件是怎么做出来的。覆盖:编辑器集成、资源导入器、拖拽、TS 类型引用、UIMesh 组件、native/wasm 编译、平台集成、以及发布后如何装进自建工程。
>
> 目标读者:想在 Cocos Creator 里做一个「有 C++ 原生运行时 + 自定义资源 + 自定义渲染组件」插件的人。

---

## 0. 先建立心智模型:一个插件长什么样

Cocos Creator 插件就是一个**放在项目 `extensions/` 目录下的文件夹**,里面有:

```
spine-runtime/
├── package.json      # 插件的「身份证」+「贡献点」声明
├── main.js           # 编辑器主进程入口(可选)
├── editor/           # 只在编辑器里跑的脚本(Node / CommonJS)
│   ├── importer/     #   资源导入器(asset-db)
│   ├── scene/        #   场景进程脚本(拖拽等)
│   ├── inspector/    #   Inspector 面板
│   └── build/        #   发布构建钩子(复制外置 wasm)
├── runtime/          # 运行时 TS(游戏里加载的脚本,asset-db 只读挂载)
│   ├── spine.ts      #   sp.spine 组件(extends UIMesh)
│   └── bindings/     #   后端加载器 + 不含二进制的 CJS wasm glue
└── native/           # C++ 原生侧(native 插件 + wasm 构建)
    ├── cc_plugin.json
    ├── *.cmake
    ├── jsb_*.cpp
    ├── third_party/spine-cpp/   # vendor 的第三方 C++ 源码
    └── wasm/
```

**核心概念 —— 三个进程**。编辑器里有三个隔离的 JS 进程,它们不共享内存、不能直接互相调用:

| 进程 | 跑什么 | 我们的代码 |
|---|---|---|
| **主进程 (main)** | `main.js`、扩展面板、菜单 | `main.js` |
| **场景进程 (scene)** | 场景/层级/Inspector 的交互 | `editor/scene/*.js`、`editor/inspector/*.js` |
| **asset-db worker** | 资源导入、解析 | `editor/importer/*.js` |

跨进程通信用 `Editor.Message.request('scene', 'execute-scene-script', {...})`。这是理解后面 importer / drop-handler 的关键。

---

## 1. package.json:插件的「贡献点」声明

一切从 `package.json` 开始。它声明了这个插件**接管编辑器的哪些能力**(叫 `contributions`):

```jsonc
{
    "name": "spine-runtime",
    "package_version": 2,
    "version": "1.0.0",
    "main": "./main.js",
    "editor": ">=3.8.0",
    "contributions": {
        "scene": { "script": "./editor/scene/drop-handle.js" },          // 场景进程脚本
        "inspector": {
            "section": { "node": { "sp.spine": "./editor/inspector/spine.js" } },
            "drop": { "node": [{ "type": "sp.spineData", "message": "inspector-drop" }] }
        },
        "messages": { "inspector-drop": { "methods": ["inspectorDrop"] } },
        "asset-db": {
            "script": "./editor/importer/asset-db-script",               // asset-db worker 脚本
            "mount":  { "path": "./runtime", "readonly": true },          // 把 runtime/ 只读挂载为项目脚本
            "asset-handler": [{ "name": "spine-skeleton", "extnames": [".json", ".skel"], "handler": "registerSpineSkeletonHandler" }]
        },
        "native": { "plugins": { "spine-runtime": { "cc_plugin.json": "./native/cc_plugin.json" } } },
        "builder": "./editor/build/builder.js"
    }
}
```

逐个解释:

- **`main`** → 指向主进程入口 `main.js`。
- **`scene.script`** → 在场景进程里加载一个脚本,通过 `load()/unload()/methods` 暴露能力(我们的 drop-handler 用它)。
- **`inspector.section`** → 给某个组件类名注册一个 Inspector 自定义面板。
- **`inspector.drop` + `messages`** → 把资源投放到已有节点时,将消息交给主进程的 `inspectorDrop` 方法。
- **`asset-db.mount`** → 把 `runtime/` 只读挂载成「项目脚本」,这样游戏运行时(场景/组件)能 import 它,而用户在资源面板里改不了它。**这是插件把 TS 运行时代码交付给游戏的方式。**
- **`asset-db.asset-handler`** → 声明「谁来导入 `.json` 资源」,即自定义资源导入器(第 3 节)。
- **`native.plugins`** → 声明原生插件,指向 `cc_plugin.json`(第 7 节)。
- **`builder`** → 注册发布构建钩子,把独立 `spine-runtime.wasm` 复制到非原生产物的 `cocos-js/`。

> 关键点:`asset-handler` 是 3.8.3 之后的**官方支持机制**(旧的 `importer` 贡献已废弃)。`mount` + `asset-handler` + `native.plugins` + `builder` 这四个贡献,组成了这个「带原生运行时和外置 wasm 的自定义资源型插件」骨架。

---

## 2. 编辑器主进程与发布构建

### 2.1 主进程入口 main.js

`main.js` 是主进程入口。本插件里它做两件事:

1. 扩展加载时把预编译 `spine-runtime.wasm` 同步到引擎的 `native/external/`,供编辑器场景和浏览器预览解析 `external:spine-runtime.wasm`;
2. 把主进程收到的 Inspector 拖拽消息**转发给场景进程**。

```js
exports.load = async function load() {
    try {
        await syncEditorWasm();
    } catch (e) {
        console.warn(`[spine-runtime] could not prepare editor WASM: ${e.message}`);
    }
};

exports.methods = {
    // Inspector 的节点面板发送的是三个参数:(dropItem, dumps, uuidList)
    async inspectorDrop (dropItem, dumps, uuidList) {
        const assetUuid = dropItem.value;
        const nodeUuids = uuidList.length ? uuidList : dumps.map((dump) => dump.uuid.value);
        for (const nodeUuid of nodeUuids) {
            await Editor.Message.request('scene', 'execute-scene-script', {
                name: 'spine-runtime',
                method: 'inspectorDrop',
                args: [nodeUuid, assetUuid],
            });
        }
    },
};
```

为什么需要它?`contributions.inspector.drop`(把资源拖到 Inspector 的节点上)会把消息发到**主进程**,但「创建组件/节点」必须在**场景进程**做。所以主进程收到 `inspectorDrop`,就通过 `Editor.Message.request('scene', 'execute-scene-script', ...)` 转发给场景进程里 `drop-handle.js` 的 `methods.inspectorDrop`。

关于这条链路,有三个**必须和编辑器对齐**的细节(都可以在编辑器源码里核对):

1. **`messages.methods` 里不要写 `default.` 前缀。** `"default.xxx"` 表示「名为 `default` 的**面板**上的方法」(例如 scene 包的 `default.create-component`)。本插件没有面板,方法在主进程 `main.js` 的 `exports.methods` 上,所以直接写 `"inspectorDrop"`——和内置 animator 的 `"dropClipToNode"` 一样。
2. **参数形态是 `(dropItem, dumps, uuidList)`。** 派发点在引擎仓库 `editor/inspector/contributions/node.js`:
   ```js
   const config = panel.dropConfig[info.type];
   if (config) {
       await Editor.Message.request(config.package, config.message, info, panel.dumps, panel.uuidList);
   }
   ```
   `dropItem`(即 `info`)是拖拽 `additional` 里的一项:`{ type: 'sp.spineData', value: '<资源 uuid>' }`。`type` 必须和 `contributions.inspector.drop.node[].type` 精确相等——也就是导入器 `assetType` 声明的类型名。
3. **整段调用被 `begin-recording` / `end-recording` 包住**,所以「加组件 + 绑资源」天然是一步撤销;场景侧不需要自己做快照,但要 `cce.Node.emit('change', node)` 让 Inspector 刷新、场景标记为已修改。

场景侧(`drop-handle.js`)用编辑器面向外部的加组件 API,而不是直接 `node.addComponent`:

```js
// 已有 sp.spine 就只重新绑定资源,否则先建组件
let comp = node.getComponent(cc.js.getClassByName('sp.spine'));
if (!comp) {
    cce.Node.createComponent(node.uuid, 'sp.spine');
    comp = node.getComponent(cc.js.getClassByName('sp.spine'));
}
comp.spineData = asset;
cce.Node.emit('change', node);
```

> 这正是引擎自己的做法,可参考 `engine-extensions/engine-extends/source/scene/drag-asset-onto-node-handlers/animation-graph.ts`(把 AnimationGraph 拖到节点上自动加 AnimationController)。

> 心智模型:主进程 = 消息总机;场景进程 = 真正改场景的地方;`execute-scene-script` 就是它们之间的总线。

### 2.2 editor/build:把外置 wasm 放进发布产物

`main.js` 只解决编辑器场景和浏览器预览的 wasm 文件位置。真正发布 Web 或小游戏时,构建产物是另一个目录,需要通过 `package.json` 的 `builder` 贡献接入构建生命周期:

```
editor/build/
├── builder.js   # 声明所有平台使用哪组 hooks
└── hooks.js     # onAfterBuild 中执行实际复制
```

`builder.js` 很薄,它把 `hooks.js` 注册到构建系统:

```js
exports.configs = {
    '*': {
        hooks: './hooks',
    },
};
```

`hooks.js` 在 `onAfterBuild` 阶段把预编译二进制复制到构建产物的 `cocos-js/`。这里只展示核心逻辑:

```js
const WASM_SOURCE = path.join(
    __dirname, '..', '..', 'native', 'wasm', 'prebuilt', 'spine-runtime.wasm',
);

exports.onAfterBuild = async function (options, result) {
    if (NATIVE_PLATFORMS.has(options && options.platform)) return;

    const cocosJsDir = findCocosJsDir(result); // result.dest 或 result.paths.output
    if (!cocosJsDir || !fs.existsSync(WASM_SOURCE)) return;

    fs.copyFileSync(WASM_SOURCE, path.join(cocosJsDir, 'spine-runtime.wasm'));
};
```

目标目录必须是 `cocos-js/`,因为 `runtime/bindings/index.ts` 给 `cc.wasm.instantiateWasm` 传的是裸文件名 `spine-runtime.wasm`:Web 端会相对引擎脚本解析它,小游戏端则把 `cocos-js/spine-runtime.wasm` 作为文件路径交给平台的 WebAssembly API。

| 运行场景 | 谁准备 wasm | 最终位置 |
|---|---|---|
| 编辑器场景 / 浏览器预览 | `main.js` | 引擎 `native/external/spine-runtime.wasm` |
| Web / 小游戏发布构建 | `editor/build/hooks.js` | 构建产物 `cocos-js/spine-runtime.wasm` |
| 原生包 / Native Simulator | 不复制 wasm | 只使用 JSB |

当前 hook 遇到源文件或目标目录缺失时只记录日志,不会中断整个构建。因此发布后若提示 wasm 找不到,先在构建日志确认出现了 `[spine-runtime] copied spine-runtime.wasm`，再检查产物的 `cocos-js/`。

---

## 3. 资源导入器(importer):让 `.json` 变成 `sp.spineData`

这是「自定义资源」插件的核心。分两层:

### 3.1 asset-db worker 脚本(注册层)

`package.json` 的 `asset-db.script` 指向 `editor/importer/asset-db-script.js`。asset-db worker 加载它并调用 `load()`。

本插件在这里**手动**做了两件事(注释里写明了原因:3.8.x 里 `asset-handler` 贡献的注册有时序 bug,包 enable 事件可能早于 asset-db worker 订阅,导致官方路径失效,所以直接手动注册):

1. 只在 asset-db worker 中注册一个最小的 `sp.spineData` 类(让 worker 能解析资源类型层级,不崩 Assets 面板);
2. 把 `spine-skeleton` handler 注册进 worker 的 handler manager。

```js
exports.load = function load() {
    registerAssetClass();   // 注册最小 sp.spineData 类
    registerHandler();      // 注册 spine-skeleton 处理器
};
```

> 注意:这里注册的是**asset-db worker 里**的最小类(避免面板崩溃),真正的 `SpineData` 类由挂载的 `runtime/` 脚本注册。`registerAssetClass()` 必须同时检查 `globalThis.AssetDB` 和 `cc.js.getClassByName('sp.spineData')`;否则 native 构建把扩展脚本与项目脚本放进同一 JS 环境时,会报 `A Class already exists with the same __cid__: sp.spineData`。

### 3.2 真正的处理器(spine-skeleton.js)

`asset-handler` 的核心是一个「handler 对象」,它有两个关键方法:

```js
module.exports = {
    name: 'spine-skeleton',
    assetType: 'sp.spineData',          // 产出的资源类型
    userDataConfig: {                    // 用户在资源 meta 面板可配的选项
        default: { scale: { displayName: 'Scale', type: 'number', default: 1 } },
    },

    // 1) 判断这个文件是不是我要的
    async validate(asset) {
        return isSpineSkeleton(asset);  // 读文件头几个字段,判断是不是 spine JSON
    },

    // 2) 真正的导入流程
    importer: {
        version: '1.0.0',
        async import(asset) {
            const fspath = asset.source;                 // 源文件绝对路径
            const jsonText = await fs.promises.readFile(fspath, 'utf8');

            const atlasPath = searchAtlas(fspath);       // 找同名 .atlas
            const atlas = parserAtlas(asset, atlasPath); // 解析 atlas → 贴图依赖

            // 组装序列化 payload —— __type__ 告诉引擎反序列化成哪个类
            const payload = {
                __type__: 'sp.spineData',
                _skeletonJsonStr: jsonText,
                _atlasText: atlas.atlasText,
                textures: atlas.texturesUUID.map(uuid => ({ __uuid__: uuid })),
                textureNames: atlas.textureNames,
                scale: (asset.userData && asset.userData.scale) || 1,
            };
            await asset.saveToLibrary('.json', JSON.stringify(payload));  // 写入 library
            return true;
        },
    },
};
```

三个核心 API:

- **`validate(asset)`** → 返回 `true` 才走导入。这里用「文件前 30 个字符里有没有 `slots/skins/bones/skeleton` 等关键字」+ `JSON.parse` 后 `Array.isArray(json.bones)` 来判定,避免误吞普通 `.json`。
- **`importer.import(asset)`** → 读源文件、解析依赖、产出序列化数据。
- **`asset.saveToLibrary('.json', payload)`** → 把结果写进引擎的 library(编译器产物)。`payload.__type__` 指定反序列化的类名,字段名要和运行时类(`SpineData`)的 `@property` 字段一一对应。

**依赖声明**用 `asset.depend(uuid)`:导入器声明「这个资源依赖哪些贴图/atlas」,它们变了,引擎会自动重新导入这个资源。贴图通过 `asset-db` 的 `queryAsset(filePath)` 反查 uuid,再拼上 sprite-frame 子资源后缀 `@6c48a`。

> 难点:handler 跑在 asset-db worker 进程,`@editor/asset-db` 不能 `require`(会走编辑器模块加载器失败),要用全局 `globalThis.AssetDB.queryAsset`。

---

## 4. 场景拖拽(drop-handler):把资源拖成节点

`editor/scene/drop-handle.js` 通过 `contributions.scene.script` 在场景进程加载,`load()` 里做三件事(这是本插件最「脏」但最必要的一环):

1. **把自己的 drop handler unshift 到拖拽分发队列最前面**——因为编辑器内置的 `AnyHandler`(接受所有类型)排在前面会对自定义类型 no-op,导致 `sp.spineData` 拖进场景没反应。
2. **把 `sp.spineData` 加进 `droppableAssetTypes`**(拖拽接受门槛,和 handler 是两套)。
3. **wrap `createNodeByAsset`**——让 Hierarchy 面板的拖拽路径也走自己的建节点逻辑。

```js
exports.load = function load() {
    const mod = tryReachHandlers();                 // 摸到编辑器内部 handler 注册表
    mod.dropHandlers.unshift(makeHandler());        // 1) 插到最前
    utils.droppableAssetTypes.push('sp.spineData'); // 2) 加进接受门槛
    wrapCreateNodeByAsset();                        // 3) 接管 Hierarchy 拖拽
};
```

**handler 的 `onDrop`** 负责真正建节点。关键点:**用 `cce.SceneFacadeManager.createNode / createComponent`,而不是裸的 `scene.addChild`** —— 前者会正确接入层级、undo/redo、场景保存。

```js
async onDrop(event, dragItems) {
    const item = dragItems[0];
    // 先 raycast 判断是不是拖到了某个已有节点上 → 加组件;否则建新节点
    const nodes = this.getRaycastResultNodes(event.clientX, event.clientY);
    if (nodes?.length) { await addComponentToNode(nodes[0], item.value); return; }
    await createNode(item);
}

async function buildNode(uuid) {
    const sfm = cce.SceneFacadeManager;
    const created = await sfm.createNode({ name: 'spine', parent: canvasUuid, snapshot: true });
    await sfm.createComponent({ uuid: created, component: 'sp.spine' });
    // 再把资源绑上去
    const node = cce.Node.query(created);
    node.getComponent('sp.spine').spineData = await loadAsset(uuid);
    cce.Engine.repaintInEditMode();
}
```

> 诚实说明:这一段严重依赖**编辑器未公开的内部 API**(`builtin/scene/dist/.../drag-drop/handlers`、`cce.SceneFacadeManager`、`droppableAssetTypes`)。代码里满是 `tryReachModule` + `try/catch` 的防御,因为不同 3.8.x 小版本路径可能变。这是 3.8 编辑器「自定义资产拖入场景」没有官方入口下的无奈之举,升级引擎时要重点回归测试。

---

## 5. TS 类型:开发 runtime TS 怎么引用引擎类型

这是很多人卡住的地方。本仓库**没有 `tsconfig.json`、没有 `.d.ts` 文件**,但 `runtime/*.ts` 却能写 `import { _decorator } from 'cc'`。原理:

**`cc` 模块的类型由 Cocos Creator 编辑器/工程环境提供,不是插件自带。** 当插件挂到 `extensions/` 后,它的 TS 是被**引擎的构建系统**(不是你的本地 tsc)用工程生成的 tsconfig(如 `temp/tsconfig.cocos.json`)编译的,那个环境里已经声明了 `cc`、`cc/env` 这些模块。

所以正确的写法就是:

```ts
import { _decorator, Color, Component } from 'cc';
import type { UIMeshSegment } from 'cc';   // 类型用 import type,避免打进产物
```

- **`cc`** → 引擎主模块(组件、节点、数学、渲染 API)。
- **`cc/env`** → 编译期宏(`EDITOR`、`NATIVE`、`WECHAT` 等),用来按平台分支。

**那本地 IDE 想要类型提示怎么办?** 两种:

1. 最省事:在**一个 Cocos Creator 工程里**开发(把插件放 `extensions/`),编辑器会生成 `temp/tsconfig.cocos.json`,IDE 自动识别 `cc` 类型。
2. 独立开发时,给插件写一个 `tsconfig.json`,用 `paths` 把 `cc` 映射到 Creator 安装目录里的声明文件(具体路径随版本变,一般是 `<Creator>/resources/resources/3d/engine/...` 下,或 `@types/cc`)。

**编辑器侧的脚本(main.js / editor/*.js)则不同**:它们是**纯 JS(CommonJS)**,跑在 Node 里,没有 TS。类型只能靠 JSDoc 或 `import()` 类型标注,例如:

```js
/**
 * @param {import('@editor/asset-db').Asset} asset
 */
async import(asset) { ... }
```

> 一句话总结:**游戏运行时用 TS + `import from 'cc'`(类型来自编辑器环境);编辑器脚本用 JS + `require('cc')` / 全局 `Editor`、`cce`(类型靠 JSDoc)**。两条线别混。

---

## 6. 用 UIMesh 写渲染组件

`sp.spine` 组件没有自己走 `cc.MeshRenderer`,而是继承引擎的 **`cc.UIMesh`** —— 一个「只管给引擎喂顶点/索引,引擎负责 buffer、合批、提交」的通用 2D 网格消费者。用它,自定义 2D 渲染就能被引擎的 2D 合批管线收编。

### 6.1 组件骨架

```ts
import { _decorator, Color } from 'cc';
import type { UIMeshSegment } from 'cc';

const { property } = _decorator;

export class Spine extends UIMesh {          // 关键:extends UIMesh
    @property({ visible: false, type: SpineData })
    protected _spineData: SpineData | null = null;
    // ... 其它属性
}
```

### 6.2 每帧喂网格:setMeshData

核心方法是 `setMeshData`,它接收**一段顶点缓冲 + 一段索引缓冲 + 若干 segment**:

```ts
private _updateMeshData(): void {
    const rd = spine().runtimeRenderData(this._runtime);   // C++ 返回的渲染数据
    const stride = this._useTint ? 28 : 24;                // 顶点格式:24B 单色 / 28B 双色
    const heap = this._heap();                             // wasm 读 HEAPU8 / JSB 读 staging buffer
    const vertexData = heap.slice(rd.vPtr, rd.vPtr + rd.vertexCount * stride);
    const indexData  = heap.slice(rd.iPtr, rd.iPtr + rd.indexCount * 2);

    // 按 (贴图, 混合模式) 分组 → 每个 segment 一个 draw call
    const segments: UIMeshSegment[] = [];
    let indexOffset = 0;
    for (const seg of rd.segments) {
        const texture  = this._textureMap.get(seg.textureId);
        const material = this._segmentMaterial(seg.blendMode);
        segments.push({ indexOffset, indexCount: seg.indexCount, texture, material });
        indexOffset += seg.indexCount;
    }

    this.setMeshData({ vertexCount: rd.vertexCount, vertexStride: stride, vertexData,
                       indexCount: rd.indexCount, indexData, segments });
}
```

要理解的点:

- **`UIMeshSegment`** = `{ indexOffset, indexCount, texture, material }`。一个 segment 对应一次「换贴图/换材质」,也就是一次 draw call。多图集 Spine 就是在这里自然分段(多一个贴图 = 多一个 segment)。
- **`vertexData` / `indexData`** 是引擎预期的顶点格式(`V3F_T2F_C4B` 24 字节,或带 dark color 的 28 字节)。**格式必须和引擎 2D 顶点格式一致**,否则渲染错乱。
- **变换烘焙**:本插件把「y 翻转 + 世界变换」提前在 C++ 里做进顶点(`runtimeSetOutputTransform`),所以这里**不用**再逐顶点乘变换矩阵 —— 合批时也不需要 per-node 变换 uniform。这是能合批的关键。
- **`markForUpdateRenderData()`** → 数据脏了主动调用,让引擎重新上传 buffer。

### 6.3 四个容易忽略的细节

```ts
// 1) 让渲染实体用「世界空间」而非本地空间(因为我们烘焙了世界变换)
this._renderEntity.setUseLocal(false);

// 2) JSB 端所有组件共用一个 staging buffer,提交前要自己 slice 一份,
//    否则下一个组件写 buffer 会覆盖你还没提交的数据
const vertexData = heap.slice(...);   // 见上面,已经 slice

// 3) UIMesh.onLoad() 会创建/初始化渲染实体,子类覆写生命周期时必须调用
public onLoad(): void {
    super.onLoad();
    // ...Spine 自己的初始化
}
```

4. **PMA 必须与资源导出方式一致**:`premultipliedAlpha` 默认是 `false`,适配普通 straight-alpha PNG;只有 Spine 图集明确按 PMA 导出时才开启。创建 runtime 后要立即 `applyParams()`,保证 C++ 顶点颜色打包和材质混合从第一帧起使用同一约定。

---

## 7. 原生编译(native + JSB)

### 7.1 引擎的原生插件机制

`package.json` 的 `contributions.native.plugins` 指向 `native/cc_plugin.json`:

```json
{
    "name": "spine-runtime",
    "version": "1.0.0",
    "engine-version": ">=3.8.0",
    "platforms": ["android", "ios", "windows", "mac"],
    "modules": [{ "target": "spine_runtime" }]
}
```

构建原生工程时,引擎的插件扫描会递归找 `cc_plugin.json`,把 `target`(`spine_runtime`)的 `_ROOT` 指向 `native/<平台>/`,生成 `Pre-AutoLoadPlugins.cmake` 并 `find_package(spine_runtime REQUIRED)`。

`native/<平台>/spine_runtime-config.cmake` 只有一行,加载共享构建脚本:

```cmake
include("${CMAKE_CURRENT_LIST_DIR}/../spine_runtime.cmake")
```

### 7.2 spine_runtime.cmake:把 C++ 编成静态库

```cmake
file(GLOB SPINE_4_3_CORE_SRC "${SPINE_CPP_DIR}/src/spine/*.cpp")   # spine-cpp 4.3 源码

# 关键:把 spine-cpp 4.3 编进私有命名空间 spine43,避免和引擎内置 spine 3.8 冲突
set_source_files_properties(
    "${SPINE_RUNTIME_ROOT}/spine-adapter/SpineRuntime.cpp"
    ${SPINE_4_3_CORE_SRC}
    PROPERTIES COMPILE_DEFINITIONS "spine=spine43")

add_library(spine_runtime STATIC
    "${SPINE_RUNTIME_ROOT}/spineruntime_plugin.cpp"     # 插件入口
    "${SPINE_RUNTIME_ROOT}/jsb_spineruntime_manual.cpp" # JSB 绑定
    "${SPINE_RUNTIME_ROOT}/spine-adapter/SpineRuntime.cpp"
    ${SPINE_4_3_CORE_SRC})

target_compile_definitions(spine_runtime PUBLIC CC_PLUGIN_STATIC ENABLE_JSON_PARSER=1 ...)
```

三个要点:

1. **`spine=spine43` 命名空间隔离** —— spine-cpp 4.3 和引擎内置 spine 3.8 都用 `spine::` 命名空间,会撞符号。`COMPILE_DEFINITIONS "spine=spine43"` 让 4.3 的所有符号落进 `spine43::`,互不冲突。这是「同一可执行文件里共存两个 spine 版本」的关键技巧。
2. **`CC_PLUGIN_STATIC`** —— 让 `CC_PLUGIN_ENTRY` 生成无修饰的 `extern "C" cc_load_plugin_<name>()`。
3. **`find_package` 依赖 `COCOS_X_PATH`**(引擎 native 路径),插件本身不含引擎,编译时才和引擎链接。

### 7.3 插件入口:把 JSB 绑定注册进引擎

```cpp
// spineruntime_plugin.cpp
CC_PLUGIN_ENTRY(spine_runtime, loadSpineRuntimePlugin)

void loadSpineRuntimePlugin() {
    se::ScriptEngine::getInstance()->addRegisterCallback(registerSpineRuntimeBinding);
}
bool registerSpineRuntimeBinding(se::Object* globalObj) {
    return register_all_spineruntime_manual(globalObj);  // 把函数挂到 globalThis.spineruntime
}
```

引擎启动时会调用 `cc_load_all_plugins()` → 触发上面的回调 → `jsb_spineruntime_manual.cpp` 用 `se::Object`/`se::Class` 把 C++ 函数暴露成 `globalThis.spineruntime.xxx`。之后 TS 层直接读这个全局对象即可(见第 9 节)。

> JSB 手动绑定是这套里最繁琐的部分:每个函数都要写 `se::ScriptEngine` 的 `defineFunction` + 参数转换。但好处是**零依赖**(不需要 binding 生成工具),且和 embind 共享同一份 `SpineRuntime.h` 接口。

### 7.4 Native Simulator 不是普通原生工程

Creator 的 stock Native Simulator 不扫描项目 `extensions/` 中的 `cc_plugin.json`,它自己的 `Game` 也不会经过普通原生模板调用插件注册表。因此仅安装插件不能让 Simulator 获得 `globalThis.spineruntime`。

仓库里的 `native/spine_runtime_simulator.cmake` 是显式构建 hook:它把 `spine_runtime` 和 `plugin_registry` 链进 Simulator,并在构建期生成带 `cc_load_all_plugins()` 调用的 `Game.cpp` 副本,不会改写引擎源码。配置一次即可:

```powershell
cmake -S <engine>/native/tools/simulator/frameworks/runtime-src -B <engine>/native/simulator -DCMAKE_PROJECT_INCLUDE=<插件绝对路径>/native/spine_runtime_simulator.cmake
cmake --build <engine>/native/simulator --config Debug
```

Native Simulator 设置了 `NATIVE`,运行时只认 JSB。绑定没有注册时应直接报错,不能回退到 wasm,否则会把 Simulator 的原生接入问题掩盖掉。

---

## 8. wasm 编译(emsdk + AOT Embind + 外置文件)

Web、编辑器/浏览器预览和小游戏不能使用 JSB,改用 **Emscripten + embind**,把同一份 C++ 编成独立的 `.wasm`。这些环境全部通过引擎的 `cc.wasm` 接口加载同一个二进制;原生环境不会走这条路径。

### 8.1 构建脚本

`native/wasm/CMakeLists.txt` 用 emcmake 驱动:

```cmake
# 用法:source <emsdk>/emsdk_env.sh && emcmake cmake -S native/wasm -B native/wasm/build && cmake --build ...
add_executable(spine-runtime
    ${SPINE_CORE_SRC}                    # spine-cpp 4.3
    ${SPINE_ADAPTER_DIR}/SpineRuntime.cpp
    spine-runtime-bindings.cpp)          # embind 绑定

target_link_options(spine-runtime PRIVATE
    "SHELL:-mno-reference-types"        # 兼容微信的 MVP call_indirect 校验
    "SHELL:-mno-bulk-memory"
    "SHELL:-s DYNAMIC_EXECUTION=0"      # 禁止 eval/new Function
    "SHELL:-s EMBIND_AOT=1"             # 构建期生成 Embind 调用器
    "SHELL:-s WASM=1"
    "SHELL:-s EXPORT_ES6=1"              # 产出 ESM
    "SHELL:-s MODULARIZE=1"
    "SHELL:-s EXPORT_NAME=SpineRuntime"  # 模块化工厂名
    "SHELL:-s EXPORTED_RUNTIME_METHODS=['HEAPU8',...]"  # 暴露堆视图
    "SHELL:--bind")                      # 启用 embind
```

**AOT Embind** 的含义是把 Embind 原本可能在运行时通过 `new Function` 生成的参数转换/调用包装器提前到编译期生成。微信小游戏等沙箱禁止动态执行代码,所以这里同时使用 `EMBIND_AOT=1` 与 `DYNAMIC_EXECUTION=0`。`-mno-reference-types -mno-bulk-memory` 则避免新版 LLVM 产出微信旧校验器不接受的 WASM 编码。

embind 绑定比 JSB 简洁得多,声明式即可:

```cpp
EMSCRIPTEN_BINDINGS(spine_runtime) {
    function("createDataJson", &createDataJson);
    function("runtimeSetOutputTransform", &runtimeSetOutputTransform);
    // ...
}
```

### 8.2 生成统一的外置 wasm glue

Emscripten 生成 ESM glue 和独立 `.wasm`。本插件保留独立二进制,只把 ESM glue 转成 asset-db 挂载脚本可加载的 CJS:

1. `native/wasm/scripts/gen-glue-file.js` 去掉 CJS 中非法的 `import.meta.url` 和不适用的环境守卫;
2. 把 `export default` 改成 `module.exports`,写入 `runtime/bindings/spine-runtime.js`;
3. 不嵌入 wasm;运行时注入 Emscripten 的 `instantiateWasm` hook,转交 `cc.wasm.instantiateWasm`。

`gen-glue.js` 只是兼容旧命令名的入口,当前会委托 `gen-glue-file.js`。

```bash
emcmake cmake -S native/wasm -B native/wasm/build
cmake --build native/wasm/build
cp native/wasm/build/spine-runtime.js native/wasm/build/spine-runtime.wasm native/wasm/prebuilt/
node native/wasm/scripts/gen-glue.js
```

必须同时更新 prebuilt 的 `.js` 和 `.wasm`,否则 glue 与二进制中的 Embind 注册表可能不匹配。`main.js` 在扩展加载时把 wasm 放到引擎 `native/external/`(编辑器/浏览器预览);`editor/build/hooks.js` 在非原生构建后把它放到产物的 `cocos-js/`(Web/小游戏)。

> 记忆点:函数注册在 **wasm 二进制**里(Embind),不是 CJS glue 文本。验证新绑定时应检查新生成的 `.wasm`,并确认它和 glue 来自同一次构建。

---

## 9. 平台集成:同一套 TS,两个后端

`runtime/bindings/index.ts` 里是整件事的精髓 —— **先按 `NATIVE` 做严格分流**,而不是“有 JSB 就用、没有就回退 wasm”:

```ts
function nativeBinding(): SpineRuntimeBinding | null {
    return globalThis.spineruntime ? globalThis.spineruntime : null;   // JSB
}

async function loadWasmFactory() {
    const glue = await import('./spine-runtime.js');                  // embind
    return glue.default;
}

export async function loadSpineRuntime() {
    if (NATIVE) {
        const native = nativeBinding();
        if (!native) throw new Error('native JSB binding is unavailable');
        return native;                                                // 原生/Simulator:仅 JSB
    }
    const factory = await loadWasmFactory();
    return factory({
        instantiateWasm(imports, done) {
            cc.wasm.instantiateWasm(WASM_BINARY, imports)
                .then((result) => done(result.instance));
        },
    });                                                               // 所有非原生环境:同一外置 wasm 路径
}
```

| 平台 | 后端 | 绑定来源 | 渲染数据来源 |
|---|---|---|---|
| 编辑器场景 / 浏览器预览 | wasm (AOT Embind) | CJS glue + `cc.wasm` 加载 `external:spine-runtime.wasm` | `Module.HEAPU8` |
| Web / 微信等小游戏构建 | wasm (AOT Embind) | CJS glue + `cc.wasm` 从产物 `cocos-js/` 加载独立 wasm | `Module.HEAPU8` |
| Android / iOS / Windows / macOS 原生包 | JSB | `globalThis.spineruntime` | TS 分配的 4MB staging buffer |
| Windows Native Simulator | JSB | 重建 Simulator 时链接的 `globalThis.spineruntime` | TS 分配的 4MB staging buffer |

- **非原生**:wasm 预编译产物已提交,无需用户安装 emsdk;但自定义引擎必须公开 `cc.wasm`(`pal/wasm`)。
- **原生**:普通原生包由插件机制自动编译 JSB;Native Simulator 需按 7.4 节重建。原生没有 `HEAPU8`,所以 TS 会 `new Uint8Array(4*1024*1024)` 注册为 staging buffer,C++ 把顶点拷进去,`vPtr/iPtr` 变成 buffer 内偏移。

> 这就是「一份 TS + 一份 SpineRuntime.h,两个后端」的全部秘密:把**所有平台差异收敛到 `bindings/index.ts` 的加载函数**和 `_heap()` 两个点,业务组件层完全无感。

---

## 10. 集成到自建工程

### 方式 A:商店一键安装

发布到 [Cocos 扩展商店](https://store.cocos.com)后,用户可从 Creator 安装到工程的 `extensions/`。提交包应让 `package.json` 位于扩展根目录;包体、依赖和审核限制以扩展商店当期规则为准。

### 方式 B:手动下载

把插件文件夹(含 `package.json` 的那一层)整个放进项目的 `extensions/`:

```
my-project/
└── extensions/
    └── spine-runtime/      # ← 整个插件文件夹放这里
        ├── package.json
        ├── main.js
        ├── editor/
        ├── runtime/
        └── native/
```

放进去后 Creator 会自动识别,`runtime/` 被只读挂载成项目脚本,`native/` 在出普通原生包时自动参与编译,`editor/build/` 负责非原生产物中的 wasm 文件复制。

> 装好后「开箱即用」的边界:目标引擎必须包含 `cc.UIMesh` 与公开的 `cc.wasm` 导出;Web/小游戏使用预编译的独立 wasm,普通原生包由引擎自动编译插件,Native Simulator 需按 7.4 节重建。spine-cpp 已 **vendor 到 `native/third_party/`**,不需要用户运行 `git submodule`。

---

## 11. 踩坑清单(来自本项目的真实经验)

1. **符号冲突**:双 spine 版本共存必须命名空间隔离(`spine=spine43`),否则链接期静默选错符号,运行时崩。
2. **asset-db 时序与类重复**:3.8.x 的 `asset-handler` 注册有时序 bug,必要时在 `load()` 里手动注册;最小资产类只能在 asset-db worker 注册,并先用 `getClassByName` 去重,否则 native 构建会出现重复 `__cid__`。
3. **拖拽无官方入口**:自定义资产拖进场景要靠编辑器内部 API,`try/catch` + 降级链要写足,升级引擎必回归。
4. **顶点格式对齐**:UIMesh 的 `vertexStride` 必须和引擎 2D 顶点格式一致(24B/28B),错一个字节渲染全乱。
5. **JSB staging buffer 共享**:多组件共用一个 buffer,提交前要 `slice` 复制,否则被覆盖。
6. **wasm 文件位置**:glue 与二进制分离后,编辑器预览要同步到 `native/external/`,发布构建要复制到 `cocos-js/`;统一交给 `cc.wasm`,不要为微信再维护另一套 loader。
7. **小游戏限制**:使用 AOT Embind 并关闭动态代码执行;微信还需要 MVP 兼容的 WASM 编码选项。
8. **Native Simulator 不等于浏览器预览**:它设置 `NATIVE` 且只支持 JSB,stock Simulator 不会自动扫描工程插件。
9. **UIMesh 生命周期**:覆写 `onLoad()` 时必须调用 `super.onLoad()`,否则渲染实体未完成初始化,页面可能没有内容。
10. **TS 类型别乱配**:`cc` 类型来自编辑器环境;编辑器脚本是 JS 不是 TS,两条线分开。

---

## 附:目录速查

| 文件 | 作用 |
|---|---|
| `package.json` | 贡献点声明(scene/inspector/asset-db/native/builder) |
| `main.js` | 主进程入口,同步编辑器预览 wasm + 消息转发 |
| `editor/build/builder.js` / `hooks.js` | 非原生发布构建复制外置 wasm |
| `editor/importer/asset-db-script.js` | asset-db worker 注册 |
| `editor/importer/spine-skeleton.js` | 资源导入器 handler |
| `editor/scene/drop-handle.js` | 场景拖拽建节点 |
| `editor/inspector/spine.js` | Inspector 面板 |
| `runtime/spine.ts` | `sp.spine` 组件(extends UIMesh) |
| `runtime/bindings/index.ts` | 按 `NATIVE` 严格选择 JSB / 外置 wasm,并接入 `cc.wasm` |
| `native/spineruntime_plugin.cpp` | 原生插件入口(CC_PLUGIN_ENTRY) |
| `native/jsb_spineruntime_manual.cpp` | JSB 手动绑定 |
| `native/spine_runtime_simulator.cmake` | stock Native Simulator 的 JSB 插件构建 hook |
| `native/wasm/spine-runtime-bindings.cpp` | embind 绑定 |
| `native/wasm/scripts/gen-glue-file.js` | 把 ESM glue 转为不内嵌二进制的 CJS glue |
