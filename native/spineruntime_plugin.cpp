/****************************************************************************
 Copyright (c) 2026
 SpineRuntime native plugin entry.

 cc_plugin.json declares the module target `spine_runtime`; the engine's plugin
 scan writes Pre-AutoLoadPlugins.cmake (find_package -> spine_runtime-config.cmake)
 and cc_plugin_entry() generates a registry that calls cc_load_all_plugins().
 CC_PLUGIN_ENTRY below emits `extern "C" void cc_load_plugin_spine_runtime()`,
 which registers the JSB manual binding into the ScriptEngine's register-callback
 list. The callback runs with the global se::Object when the engine starts, so
 the `spineruntime` JS namespace is available to the plugin's TS layer.
****************************************************************************/
#include "cocos/bindings/jswrapper/SeApi.h"
#include "cocos/plugins/Plugins.h"

#include "jsb_spineruntime_manual.h"

namespace {

bool registerSpineRuntimeBinding(se::Object* globalObj) {
    return register_all_spineruntime_manual(globalObj);
}

void loadSpineRuntimePlugin() {
    se::ScriptEngine::getInstance()->addRegisterCallback(registerSpineRuntimeBinding);
}

} // namespace

// CC_PLUGIN_STATIC must be defined when compiling this file so the entry is an
// undecorated extern "C" function (see the Config.cmake target definitions).
CC_PLUGIN_ENTRY(spine_runtime, loadSpineRuntimePlugin)
