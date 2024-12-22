#include "NameParsing.h"

#include "utf8/utf8.h"
// #include <cctype>   // For std::tolower

using namespace QuickArmorRebalance;

// Helper functions to check character type
inline bool isChineseOrKanji(char32_t ch) {
    return (ch >= 0x4E00 && ch <= 0x9FFF) ||  // Basic CJK Unified Ideographs
           (ch >= 0x3400 && ch <= 0x4DBF) ||  // CJK Extension A
           (ch >= 0x20000 && ch <= 0x2A6DF);  // CJK Extension B
}

inline bool isHiragana(char32_t ch) { return ch >= 0x3040 && ch <= 0x309F; }

inline bool isKatakana(char32_t ch) {
    return (ch >= 0x30A0 && ch <= 0x30FF) ||  // Katakana
           (ch >= 0x31F0 && ch <= 0x31FF);    // Katakana Phonetic Extensions
}

inline bool isDigit(char32_t ch) { return (ch >= '0' && ch <= '9'); }

inline bool isLatin(char32_t ch) {
    return (ch >= 'A' && ch <= 'Z') ||  // Latin uppercase
           (ch >= 'a' && ch <= 'z');    // Latin lowercase
}

bool isCyrillic(char32_t ch) {
    return (ch >= 0x0400 && ch <= 0x04FF) ||  // Basic Cyrillic
           (ch >= 0x0500 && ch <= 0x052F) ||  // Cyrillic Supplement
           (ch >= 0x2DE0 && ch <= 0x2DFF) ||  // Cyrillic Extended-A
           (ch >= 0xA640 && ch <= 0xA69F) ||  // Cyrillic Extended-B
           (ch >= 0x1C80 && ch <= 0x1C8F) ||  // Cyrillic Extended-C
           (ch >= 0x1E030 && ch <= 0x1E08F);  // Cyrillic Extended-D
}

inline bool isUppercaseLatin(char32_t ch) { return ch >= 'A' && ch <= 'Z'; }
inline bool isUppercaseCyrillic(char32_t ch) { return ch >= 0x0410 && ch <= 0x042F; }
inline bool isUppercase(char32_t ch) { return isUppercaseLatin(ch) || isUppercaseCyrillic(ch); }

inline bool isLowercaseLatin(char32_t ch) { return ch >= 'a' && ch <= 'z'; }
inline bool isLowercaseCyrillic(char32_t ch) { return ch >= 0x0430 && ch <= 0x044F; }
inline bool isLowercase(char32_t ch) { return isLowercaseLatin(ch) || isLowercaseCyrillic(ch); }

inline bool isAlphaNum(char32_t ch) { return (ch < 0xf0 && ch != ' ') || isCyrillic(ch); }

inline bool isValidTrailingUpper(char32_t ch) { return ((ch < 0xf0 && ch != ' ' && !isDigit(ch) && !isLowercaseLatin(ch)) || isUppercaseCyrillic(ch)); }
inline bool isValidTrailingLower(char32_t ch) { return ((ch < 0xf0 && ch != ' ' && !isDigit(ch) && !isUppercaseLatin(ch)) || isLowercaseCyrillic(ch)); }

// Helper to add word results
inline void addWordResult(const std::string& word, WordSet& results, std::vector<std::string>* pStrings) {
    results.insert(std::hash<std::string>{}(word));
    if (pStrings) pStrings->push_back(word);
}

inline char32_t to_lower_unicode(char32_t ch) {
    if (isUppercase(ch)) {
        return ch + 0x0020;
    }
    // If not a recognized uppercase character, return as is
    return ch;
}

std::string& toLowerUTF8(std::string& utf8_str) {
    if (!utf8::is_valid(utf8_str.begin(), utf8_str.end())) utf8_str = utf8::replace_invalid(utf8_str);

    for (auto it = utf8_str.begin(), end = utf8_str.end(); it != end;) {
        auto pos = it;
        utf8::append(to_lower_unicode(utf8::next(it, end)), pos);  // Overwrite the current position with the lowercase character
    }
    return utf8_str;
}

