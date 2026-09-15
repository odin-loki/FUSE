#pragma once

#include <fuse/jobs/job_counter.hpp>
#include <fuse/jobs/job_scheduler.hpp>
#include <fuse/types.hpp>

#include <functional>
#include <string>
#include <unordered_map>
#include <vector>

namespace fuse::ecs {

/// Declarative ECS system graph executed through JobScheduler fork-join barriers.
class SystemScheduler {
public:
    using SystemFn = std::function<void()>;

    struct SystemDesc {
        std::string name;
        SystemFn run;
        std::vector<std::string> dependencies;
    };

    void clear();
    void register_system(const SystemDesc& desc);
    void run_all();

    usize system_count() const { return m_systems.size(); }

private:
    std::vector<SystemDesc> m_systems;
    std::unordered_map<std::string, u32> m_order;
};

} // namespace fuse::ecs
