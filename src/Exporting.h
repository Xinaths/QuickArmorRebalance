#pragma once

namespace QuickArmorRebalance {
    void ExportToDAV(const RE::TESFile* file, const rapidjson::Value& ls, bool bRebuild = false);
}