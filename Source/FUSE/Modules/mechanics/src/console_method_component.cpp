#include <fuse/mechanics/console_method_component.hpp>

namespace fuse::mechanics {

ConsoleMethodComponent::ConsoleMethodComponent() = default;

ConsoleMethodComponent::ConsoleMethodComponent(std::string name) : Component(std::move(name)) {}

void ConsoleMethodComponent::registerMethod(const std::string& name, MethodFn fn) {
    m_methods[name] = std::move(fn);
}

bool ConsoleMethodComponent::invoke(const std::string& name) {
    const auto it = m_methods.find(name);
    if (it == m_methods.end() || !it->second) {
        return false;
    }
    it->second();
    ++m_invokeCount;
    return true;
}

bool ConsoleMethodComponent::hasMethod(const std::string& name) const {
    return m_methods.find(name) != m_methods.end();
}

} // namespace fuse::mechanics
