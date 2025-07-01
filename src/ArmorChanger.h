#pragma once

#include "Data.h"
#include "Config.h"

namespace QuickArmorRebalance
{
    ArmorSlots GetConvertableArmorSlots(const ArmorChangeParams& params);
    int MakeArmorChanges(const ArmorChangeParams& params);


    int AddDynamicVariants(const ArmorChangeParams& params);
    int RescanPreferenceVariants();

    int ApplyChanges(const RE::TESFile* file, const rapidjson::Value& ls, const Permissions& perm);
    bool ApplyChanges(const RE::TESFile* file, RE::FormID id, const rapidjson::Value& changes, const Permissions& perm, unsigned int& changed);

    void DeleteChanges(std::set<RE::TESBoundObject*> items, const char** fields = nullptr);

    bool LoadKeywordChanges(const RE::TESFile* file, std::filesystem::path path);
    KeywordChangeMap LoadKeywordChanges(const ArmorChangeParams& params);
    int MakeKeywordChanges(const ArmorChangeParams& params, bool bApply = true);
}
