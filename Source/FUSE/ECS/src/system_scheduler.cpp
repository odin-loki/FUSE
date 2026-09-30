#include <fuse/ecs/system_scheduler.hpp>

#include <stdexcept>

namespace fuse::ecs {

void SystemScheduler::clear() {
    m_systems.clear();
    m_order.clear();
    m_orderDirty = true;
}

void SystemScheduler::register_system(const SystemDesc& desc) {
    if (desc.name.empty() || !desc.run) {
        return;
    }
    for (const SystemDesc& existing : m_systems) {
        if (existing.name == desc.name) {
            return;
        }
    }
    m_systems.push_back(desc);
    m_orderDirty = true;
}

void SystemScheduler::compile_() {
    const u32 count = static_cast<u32>(m_systems.size());
    // Edges dependency -> dependent, resolved to registration indices once.
    std::vector<u32> indegree(count, 0u);
    std::vector<std::vector<u32>> dependents(count);
    for (u32 i = 0; i < count; ++i) {
        for (const std::string& dependency : m_systems[i].dependencies) {
            for (u32 j = 0; j < count; ++j) {
                if (m_systems[j].name == dependency) {
                    ++indegree[i];
                    dependents[j].push_back(i);
                    break;
                }
            }
        }
    }

    // Kahn's algorithm, always taking the lowest ready registration index (deterministic order).
    std::vector<bool> done(count, false);
    m_order.clear();
    m_order.reserve(count);
    for (u32 emitted = 0; emitted < count; ++emitted) {
        u32 next = count;
        for (u32 i = 0; i < count; ++i) {
            if (!done[i] && indegree[i] == 0u) {
                next = i;
                break;
            }
        }
        if (next == count) {
            m_order.clear();
            m_orderDirty = true;
            throw std::runtime_error("SystemScheduler: cyclic system dependency detected");
        }
        done[next] = true;
        m_order.push_back(next);
        for (const u32 dependent : dependents[next]) {
            --indegree[dependent];
        }
    }
    m_orderDirty = false;
}

const std::vector<u32>& SystemScheduler::execution_order() {
    if (m_orderDirty) {
        compile_();
    }
    return m_order;
}

std::string_view SystemScheduler::system_name(u32 index) const {
    return index < m_systems.size() ? std::string_view(m_systems[index].name) : std::string_view{};
}

void SystemScheduler::run_all() {
    if (m_orderDirty) {
        compile_();
    }

    auto& scheduler = jobs::JobScheduler::instance();
    const bool inline_run =
        m_mode == ExecutionMode::Inline || !scheduler.isInitialized() || scheduler.isSingleThreaded();
    for (const u32 index : m_order) {
        const SystemDesc& system = m_systems[index];
        if (inline_run) {
            system.run();
            continue;
        }
        // Fork-join barrier per system: the next system starts only after this one finished.
        jobs::JobCounter counter(1);
        scheduler.submit([&system, &counter]() {
            system.run();
            counter.signal();
        });
        counter.wait();
    }
    ++m_runCount;
}

} // namespace fuse::ecs
