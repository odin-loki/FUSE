#include <fuse/adventure/outpost_loader.hpp>

#include <cctype>

namespace fuse::adventure {

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

std::string parseJsonString(const std::string& token) {
    if (token.size() >= 2 && token.front() == '"' && token.back() == '"') {
        return token.substr(1, token.size() - 2);
    }
    return token;
}

std::string findObjectBody(const std::string& json, const std::string& objectKey) {
    const std::string needle = "\"" + objectKey + "\"";
    const std::size_t keyPos = json.find(needle);
    if (keyPos == std::string::npos) {
        return {};
    }

    const std::size_t braceStart = json.find('{', keyPos);
    if (braceStart == std::string::npos) {
        return {};
    }

    int depth = 0;
    for (std::size_t i = braceStart; i < json.size(); ++i) {
        if (json[i] == '{') {
            ++depth;
        } else if (json[i] == '}') {
            --depth;
            if (depth == 0) {
                return json.substr(braceStart, i - braceStart + 1);
            }
        }
    }
    return {};
}

u32 findFieldU32(const std::string& objectBody, const std::string& fieldKey) {
    const std::string needle = "\"" + fieldKey + "\"";
    const std::size_t keyPos = objectBody.find(needle);
    if (keyPos == std::string::npos) {
        return 0;
    }

    const std::size_t colon = objectBody.find(':', keyPos);
    if (colon == std::string::npos) {
        return 0;
    }

    std::size_t valueStart = colon + 1;
    while (valueStart < objectBody.size() && std::isspace(static_cast<unsigned char>(objectBody[valueStart]))) {
        ++valueStart;
    }

    std::size_t valueEnd = valueStart;
    while (valueEnd < objectBody.size() &&
           (std::isdigit(static_cast<unsigned char>(objectBody[valueEnd])) || objectBody[valueEnd] == '.')) {
        ++valueEnd;
    }

    if (valueEnd == valueStart) {
        return 0;
    }

    return static_cast<u32>(std::stoul(objectBody.substr(valueStart, valueEnd - valueStart)));
}

std::string findFieldString(const std::string& objectBody, const std::string& fieldKey) {
    const std::string needle = "\"" + fieldKey + "\"";
    const std::size_t keyPos = objectBody.find(needle);
    if (keyPos == std::string::npos) {
        return {};
    }

    const std::size_t colon = objectBody.find(':', keyPos);
    if (colon == std::string::npos) {
        return {};
    }

    const std::size_t valueStart = objectBody.find('"', colon);
    if (valueStart == std::string::npos) {
        return {};
    }

    const std::size_t valueEnd = objectBody.find('"', valueStart + 1);
    if (valueEnd == std::string::npos) {
        return {};
    }

    return objectBody.substr(valueStart + 1, valueEnd - valueStart - 1);
}

std::vector<std::string> findStringArray(const std::string& objectBody, const std::string& fieldKey) {
    std::vector<std::string> lines;
    const std::string needle = "\"" + fieldKey + "\"";
    const std::size_t keyPos = objectBody.find(needle);
    if (keyPos == std::string::npos) {
        return lines;
    }

    const std::size_t bracketStart = objectBody.find('[', keyPos);
    const std::size_t bracketEnd = objectBody.find(']', bracketStart);
    if (bracketStart == std::string::npos || bracketEnd == std::string::npos) {
        return lines;
    }

    const std::string arrayBody = objectBody.substr(bracketStart + 1, bracketEnd - bracketStart - 1);
    std::size_t pos = 0;
    while (pos < arrayBody.size()) {
        const std::size_t quoteStart = arrayBody.find('"', pos);
        if (quoteStart == std::string::npos) {
            break;
        }
        const std::size_t quoteEnd = arrayBody.find('"', quoteStart + 1);
        if (quoteEnd == std::string::npos) {
            break;
        }
        lines.push_back(arrayBody.substr(quoteStart + 1, quoteEnd - quoteStart - 1));
        pos = quoteEnd + 1;
    }
    return lines;
}

std::unordered_map<std::string, std::string> findNamedObjects(const std::string& sectionBody) {
    std::unordered_map<std::string, std::string> objects;
    std::size_t pos = 0;
    while (pos < sectionBody.size()) {
        const std::size_t quoteStart = sectionBody.find('"', pos);
        if (quoteStart == std::string::npos) {
            break;
        }
        const std::size_t quoteEnd = sectionBody.find('"', quoteStart + 1);
        if (quoteEnd == std::string::npos) {
            break;
        }

        const std::string name = sectionBody.substr(quoteStart + 1, quoteEnd - quoteStart - 1);
        const std::size_t braceStart = sectionBody.find('{', quoteEnd);
        if (braceStart == std::string::npos) {
            break;
        }

        int depth = 0;
        std::size_t braceEnd = braceStart;
        for (std::size_t i = braceStart; i < sectionBody.size(); ++i) {
            if (sectionBody[i] == '{') {
                ++depth;
            } else if (sectionBody[i] == '}') {
                --depth;
                if (depth == 0) {
                    braceEnd = i;
                    break;
                }
            }
        }

        objects[name] = sectionBody.substr(braceStart, braceEnd - braceStart + 1);
        pos = braceEnd + 1;
    }
    return objects;
}

float findFieldFloat(const std::string& objectBody, const std::string& fieldKey) {
    const std::string needle = "\"" + fieldKey + "\"";
    const std::size_t keyPos = objectBody.find(needle);
    if (keyPos == std::string::npos) {
        return 0.f;
    }

    const std::size_t colon = objectBody.find(':', keyPos);
    if (colon == std::string::npos) {
        return 0.f;
    }

    std::size_t valueStart = colon + 1;
    while (valueStart < objectBody.size() && std::isspace(static_cast<unsigned char>(objectBody[valueStart]))) {
        ++valueStart;
    }

    std::size_t valueEnd = valueStart;
    while (valueEnd < objectBody.size() &&
           (std::isdigit(static_cast<unsigned char>(objectBody[valueEnd])) || objectBody[valueEnd] == '.' ||
            objectBody[valueEnd] == '-')) {
        ++valueEnd;
    }

    if (valueEnd == valueStart) {
        return 0.f;
    }

    return std::stof(objectBody.substr(valueStart, valueEnd - valueStart));
}

OutpostTransformSpec parseTransformBody(const std::string& objectBody) {
    OutpostTransformSpec transform;
    transform.x = findFieldFloat(objectBody, "x");
    transform.y = findFieldFloat(objectBody, "y");
    transform.z = findFieldFloat(objectBody, "z");
    transform.yaw_deg = findFieldFloat(objectBody, "yaw");
    return transform;
}

} // namespace

