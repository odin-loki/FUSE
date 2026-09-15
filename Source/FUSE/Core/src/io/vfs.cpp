#include <fuse/io/vfs.hpp>

#include <fuse/jobs/job_scheduler.hpp>

#include <fstream>
#include <utility>

namespace fuse::io {
namespace {

bool readFileBytes(const std::string& path, std::vector<u8>& outBytes) {
    std::ifstream input(path, std::ios::binary);
    if (!input) {
        return false;
    }

    input.seekg(0, std::ios::end);
    const std::streamoff size = input.tellg();
    if (size < 0) {
        return false;
    }

    outBytes.resize(static_cast<usize>(size));
    if (size == 0) {
        return true;
    }

    input.seekg(0, std::ios::beg);
    input.read(reinterpret_cast<char*>(outBytes.data()), size);
    return input.good() || input.eof();
}

} // namespace

VirtualFileSystem& VirtualFileSystem::instance() {
    static VirtualFileSystem vfs;
    return vfs;
}

void VirtualFileSystem::pushCompleted(CompletedLoad load) {
    std::lock_guard<std::mutex> lock(m_completedMutex);
    m_completed.push_back(std::move(load));
}

void VirtualFileSystem::mount(MountKind kind, std::string_view physicalPath, std::string_view virtualPrefix) {
    MountPoint point;
    point.kind = kind;
    point.physicalPath = std::string(physicalPath);
    point.virtualPrefix = std::string(virtualPrefix);
    m_mounts.push_back(std::move(point));
}

bool VirtualFileSystem::resolve(std::string_view virtualPath, std::string& outPhysical) const {
    for (const MountPoint& mount : m_mounts) {
        if (virtualPath.size() < mount.virtualPrefix.size()) {
            continue;
        }
        if (virtualPath.substr(0, mount.virtualPrefix.size()) != mount.virtualPrefix) {
            continue;
        }

        const std::string_view remainder = virtualPath.substr(mount.virtualPrefix.size());
        outPhysical = mount.physicalPath;
        if (!remainder.empty() && remainder.front() != '/') {
            outPhysical.push_back('/');
        }
        outPhysical.append(remainder);
        return true;
    }
    return false;
}

u32 VirtualFileSystem::mountCount() const {
    return static_cast<u32>(m_mounts.size());
}

LoadId VirtualFileSystem::submitLoadAsync(std::string_view virtualPath) {
    std::string physicalPath;
    const LoadId loadId = m_nextLoadId++;

    if (!resolve(virtualPath, physicalPath)) {
        CompletedLoad failed;
        failed.id = loadId;
        failed.success = false;
        failed.asset.virtualPath = std::string(virtualPath);
        pushCompleted(std::move(failed));
        return loadId;
    }

    const std::string virtualCopy(virtualPath);
    fuse::jobs::JobScheduler::instance().submit([this, loadId, physicalPath = std::move(physicalPath), virtualCopy]() {
        CompletedLoad completed;
        completed.id = loadId;
        completed.asset.virtualPath = virtualCopy;
        completed.success = readFileBytes(physicalPath, completed.asset.bytes);
        pushCompleted(std::move(completed));
    });

    return loadId;
}

u32 VirtualFileSystem::drainCompletedLoads(HandleTable<Asset>& table) {
    std::vector<CompletedLoad> batch;
    {
        std::lock_guard<std::mutex> lock(m_completedMutex);
        batch.swap(m_completed);
    }

    m_lastDrained = batch;
    for (CompletedLoad& load : batch) {
        if (load.success) {
            table.enqueuePublish(std::move(load.asset));
        }
    }
    return static_cast<u32>(batch.size());
}

u32 VirtualFileSystem::completedLoadCount() const {
    std::lock_guard<std::mutex> lock(m_completedMutex);
    return static_cast<u32>(m_completed.size());
}

std::vector<LoadId> VirtualFileSystem::peekCompletedLoadOrder() const {
    std::lock_guard<std::mutex> lock(m_completedMutex);
    std::vector<LoadId> order;
    order.reserve(m_completed.size());
    for (const CompletedLoad& load : m_completed) {
        order.push_back(load.id);
    }
    return order;
}

} // namespace fuse::io
