#pragma once

namespace QuickArmorRebalance {
    using namespace rapidjson;

    struct ArmorChangeParams;

    void LoadLootConfig(const Value& jsonLoot);
    void ValidateLootConfig();

    Value MakeLootChanges(const ArmorChangeParams& params, RE::TESObjectARMO* i, MemoryPoolAllocator<>& al);
    void LoadLootChanges(RE::TESObjectARMO* item, const Value& jsonLoot);
    

    void SetupLootLists();
}