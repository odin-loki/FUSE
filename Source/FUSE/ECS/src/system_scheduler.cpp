#include <fuse/ecs/system_scheduler.hpp>

#include <algorithm>
#include <queue>
#include <stdexcept>
#include <unordered_map>

namespace fuse::ecs {

void SystemScheduler::clear() {
    m_systems.clear();
    m_order.clear();
}

void SystemScheduler::register_system(const SystemDesc& desc) {
    if (desc.name.empty() || !desc.run) {
        return;
    }
    m_systems.push_back(desc);
}

void SystemScheduler::run_all() {
    m_order.clear();
    std::unordered_map<std::string, u32> indegree;
    std::unordered_map<std::string, std::vector<std::string>> dependents;
    std::unordered_map<std::string, const SystemDesc*> by_name;

    for (const SystemDesc& system : m_systems) {
        indegree.emplace(system.name, 0);
        by_name.emplace(system.name, &system);
    }

    for (const SystemDesc& system : m_systems) {
        for (const std::string& dependency : system.dependencies) {
            if (by_name.find(dependency) == by_name.end()) {
                continue;
            }
            ++indegree[system.name];
            dependents[dependency].push_back(system.name);
        }
    }

    std::queue<std::string> ready;
    for (const auto& [name, count] : indegree) {
        if (count == 0) {
            ready.push(name);
        }
    }

    u32 order = 0;
    while (!ready.empty()) {
        const std::string current = ready.front();
        ready.pop();
        m_order[current] = order++;

        for (const std::string& dependent : dependents[current]) {
            if (--indegree[dependent] == 0) {
                ready.push(dependent);
            }
        }
    }

    if (m_order.size() != m_systems.size()) {
        throw std::runtime_error("SystemScheduler: cyclic system dependency detected");
    }

    std::vector<const SystemDesc*> sorted;
    sorted.reserve(m_systems.size());
    for (const SystemDesc& system : m_systems) {
        sorted.push_back(by_name.at(system.name));
    }
    std::sort(sorted.begin(), sorted.end(), [&](const SystemDesc* a, const SystemDesc* b) {
        return m_order.at(a->name) < m_order.at(b->name);
    });

    auto& scheduler = jobs::JobScheduler::instance();
    for (const SystemDesc* system : sorted) {
        jobs::JobCounter counter(1);
        scheduler.submit([&]() {
            system->run();
            counter.signal();
        });
        counter.wait();
    }
}

} // namespace fuse::ecs
