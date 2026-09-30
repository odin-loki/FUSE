#pragma once

#include <fuse/jobs/job_counter.hpp>
#include <fuse/jobs/job_scheduler.hpp>
#include <fuse/types.hpp>

#include <functional>
#include <string>
#include <string_view>
#include <vector>

namespace fuse::ecs {

/// Declarative ECS system graph (GAP-GAME-LOOP-ECS runtime frame schedule).
///
/// Systems run in a topological order of their dependencies; ties are broken by registration order,
/// so the order is fully deterministic (it does not depend on hashing). The order is compiled on
/// the first `run_all` after a registration change and cached: a steady-state `run_all` makes no
/// heap allocations (Inline mode, or Jobs mode on a single-threaded / uninitialised JobScheduler).
///
/// Jobs mode (default) runs each system as a JobScheduler job behind a fork-join barrier, so a
/// system body executes on a worker thread; Inline mode runs the bodies on the calling thread
/// (systems still parallelise internally with parallel_for / each_parallel).
class SystemScheduler {
public:
    using SystemFn = std::function<void()>;

    enum class ExecutionMode : u8 { Jobs = 0, Inline = 1 };

    struct SystemDesc {
        std::string name;
        SystemFn run;
        std::vector<std::string> dependencies;
    };

    void clear();
    /// Ignored when the name is empty, the body is missing or the name is already registered.
    void register_system(const SystemDesc& desc);
    /// Runs every system once in dependency order. Throws std::runtime_error on a dependency cycle.
    /// Dependencies naming unregistered systems are ignored.
    void run_all();

    void set_execution_mode(ExecutionMode mode) { m_mode = mode; }
    ExecutionMode execution_mode() const { return m_mode; }

    usize system_count() const { return m_systems.size(); }
    /// Registration indices in execution order (compiles the order if needed).
    const std::vector<u32>& execution_order();
    std::string_view system_name(u32 index) const;
    /// Completed `run_all` calls.
    u64 run_count() const { return m_runCount; }

private:
    void compile_();

    std::vector<SystemDesc> m_systems;
    std::vector<u32> m_order;
    bool m_orderDirty = true;
    ExecutionMode m_mode = ExecutionMode::Jobs;
    u64 m_runCount = 0;
};

} // namespace fuse::ecs
