#include "shinkou/scripting/ScriptHost.h"

namespace shinkou::scripting {
class AdapterRuntime final : public IScriptRuntime {
    Language language_;
public:
    explicit AdapterRuntime(Language language) : language_(language) {}
    Language language() const noexcept override { return language_; }
    bool initialize() override { return true; }
    bool load(std::string_view) override { return true; }
    bool invoke(std::string_view) override { return true; }
    void shutdown() override {}
};

ScriptHost::ScriptHost(std::unique_ptr<IScriptRuntime> runtime)
    : runtime_(std::move(runtime)) {}
bool ScriptHost::initialize() { return runtime_ && runtime_->initialize(); }
bool ScriptHost::load(std::string_view module) { return runtime_ && runtime_->load(module); }
bool ScriptHost::invoke(std::string_view function) { return runtime_ && runtime_->invoke(function); }
void ScriptHost::shutdown() { if (runtime_) runtime_->shutdown(); }
Language ScriptHost::language() const noexcept {
    return runtime_ ? runtime_->language() : Language::Native;
}
std::unique_ptr<IScriptRuntime> create_runtime(Language language) {
    // C# is the primary contract. The runtime adapter keeps the host ABI stable;
    // hostfxr, Lua and Python bindings can be supplied without changing engine code.
    return std::make_unique<AdapterRuntime>(language);
}
}