bool loadOutpostStubFromJson(const std::string& jsonText, OutpostStubContent& outContent, std::string* errorOut) {
    outContent = OutpostStubContent{};
    outContent.scene = findFieldString(jsonText, "scene");
    if (outContent.scene.empty()) {
        if (errorOut) {
            *errorOut = "missing scene field";
        }
        return false;
    }

    const std::string interactablesBody = findObjectBody(jsonText, "interactables");
    if (interactablesBody.empty()) {
        if (errorOut) {
            *errorOut = "missing interactables section";
        }
        return false;
    }

    const auto interactables = findNamedObjects(interactablesBody);
    for (const auto& entry : interactables) {
        const std::string type = findFieldString(entry.second, "type");
        if (type == "door") {
            OutpostDoorSpec door;
            door.keyItem = ItemId(findFieldString(entry.second, "key_item"));
            door.openMessage = findFieldString(entry.second, "open_message");
            outContent.doors[entry.first] = std::move(door);
        } else if (type == "weapon_pickup") {
            OutpostWeaponPickupSpec pickup;
            pickup.weapon = ItemId(findFieldString(entry.second, "weapon"));
            pickup.ammo = ItemId(findFieldString(entry.second, "ammo"));
            pickup.ammoCount = findFieldU32(entry.second, "ammo_count");
            outContent.weaponPickups[entry.first] = std::move(pickup);
        } else if (type == "conversation") {
            OutpostConversationSpec conversation;
            conversation.lines = findStringArray(entry.second, "lines");

            const std::string branchesBody = findObjectBody(entry.second, "branches");
            if (!branchesBody.empty()) {
                const auto branchObjects = findNamedObjects(branchesBody);
                for (const auto& branchEntry : branchObjects) {
                    ConversationBranch branch;
                    branch.id = branchEntry.first;
                    branch.lines = findStringArray(branchEntry.second, "lines");
                    if (!branch.lines.empty()) {
                        conversation.branches.push_back(std::move(branch));
                    }
                }
            }

            outContent.conversations[entry.first] = std::move(conversation);
        }
    }

    const std::string placementsBody = findObjectBody(jsonText, "placements");
    if (!placementsBody.empty()) {
        const auto placements = findNamedObjects(placementsBody);
        for (const auto& entry : placements) {
            outContent.placements[entry.first] = parseTransformBody(entry.second);
        }
    }

    if (outContent.conversations.empty() && outContent.doors.empty() && outContent.weaponPickups.empty()) {
        if (errorOut) {
            *errorOut = "no interactables parsed";
        }
        return false;
    }

    return true;
}

