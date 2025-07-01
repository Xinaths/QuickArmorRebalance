#pragma once

namespace QuickArmorRebalance {
    using namespace rapidjson;

    struct ArmorChangeParams;

    void LoadLootConfig(const Value& jsonLoot);
    void ValidateLootConfig();

    Value MakeLootChanges(const ArmorChangeParams& params, RE::TESBoundObject* i, MemoryPoolAllocator<>& al);
    void LoadLootChanges(RE::TESBoundObject* item, const Value& jsonLoot, unsigned int& changed);
    

    void SetupLootLists();
}