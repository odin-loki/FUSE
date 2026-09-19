#include <fuse/fx/afx_template_pack.hpp>

#include <fuse/fx/effect_descriptor.hpp>

#include <cctype>
#include <sstream>

namespace fuse::fx {

namespace {

std::string trim(const std::string& input) {
    std::size_t start = 0;
    while (start < input.size() && std::isspace(static_cast<unsigned char>(input[start]))) {
        ++start;
    }
    std::size_t end = input.size();
    while (end > start && std::isspace(static_cast<unsigned char>(input[end - 1]))) {
        --end;
    }
    return input.substr(start, end - start);
}

EffectDescriptor makeTemplateEffect(const std::string& id, float duration) {
    EffectDescriptor descriptor;
    descriptor.id = id;
    descriptor.duration = duration;
    descriptor.loopCount = 1;
    EffectEntry entry;
    entry.effectTypeId = id;
    entry.timing.lifetime = duration;
    descriptor.entries.push_back(entry);
    return descriptor;
}

} // namespace

bool load_afx_template_pack_from_text(const std::string& text, FxComposer& composer, std::string* errorOut) {
    std::stringstream stream(text);
    std::string line;
    bool sawEffect = false;

    while (std::getline(stream, line)) {
        line = trim(line);
        if (line.empty() || line[0] == '#') {
            continue;
        }

        std::stringstream lineStream(line);
        std::string keyword;
        if (!(lineStream >> keyword)) {
            continue;
        }

        if (keyword == "effect") {
            std::string id;
            float duration = 0.5f;
            if (!(lineStream >> id)) {
                if (errorOut) {
                    *errorOut = "effect line missing id";
                }
                return false;
            }

            std::string token;
            while (lineStream >> token) {
                const std::size_t eq = token.find('=');
                if (eq != std::string::npos && token.substr(0, eq) == "duration") {
                    duration = std::stof(token.substr(eq + 1));
                }
            }

            if (!composer.registerEffect(makeTemplateEffect(id, duration))) {
                if (errorOut) {
                    *errorOut = "failed to register effect: " + id;
                }
                return false;
            }
            sawEffect = true;
        }
    }

    if (!sawEffect) {
        if (errorOut) {
            *errorOut = "pack contained no effects";
        }
        return false;
    }
    return true;
}

bool registerAfxTemplateSamplePack(FxComposer& composer) {
    static const char* kPackText =
        "# AFX-Template minimal sample pack (ore: AFXDemo_Minimal.mis naming)\n"
        "effect afx_demo_spark duration=0.5\n"
        "effect afx_demo_smoke duration=1.2\n"
        "effect afx_demo_fireball_trail duration=2.0\n";

    std::string error;
    return load_afx_template_pack_from_text(kPackText, composer, &error);
}

} // namespace fuse::fx
