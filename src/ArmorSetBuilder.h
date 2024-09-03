#pragma once

namespace QuickArmorRebalance
{
    std::vector<RE::TESObjectARMO*> BuildSetFrom(RE::TESBoundObject* baseItem,
                                                 const std::vector<RE::TESBoundObject*>& items);
}