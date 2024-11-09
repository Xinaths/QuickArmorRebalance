#pragma once

#include "Data.h"

namespace QuickArmorRebalance {
    ArmorSet BuildSetFrom(RE::TESBoundObject* baseItem, const std::vector<RE::TESBoundObject*>& items);

    struct AnalyzeResults {
        enum {
            eWords_DynamicVariants,
            eWords_EitherVariants,
            eWords_StaticVariants,
            eWords_NonVariants,
            eWords_Pieces,
            eWords_NameAndAuthor,
            eWords_Count
        };

        struct WordContents {
            std::vector<RE::TESObjectARMO*> items;
            std::string strItemList;
        };

        WordSet sets[eWords_Count];

        std::map<std::size_t, std::string> mapWordStrings;
        std::map<size_t, WordContents> mapWordItems;
        std::map<RE::TESObjectARMO*, WordSet> mapArmorWords;

        void Clear();
    };

    std::size_t HashWordSet(const WordSet& set, RE::TESObjectARMO* armor, std::size_t skip = 0,
                            bool includeTypeAndSlot = true);

    void AnalyzeArmor(const std::vector<RE::TESBoundObject*>& items, AnalyzeResults& results);
    void AnalyzeAllArmor();

    DynamicVariantSets MapVariants(AnalyzeResults& results,
                                   const std::map<const DynamicVariant*, std::vector<std::size_t>>& mapDVWords);

    std::map<std::string, std::vector<RE::TESBoundObject*>> GroupItems(const std::vector<RE::TESBoundObject*>& items,
                                               AnalyzeResults& results);

}