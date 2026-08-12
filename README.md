# sp.spine — Spine 4.3 Runtime for Cocos Creator

一个 Cocos Creator 插件。通过 `SpineRuntime` 绑定驱动 **spine-cpp 4.3** 原生运行时,以 **cc.UIMesh** 消费者渲染。Web、编辑器/浏览器预览和小游戏走外置 wasm(embind),原生包与 Native Simulator 严格走 JSB(手动绑定),两侧共用同一份 TypeScript 层。

```
spine-cpp 4.3  +  SpineRuntime 封装(C++)
        │
        ├── embind 绑定(web / minigame)  native/wasm/spine-runtime-bindings.cpp
        └── JSB 手动绑定(native/simulator) native/jsb_spineruntime_manual.cpp
        │
        ▼
    Spine 组件 + SpineData 资源 + 兼容包装(runtime/*.ts)
        │
        ▼
    cc.UIMesh 消费者(引擎负责 buffer、合批、提交)
```

---

## 目录结构

| 路径 | 内容 |
|---|---|
| `runtime/` | TS 组件层。作为 asset-db 只读挂载,场景运行时加载 |
| `runtime/spine.ts` | `sp.spine` 组件类(`Spine extends UIMesh`),核心 API |
| `runtime/spine-data.ts` | `sp.spineData` 资源类(`SpineData extends Asset`) |
| `runtime/runtime-data.ts` | `RuntimeData`(Data 句柄包装)/ `RuntimeTextureMap`(纹理 id→Texture2D) |
| `runtime/runtime-objects.ts` | spine-ts 兼容包装:`TrackEntry / Bone / Slot / Attachment / Animation / Skin / Event / AnimationState` |
| `runtime/bindings/index.ts` | 绑定加载器（`loadSpineRuntime` / `spine`）+ `SpineRuntimeBinding` 接口 |
| `editor/importer/spine-skeleton.js` | 编辑器资源导入器:`.json`(+ 同名 `.atlas` + `.png`)→ `sp.spineData` |
| `editor/scene/drop-handle.js` | 编辑器场景拖拽处理 |
| `editor/build/` | 构建钩子:把外置 `spine-runtime.wasm` 复制到非原生构建的 `cocos-js/` |
| `native/spine-adapter/` | 本项目的 C++ 适配层：`SpineRuntime.h/.cpp` 与独立 POC |
| `native/third_party/spine-cpp/` | 官方 spine-cpp 4.3 源码（vendored，固定快照 `51d8d78b`，随仓库分发） |
| `native/wasm/` | wasm 平台构建与工具（CMake、embind、prebuilt、`scripts/gen-glue.js`） |
| `native/spine_runtime_simulator.cmake` | 把 JSB 插件链接进 Creator Native Simulator 的构建 hook |

---

## 工作原理

**不透明句柄 + 自由函数**。`Data`/`Runtime` 以指针值(句柄)暴露给 JS,所有操作通过顶层自由函数进行,例如 `createDataJson(json, atlas, texNames, scale)`、`runtimeSetAnimation(handle, track, name, loop)`、`runtimeRenderData(handle)`。句柄用后须显式 `disposeData` / `disposeRuntime` 释放。

**渲染路径**(每帧):
1. `runtimeSetOutputTransform(...)` — 把节点世界变换与 y 翻转交给 C++ 烘焙进顶点
2. `runtimeUpdate(handle, dt)` — 推进动画/姿态/几何
3. `runtimeRenderData(handle)` — 返回 `{vertexCount, indexCount, vertexStrideBytes, vPtr, iPtr, segments[], segmentCount, indexOverflow}`;`vPtr`/`iPtr` 是内存缓冲区的字节偏移
4. TS 用 `heap.slice(vPtr, vPtr + len)` 取得自有顶点/索引副本,按 segment 分组后交给 `UIMesh.setMeshData(...)`

