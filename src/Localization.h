#pragma once

namespace QuickArmorRebalance {
    inline std::string WStringToString(const std::wstring& wstr) {
        int size_needed = WideCharToMultiByte(CP_UTF8, 0, wstr.c_str(), (int)wstr.length(), nullptr, 0, nullptr, nullptr);
        if (!size_needed) return "";
        std::string str(size_needed, 0);
        WideCharToMultiByte(CP_UTF8, 0, wstr.c_str(), (int)wstr.length(), &str[0], size_needed, nullptr, nullptr);
        return str;
    }

    inline std::wstring StringToWString(const std::string& str) {
        int bufferSize = MultiByteToWideChar(CP_UTF8, 0, str.c_str(), -1, NULL, 0);
        if (!bufferSize) return std::wstring(); 

        std::wstring wstr(bufferSize - 1, L'\0');

        // Perform the conversion
        MultiByteToWideChar(CP_UTF8, 0, str.c_str(), -1, &wstr[0], bufferSize);

        return wstr;
    }

    class Localization {
    public:
        static Localization* Get() {
            static Localization singleton;
            return &singleton;
        }

        void Export();
        void Reset() { mapPtrs.clear(); }

        const char* GetLocalizedString(const char* str) { 
            auto it = mapPtrs.find(str);
            if (it != mapPtrs.end()) return it->second;
            return FindTranslation(str);
        }

        void LoadTranslation(const rapidjson::Value& jsonTranslation);
        bool HasTranslation(const wchar_t* code) const {
            auto it = translations.find(code);
            if (it == translations.end()) return false;
            if (!it->second.empty() || !wcscmp(code, L"en")) return true;
            return false;
        }
        void SetTranslation(std::wstring code);

        const char* FindTranslation(const char* str);

        std::unordered_map<const char*, const char*> mapPtrs;
        std::vector<const char*> lsUnfound;

        using TranslationMap = std::unordered_map<std::size_t, std::string>;
        std::map<std::wstring, TranslationMap> translations;

        std::wstring language;
        TranslationMap* mapTranslated = nullptr;
    };

    inline const char* Localize(const char* str) { return Localization::Get()->GetLocalizedString(str); }
}