#pragma once

#include "Data.h"

namespace QuickArmorRebalance {
    void ExportToDAV(const RE::TESFile* file, const rapidjson::Value& ls, bool bRebuild = false);
    void ExportAllToDAV();

    bool ExportToKID(const std::vector<RE::TESBoundObject*>& items, const KeywordChangeMap& map, std::filesystem::path path);
    bool ExportToSkypatcher(const std::vector<RE::TESBoundObject*>& items, const KeywordChangeMap& map, std::filesystem::path filename);

    void ImportFromDAV();

    void ImportFromBOS();
}