// Function to split text into hashed words and optionally store strings
WordSet QuickArmorRebalance::SplitWords(const char* str, std::vector<std::string>* pStrings) {
    WordSet results;

    std::string input(str);
    if (!utf8::is_valid(input.begin(), input.end())) input = utf8::replace_invalid(input);

    bool includesSpaces = false;

    {
        const char32_t space = ' ';
        const std::u32string unwanted = U"()[]{}<>‒–—―〜/\\|:;.,_+^&#@~（）【】《》ー／、．〜，.／";

        auto readIt = input.begin();
        auto end = input.end();

        while (readIt != end) {
            // Decode the next UTF-8 character into a Unicode code point
            auto prevIt = readIt;
            char32_t ch = utf8::next(readIt, end);
            if (ch == space) includesSpaces = true;

            if (unwanted.find(ch) != std::u32string::npos) {
                // Replace all bytes in the range [prevIt, readIt) with spaces
                while (prevIt < readIt) *prevIt++ = ' ';
            }
        }
    }

    /*
    // If there are spaces, split by spaces
    if (includesSpaces) {
        size_t start = 0, pos;
        while ((pos = input.find(' ', start)) != std::string::npos) {
            if (pos > start) {
                std::string word = input.substr(start, pos - start);
                addWordResult(toLowerUTF8(word), results, pStrings);
            }
            start = pos + 1;
        }
        if (start < input.size()) {
            std::string word = input.substr(start);
            addWordResult(toLowerUTF8(word), results, pStrings);
        }
        return results;
    }
    */

    // Process character by character for no-space text
    auto it = input.begin();
    auto end = input.end();

    char32_t ch;
    const auto Extract = [&](auto fnValidTail, bool toLower = false) {
        std::string word;
        utf8::append(ch, std::back_inserter(word));

        while (it != end) {
            char32_t nextCh = utf8::peek_next(it, end);
            if (fnValidTail(nextCh)) {
                utf8::append(utf8::next(it, end), std::back_inserter(word));
            } else {
                break;
            }
        }
        addWordResult(toLower ? toLowerUTF8(word) : word, results, pStrings);
    };

    std::string other;
    while (it != end) {
        ch = utf8::next(it, end);

        if (ch == ' ')
            ;  // Seperator
        else if (isChineseOrKanji(ch)) {
            Extract(isHiragana);
        } else if (isKatakana(ch)) {
            Extract(isKatakana);
        } else if (includesSpaces && isAlphaNum(ch)) {  // Trust mod author when spaces are present to keep words together
            Extract(isAlphaNum, true);
        }  // Item name has no spaces, so try to split based on capitalization or numbers
        else if (isDigit(ch)) {
            Extract(isDigit);
        } else if (isUppercase(ch)) {
            std::string word;
            utf8::append(ch, std::back_inserter(word));

            // Use uppercase to split, but allow all leading uppercase
            while (it != end) {
                char32_t nextCh = utf8::peek_next(it, end);
                if (isValidTrailingUpper(nextCh)) {
                    utf8::append(utf8::next(it, end), std::back_inserter(word));
                } else {
                    break;
                }
            }

            // Lowercase only
            while (it != end) {
                char32_t nextCh = utf8::peek_next(it, end);
                if (isValidTrailingLower(nextCh)) {
                    utf8::append(utf8::next(it, end), std::back_inserter(word));
                } else {
                    break;
                }
            }

            // Add full word
            addWordResult(toLowerUTF8(word), results, pStrings);
        } else if (isLowercase(ch)) {
            Extract(isValidTrailingLower);
        } else {  // String leftover unknowns together
            utf8::append(ch, std::back_inserter(other));
            continue;
        }

        // Add in other leftovers - may be out of order if mixed languages
        if (!other.empty()) {
            addWordResult(other, results, pStrings);
            other.clear();
        }
    }

    if (!other.empty()) {
        addWordResult(other, results, pStrings);
    }

    return results;
}
