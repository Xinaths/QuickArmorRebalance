#pragma once

namespace QuickArmorRebalance {
    void InstallEnchantmentHooks();

    bool IsEnchanted(RE::TESBoundObject* obj);

    void FinalizeEnchantmentConfig();
}