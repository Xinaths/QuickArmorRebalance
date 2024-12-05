#include "ConsoleCommands.h"

#include "Config.h"
#include "ImGuiIntegration.h"

// Significantly pulled from Custom Console
//   https://www.nexusmods.com/skyrimspecialedition/mods/113094
//   https://github.com/ponzipyramid/CustomConsole/tree/main

// Custom Console got transfered/renamed, now at ConsoleUtil-Extended
//  https://www.nexusmods.com/skyrimspecialedition/mods/133569
//  https://github.com/Scrabx3/ConsoleUtil-Extended/tree/main

using namespace QuickArmorRebalance;

REL::Relocation<void(RE::Script* a_script, RE::ScriptCompiler* a_compiler, RE::COMPILER_NAME a_name, RE::TESObjectREFR* a_targetRef)> _CompileAndRun;

void CompileAndRun(RE::Script* a_script, RE::ScriptCompiler* a_compiler, RE::COMPILER_NAME a_name, RE::TESObjectREFR* a_targetRef) {
    std::istringstream iss(a_script->GetCommand());
    std::vector<std::string> tokens;
    std::string split;

    while (iss >> std::quoted(split)) {
        tokens.push_back(split);
    }

    if (tokens.empty()) return;

    // Only one command atm, so lazy check
    if (!_stricmp(tokens[0].c_str(), "qar")) {
        ImGuiIntegration::Show(true);

        if (g_Config.bCloseConsole) {
            RE::UIMessage msg{"Console", RE::UI_MESSAGE_TYPE::kHide};
            msg.data = nullptr;
            msg.isPooled = false;

            RE::UI::GetSingleton()->GetMenu<RE::Console>()->ProcessMessage(msg);
        }

        return;
    }

    _CompileAndRun(a_script, a_compiler, a_name, a_targetRef);
}

bool QuickArmorRebalance::InstallConsoleCommands() {
    SKSE::AllocTrampoline(1 << 4);

    REL::Relocation<std::uintptr_t> hookPoint;
    if (REL::Module::GetRuntime() != REL::Module::Runtime::VR)
        hookPoint = decltype(hookPoint){REL::RelocationID(52065, 52952), REL::VariantOffset(0xE2, 0x52, 0xE2)};
    else
        hookPoint = 0x90E1F0 + 0xE2;

    auto& trampoline = SKSE::GetTrampoline();
    _CompileAndRun = trampoline.write_call<5>(hookPoint.address(), CompileAndRun);

    logger::debug("Installed console hook");
    return true;
}