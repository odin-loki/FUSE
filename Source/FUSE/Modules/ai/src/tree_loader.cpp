#include <fuse/ai/tree_loader.hpp>
#include <fuse/ai/node_registry.hpp>

#include <cctype>
#include <sstream>
#include <string>

namespace fuse::ai {

namespace {

bool parseU32(const std::string& token, u32& outValue) {
    try {
        const unsigned long value = std::stoul(token);
        outValue = static_cast<u32>(value);
        return true;
    } catch (...) {
        return false;
    }
}

bool parseFloat(const std::string& token, float& outValue) {
    try {
        outValue = std::stof(token);
        return true;
    } catch (...) {
        return false;
    }
}

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

bool parseChildren(const std::string& value, std::vector<u32>& outChildren) {
    std::stringstream stream(value);
    std::string part;
    while (std::getline(stream, part, ',')) {
        part = trim(part);
        if (part.empty()) {
            continue;
        }
        u32 child = 0;
        if (!parseU32(part, child)) {
            return false;
        }
        outChildren.push_back(child);
    }
    return !outChildren.empty();
}

bool parseNodeLine(const std::string& line, NodeLoadSpec& outSpec, std::string* errorOut) {
    std::stringstream stream(line);
    std::string token;
    if (!(stream >> token)) {
        if (errorOut) {
            *errorOut = "empty node line";
        }
        return false;
    }

    outSpec = NodeLoadSpec{};
    outSpec.typeId = token;

    while (stream >> token) {
        const std::size_t eq = token.find('=');
        if (eq == std::string::npos) {
            if (errorOut) {
                *errorOut = "expected key=value token: " + token;
            }
            return false;
        }

        const std::string key = token.substr(0, eq);
        const std::string value = token.substr(eq + 1);

        if (key == "threshold") {
            if (!parseFloat(value, outSpec.threshold)) {
                if (errorOut) {
                    *errorOut = "invalid threshold: " + value;
                }
                return false;
            }
        } else if (key == "flag") {
            if (!parseU32(value, outSpec.flagIndex)) {
                if (errorOut) {
                    *errorOut = "invalid flag index: " + value;
                }
                return false;
            }
        } else if (key == "loops") {
            if (!parseU32(value, outSpec.loopCount)) {
                if (errorOut) {
                    *errorOut = "invalid loop count: " + value;
                }
                return false;
            }
        } else if (key == "children") {
            if (!parseChildren(value, outSpec.childIndices)) {
                if (errorOut) {
                    *errorOut = "invalid children list: " + value;
                }
                return false;
            }
        } else if (key == "hook") {
            outSpec.scriptHook = value;
        } else {
            if (errorOut) {
                *errorOut = "unknown node attribute: " + key;
            }
            return false;
        }
    }

    return NodeRegistry::instance().hasFactory(outSpec.typeId);
}

} // namespace

bool loadTreeFromText(const std::string& text, BehaviorTree& outTree, std::string* errorOut) {
    std::vector<NodeLoadSpec> specs;
    u32 rootIndex = 0;
    bool rootSpecified = false;

    std::stringstream stream(text);
    std::string line;
    while (std::getline(stream, line)) {
        line = trim(line);
        if (line.empty() || line[0] == '#') {
            continue;
        }

        if (line.rfind("root=", 0) == 0) {
            if (!parseU32(line.substr(5), rootIndex)) {
                if (errorOut) {
                    *errorOut = "invalid root index";
                }
                return false;
            }
            rootSpecified = true;
            continue;
        }

        NodeLoadSpec spec;
        if (!parseNodeLine(line, spec, errorOut)) {
            return false;
        }
        specs.push_back(std::move(spec));
    }

    if (specs.empty()) {
        if (errorOut) {
            *errorOut = "no nodes parsed";
        }
        return false;
    }

    if (!rootSpecified) {
        rootIndex = static_cast<u32>(specs.size() - 1);
    }

    return loadTreeFromSpecs(specs, rootIndex, outTree);
}

} // namespace fuse::ai
