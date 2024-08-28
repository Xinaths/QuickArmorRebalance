#include "ArmorSetBuilder.h"
#include "Config.h"

namespace {
    RE::TESObjectARMO* PickBetterType(RE::TESObjectARMO* base, RE::TESObjectARMO* first, RE::TESObjectARMO* second)
    {
        if (first->bipedModelData.armorType == second->bipedModelData.armorType) return nullptr;
        if (first->bipedModelData.armorType == base->bipedModelData.armorType) return first;
        return second;
    }

    std::set<RE::BGSKeyword*> GetKeywordSetFor(RE::TESObjectARMO* form) {
        std::set<RE::BGSKeyword*> s;
        for (unsigned int i = 0; i < form->numKeywords; i++) s.insert(form->keywords[i]);
        return s;
    }

    RE::TESObjectARMO* PickBetterKeywords(RE::TESObjectARMO* base, RE::TESObjectARMO* first, RE::TESObjectARMO* second) {
        auto set1(GetKeywordSetFor(first));
        auto set2(GetKeywordSetFor(second));

        int matches1 = 0;
        int matches2 = 0;

        for (unsigned int i = 0; i < base->numKeywords; i++) {
            auto kw = base->keywords[i];
            if (!QuickArmorRebalance::g_Config.kwSet.contains(kw)) continue; //Only match keywords we care about

            if (set1.contains(kw)) matches1++;
            if (set2.contains(kw)) matches2++;
        }

        if (matches1 > matches2) return first;
        if (matches2 > matches1) return second;

        return nullptr;
    }

    void SplitNumbers(char* token, std::set<std::size_t>& words)
    {
        auto tail = token;

        while (*tail && !std::isdigit(*tail)) tail++;

        if (*tail)
        {
            //Include whole name too, might cover some rarer potential issues
            auto hash = std::hash<std::string>{}(token);
            words.insert(hash);

            //Note that its keeping stuff like "2a" together intentionally, but might be better to break down?
            hash = std::hash<std::string>{}(tail);
            words.insert(hash);
            *tail = '\0';
        }

        auto hash = std::hash<std::string>{}(token);
        words.insert(hash);
    }

    std::set<std::size_t> GetWords(RE::TESObjectARMO* item) { 
        std::string text(item->fullName);

        auto delimeters = " ()[]<>.,-_:;\\/{}~&";
        std::set<std::size_t> words;

        auto token = std::strtok(text.data(), delimeters);
        while (token)
        {
            SplitNumbers(token, words);
            token = std::strtok(nullptr, delimeters);
        }

        return words;
    }

    RE::TESObjectARMO* PickBetterName(RE::TESObjectARMO* base, RE::TESObjectARMO* first, RE::TESObjectARMO* second) {
        auto baseWords = GetWords(base);
        auto words1 = GetWords(first);
        auto words2 = GetWords(second);

        int diff1 = 0;
        int diff2 = 0;

        for (auto i : baseWords) {
            if (!words1.contains(i)) diff1++;
            if (!words2.contains(i)) diff2++;
        }

        for (auto i : words1) {
            if (!baseWords.contains(i)) diff1++;
        }

        for (auto i : words2) {
            if (!baseWords.contains(i)) diff2++;
        }

        if (diff1 < diff2) return first;
        if (diff2 < diff1) return second;

        return nullptr;
    }

    RE::TESObjectARMO* PickBetter(RE::TESObjectARMO* base, RE::TESObjectARMO* first, RE::TESObjectARMO* second) 
    {
        if (auto better = PickBetterType(base, first, second)) return better;
        if (auto better = PickBetterKeywords(base, first, second)) return better;
        if (auto better = PickBetterName(base, first, second)) return better;

        return nullptr;
    }

    std::vector<RE::TESObjectARMO*> FindBestMatches(RE::TESObjectARMO* baseItem, const std::vector<RE::TESObjectARMO*>& items,
                                                    unsigned int slots, unsigned int covered)
    {
        std::vector<RE::TESObjectARMO*> best;
        
        for (auto i : items) {
            auto slot = (unsigned int)i->GetSlotMask();
            if ((slots & slot) == 0) continue;
            if ((slot & covered) != 0) continue;

            if (best.empty()) {
                best.push_back(i);
                continue;
            }

            auto better = PickBetter(baseItem, best[0], i);
            if (better)
            {
                if (better != best[0]) {
                    best.clear();
                    best.push_back(i);
                }
            } else
                best.push_back(i);
        }

        return best;
    }
}

std::vector<RE::TESObjectARMO*> QuickArmorRebalance::BuildSetFrom(RE::TESObjectARMO* baseItem,
                                                                  const std::vector<RE::TESObjectARMO*>& items) {
    std::vector<RE::TESObjectARMO*> armorSet;
    unsigned int slots = (unsigned int)baseItem->GetSlotMask();

    armorSet.push_back(baseItem);

    for (auto i : items)
    {
        auto slot = (unsigned int)i->GetSlotMask();
        if ((slots & slot)) continue;

        auto best = FindBestMatches(baseItem, items, slot, slots);
        for (auto j : best) slots |= (unsigned int)j->GetSlotMask();
        armorSet.insert(armorSet.end(), best.begin(), best.end());
    }

    return armorSet;
}
