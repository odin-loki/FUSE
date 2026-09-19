#pragma once

// Ore: 3DAAK conversation script file loader (without TorqueScript)

#include <fuse/adventure/conversation_script_vm.hpp>

#include <string>
#include <vector>

namespace fuse::adventure {

/// Load conversation script hooks from line-based `.conv` text.
[[nodiscard]] bool load_conversation_hooks_from_text(const std::string& convText,
                                                     std::vector<ConversationScriptHook>& outHooks,
                                                     std::string* errorOut = nullptr);

/// Register hooks parsed from `.conv` text on the VM.
[[nodiscard]] bool register_conversation_hooks_from_text(const std::string& convText,
                                                           ConversationScriptVm& vm,
                                                           std::string* errorOut = nullptr);

/// Parse TorqueScript-style conversation function declarations from script text.
[[nodiscard]] bool load_conversation_hooks_from_torquescript(const std::string& scriptText,
                                                              std::vector<ConversationScriptHook>& outHooks,
                                                              std::string* errorOut = nullptr);

/// Register TorqueScript conversation hooks on the VM.
[[nodiscard]] bool register_conversation_hooks_from_torquescript(const std::string& scriptText,
                                                                   ConversationScriptVm& vm,
                                                                   std::string* errorOut = nullptr);

} // namespace fuse::adventure
