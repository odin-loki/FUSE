#pragma once
// ShellServices for tests: queued answers, records what was asked.
#include <fuse/editor/editor_controller.hpp>

#include <deque>
#include <string>
#include <vector>

namespace fuse::editor::test {

struct ScriptedServices final : ShellServices {
    std::deque<std::optional<std::string>> files;
    std::deque<PromptAnswer> answers;
    std::vector<std::string> asked, warnings, opened;
    std::optional<std::string> next_() {
        if (files.empty()) return std::nullopt;
        auto f = files.front();
        files.pop_front();
        return f;
    }
    std::optional<std::string> pickOpenFile(std::string_view t, std::string_view, std::string_view) override { asked.emplace_back(t); return next_(); }
    std::optional<std::string> pickSaveFile(std::string_view t, std::string_view, std::string_view) override { asked.emplace_back(t); return next_(); }
    std::optional<std::string> pickDirectory(std::string_view t, std::string_view) override { asked.emplace_back(t); return next_(); }
    std::optional<Rgba> pickColour(Rgba c) override { return c; }
    PromptAnswer askSaveDiscardCancel(std::string_view t) override {
        asked.emplace_back(t);
        if (answers.empty()) return PromptAnswer::Cancel;
        const PromptAnswer a = answers.front();
        answers.pop_front();
        return a;
    }
    void warn(std::string_view t, std::string_view x) override { warnings.emplace_back(std::string(t) + ": " + std::string(x)); }
    bool openExternal(std::string_view path) override { opened.emplace_back(path); return true; }
};

} // namespace fuse::editor::test
