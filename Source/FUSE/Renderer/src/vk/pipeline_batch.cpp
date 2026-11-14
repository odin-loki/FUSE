// Pipeline builds on worker threads: see include/fuse/renderer/vk/pipeline_batch.hpp.
#include <fuse/renderer/vk/pipeline_batch.hpp>

#include <algorithm>
#include <utility>

#if defined(__linux__)
#include <sched.h>
#endif

#if defined(FUSE_VULKAN_BACKEND)
#include <vulkan/vulkan.h>
#endif

namespace fuse::renderer {

namespace {
thread_local PipelineBatch* t_current = nullptr;
constexpr u32 kMaxThreads = 16u;

/// Hardware threads this process may run on (the affinity mask: a container or `taskset` limits it below
/// std::thread::hardware_concurrency(), which counts every online CPU).
u32 usableCpus() {
#if defined(__linux__)
    cpu_set_t set;
    CPU_ZERO(&set);
    if (sched_getaffinity(0, sizeof(set), &set) == 0) {
        const int n = CPU_COUNT(&set);
        if (n > 0) {
            return static_cast<u32>(n);
        }
    }
#endif
    return std::max(1u, std::thread::hardware_concurrency());
}
} // namespace

PipelineBatch::PipelineBatch(u32 threads) {
    if (t_current != nullptr) {
        m_outer = t_current;
        return;
    }
    // The opening thread runs builds too once it waits, so one worker fewer than the usable CPUs.
    const u32 cpus = usableCpus();
    const u32 workers = cpus > 1u ? cpus - 1u : 1u;
    m_threadLimit = std::min(threads != 0u ? threads : workers, kMaxThreads);
    t_current = this;
}

PipelineBatch::~PipelineBatch() {
    if (m_outer != nullptr) {
        return;
    }
    (void)wait();
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        m_stop = true;
    }
    m_work.notify_all();
    for (std::thread& t : m_threads) {
        t.join();
    }
    if (t_current == this) {
        t_current = nullptr;
    }
}

PipelineBatch* PipelineBatch::current() { return t_current; }

void PipelineBatch::enqueue(const void* owner, std::function<bool()> build, u64 cost) {
    if (m_outer != nullptr) {
        m_outer->enqueue(owner, std::move(build), cost);
        return;
    }
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        m_tasks.push_back(Task{owner, std::move(build), cost});
        ++m_queued;
        // One worker per queued build up to the limit; idle workers pick up later builds.
        if (m_threads.size() < m_threadLimit && m_threads.size() < m_tasks.size() + m_runningOwners.size()) {
            m_threads.emplace_back([this] { workerLoop(); });
        }
    }
    m_work.notify_one();
}

bool PipelineBatch::runOne(std::unique_lock<std::mutex>& lock, const void* owner, bool anyOwner) {
    auto it = m_tasks.end();
    for (auto t = m_tasks.begin(); t != m_tasks.end(); ++t) {
        if ((anyOwner || t->owner == owner) && (it == m_tasks.end() || t->cost > it->cost)) {
            it = t;
        }
    }
    if (it == m_tasks.end()) {
        return false;
    }
    Task task = std::move(*it);
    m_tasks.erase(it);
    m_runningOwners.push_back(task.owner);
    lock.unlock();
    const bool ok = task.build ? task.build() : false;
    task.build = nullptr; // release captures outside the lock
    lock.lock();
    m_runningOwners.erase(std::find(m_runningOwners.begin(), m_runningOwners.end(), task.owner));
    if (!ok) {
        m_failed.push_back(task.owner);
    }
    m_done.notify_all();
    return true;
}

void PipelineBatch::workerLoop() {
    std::unique_lock<std::mutex> lock(m_mutex);
    for (;;) {
        m_work.wait(lock, [this] { return m_stop || !m_tasks.empty(); });
        if (m_tasks.empty()) {
            return; // m_stop
        }
        (void)runOne(lock, nullptr, true);
    }
}

bool PipelineBatch::wait() {
    if (m_outer != nullptr) {
        return m_outer->wait();
    }
    std::unique_lock<std::mutex> lock(m_mutex);
    while (runOne(lock, nullptr, true)) {
    }
    m_done.wait(lock, [this] { return m_tasks.empty() && m_runningOwners.empty(); });
    return m_failed.empty();
}

void PipelineBatch::waitFor(const void* owner) {
    if (m_outer != nullptr) {
        m_outer->waitFor(owner);
        return;
    }
    std::unique_lock<std::mutex> lock(m_mutex);
    while (runOne(lock, owner, false)) {
    }
    m_done.wait(lock, [this, owner] {
        return std::find(m_runningOwners.begin(), m_runningOwners.end(), owner) == m_runningOwners.end();
    });
}

bool PipelineBatch::failed(const void* owner) const {
    if (m_outer != nullptr) {
        return m_outer->failed(owner);
    }
    std::lock_guard<std::mutex> lock(m_mutex);
    return std::find(m_failed.begin(), m_failed.end(), owner) != m_failed.end();
}

u32 PipelineBatch::queued() const {
    if (m_outer != nullptr) {
        return m_outer->queued();
    }
    std::lock_guard<std::mutex> lock(m_mutex);
    return m_queued;
}

bool buildPipeline(const void* owner, std::function<bool()> build, u64 cost) {
    if (PipelineBatch* batch = PipelineBatch::current()) {
        batch->enqueue(owner, std::move(build), cost);
        return true;
    }
    return build ? build() : false;
}

void waitPipelineBuilds(const void* owner) {
    if (PipelineBatch* batch = PipelineBatch::current()) {
        batch->waitFor(owner);
    }
}

bool createComputePipelineSpirv(void* device, const u32* words, usize bytes, void* layout, u32 createFlags, void** out) {
#if defined(FUSE_VULKAN_BACKEND)
    if (device == nullptr || words == nullptr || bytes == 0u || out == nullptr) {
        return false;
    }
    const VkDevice vk = static_cast<VkDevice>(device);
    VkShaderModuleCreateInfo moduleInfo{};
    moduleInfo.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO;
    moduleInfo.codeSize = bytes;
    moduleInfo.pCode = words;
    VkShaderModule module = VK_NULL_HANDLE;
    if (vkCreateShaderModule(vk, &moduleInfo, nullptr, &module) != VK_SUCCESS) {
        return false;
    }
    VkComputePipelineCreateInfo info{};
    info.sType = VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO;
    info.flags = static_cast<VkPipelineCreateFlags>(createFlags);
    info.stage.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    info.stage.stage = VK_SHADER_STAGE_COMPUTE_BIT;
    info.stage.module = module;
    info.stage.pName = "main";
    info.layout = static_cast<VkPipelineLayout>(layout);
    VkPipeline pipeline = VK_NULL_HANDLE;
    const VkResult result = vkCreateComputePipelines(vk, VK_NULL_HANDLE, 1, &info, nullptr, &pipeline);
    vkDestroyShaderModule(vk, module, nullptr);
    if (result != VK_SUCCESS) {
        return false;
    }
    *out = pipeline;
    return true;
#else
    (void)device;
    (void)words;
    (void)bytes;
    (void)layout;
    (void)createFlags;
    (void)out;
    return false;
#endif
}

bool buildComputePipeline(const void* owner, void* device, const u32* words, usize bytes, void* layout, u32 createFlags,
                          void** out) {
    return buildPipeline(
        owner, [device, words, bytes, layout, createFlags, out] {
            return createComputePipelineSpirv(device, words, bytes, layout, createFlags, out);
        },
        bytes);
}

} // namespace fuse::renderer
