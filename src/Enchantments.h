#pragma once

namespace QuickArmorRebalance {
    void InstallEnchantmentHooks();
    void LoadEnchantmentConfigs(std::filesystem::path path, rapidjson::Document& d);

    bool IsEnchanted(RE::TESBoundObject* obj);

    void FinalizeEnchantmentConfig();
}