#pragma once

#include <functional>
#include <memory>
#include <string>
#include <string_view>

namespace shinkou::scripting {
enum class Language { CSharp, Lua, Python, Native };

class IScriptRuntime {
public:
    virtual ~IScriptRuntime() = default;
    virtual Language language() const noexcept = 0;
    virtual bool initialize() = 0;
    virtual bool load(std::string_view module) = 0;
    virtual bool invoke(std::string_view function) = 0;
    virtual void shutdown() = 0;
};

class ScriptHost {
    std::unique_ptr<IScriptRuntime> runtime_;
public:
    explicit ScriptHost(std::unique_ptr<IScriptRuntime> runtime);
    bool initialize();
    bool load(std::string_view module);
    bool invoke(std::string_view function);
    void shutdown();
    Language language() const noexcept;
};

std::unique_ptr<IScriptRuntime> create_runtime(Language language);
}