**两种后端的"堆"来源不同**:
- **wasm**: 直接读 `Module.HEAPU8`,`vPtr`/`iPtr` 是 wasm 线性内存地址
- **JSB**: 原生没有 HEAPU8,TS 分配一个 4MB `Uint8Array` 并通过 `setRenderBuffer` 注册;`runtimeRenderData` 把 C++ 顶点/索引拷入该 buffer,`vPtr=0`、`iPtr=vertexBytes`。读取路径由 `spine.ts` 的 `_heap()` 按平台分支。

**事件**: C 回调桥接回 JS(全局注册表按句柄存 JS 回调),每帧 `runtimeUpdate` 时同步派发 START / INTERRUPT / END / COMPLETE / DISPOSE / EVENT。

---

## 安装与接入

1. 把插件目录放入项目的 `extensions/`(或按 store 插件流程安装)。
2. `package.json` 中的 `contributions` 负责:
   - `asset-db.mount`: 把 `runtime/` 只读挂载为资源,场景脚本可加载
   - `asset-db.asset-handler`: 注册 `spine-skeleton` 处理器,接管 `.json` 骨骼资源导入
   - `native.plugins`: 声明原生插件 `spine-runtime`
   - `builder`: 非原生构建结束后把外置 wasm 放入产物的 `cocos-js/`
3. 引擎侧需要:
   - `cc.UIMesh` 消费者可用
   - 非原生目标可用 `cc.wasm.instantiateWasm`(本项目的自定义引擎通过 `exports/webassembly.ts` 导出 `pal/wasm`)
   - 内置资源 `default-spine-material`(`spine.ts` 的 `updateMaterial()` 会取它)

> `native/cc_plugin.json` 及其配套 CMake 配置已在仓库中提供(见「构建」章节)。原生端通过引擎的原生插件机制接入:`find_package` 加载平台 `spine_runtime-config.cmake`,`cc_load_plugin_spine_runtime()` 在引擎启动时注册 JSB 绑定。要求引擎开启 `USE_PLUGINS`(默认开)。
>
> Creator 的 stock Native Simulator 不扫描项目扩展。它属于原生环境,不会回退到 wasm;首次使用前需要通过 `native/spine_runtime_simulator.cmake` 重建 Simulator,详见「构建」。

---

## 资源导入

把 **spine 4.3 导出的 `.json`** 放进项目资源目录,同名 `.atlas`(或 `.txt` / `.atlas.txt`)和贴图 `.png` 应放在旁边。

- 导入器自动查找同名 atlas、解析纹理页、建立 `Texture2D` 依赖,生成 `sp.spineData` 资源
- 支持 **JSON 格式**(`skeleton + slots + skins + animations + bones`)
- **不支持二进制 `.skel`**(导入会直接报错)
- 在资源 meta 的 Inspector 里可配置 **Scale**(缩放骨骼位置与图像大小)

---

## 在编辑器中使用

1. 把 `sp.spineData` 资源**拖进场景** → 自动创建带 `sp.spine` 组件的节点(菜单:`Spine/spine`,类名 `sp.spine`,组件继承 `UIMesh`,支持编辑器内运行)。
2. 在 Inspector 上:
   - **spineData**: 指定骨骼资源(赋值即加载)
   - **defaultAnimation / defaultSkin**: 加载后自动播放的动画 / 自动应用的皮肤
   - **loop**: 默认循环
   - **premultipliedAlpha**(默认 false；仅当 Spine 图集以 PMA 导出时开启)、**useTint**(默认 false)、**timeScale**
   - **sockets**: 挂点列表(`path` 为骨骼路径,`target` 为要同步的节点)
3. 或手动:创建节点 → 添加组件 `sp.spine` → 赋值 `spineData`。

## 在代码中使用

