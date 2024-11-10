#include "ArmorSetBuilder.h"

#include "Config.h"

using namespace QuickArmorRebalance;

namespace {
    RE::TESObjectARMO* PickBetterType(RE::TESObjectARMO* base, RE::TESObjectARMO* first, RE::TESObjectARMO* second) {
        if (first->bipedModelData.armorType == second->bipedModelData.armorType) return nullptr;
        if (first->bipedModelData.armorType == base->bipedModelData.armorType) return first;
        return second;
    }

    std::set<RE::BGSKeyword*> GetKeywordSetFor(RE::TESObjectARMO* form) {
        std::set<RE::BGSKeyword*> s;
        for (unsigned int i = 0; i < form->numKeywords; i++) s.insert(form->keywords[i]);
        return s;
    }

    RE::TESObjectARMO* PickBetterKeywords(RE::TESObjectARMO* base, RE::TESObjectARMO* first,
                                          RE::TESObjectARMO* second) {
        auto set1(GetKeywordSetFor(first));
        auto set2(GetKeywordSetFor(second));

        int matches1 = 0;
        int matches2 = 0;

        for (unsigned int i = 0; i < base->numKeywords; i++) {
            auto kw = base->keywords[i];
            if (!QuickArmorRebalance::g_Config.kwSet.contains(kw)) continue;  // Only match keywords we care about

            if (set1.contains(kw)) matches1++;
            if (set2.contains(kw)) matches2++;
        }

        if (matches1 > matches2) return first;
        if (matches2 > matches1) return second;

        return nullptr;
    }

    void SplitNumbers(char* token, std::set<std::size_t>& words, std::vector<std::string>* pStrings) {
        auto tail = token;

        while (*tail && !std::isdigit(*tail)) tail++;

        if (*tail && tail != token) {
            // Include whole name too, might cover some rarer potential issues
            auto hash = std::hash<std::string>{}(token);
            words.insert(hash);

            if (pStrings) pStrings->push_back(token);

            // Note that its keeping stuff like "2a" together intentionally, but might be better to break down?
            hash = std::hash<std::string>{}(tail);
            words.insert(hash);
            if (pStrings) pStrings->push_back(tail);
            *tail = '\0';
        }

        auto hash = std::hash<std::string>{}(token);
        words.insert(hash);
        if (pStrings) pStrings->push_back(token);
    }

