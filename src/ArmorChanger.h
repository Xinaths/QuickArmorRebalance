#pragma once

#include "Config.h"

#include <rapidjson/fwd.h>

namespace QuickArmorRebalance
{
    void MakeArmorChanges(const ArmorChangeParams& params);

    void ApplyChanges(const RE::TESFile* file, const rapidjson::Value& ls, const Permissions& perm);
    bool ApplyChanges(const RE::TESFile* file, RE::FormID id, const rapidjson::Value& changes, const Permissions& perm);
}