```ts
import { Node, Texture2D } from 'cc';

// 组件通常从节点上取(类注册名 sp.spine;也可 import { Spine } 使用类型)
const spine = node.getComponent('sp.spine') as any;

// 播放
spine.setAnimation(0, 'walk', true);           // 轨道 0 播放 walk,循环
spine.addAnimation(0, 'run', true, 0);         // 排队:walk 结束后切 run
spine.setAnimation(0, 'attack', false);
spine.clearTrack(0);                           // 清除某轨道
spine.clearTracks();                           // 清所有轨道,回 setup pose
spine.setSkin('guild-1');                      // 换肤
spine.setMix('walk', 'run', 0.2);              // 设置过渡时长

// 事件(可选,任一 listener 设置后即绑定)
spine.setStartListener((entry) => { /* entry: TrackEntry */ });
spine.setCompleteListener((entry) => { });
spine.setEventListener((entry, ev) => {
    // ev: Event —— name / intValue / floatValue / stringValue / audioPath ...
    console.log('spine event:', ev.name, ev.intValue);
});

// 骨骼 / 槽位 / 附件
const bone = spine.findBone('arm');
console.log(bone?.worldX, bone?.worldY);
const slot = spine.findSlot('weapon');
slot?.setAttachment('sword');
slot?.setColor(1, 0, 0, 1);
spine.setSlotTexture('weapon', someTex2d, true);   // 局部换装
spine.setSlotsRange(0, 5);                           // 限制渲染槽位范围

// 挂点(每帧自动把骨骼变换同步到目标节点)
spine.sockets = [{ path: 'hand.right', target: node2 }];

// 暂停 / 变速
spine.paused = true;
spine.timeScale = 0.5;

// 程序化加载(不走资源导入流程)
const ok = spine.loadFromJson(jsonText, atlasText, [tex2d], ['tex.png'], 1);
```

组件类注册名是 `sp.spine`;运行时导出入口 `runtime/index.ts` 提供 `Spine`、`SpineSocket`、`SpineData`、`SpineMaterialType` 和 `SpineAnimationCacheMode`。

---

## API 速查

### Spine 组件(`sp.spine`,extends UIMesh)

| 类别 | 成员 |
|---|---|
| 属性 | `spineData`、`defaultAnimation`、`defaultSkin`、`animation`、`loop`、`timeScale`、`premultipliedAlpha`、`useTint`、`paused`、`sockets`、`cacheMode`(恒 REALTIME) |
| 加载 | `loadFromJson(json, atlas, textures[], textureNames?, scale?)`、`setSkeletonData(data)` |
| 播放 | `setAnimation(track, name, loop?)`、`addAnimation(track, name, loop, delay?)`、`findAnimation(name)`、`getCurrent(track)`、`clearAnimation(track?)`、`clearAnimations()`、`clearTrack(track)`、`clearTracks()`、`setMix(from, to, duration)` |
| 皮肤/姿势 | `setSkin(name)`、`setToSetupPose()`、`setBonesToSetupPose()`、`setSlotsToSetupPose()` |
| 骨骼/槽位/附件 | `findBone(name)`、`findSlot(name)`、`setAttachment(slot, att)`、`getAttachment(slot, att)`、`setSlotTexture(slot, tex2d, createNew?)`、`setSlotsRange(start, end)` |
| 事件 | `setStartListener` / `setInterruptListener` / `setEndListener` / `setDisposeListener` / `setCompleteListener` / `setEventListener` |
| 其他 | `getState()`(AnimationState)、`querySockets()`、`updateMaterial()`、`getSpineMaterialForBlendAndTint(src, dst, useTint)`、`isAnimationCached()`(false)、`setAnimationCacheMode()`(仅 REALTIME)、`markForUpdateRenderData()` |

### 兼容包装(runtime-objects.ts,spine-ts 风格)

| 类 | 说明 |
|---|---|
| `TrackEntry` | 轨道条目:`trackIndex`、`animation`、`loop`、`reverse`、`additive`、`delay`、`trackTime`、`trackEnd`、`timeScale`、`alpha`、`mixTime`、`mixDuration`、`next`/`mixingFrom`/`mixingTo` 及对应 setter、`setThresholds()`、`resetRotationDirections()` |
| `AnimationState` | `setAnimation` / `addAnimation` / `setEmptyAnimation` / `setEmptyAnimations` / `clearTrack` / `clearTracks` / `setMix` / `getCurrent` |
| `Bone` | 变换读取(`a b c d worldX worldY`、`x/y/rotation/scaleX/scaleY/shearX/shearY`)、写入本地变换 setter、`worldToLocal()` / `localToWorld()`、`parent` |
| `Slot` | `bone`、`attachmentName`、`blendMode`、`color`、`getAttachment()`、`setAttachment()`、`setColor()` |
| `Attachment` / `Animation` / `Skin` / `Event` | 只读信息对象 |

