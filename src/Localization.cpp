#include "Localization.h"

#include "Config.h"
#include "Data.h"

using namespace rapidjson;

using namespace QuickArmorRebalance;


void QuickArmorRebalance::Localization::LoadTranslation(const Value& jsonTranslation) {
    if (!jsonTranslation.IsObject()) return;
    for (const auto& lang : jsonTranslation.GetObj()) {
        if (!lang.value.IsObject()) continue;

        auto code = StringToWString(lang.name.GetString());
        auto& trans = translations[code];

        for (const auto& i : lang.value.GetObj()) {
            if (!strcmp(i.name.GetString(), i.value.GetString())) continue; //No actual translation, don't load

            auto hash = std::hash<std::string>{}(i.name.GetString());
            if (trans.contains(hash)) {
                if (trans[hash].compare(i.value.GetString())) {
                    logger::error("Translation hash conflict - double translation or actual conflict?");
                    logger::error("Key: {}", i.name.GetString());
                    logger::error("Value: {}", i.value.GetString());
                    logger::error("Value (existing): {}", trans[hash].c_str());
                }
            }

            trans[hash] = i.value.GetString();
        }
    }
}

void QuickArmorRebalance::Localization::SetTranslation(std::wstring code) {
    Reset();
    mapTranslated = &translations[language = code];
}

const char* Localization::FindTranslation(const char* str) {
    auto hash = std::hash<std::string>{}(str);
    const char* use = str;

    if (mapTranslated) {
        auto it = mapTranslated->find(hash);
        if (it != mapTranslated->end()) use = it->second.c_str();
        else lsUnfound.push_back(str);
    }

    return mapPtrs[str] = use;
}

void QuickArmorRebalance::Localization::Export() {
    if (lsUnfound.empty()) return;
    if (language.empty()) return;

    auto strLanguage = WStringToString(language);

    auto path = std::filesystem::current_path() / PATH_ROOT PATH_CONFIGS;
    path /= std::format("Untranslated {}.json", strLanguage);

    Document d;
    ReadJSONFile(path, d, true);

    if (!d.IsObject()) {
        d.SetObject();
    }
    auto& al = d.GetAllocator();

    auto& objTrans = EnsureHas(d.GetObj(), "translation", kObjectType, al);
    auto& objList = EnsureHas(objTrans.GetObj(), strLanguage.c_str(), kObjectType, al);

    for (auto i : lsUnfound) {
        if (!objList.HasMember(i)) {
            objList.AddMember(Value(i, al), Value(i, al), al);
        }
    }

    WriteJSONFile(path, d);
    lsUnfound.clear();
}