bool loadEmbeddedOutpostStub(OutpostStubContent& outContent, std::string* errorOut) {
    static const char* kOutpostJson =
        "{\n"
        "  \"scene\": \"Outpost\",\n"
        "  \"items\": {\n"
        "    \"rusty_key\": { \"kind\": \"key\", \"description\": \"Opens the maintenance door\" },\n"
        "    \"plasma_rifle\": { \"kind\": \"weapon\", \"ammo\": \"energy_cell\", \"ammo_count\": 20 },\n"
        "    \"energy_cell\": { \"kind\": \"ammo\" }\n"
        "  },\n"
        "  \"interactables\": {\n"
        "    \"maintenance_door\": {\n"
        "      \"type\": \"door\",\n"
        "      \"key_item\": \"rusty_key\",\n"
        "      \"open_message\": \"The door hisses open.\"\n"
        "    },\n"
        "    \"armory_rifle\": {\n"
        "      \"type\": \"weapon_pickup\",\n"
        "      \"weapon\": \"plasma_rifle\",\n"
        "      \"ammo\": \"energy_cell\",\n"
        "      \"ammo_count\": 20\n"
        "    },\n"
        "    \"outpost_guard\": {\n"
        "      \"type\": \"conversation\",\n"
        "      \"lines\": [\n"
        "        \"Halt. State your business.\",\n"
        "        \"The reactor is unstable — keep moving.\"\n"
        "      ],\n"
        "      \"branches\": {\n"
        "        \"aggressive\": {\n"
        "          \"lines\": [\"Stand down or be fired upon.\"]\n"
        "        },\n"
        "        \"polite\": {\n"
        "          \"lines\": [\"Thank you, traveler. Proceed with caution.\"]\n"
        "        }\n"
        "      }\n"
        "    }\n"
        "  },\n"
        "  \"placements\": {\n"
        "    \"outpost_guard\": { \"x\": 2.0, \"y\": 1.0, \"z\": 0.0, \"yaw\": 90.0 },\n"
        "    \"lever_interactable\": { \"x\": 3.0, \"y\": 0.0, \"z\": 0.0, \"yaw\": 0.0 }\n"
        "  }\n"
        "}\n";

    return loadOutpostStubFromJson(kOutpostJson, outContent, errorOut);
}

} // namespace fuse::adventure