### 导出(运行时 `runtime/index.ts`)

- 组件/资源:`Spine`、`SpineSocket`、`SpineData`
- 枚举:`AnimationEventType`(`START=0 INTERRUPT=1 END=2 COMPLETE=3 DISPOSE=4 EVENT=5`)、`SpineMaterialType`、`SpineAnimationCacheMode`
- 绑定:`spine`、`loadSpineRuntime`,类型 `SpineRuntimeBinding` / `SpineRenderData` / `SpineTrackInfo` / `SpineBoneInfo` / `SpineSlotInfo` / `SpineEventInfo` / `SpineAttachmentInfo` / `SpineTexturePage`

---

## 平台差异与已知问题

| 项 | Web / 编辑器与浏览器预览 / 小游戏 (wasm) | 原生包 / Native Simulator (JSB) |
|---|---|---|
| 绑定对象 | 本地 CJS glue + `cc.wasm.instantiateWasm(...)` 加载外置 `spine-runtime.wasm` | `globalThis.spineruntime`;缺失时直接报错,不回退 wasm |
| 渲染数据来源 | `Module.HEAPU8` | TS 分配的 4MB staging buffer(`setRenderBuffer` 注册) |
| 句柄位数 | uint32 | uint64(BigInt 传输,防 64 位指针截断) |

**已知问题**:

1. **`runtimeGetTrackInfo` 字段不一致**:wasm 只填 18 个字段,JSB 填 31 个。因此 **Web 端** `TrackEntry` 的 `next` / `mixingFrom` / `mixingTo` / 各类 threshold 等只读字段为 `undefined`。
2. **culling 分段管线已导出但未接线**:`runtimeUpdateAnimation` / `runtimeUpdatePose` / `runtimeExtractRenderData` / `runtimeUpdateRenderData` / `runtimeUpdateWorldTransform` / `runtimeGetBounds` 已在 wasm + JSB 双端导出并在接口声明(为未来视口剔除预留),但当前每帧仍走组合版 `runtimeUpdate`。
3. **Native Simulator 需单独构建**:stock Simulator 不走工程的插件扫描流程;仓库提供 `native/spine_runtime_simulator.cmake` 注入插件注册表和 `cc_load_all_plugins()`。

**当前验证状态**:

- 微信小游戏:外置 wasm + AOT Embind 加载、运行验证通过。
- Windows Native Simulator:JSB 模式验证通过。
- Android:MuMu Android 15(arm64-v8a)全量 clean、编译、安装、运行验证通过。
- iOS / macOS:原生插件配置已接入,尚未做设备运行验证。

**其他限制**:

- 仅支持 JSON 骨骼资源;二进制 `.skel` 不支持。
- 动画缓存已移除,只有 `REALTIME`;`isAnimationCached()` 恒 false。
- `getTextureAtlas()` / `getDebugShapes()` 恒返回 `null`(不支持)。
- 每个包装器属性读取都是一次 JS→native 往返,无缓存字段(热路径上注意开销)。
- 每帧无条件更新(离屏剔除尚未实现)。
- 需要引擎内置 `default-spine-material`、`cc.UIMesh` 与公开的 `cc.wasm`。

---

## 构建

**开箱即用**：wasm 二进制与 CJS glue 已预编译并提交（`native/wasm/prebuilt/` 与 `runtime/bindings/spine-runtime.js`）；扩展加载时 `main.js` 为编辑器/浏览器预览准备外置 wasm，非原生发布构建由 `editor/build/hooks.js` 把它复制到 `cocos-js/`。原生端源码已 vendor（`native/third_party/spine-cpp/`），正常原生工程由引擎构建系统自动编译。前提是使用提供 `cc.UIMesh` 与 `cc.wasm` 导出的自定义引擎。

**wasm**(web 运行时地基，改 C++ 后重新生成):