    WordSet GetWords(RE::TESObjectARMO* item, std::vector<std::string>* pStrings = nullptr) {
        std::string text(item->fullName);

        // Might be a bad idea to transform to lower, as SomeModsMightNameThingsWithoutSpaces which could be handled, if
        // encountered
        ToLower(text);

        auto delimeters = " ()[]<>.,-_:;\\/{}~&";
        WordSet words;

        auto token = std::strtok(text.data(), delimeters);
        while (token) {
            SplitNumbers(token, words, pStrings);
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

    RE::TESObjectARMO* PickBetter(RE::TESObjectARMO* base, RE::TESObjectARMO* first, RE::TESObjectARMO* second) {
        if (auto better = PickBetterType(base, first, second)) return better;
        if (auto better = PickBetterKeywords(base, first, second)) return better;
        if (auto better = PickBetterName(base, first, second)) return better;

        return nullptr;
    }

    std::vector<RE::TESObjectARMO*> FindBestMatches(RE::TESObjectARMO* baseItem,
                                                    const std::vector<RE::TESBoundObject*>& items, unsigned int slots,
                                                    unsigned int covered) {
        std::vector<RE::TESObjectARMO*> best;

        for (auto i : items) {
            if (auto armor = i->As<RE::TESObjectARMO>()) {
                auto slot = (unsigned int)armor->GetSlotMask();
                if ((slots & slot) == 0) continue;
                if ((slot & covered) != 0) continue;

                if (best.empty()) {
                    best.push_back(armor);
                    continue;
                }

                auto better = PickBetter(baseItem, best[0], armor);
                if (better) {
                    if (better != best[0]) {
                        best.clear();
                        best.push_back(armor);
                    }
                } else
                    best.push_back(armor);
            }
        }

        return best;
    }

    bool ContainsAny(const WordSet& group, const WordSet& list) {
        for (auto w : list)
            if (group.contains(w)) return true;
        return false;
    }
}

ArmorSet QuickArmorRebalance::BuildSetFrom(RE::TESBoundObject* baseObj, const std::vector<RE::TESBoundObject*>& items) {
    auto baseItem = baseObj->As<RE::TESObjectARMO>();
    if (!baseItem) return {};

    ArmorSet armorSet;
    unsigned int slots = (unsigned int)baseItem->GetSlotMask();

    armorSet.push_back(baseItem);

    for (auto i : items) {
        if (auto armor = i->As<RE::TESObjectARMO>()) {
            auto slot = (unsigned int)armor->GetSlotMask();
            if ((slots & slot)) continue;

            auto best = FindBestMatches(baseItem, items, slot, slots);
            for (auto j : best) slots |= (unsigned int)j->GetSlotMask();
            armorSet.insert(armorSet.end(), best.begin(), best.end());
        }
    }

    return armorSet;
}

struct WordStats {
    int count = 0;
    ArmorSlots slots = 0;
    unsigned int pos = 0;
    unsigned int posSlots[32] = {};

    WordSet otherWords;
    std::vector<RE::TESObjectARMO*> items;
    std::string strContents;
};

struct SlotStats {
    WordSet words;
    std::vector<RE::TESObjectARMO*> items;

    bool foundPieceWords = false;
};

void QuickArmorRebalance::AnalyzeResults::Clear() {
    for (auto& i : sets) i.clear();
    mapWordStrings.clear();
    mapWordItems.clear();
    mapArmorWords.clear();
}

void QuickArmorRebalance::AnalyzeArmor(const std::vector<RE::TESBoundObject*>& items, AnalyzeResults& results) {
    int nArmors = 0;

    auto& mapWordLookup = results.mapWordStrings;

    std::map<std::size_t, std::size_t> mapWordLinks;
    std::map<std::size_t, std::size_t> mapWordLinksPrev;

    std::map<size_t, WordStats> mapWords;
    std::map<size_t, WordStats*> remainingWords;
    std::map<RE::TESObjectARMO*, WordSet> mapArmorWords;

    auto& setAuthorOrName = results.sets[AnalyzeResults::eWords_NameAndAuthor];
    auto& setPieces = results.sets[AnalyzeResults::eWords_Pieces];
    auto& setNonVariant = results.sets[AnalyzeResults::eWords_NonVariants];
    auto& setVariantsStatic = results.sets[AnalyzeResults::eWords_StaticVariants];
    auto& setVariantsDynamic = results.sets[AnalyzeResults::eWords_DynamicVariants];
    auto& setVariantsEither = results.sets[AnalyzeResults::eWords_EitherVariants];

    SlotStats slotData[32];
    ArmorSlots slotsUsed = 0;

    for (auto i : items) {
        if (auto armor = i->As<RE::TESObjectARMO>()) {
            nArmors++;

            unsigned long slot;
            if (!_BitScanForward(&slot, (ArmorSlots)armor->GetSlotMask())) continue;

            slotData[slot].items.push_back(armor);
            if (slotData[slot].items.size() > 1)  // Only care about a slot if it has more then 1 item to choose from
                slotsUsed |= slot;

            std::vector<std::string> strings;
            auto words = GetWords(armor, &strings);
            for (auto word : words) {
                auto& ws = mapWords[word];
                ws.count++;
                ws.slots |= (1 << slot);
                ws.items.push_back(armor);

                slotData[slot].words.insert(word);

                ws.otherWords.insert(words.begin(), words.end());
            }

            std::size_t prevWord = 0;
            for (int j = 0; j < strings.size(); j++) {
                auto word = std::hash<std::string>{}(strings[j]);
                mapWordLookup[word] = strings[j];
                auto& ws = mapWords[word];
                ws.pos |= 1 << j;
                ws.posSlots[slot] |= 1 << j;

                if (prevWord) {
                    auto it = mapWordLinks.find(prevWord);
                    if (it == mapWordLinks.end()) {
                        if (mapWordLinks.contains(word))
                            mapWordLinks[prevWord] = 0;  // Second word has already been seen seperately
                        else
                            mapWordLinks[prevWord] = word;
                    } else {
                        if (it->second != word) it->second = 0;
                    }

                    it = mapWordLinksPrev.find(word);
                    if (it == mapWordLinksPrev.end()) {
                        mapWordLinksPrev[word] = prevWord;
                    } else {
                        if (it->second != prevWord) {
                            mapWordLinks[prevWord] = 0;    // Break current chain
                            mapWordLinks[it->second] = 0;  // Break existing chain
                        }
                        // Not necessary to 0 out the back looking table, as its not used for anything else but breaking
                        // the previous link
                    }
                }

                prevWord = word;
            }
            mapWordLinks[prevWord] = 0;  // Break any links at end of name

            mapArmorWords[armor] = std::move(words);
        }
    }

    for (auto& i : mapWords) {
        remainingWords[i.first] = &i.second;

        std::sort(i.second.items.begin(), i.second.items.end(),
                  [](RE::TESObjectARMO* a, RE::TESObjectARMO* b) { return _stricmp(a->GetName(), b->GetName()) < 0; });

        size_t nLen = 0;
        for (auto item : i.second.items) nLen += strlen(item->GetName()) + 1;

        i.second.strContents.reserve(nLen);
        for (auto item : i.second.items) {
            if (!i.second.strContents.empty()) i.second.strContents.append("\n");
            i.second.strContents.append(item->GetName());
        }
    }

    // Hide the later parts of multi-word sequences
    for (auto& i : mapWordLinks) {
        if (i.second) {
            if (!g_Config.wordsAllVariants.contains(i.second))  // Break links of known variants
            {
                for (auto item : mapWords[i.second].items) {
                    mapArmorWords[item].erase(i.second);
                }
                remainingWords.erase(i.second);
            }
            else
                i.second = 0;
        }
    }

    // If its on almost every item, its probably the root words, so discard
    const int nExclude = (int)(0.75f * nArmors);

    for (const auto& i : remainingWords) {
        if (i.second->count > nExclude) {
            setAuthorOrName.insert(i.first);
        }
    }
    for (auto i : setAuthorOrName) remainingWords.erase(i);

    // If there's a multi-set mod, and no author signature, the names probably won't match, guess its the first word
    // instead
    if (setAuthorOrName.empty()) {
        for (const auto& i : remainingWords) {
            if (i.second->pos & 1) {
                setAuthorOrName.insert(i.first);
            }
        }
        for (auto i : setAuthorOrName) remainingWords.erase(i);
    }

    // If a word is only one one slot, and all items of the slot, its probably the armor piece
    // (chest/greaves/pants/etc.) Fails if a slot has multiple varations (eg shoes and boots)
    unsigned int posPieces = 0;
    for (const auto& i : remainingWords) {
        if (IsSingleSlot(i.second->slots)) {
            auto slot = GetSlotIndex(i.second->slots);
            if (i.second->count == slotData[slot].items.size()) {
                setPieces.insert(i.first);

                posPieces |= i.second->pos;
                slotData[slot].foundPieceWords = true;
            }
        }
    }
    for (auto i : setPieces) remainingWords.erase(i);

    // If there's piece varations a piece won't be found for that slot, but should be able to infer from positions in
    // previous steps
    if (posPieces) {
        for (ArmorSlot slot = 0; slot < 32; slot++) {
            const auto& sd = slotData[slot];

            if (sd.foundPieceWords || sd.items.empty()) continue;

            // Two passes - pick out potential words, and then only grab the first that appear
            // Otherwise can end up adding multiple words as the piece slot
            unsigned int pos = ~0u;
            for (const auto& i : remainingWords) {
                if (i.second->slots == (1u << slot)                   // Only if specific slot
                    && i.second->pos == (i.second->pos & posPieces))  // Matching word position
                {
                    pos = std::min(i.second->pos, pos);
                }
            }

            for (const auto& i : remainingWords) {
                if (i.second->slots == (1u << slot) && i.second->pos == pos) {
                    setPieces.insert(i.first);
                }
            }
        }
    }
    for (auto i : setPieces) remainingWords.erase(i);

    // Want to only have variant words left
    // Variants should have the property that they have a different item in the same slot with the same name aside from
    // the variant word
    // Variant word could be missing or replaced

    for (const auto& i : remainingWords) {
        if (g_Config.wordsAllVariants.contains(i.first)) continue;  // List of basic likely variants get a pass

        auto& ws = *i.second;

        bool isNonVariant = false;

        for (auto armor : ws.items) {
            auto slots = (ArmorSlots)armor->GetSlotMask();
            if (!slots) continue;

            auto slot = GetSlotIndex(slots);

            if (slotData[slot].items.size() < 2) {  // No variants on this slot
                // Shouldn't be a variant word, but encountered situations where there's Armor 01, but no 02, but it's
                // clearly a variant word on other slots
                // Hopefully the general variant words list will catch those

                isNonVariant = true;
                break;
            }

            bool bMatching = false;
            const auto& words = mapArmorWords[armor];

            for (auto armor2 : slotData[slot].items) {
                if (armor2 == armor) continue;

                const auto& words2 = mapArmorWords[armor2];

                auto diff = words.size() - words2.size();
                if (diff != 0 && diff != 1) continue;
                if (words2.contains(i.first)) continue;

                bool all = true;
                for (auto w : words) {
                    if (w == i.first) continue;
                    if (!words2.contains(w)) {
                        all = false;
                        break;
                    }
                }

                if (all) {
                    bMatching = true;
                    break;
                }
            }

            if (!bMatching) {
                isNonVariant = true;
                break;
            }
        }

        if (isNonVariant) setNonVariant.insert(i.first);
    }
    for (auto i : setNonVariant) remainingWords.erase(i);

    // Group words if they share the same position & slots
    std::map<uint64_t, WordSet> mapWordGroups;
    for (const auto& i : remainingWords) {
        mapWordGroups[(((uint64_t)i.second->pos) << 32) | i.second->slots].insert(i.first);
    }

    // Combine groups if one looks like a seperated part of a group (eg one slot has 1-4, another has 1-5, the 5 will
    // end up on its own)
    std::set<uint64_t> mergedGroups;
    for (auto& g1 : mapWordGroups) {
        if (mergedGroups.contains(g1.first)) continue;

        for (auto& g2 : mapWordGroups) {
            if (mergedGroups.contains(g2.first)) continue;

            if ((g1.first & g2.first) == g2.first) {  // All slots & positions are a subset of g1

                // Only merge if words are mutally exclusive
                bool bConflicts = false;
                for (auto i : g2.second) {
                    const auto& ws2 = mapWords[i];

                    for (auto j : g1.second) {
                        const auto& ws1 = mapWords[j];

                        if (ws2.otherWords.contains(j)) {
                            bConflicts = true;
                            break;
                        }

                        // Need to verify slot sameness on a per slot basis
                        for (auto slots = ws1.slots; slots; slots &= slots - 1) {
                            auto slot = GetSlotIndex(slots);
                            if (ws2.posSlots[slot] && (ws1.posSlots[slot] & ws2.posSlots[slot]) != ws2.posSlots[slot]) {
                                bConflicts = true;
                                break;
                            }
                        }
                    }

                    if (bConflicts) break;
                }

                if (!bConflicts) {
                    g1.second.insert(g2.second.begin(), g2.second.end());
                    mergedGroups.insert(g2.first);
                }
            }
        }
    }

    for (auto i : mergedGroups) mapWordGroups.erase(i);

    // If any members of groups are known, put them in the associated groups
    for (auto& g : mapWordGroups) {
        WordSet* pSet = nullptr;
        if (ContainsAny(g_Config.wordsDynamicVariants, g.second))
            pSet = &setVariantsDynamic;
        else if (ContainsAny(g_Config.wordsStaticVariants, g.second)) {
            pSet = &setVariantsStatic;
        } else if (ContainsAny(g_Config.wordsEitherVariants, g.second)) {
            pSet = &setVariantsEither;
        } else if (ContainsAny(g_Config.wordsPieces, g.second))
            pSet = &setPieces;
        else if (ContainsAny(g_Config.wordsDescriptive, g.second))
            pSet = &setNonVariant;
        else {
            // Big groups are probably static variants, small groups are potentially anything
            if (g.second.size() > 3)
                pSet = &setVariantsStatic;
            else
                pSet = &setVariantsEither;
        }

        if (pSet) pSet->insert(g.second.begin(), g.second.end());
    }

    // Combine linked words
    for (auto& i : mapWordLinks) {
        auto next = i.second;
        while (next) {
            mapWordLookup[i.first] = mapWordLookup[i.first] + " " + mapWordLookup[next];
            auto it = mapWordLinks.find(next);
            if (it == mapWordLinks.end()) break;
            next = it->second;
        }
        i.second = 0;  // break up chains or else 3 part sequences can look like "1 2 3 2 3" etc., and we don't use this
                       // data anymore
    }

    /*
    logger::info("Word links:");
    for (const auto& i : mapWordLinks) {
        if (i.second) {
            logger::info("- {} {}", mapWordLookup[i.first], mapWordLookup[i.second]);
        }
    }

    logger::info("Author & Name:");
    for (const auto& i : setAuthorOrName) {
        logger::info("-- {}", mapWordLookup[i]);
    }

    logger::info("Pieces:");
    for (const auto& i : setPieces) {
        logger::info("-- {}", mapWordLookup[i]);
    }

    logger::info("Other non-variant:");
    for (const auto& i : setNonVariant) {
        logger::info("-- {}", mapWordLookup[i]);
    }

    logger::info("Static variants:");
    for (const auto& i : setVariantsStatic) {
        logger::info("-- {}", mapWordLookup[i]);
    }

    logger::info("Dynamic variants:");
    for (const auto& i : setVariantsDynamic) {
        logger::info("-- {}", mapWordLookup[i]);
    }
    */

    /*
    logger::info("Remaining words:");
    for (const auto& i : mapWords) {
        logger::info("-- {}", mapWordLookup[i.first]);
    }
    */

    /*
    for (const auto& g : mapWordGroups) {
        logger::info("Group {:018x}:", g.first);
        for (const auto& i : g.second) {
            logger::info("-- {}", mapWordLookup[i]);
        }
    }
    */

    for (auto& i : mapWords) {
        results.mapWordItems.emplace(i.first, std::move(AnalyzeResults::WordContents{std::move(i.second.items),
                                                                                     std::move(i.second.strContents)}));
    }
    results.mapArmorWords = std::move(mapArmorWords);
}

// Helper function to collect words from all installed mods
// Meant for testing / internal use as its results are expected to have a high rate of garbage

toml::array WordList(std::map<std::size_t, int>& mapWordUsage, std::map<std::size_t, std::string>& mapWordStrings) {
    std::vector<std::size_t> words;
    for (const auto& w : mapWordUsage) {
        if (w.second > 1) words.push_back(w.first);
    }

    std::sort(words.begin(), words.end(), [&](std::size_t a, std::size_t b) {
        auto nA = mapWordUsage[a];
        auto nB = mapWordUsage[b];
        if (nA == nB)
            return _stricmp(mapWordStrings[a].c_str(), mapWordStrings[b].c_str()) < 0;
        else
            return nA > nB;
    });

    auto arr = toml::array{};
    for (auto w : words) {
        arr.push_back(mapWordStrings[w]);
    }

    return arr;
}

void QuickArmorRebalance::AnalyzeAllArmor() {
    logger::info("Starting all armor analysis...");

    std::map<std::size_t, int> mapWordUsage[AnalyzeResults::eWords_Count];
    std::map<std::size_t, std::string> mapWordStrings;

    std::map<std::size_t, int> mapWordConflicts;

    for (auto i : g_Data.sortedMods) {
        AnalyzeResults results;

        std::vector<RE::TESBoundObject*> items(i->items.begin(), i->items.end());
        AnalyzeArmor(items, results);

        for (int j = 0; j < AnalyzeResults::eWords_Count; j++) {
            for (auto w : results.sets[j]) {
                mapWordUsage[j][w] = 1 + MapFindOr(mapWordUsage[j], w, 0);
            }
        }

        mapWordStrings.merge(results.mapWordStrings);
    }

    for (int i = 0; i < AnalyzeResults::eWords_Count; i++) {
        for (int j = i + 1; j < AnalyzeResults::eWords_Count; j++) {
            if (i == j) continue;

            // Insignificant conflicts
            switch (i) {
                case AnalyzeResults::eWords_EitherVariants:
                    if (j == AnalyzeResults::eWords_StaticVariants) continue;
                    break;
                case AnalyzeResults::eWords_NonVariants:
                    if (j == AnalyzeResults::eWords_Pieces) continue;
                    break;
            }

            for (auto it = mapWordUsage[i].begin(); it != mapWordUsage[i].end();) {
                auto w = *it;
                if (mapWordUsage[j].contains(w.first)) {
                    auto n = mapWordUsage[j][w.first];

                    // Ignore conflicts if overwhelming in one category
                    if (w.second >= n * 4) {
                        mapWordUsage[j].erase(w.first);
                    } else if (n >= w.second * 4) {
                        mapWordUsage[i].erase(it++->first);
                    } else
                        mapWordConflicts[w.first] = w.second + mapWordUsage[j][w.first];
                }
                it++;
            }
        }
    }

    for (auto w : mapWordConflicts) {
        for (int i = 0; i < AnalyzeResults::eWords_Count; i++) mapWordUsage[i].erase(w.first);
    }

    auto tbl = toml::table{};

    tbl.insert("DynamicVariants", WordList(mapWordUsage[AnalyzeResults::eWords_DynamicVariants], mapWordStrings));
    tbl.insert("StaticVariants", WordList(mapWordUsage[AnalyzeResults::eWords_StaticVariants], mapWordStrings));
    tbl.insert("EitherVariants", WordList(mapWordUsage[AnalyzeResults::eWords_EitherVariants], mapWordStrings));
    tbl.insert("NonVariants", WordList(mapWordUsage[AnalyzeResults::eWords_NonVariants], mapWordStrings));
    tbl.insert("Pieces", WordList(mapWordUsage[AnalyzeResults::eWords_Pieces], mapWordStrings));
    tbl.insert("AuthorOrName", WordList(mapWordUsage[AnalyzeResults::eWords_NameAndAuthor], mapWordStrings));
    tbl.insert("Conflicts", WordList(mapWordConflicts, mapWordStrings));

    std::ofstream file(std::filesystem::current_path() / PATH_ROOT "Analyzed Words.json");
    file << toml::json_formatter{tbl};

    logger::info("Finished all armor analysis");
}

inline std::size_t HashStep(std::size_t& hash, std::size_t n) {
    return hash ^= (n + 0x9e3779b9 + (hash << 6) + (hash >> 2));
}

std::size_t QuickArmorRebalance::HashWordSet(const WordSet& set, RE::TESObjectARMO* armor, std::size_t skip,
                                             bool includeTypeAndSlot) {
    std::size_t hash = 0;
    if (includeTypeAndSlot) {
        HashStep(hash, (int)armor->bipedModelData.armorType.get());
        HashStep(hash, (ArmorSlots)armor->GetSlotMask());
    }
    for (auto w : set)
        if (w != skip) {
            HashStep(hash, w);
        }

    return hash;
}

DynamicVariantSets QuickArmorRebalance::MapVariants(
    AnalyzeResults& results, const std::map<const DynamicVariant*, std::vector<std::size_t>>& mapDVWords) {
    DynamicVariantSets ret;

    for (auto& dv : mapDVWords) {
        if (dv.second.empty()) continue;

        // logger::trace("Dynamic Variant: {}", dv.first->name);

        VariantSetMap mapVariants;
        for (auto i : results.mapArmorWords) {
            // logger::trace("Hashing {}:", i.first->GetName());
            // logger::trace("Slots {}:", (ArmorSlots)i.first->GetSlotMask());
            auto hash = HashWordSet(i.second, i.first);
            mapVariants[hash].clear();  // prevent duplicates
            mapVariants[hash].push_back(i.first);
        }

        for (auto w : dv.second) {
            const auto& items = results.mapWordItems[w].items;

            for (auto item : items) {
                // logger::trace("Hashing {} (skip {}):", item->GetName(), results.mapWordStrings[w]);
                // logger::trace("Slots {}:", (ArmorSlots)item->GetSlotMask());
                auto hash = HashWordSet(results.mapArmorWords[item], item, w);
                mapVariants[hash].push_back(item);
                // logger::trace("Set size: {}", mapVariants[hash].size());
            }
        }

        std::erase_if(mapVariants, [](auto& i) { return i.second.size() < 2; });

        /*
        for (auto& i : mapVariants) {
            logger::info("Variant set:");

            for (auto j : i.second) {
                logger::info("-- {}", j->GetName());
            }
        }
        */

        ret[dv.first] = std::move(mapVariants);
    }

    return ret;
}

std::map<std::string, std::vector<RE::TESBoundObject*>> QuickArmorRebalance::GroupItems(
    const std::vector<RE::TESBoundObject*>& items, AnalyzeResults& results) {

    if (results.mapArmorWords.empty()) { //No data to work with - probably not a singular mod, so just return everything solo
        std::map<std::string, std::vector<RE::TESBoundObject*>> ret;
        for (auto item : items) {
            ret.emplace(item->GetName(), std::vector{item});
        }

        return ret;   
    }

    std::map<std::size_t, ArmorSet> sets;

    /*
    for (auto item : items) {
        auto armor = item->As<RE::TESObjectARMO>();
        if (!armor) continue;

        auto it = results.mapArmorWords.find(armor);
        if (it == results.mapArmorWords.end()) continue;

        {
            auto hash = HashWordSet(it->second, armor, 0, false);
            auto& set = sets[hash];
            set.insert(set.begin(), armor);
        }

        for (auto w : it->second) {
            auto hash = HashWordSet(it->second, armor, w, false);
            sets[hash].push_back(armor);
        }
    }
    */

    std::unordered_set<RE::TESObjectARMO*> usedArmor;
    std::unordered_set<RE::TESObjectARMO*> inGroup;
    std::unordered_map<std::size_t, std::string> mapNames;

    for (auto item : items) {
        auto armor = item->As<RE::TESObjectARMO>();
        if (!armor) continue;

        usedArmor.insert(armor);
        auto it = results.mapArmorWords.find(armor);
        if (it == results.mapArmorWords.end()) continue;

        {
            auto hash = HashWordSet(it->second, armor);
            auto& set = sets[hash];
            set.insert(set.begin(), armor);
        }
    }

    for (int eType = AnalyzeResults::eWords_StaticVariants; eType > AnalyzeResults::eWords_DynamicVariants; eType--) {
        for (auto w : results.sets[eType]) {
            auto& witems = results.mapWordItems[w].items;

            // Only group if items aren't already accounted for
            // This will hopefully minimize redundant entries when multiple variants might be stacked (color + variant
            // usually)
            bool bMissing = false;
            for (auto armor : witems) {
                if (!usedArmor.contains(armor)) continue;
                if (!inGroup.contains(armor)) {
                    bMissing = true;
                    break;
                }
            }

            if (!bMissing) continue;

            for (auto armor : witems) {
                if (!usedArmor.contains(armor)) continue;

                auto hash = HashWordSet(results.mapArmorWords[armor], armor, w);
                auto& set = sets[hash];
                switch (set.size()) {
                    case 0: {  // First entry, so make name with *'d word
                        std::string lower = MakeLower(armor->fullName.c_str());
                        auto& strWord = results.mapWordStrings[w];
                        auto pos = lower.find(strWord);
                        if (pos != std::string::npos) {  // Shouldn't ever happen
                            mapNames[hash] = std::string(armor->fullName.c_str()).replace(pos, strWord.size(), "*");
                        }

                    } break;
                    case 1:  // Second entry, add the first as an assigned
                        inGroup.insert(*set.begin());
                        [[fallthrough]];
                    default:  // Fallthrough for all others, just add current as assigned
                        inGroup.insert(armor);
                        break;
                }
                set.push_back(armor);
            }
        }
    }

    std::erase_if(sets, [](auto& i) {
        if (i.second.size() < 2) return true;

        auto type = i.second[0]->bipedModelData.armorType.get();
        auto slots = i.second[0]->GetSlotMask();

        for (auto j : i.second) {
            if (type != j->bipedModelData.armorType.get() || slots != j->GetSlotMask()) return true;
        }

        return false;
    });

    std::map<std::string, std::vector<RE::TESBoundObject*>> ret;
    for (auto& i : sets) {
        auto it = mapNames.find(i.first);
        std::string name;
        if (it == mapNames.end())
            name = i.second[0]->fullName;
        else
            name = it->second;

        std::sort(i.second.begin(), i.second.end(),
                  [](auto& a, auto& b) { return _stricmp(a->fullName.c_str(), b->fullName.c_str()) < 0; });

        ret.emplace(std::move(name), std::vector<RE::TESBoundObject*>(i.second.begin(), i.second.end()));
    }

    // Add in all items that are solo
    for (auto item : items) {
        auto armor = item->As<RE::TESObjectARMO>();
        if (armor && inGroup.contains(armor)) continue;

        ret.emplace(item->GetName(), std::vector{item});
    }

    // std::sort(ret.begin(), ret.end(), [](auto& a, auto& b) { return _stricmp(a[0]->GetName(), b[0]->GetName()) < 0;
    // });

    return ret;
}
