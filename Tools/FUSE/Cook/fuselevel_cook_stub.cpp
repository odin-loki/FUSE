#include "fuselevel_cook_stub.hpp"

#include <fuse/project/world_converter.hpp>

namespace fuse::cook {

namespace {

FuselevelCookResult fromConvertResult(const fuse::project::ConvertResult& converted) {
    FuselevelCookResult result;
    result.entityCount = converted.entityCount;
    result.note = converted.note;

    switch (converted.status) {
    case fuse::project::ConvertStatus::Ok:
        result.status = FuselevelCookStatus::Ok;
        break;
    case fuse::project::ConvertStatus::ParseError:
    case fuse::project::ConvertStatus::UnsupportedSource:
        result.status = FuselevelCookStatus::UnsupportedSource;
        break;
    default:
        result.status = FuselevelCookStatus::IoError;
        break;
    }

    return result;
}

} // namespace

FuselevelCookResult cookFuselevelFromMis(const std::string& missionPath, const std::string& outputPath) {
    return fromConvertResult(fuse::project::convertT3DMissionToFuselevel(missionPath, outputPath));
}

FuselevelCookResult cookFuselevelFromModule(const std::string& modulePath, const std::string& outputPath) {
    return fromConvertResult(fuse::project::convertT2DModuleToFuselevel(modulePath, outputPath));
}

} // namespace fuse::cook