```bash
source <你的 emsdk 安装路径>/emsdk_env.sh   # 先安装 Emscripten SDK
emcmake cmake -S native/wasm -B native/wasm/build
cmake --build native/wasm/build
cp native/wasm/build/spine-runtime.js native/wasm/build/spine-runtime.wasm native/wasm/prebuilt/
node native/wasm/scripts/gen-glue.js
```

构建产出 `spine-runtime.wasm` 与 ESM glue。先同步到 `native/wasm/prebuilt/`,再由 `gen-glue.js`（兼容入口,实际委托 `gen-glue-file.js`）把 ESM glue 转成 `runtime/bindings/spine-runtime.js` 的 CJS 形式。wasm 始终是独立文件,不会再 gzip/base64 内嵌。`EMBIND_AOT=1` + `DYNAMIC_EXECUTION=0` 会提前生成 Embind 调用器,避免小游戏沙箱中的 `eval/new Function`;`-mno-reference-types -mno-bulk-memory` 保持微信 WASM 校验器兼容。

**native**:通过引擎的原生插件机制接入(`package.json` 的 `contributions["native"]` 声明了 `cc_plugin.json`):

```
native/
├── cc_plugin.json                  # 插件声明(module target: spine_runtime)
├── spineruntime_plugin.cpp         # 插件入口:CC_PLUGIN_ENTRY + addRegisterCallback
├── spine_runtime.cmake             # 共享源码构建(编译绑定 + SpineRuntime + spine-cpp 4.3)
├── spine_runtime_simulator.cmake   # stock Native Simulator 的显式接入 hook
├── jsb_spineruntime_manual.cpp/.h  # JSB 手动绑定
├── third_party/spine-cpp/          # 官方 spine-cpp 4.3（vendored，随仓库分发）
├── wasm/                           # WebAssembly 平台构建
└── {android,windows,ios,mac}/spine_runtime-config.cmake   # find_package 入口
```

构建流程(由引擎构建系统自动完成,基于 `native/cmake/scripts/plugins_parser.js`):

1. 构建原生工程时,引擎在项目 `native/` 和 `extensions/` 下递归查找 `cc_plugin.json`(需 `USE_PLUGINS` 开启,默认 ON)。
2. 解析器把 `spine_runtime` 的 `_ROOT` 指向 `native/<平台>/`,生成 `Pre-AutoLoadPlugins.cmake` 并 `find_package(spine_runtime REQUIRED)`。
3. `<平台>/spine_runtime-config.cmake` 加载 `spine_runtime.cmake`,把插件编译为静态库 `spine_runtime`(`CC_PLUGIN_STATIC` 由该 CMake 定义,使 `CC_PLUGIN_ENTRY` 生成无修饰的 `cc_load_plugin_spine_runtime()`)。
4. `cc_plugin_entry()` 生成 `plugin_registry`,`cc_load_all_plugins()` 在引擎启动时调用入口,把绑定注册进 `ScriptEngine` 的 register 回调(`addRegisterCallback`),从而暴露 `globalThis.spineruntime`。

**Windows Native Simulator**:它不扫描工程扩展,需单独重建一次自定义引擎 Simulator:

```powershell
cmake -S <engine>/native/tools/simulator/frameworks/runtime-src -B <engine>/native/simulator -DCMAKE_PROJECT_INCLUDE=<插件绝对路径>/native/spine_runtime_simulator.cmake
cmake --build <engine>/native/simulator --config Debug
```

已有 Simulator 构建目录时继续使用原来的 `-G/-A` 配置。该 hook 只在构建期生成替代 `Game.cpp` 并链接 `plugin_registry`;Native Simulator 运行时只支持 JSB,不会尝试 wasm。

> ✅ **符号隔离已实现**:vendored 的 spine-cpp 4.3 通过 `COMPILE_DEFINITIONS "spine=spine43"` 编译,全部 4.3 符号落入 `spine43::` 命名空间,与引擎内置 spine(3.8 的 `spine::`)互不冲突。Android 产物已用 `llvm-nm` 验证无重复符号。
