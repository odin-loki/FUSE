// FUSE Relight RL-6.3: game-setup assistant.
//
// Reads a live capture record (relight_capture.jsonl of relight.tap.mode = capture: per draw the bound
// textures, the RL-1.2 classification and the RL-1.5 translation; per frame the cameras) and proposes a
// per-game profile:
//
//   texture categories   per colour texture (the texture Remix categorises by), from the RL-1.2 outcome
//                        (categories, reason, sky auto-detection) and the draw statistics:
//                          sky      every use is a sky draw (Sky category, sky camera, or viewport MinZ >= 1
//                                   with depth writes off)
//                          ui       every use is screen space (orthographic / no camera with depth test off,
//                                   POSITIONT, or already classified UserInterface)
//                          particle blended, depth writes off, small world-space batches, additive or alpha
//                          decal    depth-tested, depth writes off, blended or alpha-tested world geometry
//                                   drawn over opaque geometry (not additive)
//                          ignore   full-screen screen-space quads sampling a render target (post effects)
//                          raytracedRenderTarget  a render-target texture sampled by world geometry (its
//                                   descriptor hash, as rtx.raytracedRenderTargetTextures expects)
//                        A texture used by both world and sky / screen-space draws is only suggested,
//                        with the conflict in the reason (tagging it changes the world draws too).
//   options              rtx.preTransformedVerticesIsUI when POSITIONT draws come after all 3D draws;
//                        a note when orthographic screen-space draws are ray traced (depth writes on).
//   camera sanity        frames without a main camera, several main cameras, implausible FOV / near / far.
//   hash-rule choice     draws whose geometry asset hash changes between frames with the same texture and
//                        vertex count (animated or re-uploaded vertices): suggests an asset hash rule
//                        without positions.
//
// Proposals at or above AssistantOptions::applyThreshold go into the profile's textures / options (applied);
// the others are listed in "suggestions" for review (fuse_relight_setup accept ...).
#pragma once

#include <fuse/relight/setup/profile.hpp>

#include <cstdint>
#include <map>
#include <optional>
#include <set>
#include <string>
#include <string_view>
#include <vector>

namespace fuse::relight::setup {

struct TextureUsage {
    std::uint64_t hash = 0;
    std::uint64_t descriptor = 0; ///< render-target descriptor hash (0 for uploaded textures)
    bool renderTarget = false;    ///< the texture is a render target (origin render_target)
    std::uint32_t draws = 0;
    std::set<std::uint32_t> frames;
    std::uint32_t skyDraws = 0;
    std::uint32_t screenDraws = 0;      ///< orthographic / POSITIONT / UI-classified
    std::uint32_t worldDraws = 0;       ///< neither sky nor screen space
    std::uint32_t fullScreenDraws = 0;  ///< screen-space quads covering the viewport
    std::uint32_t blendedWorld = 0;     ///< world draws with alpha blending
    std::uint32_t additiveWorld = 0;    ///< world draws with DESTBLEND ONE
    std::uint32_t alphaTestedWorld = 0;
    std::uint32_t noZWriteWorld = 0;    ///< world draws with depth test on and depth writes off
    std::uint32_t maxVertices = 0;
    std::set<std::string> classifiedCategories; ///< RL-1.2 categories already set on its draws
    std::set<std::string> reasons;
};

struct CameraIssue {
    std::int64_t frame = -1; ///< -1: whole capture
    std::string message;
};

struct SetupReport {
    std::uint32_t frames = 0;
    std::uint32_t draws = 0;
    std::uint32_t positionTDraws = 0;
    std::uint32_t positionTBeforeScene = 0; ///< POSITIONT draws followed by 3D draws in the same frame
    std::uint32_t positionTAsUI = 0;        ///< already classified PositionTAsUI
    std::uint32_t raytracedScreenSpace = 0; ///< screen-space draws still ray traced
    std::uint32_t unstableAssetDraws = 0;   ///< draw slots whose asset hash changes between frames
    std::uint32_t comparableAssetDraws = 0;
    std::map<std::uint64_t, TextureUsage> textures;
    std::vector<ProfileSuggestion> proposals; ///< sorted: category / key, then hash
    std::vector<CameraIssue> cameraIssues;
    std::vector<std::string> notes;
};

struct AssistantOptions {
    double applyThreshold = 0.8; ///< proposals at or above it are applied in proposeProfile()
};

/// Analyse a capture record (JSON Lines). nullopt on a malformed line (`error` says which).
std::optional<SetupReport> analyzeCaptureRecord(std::string_view jsonl, std::string* error = nullptr);
std::optional<SetupReport> analyzeCaptureFile(const std::string& path, std::string* error = nullptr);

/// A profile from the report: match by `exeName` (and `exeHash` when given), applied and pending
/// suggestions, camera issues and notes.
GameProfile proposeProfile(const SetupReport& report, const std::string& name, const std::string& exeName,
                           std::optional<std::uint64_t> exeHash = std::nullopt, const AssistantOptions& options = {});

/// Human-readable summary (one line per texture and proposal).
std::string formatReport(const SetupReport& report);

} // namespace fuse::relight::setup
