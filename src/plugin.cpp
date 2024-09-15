#include "Config.h"
#include "ConsoleCommands.h"
#include "Data.h"
#include "ImGUIIntegration.h"
#include "UI.h"
#include "LootLists.h"
#include "WarmthHook.h"

namespace QuickArmorRebalance {
    void OnDataLoaded();
    bool BindPapyrusFunctions(RE::BSScript::IVirtualMachine* vm);
    
    SKSEPluginLoad(const SKSE::LoadInterface* skse) {
        SKSE::Init(skse);
        SKSE::GetPapyrusInterface()->Register(BindPapyrusFunctions);
        SetupLog();

        ImGuiIntegration::Start(RenderUI);
        InstallConsoleCommands();

        SKSE::GetMessagingInterface()->RegisterListener([](SKSE::MessagingInterface::Message* message) {
            switch (message->type) {
                case SKSE::MessagingInterface::kDataLoaded:
                    OnDataLoaded();
                    break;
                case SKSE::MessagingInterface::kPostLoadGame:
                case SKSE::MessagingInterface::kNewGame:
                    break;
            }
        });

        return true;
    }

    void OnDataLoaded() {
        g_Data.loot = std::make_unique<decltype(g_Data.loot)::element_type>();

        if (!g_Config.Load()) {
            logger::error("Failed to load configuration files, aborting");
            return;
        }
        ProcessData();
        LoadChangesFromFiles();
        SetupLootLists();

        g_Data.loot.release();

        if (g_Config.bEnableSkyrimWarmthHook)
            InstallWarmthHooks();
    }

    void Papyrus_OpenUI(RE::StaticFunctionTag*) {
        ImGuiIntegration::Show(true);        
    }

    bool BindPapyrusFunctions(RE::BSScript::IVirtualMachine* vm) {
        vm->RegisterFunction("Open", "QuickArmorRebalance", Papyrus_OpenUI);
        return true;
    }
}
