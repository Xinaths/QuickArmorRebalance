#pragma once

#include "Data.h"
#include "Config.h"

namespace QuickArmorRebalance
{
    ArmorSlots GetConvertableArmorSlots(const ArmorChangeParams& params);
    void MakeArmorChanges(const ArmorChangeParams& params);

    void AddDynamicVariants(const ArmorChangeParams& params);
    void RescanPreferenceVariants();

    void ApplyChanges(const RE::TESFile* file, const rapidjson::Value& ls, const Permissions& perm);
    bool ApplyChanges(const RE::TESFile* file, RE::FormID id, const rapidjson::Value& changes, const Permissions& perm);

    void DeleteChanges(std::set<RE::TESBoundObject*> items, const char** fields = nullptr);
}
