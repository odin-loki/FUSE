#pragma once

// Ore: third_party/addons/GMK/Engine/source/component/dynamicConsoleMethodComponent.h

#include <fuse/mechanics/component.hpp>
#include <fuse/types.hpp>

#include <functional>
#include <string>
#include <unordered_map>

namespace fuse::mechanics {

/// Named callback registry (GMK DynamicConsoleMethodComponent without Con::).
class ConsoleMethodComponent : public Component {
public:
    using MethodFn = std::function<void()>;

    ConsoleMethodComponent();
    explicit ConsoleMethodComponent(std::string name);

    const char* typeName() const override { return "ConsoleMethodComponent"; }

    void registerMethod(const std::string& name, MethodFn fn);
    bool invoke(const std::string& name);

    u32 invokeCount() const { return m_invokeCount; }
    bool hasMethod(const std::string& name) const;

private:
    std::unordered_map<std::string, MethodFn> m_methods;
    u32 m_invokeCount = 0;
};

} // namespace fuse::mechanics
