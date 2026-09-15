#include <fuse/cinematics/group.hpp>

namespace fuse::cinematics {

TrackGroup::TrackGroup(const std::string& label) : label_(label) {}

Track& TrackGroup::add_track(const std::string& label) {
    tracks_.emplace_back(label);
    return tracks_.back();
}

} // namespace fuse::cinematics
