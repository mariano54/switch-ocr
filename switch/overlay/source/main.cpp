#define TESLA_INIT_IMPL
#define TESLA_FULLSCREEN_HUD
#include <tesla.hpp>

#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#include "switchocr_keys.hpp"

#define CONFIG_DIR "sdmc:/config/switch-ocr"
#define REQUEST_PATH CONFIG_DIR "/request.txt"
#define RESULT_PATH CONFIG_DIR "/result.txt"
#define RESULT_JSON_PATH CONFIG_DIR "/result.json"
#define TARGET_PATH CONFIG_DIR "/target.txt"
#define STATUS_PATH CONFIG_DIR "/status.txt"
#define HUD_PATH CONFIG_DIR "/hud.json"
#define KEYS_PATH CONFIG_DIR "/keys.json"
#define GAMES_DIR CONFIG_DIR "/games"

namespace {

constexpr size_t MaxWords = 48;
constexpr size_t SurfaceSize = 160;
constexpr size_t BaseSize = 160;
constexpr size_t ReadingSize = 128;
constexpr size_t DefinitionSize = 384;
constexpr size_t FrequencySize = 32;
constexpr size_t KanjiSize = 128;
constexpr size_t SentenceSize = 2048;
constexpr size_t ResultJsonSize = 12000;
constexpr size_t HudJsonSize = 1024;
constexpr size_t MiningStatusSize = 160;

struct OcrWord {
    char surface[SurfaceSize];
    char base[BaseSize];
    char reading[ReadingSize];
    char definition[DefinitionSize];
    char frequency[FrequencySize];
    char kanji[KanjiSize];
    int frequency_value;
    bool selectable;
};

char g_status[512] = "Switch OCR ready.";
char g_result[SentenceSize] = "Switch OCR ready.";
char g_target[1024] = "";
char g_result_json[ResultJsonSize] = "";
char g_hud_json[HudJsonSize] = "";
char g_hud_frequency[FrequencySize] = "";
char g_hud_kanji[KanjiSize] = "";
char g_hud_mining_status[MiningStatusSize] = "";
OcrWord g_words[MaxWords] = {};
size_t g_word_count = 0;
int g_selected_word = -1;
int g_saved_word_total = 0;
int g_lookup_count = 0;
u32 g_spinner_frame = 0;
u32 g_hud_generation = 0;
bool g_hud_pending = false;
bool g_hud_saved = false;
bool g_hud_saving = false;
bool g_ocr_visual_timed_out = false;
bool g_sd_mounted = false;

u64 g_program_id = 0;
bool g_passthrough = true;
bool g_position_top = false;
int g_hud_opacity = 11;
switchocr::KeyBindings g_bindings = switchocr::defaultBindings();

void copyText(char *out, size_t outSize, const char *text) {
    if (outSize == 0) {
        return;
    }
    snprintf(out, outSize, "%s", text != nullptr ? text : "");
}

bool appendChar(char *out, size_t outSize, size_t &used, char value) {
    if (used + 1 >= outSize) {
        return false;
    }
    out[used++] = value;
    out[used] = '\0';
    return true;
}

bool appendText(char *out, size_t outSize, const char *text) {
    const size_t used = strlen(out);
    if (used + 1 >= outSize) {
        return false;
    }
    snprintf(out + used, outSize - used, "%s", text != nullptr ? text : "");
    return true;
}

bool readText(const char *path, char *out, size_t outSize) {
    if (!g_sd_mounted || outSize == 0) {
        return false;
    }

    FILE *file = fopen(path, "r");
    if (file == nullptr) {
        return false;
    }

    const size_t read = fread(out, 1, outSize - 1, file);
    fclose(file);
    out[read] = '\0';

    size_t size = read;
    while (size > 0 && (out[size - 1] == '\n' || out[size - 1] == '\r')) {
        out[--size] = '\0';
    }
    return size > 0;
}

bool isWhitespace(char value) {
    return value == ' ' || value == '\n' || value == '\r' || value == '\t';
}

void skipWhitespace(const char *&cursor, const char *end) {
    while (cursor < end && isWhitespace(*cursor)) {
        cursor++;
    }
}

const char *parseJsonString(const char *cursor, const char *end, char *out, size_t outSize) {
    if (outSize == 0 || cursor >= end || *cursor != '"') {
        return nullptr;
    }

    cursor++;
    out[0] = '\0';
    size_t used = 0;
    while (cursor < end) {
        const char ch = *cursor++;
        if (ch == '"') {
            return cursor;
        }

        if (ch == '\\' && cursor < end) {
            const char escaped = *cursor++;
            switch (escaped) {
                case '"':
                case '\\':
                case '/':
                    appendChar(out, outSize, used, escaped);
                    break;
                case 'n':
                    appendChar(out, outSize, used, '\n');
                    break;
                case 'r':
                    appendChar(out, outSize, used, '\r');
                    break;
                case 't':
                    appendChar(out, outSize, used, '\t');
                    break;
                case 'u':
                    appendChar(out, outSize, used, '?');
                    for (int i = 0; i < 4 && cursor < end; i++) {
                        cursor++;
                    }
                    break;
                default:
                    appendChar(out, outSize, used, escaped);
                    break;
            }
            continue;
        }

        appendChar(out, outSize, used, ch);
    }
    return nullptr;
}

bool parseJsonField(const char *objectStart, const char *objectEnd, const char *key, char *out, size_t outSize) {
    char keyPattern[16];
    snprintf(keyPattern, sizeof(keyPattern), "\"%s\"", key);

    const char *cursor = objectStart;
    while (cursor < objectEnd) {
        const char *match = strstr(cursor, keyPattern);
        if (match == nullptr || match >= objectEnd) {
            return false;
        }

        cursor = match + strlen(keyPattern);
        skipWhitespace(cursor, objectEnd);
        if (cursor >= objectEnd || *cursor != ':') {
            continue;
        }
        cursor++;
        skipWhitespace(cursor, objectEnd);
        return parseJsonString(cursor, objectEnd, out, outSize) != nullptr;
    }

    return false;
}

bool parseJsonIntField(const char *objectStart, const char *objectEnd, const char *key, int &out) {
    char text[24];
    if (parseJsonField(objectStart, objectEnd, key, text, sizeof(text))) {
        char *end = nullptr;
        const long value = strtol(text, &end, 10);
        if (end != text) {
            out = static_cast<int>(value);
            return true;
        }
    }

    char keyPattern[16];
    snprintf(keyPattern, sizeof(keyPattern), "\"%s\"", key);
    const char *cursor = strstr(objectStart, keyPattern);
    if (cursor == nullptr || cursor >= objectEnd) {
        return false;
    }
    cursor += strlen(keyPattern);
    skipWhitespace(cursor, objectEnd);
    if (cursor >= objectEnd || *cursor != ':') {
        return false;
    }
    cursor++;
    skipWhitespace(cursor, objectEnd);

    char *end = nullptr;
    const long value = strtol(cursor, &end, 10);
    if (end == cursor || end > objectEnd) {
        return false;
    }
    out = static_cast<int>(value);
    return true;
}

bool parseJsonBoolField(const char *objectStart, const char *objectEnd, const char *key, bool &out) {
    char keyPattern[20];
    snprintf(keyPattern, sizeof(keyPattern), "\"%s\"", key);
    const char *cursor = strstr(objectStart, keyPattern);
    if (cursor == nullptr || cursor >= objectEnd) {
        return false;
    }
    cursor += strlen(keyPattern);
    skipWhitespace(cursor, objectEnd);
    if (cursor >= objectEnd || *cursor != ':') {
        return false;
    }
    cursor++;
    skipWhitespace(cursor, objectEnd);
    if (cursor + 4 <= objectEnd && strncmp(cursor, "true", 4) == 0) {
        out = true;
        return true;
    }
    if (cursor + 5 <= objectEnd && strncmp(cursor, "false", 5) == 0) {
        out = false;
        return true;
    }
    return false;
}

bool copyJsonString(const char *json, const char *key, char *out, size_t outSize) {
    char keyPattern[32];
    snprintf(keyPattern, sizeof(keyPattern), "\"%s\"", key);
    const char *cursor = strstr(json, keyPattern);
    if (cursor == nullptr) {
        return false;
    }

    const char *end = json + strlen(json);
    cursor += strlen(keyPattern);
    skipWhitespace(cursor, end);
    if (cursor >= end || *cursor != ':') {
        return false;
    }
    cursor++;
    skipWhitespace(cursor, end);
    return parseJsonString(cursor, end, out, outSize) != nullptr;
}

void stripRubyPlain(const char *input, char *out, size_t outSize) {
    if (outSize == 0) {
        return;
    }

    out[0] = '\0';
    size_t used = 0;
    bool readingRt = false;
    const char *cursor = input;
    while (*cursor != '\0' && used + 1 < outSize) {
        if (*cursor == '<') {
            const char *tagEnd = strchr(cursor, '>');
            if (tagEnd == nullptr) {
                break;
            }
            if (strncmp(cursor, "<rt", 3) == 0) {
                readingRt = true;
            } else if (strncmp(cursor, "</rt", 4) == 0) {
                readingRt = false;
            }
            cursor = tagEnd + 1;
            continue;
        }
        if (!readingRt) {
            out[used++] = *cursor;
            out[used] = '\0';
        }
        cursor++;
    }
}

void extractRubyReading(const char *input, char *out, size_t outSize) {
    if (outSize == 0) {
        return;
    }

    out[0] = '\0';
    size_t used = 0;
    bool readingRt = false;
    const char *cursor = input;
    while (*cursor != '\0' && used + 1 < outSize) {
        if (*cursor == '<') {
            const char *tagEnd = strchr(cursor, '>');
            if (tagEnd == nullptr) {
                break;
            }
            if (strncmp(cursor, "<rt", 3) == 0) {
                readingRt = true;
            } else if (strncmp(cursor, "</rt", 4) == 0) {
                readingRt = false;
            }
            cursor = tagEnd + 1;
            continue;
        }
        if (readingRt) {
            out[used++] = *cursor;
            out[used] = '\0';
        }
        cursor++;
    }
}

bool isSelectableWord(const OcrWord &word) {
    return word.definition[0] != '\0' || word.frequency[0] != '\0' || word.kanji[0] != '\0' || word.base[0] != '\0';
}

int defaultSelectedWord(const OcrWord *words, size_t wordCount) {
    int selected = -1;
    int bestFrequencyValue = -1;
    for (size_t i = 0; i < wordCount; i++) {
        if (words[i].frequency_value >= 0 && words[i].frequency_value > bestFrequencyValue) {
            bestFrequencyValue = words[i].frequency_value;
            selected = static_cast<int>(i);
        }
    }
    if (selected >= 0) {
        return selected;
    }

    for (size_t i = 0; i < wordCount; i++) {
        if (words[i].selectable) {
            return static_cast<int>(i);
        }
    }

    return wordCount > 0 ? 0 : -1;
}

bool parseWordsJson(const char *json) {
    const char *wordsArray = strstr(json, "\"words\"");
    if (wordsArray == nullptr) {
        char display[SentenceSize];
        if (copyJsonString(json, "display_text", display, sizeof(display)) ||
            copyJsonString(json, "error", display, sizeof(display))) {
            copyText(g_result, sizeof(g_result), display);
            g_word_count = 0;
            g_selected_word = -1;
            return true;
        }
        return false;
    }

    const char *cursor = strchr(wordsArray, '[');
    if (cursor == nullptr) {
        return false;
    }
    cursor++;

    OcrWord words[MaxWords] = {};
    size_t wordCount = 0;
    char sentence[SentenceSize] = "";

    while (*cursor != '\0' && *cursor != ']' && wordCount < MaxWords) {
        const char *objectStart = strchr(cursor, '{');
        if (objectStart == nullptr) {
            break;
        }

        const char *objectEnd = strchr(objectStart, '}');
        if (objectEnd == nullptr) {
            break;
        }

        char rawSurface[SurfaceSize] = "";
        char rawBase[BaseSize] = "";
        char rawReading[ReadingSize] = "";
        char rawDefinition[DefinitionSize] = "";
        char rawFrequency[FrequencySize] = "";
        char rawKanji[KanjiSize] = "";
        parseJsonField(objectStart, objectEnd, "w", rawSurface, sizeof(rawSurface));
        parseJsonField(objectStart, objectEnd, "b", rawBase, sizeof(rawBase));
        parseJsonField(objectStart, objectEnd, "r", rawReading, sizeof(rawReading));
        parseJsonField(objectStart, objectEnd, "t", rawDefinition, sizeof(rawDefinition));
        parseJsonField(objectStart, objectEnd, "f", rawFrequency, sizeof(rawFrequency));
        parseJsonField(objectStart, objectEnd, "k", rawKanji, sizeof(rawKanji));

        OcrWord &word = words[wordCount];
        stripRubyPlain(rawSurface, word.surface, sizeof(word.surface));
        stripRubyPlain(rawBase, word.base, sizeof(word.base));
        stripRubyPlain(rawReading, word.reading, sizeof(word.reading));
        if (word.reading[0] == '\0') {
            extractRubyReading(rawSurface, word.reading, sizeof(word.reading));
        }
        stripRubyPlain(rawDefinition, word.definition, sizeof(word.definition));
        copyText(word.kanji, sizeof(word.kanji), rawKanji);
        word.frequency_value = -1;
        parseJsonIntField(objectStart, objectEnd, "fv", word.frequency_value);
        if (word.frequency_value >= 0) {
            snprintf(word.frequency, sizeof(word.frequency), "%d", word.frequency_value);
        } else {
            word.frequency[0] = '\0';
            size_t frequencyUsed = 0;
            for (const char *frequency = rawFrequency; *frequency != '\0' && frequencyUsed + 1 < sizeof(word.frequency); frequency++) {
                if (*frequency >= '0' && *frequency <= '9') {
                    appendChar(word.frequency, sizeof(word.frequency), frequencyUsed, *frequency);
                } else if (frequencyUsed > 0) {
                    break;
                }
            }
            if (frequencyUsed > 0) {
                char *end = nullptr;
                const long parsedFrequency = strtol(word.frequency, &end, 10);
                if (end != word.frequency && parsedFrequency >= 0) {
                    word.frequency_value = static_cast<int>(parsedFrequency);
                }
            }
        }
        word.selectable = isSelectableWord(word);

        if (word.surface[0] != '\0' || word.base[0] != '\0' || word.definition[0] != '\0') {
            appendText(sentence, sizeof(sentence), word.surface[0] != '\0' ? word.surface : word.base);
            wordCount++;
        }

        cursor = objectEnd + 1;
    }

    if (wordCount == 0) {
        char display[SentenceSize];
        if (copyJsonString(json, "display_text", display, sizeof(display)) ||
            copyJsonString(json, "error", display, sizeof(display))) {
            copyText(g_result, sizeof(g_result), display);
        }
        g_word_count = 0;
        g_selected_word = -1;
        return false;
    }

    memcpy(g_words, words, sizeof(g_words));
    g_word_count = wordCount;
    g_selected_word = defaultSelectedWord(g_words, g_word_count);
    copyText(g_result, sizeof(g_result), sentence);
    return true;
}

void parseHudJson(const char *json) {
    const char *end = json + strlen(json);
    int selected = -1;
    bool hasSelected = false;
    if (parseJsonIntField(json, end, "selected", selected)) {
        hasSelected = true;
        g_selected_word = (selected >= 0 && selected < static_cast<int>(g_word_count)) ? selected : -1;
    }
    int generation = 0;
    if (parseJsonIntField(json, end, "generation", generation) && generation >= 0) {
        g_hud_generation = static_cast<u32>(generation);
    }
    g_hud_frequency[0] = '\0';
    g_hud_kanji[0] = '\0';
    g_hud_mining_status[0] = '\0';
    g_hud_saved = false;
    g_hud_saving = false;
    if (!hasSelected || g_selected_word >= 0) {
        parseJsonField(json, end, "f", g_hud_frequency, sizeof(g_hud_frequency));
        parseJsonField(json, end, "k", g_hud_kanji, sizeof(g_hud_kanji));
    }
    parseJsonField(json, end, "m", g_hud_mining_status, sizeof(g_hud_mining_status));
    parseJsonIntField(json, end, "saved_count", g_saved_word_total);
    parseJsonIntField(json, end, "lookup_count", g_lookup_count);
    bool saved = false;
    if (parseJsonBoolField(json, end, "saved", saved)) {
        g_hud_saved = saved;
    }
    bool saving = false;
    if (parseJsonBoolField(json, end, "saving", saving)) {
        g_hud_saving = saving;
    }
    bool pending = false;
    if (parseJsonBoolField(json, end, "pending", pending)) {
        g_hud_pending = pending;
    }
}

void refreshDisplayFiles() {
    char status[sizeof(g_status)];
    if (readText(STATUS_PATH, status, sizeof(status))) {
        copyText(g_status, sizeof(g_status), status);
    }
    char result[sizeof(g_result)];
    if (readText(RESULT_PATH, result, sizeof(result))) {
        copyText(g_result, sizeof(g_result), result);
    }
    char target[sizeof(g_target)];
    if (readText(TARGET_PATH, target, sizeof(target))) {
        copyText(g_target, sizeof(g_target), target);
    }
    char resultJson[sizeof(g_result_json)];
    if (readText(RESULT_JSON_PATH, resultJson, sizeof(resultJson))) {
        if (strcmp(resultJson, g_result_json) != 0 || g_word_count == 0) {
            copyText(g_result_json, sizeof(g_result_json), resultJson);
            parseWordsJson(g_result_json);
        }
    }
    char hudJson[sizeof(g_hud_json)];
    if (readText(HUD_PATH, hudJson, sizeof(hudJson))) {
        copyText(g_hud_json, sizeof(g_hud_json), hudJson);
        parseHudJson(g_hud_json);
    }
    if (strncmp(g_target, "Loading", 7) == 0 || g_hud_pending) {
        g_word_count = 0;
        g_selected_word = -1;
    }
}

void writeTextFile(const char *path, const char *text) {
    if (!g_sd_mounted) {
        return;
    }
    FILE *file = fopen(path, "w");
    if (file == nullptr) {
        return;
    }
    fputs(text, file);
    fclose(file);
}

void gameSettingsPath(char *out, size_t outSize) {
    snprintf(out, outSize, "%s/%016" PRIX64 ".json", GAMES_DIR, g_program_id);
}

void loadKeyBindings() {
    char buffer[256];
    if (readText(KEYS_PATH, buffer, sizeof(buffer))) {
        g_bindings = switchocr::parseBindings(buffer);
    } else {
        g_bindings = switchocr::defaultBindings();
    }
}

void saveKeyBindings() {
    char buffer[160];
    switchocr::serializeBindings(g_bindings, buffer, sizeof(buffer));
    writeTextFile(KEYS_PATH, buffer);
}

int clampHudOpacity(int opacity) {
    if (opacity < 1) {
        return 1;
    }
    if (opacity > 15) {
        return 15;
    }
    return opacity;
}

void formatHudOpacity(char *out, size_t outSize) {
    const int opacity = clampHudOpacity(g_hud_opacity);
    const int percent = (opacity * 100 + 7) / 15;
    snprintf(out, outSize, "%d%%", percent);
}

void loadGameSettings() {
    g_passthrough = true;
    g_position_top = false;
    g_hud_opacity = 11;

    char path[128];
    gameSettingsPath(path, sizeof(path));
    char buffer[160];
    if (!readText(path, buffer, sizeof(buffer))) {
        return;
    }

    const char *end = buffer + strlen(buffer);
    bool passthrough = true;
    if (parseJsonBoolField(buffer, end, "passthrough", passthrough)) {
        g_passthrough = passthrough;
    }
    char position[16];
    if (parseJsonField(buffer, end, "position", position, sizeof(position))) {
        g_position_top = strcmp(position, "top") == 0;
    }
    int opacity = g_hud_opacity;
    if (parseJsonIntField(buffer, end, "opacity", opacity)) {
        g_hud_opacity = clampHudOpacity(opacity);
    }
}

void saveGameSettings() {
    mkdir(GAMES_DIR, 0777);
    char path[128];
    gameSettingsPath(path, sizeof(path));
    char buffer[160];
    snprintf(buffer, sizeof(buffer), "{\"passthrough\":%s,\"position\":\"%s\",\"opacity\":%d}\n",
             g_passthrough ? "true" : "false", g_position_top ? "top" : "bottom", g_hud_opacity);
    writeTextFile(path, buffer);
}

void loadSettings() {
    g_program_id = tsl::hlp::getForegroundProgramId();
    loadKeyBindings();
    loadGameSettings();
    tsl::cfg::passthroughMode = g_passthrough;
}

} // namespace

class SettingsGui final : public tsl::Gui {
public:
    tsl::elm::Element *createUI() override {
        auto *frame = new tsl::elm::OverlayFrame("Switch OCR", "Settings");
        auto *list = new tsl::elm::List();

        auto *passToggle = new tsl::elm::ToggleListItem("Pass-through (live HUD)", g_passthrough, "On", "Off");
        passToggle->setStateChangedListener([](bool state) {
            g_passthrough = state;
            saveGameSettings();
        });
        list->addItem(passToggle);

        auto *positionToggle = new tsl::elm::ToggleListItem("HUD position", g_position_top, "Top", "Bottom");
        positionToggle->setStateChangedListener([](bool state) {
            g_position_top = state;
            saveGameSettings();
        });
        list->addItem(positionToggle);

        char opacityValue[16];
        formatHudOpacity(opacityValue, sizeof(opacityValue));
        auto *opacityItem = new tsl::elm::ListItem("HUD background opacity");
        opacityItem->setValue(opacityValue);
        opacityItem->setClickListener([opacityItem](u64 keys) {
            if (!(keys & HidNpadButton_A)) {
                return false;
            }
            g_hud_opacity += 1;
            if (g_hud_opacity > 15) {
                g_hud_opacity = 1;
            }
            saveGameSettings();

            char value[16];
            formatHudOpacity(value, sizeof(value));
            opacityItem->setValue(value);
            return true;
        });
        list->addItem(opacityItem);

        list->addItem(new tsl::elm::CategoryHeader("Key bindings (global)"));

        for (int action = 0; action < switchocr::Action_Count; action++) {
            auto *item = new tsl::elm::ListItem(switchocr::kActions[action].label);
            item->setValue(switchocr::nameForMask(g_bindings.mask[action]));
            m_remapItems[action] = item;
            item->setClickListener([this, action](u64 keys) {
                // Only start capture when not already capturing, otherwise the A
                // press meant to bind A would just restart capture instead.
                if ((keys & HidNpadButton_A) && m_capturing < 0) {
                    m_capturing = action;
                    m_captureArmed = false;
                    m_remapItems[action]->setValue("Press button...");
                    return true;
                }
                return false;
            });
            list->addItem(item);
        }

        auto *reset = new tsl::elm::ListItem("Reset key bindings to defaults");
        reset->setClickListener([this](u64 keys) {
            if ((keys & HidNpadButton_A) && m_capturing < 0) {
                g_bindings = switchocr::defaultBindings();
                saveKeyBindings();
                for (int action = 0; action < switchocr::Action_Count; action++) {
                    if (m_remapItems[action] != nullptr) {
                        m_remapItems[action]->setValue(switchocr::nameForMask(g_bindings.mask[action]));
                    }
                }
                return true;
            }
            return false;
        });
        list->addItem(reset);

        frame->setContent(list);
        return frame;
    }

    // The menu always runs focused so its navigation never leaks to the game.
    void update() override {
        tsl::cfg::passthroughMode = false;
    }

    bool handleInput(u64 keysDown, u64 keysHeld, const HidTouchState &, HidAnalogStickState, HidAnalogStickState) override {
        if (m_capturing < 0) {
            return false;
        }
        // Ignore input until the A press that started capture is released.
        if (!m_captureArmed) {
            if (!(keysHeld & HidNpadButton_A)) {
                m_captureArmed = true;
            }
            return true;
        }
        if (keysDown & HidNpadButton_B) {
            m_remapItems[m_capturing]->setValue(switchocr::nameForMask(g_bindings.mask[m_capturing]));
            m_capturing = -1;
            return true;
        }
        for (size_t i = 0; i < switchocr::kButtonCount; i++) {
            if (keysDown & switchocr::kButtons[i].mask) {
                g_bindings.mask[m_capturing] = switchocr::kButtons[i].mask;
                saveKeyBindings();
                m_remapItems[m_capturing]->setValue(switchocr::kButtons[i].name);
                m_capturing = -1;
                return true;
            }
        }
        return true;
    }

private:
    tsl::elm::ListItem *m_remapItems[switchocr::Action_Count] = {};
    int m_capturing = -1;
    bool m_captureArmed = false;
};

class SwitchOcrGui final : public tsl::Gui {
public:
    tsl::elm::Element *createUI() override {
        loadSettings();
        switchocr::formatHelpResult(g_bindings, g_result, sizeof(g_result));
        switchocr::formatHelpTarget(g_target, sizeof(g_target));
        refreshDisplayFiles();
        return new HudElement();
    }

    void update() override {
        tsl::cfg::passthroughMode = g_passthrough;
        if (++m_refresh_tick >= 15) {
            const u64 currentProgram = tsl::hlp::getForegroundProgramId();
            if (currentProgram != g_program_id) {
                g_program_id = currentProgram;
                loadGameSettings();
            }
            refreshDisplayFiles();
            updateOcrWatchdog();
            m_refresh_tick = 0;
        }
    }

    // Hold ZL+ZR and press Up to open the settings/remap menu.
    bool handleInput(u64 keysDown, u64 keysHeld, const HidTouchState &, HidAnalogStickState, HidAnalogStickState) override {
        constexpr u64 modifier = HidNpadButton_ZL | HidNpadButton_ZR;
        if ((keysHeld & modifier) == modifier && (keysDown & HidNpadButton_Up)) {
            tsl::changeTo<SettingsGui>();
            return true;
        }
        return false;
    }

    class HudElement final : public tsl::elm::Element {
    public:
        void draw(tsl::gfx::Renderer *renderer) override {
            g_spinner_frame++;
            renderer->clearScreen();

            constexpr s32 panel_x = 0;
            constexpr s32 panel_w = 1280;
            constexpr s32 panel_h = 85;
            const s32 panel_y = g_position_top ? 0 : 720 - panel_h;
            renderer->drawRect(panel_x, panel_y, panel_w, panel_h, tsl::Color(0, 0, 0, static_cast<u8>(g_hud_opacity)));

            drawParagraph(renderer, panel_x + 16, panel_y + 33, panel_w - 32);
            drawTargetRow(renderer, panel_x + 16, panel_y + 59, panel_w - 32);
            drawStatusToggles(renderer, panel_x, panel_y, panel_w);
        }

        void layout(u16 parentX, u16 parentY, u16 parentWidth, u16 parentHeight) override {
            setBoundaries(parentX, parentY, parentWidth, parentHeight);
        }

        tsl::elm::Element *requestFocus(tsl::elm::Element *, tsl::FocusDirection) override {
            return nullptr;
        }

    private:
        static constexpr float ParagraphFontSize = 18.0F;
        static constexpr float FuriganaFontSize = 14.0F;
        static constexpr s32 FuriganaMinGap = 4;
        static constexpr s32 ParagraphLineHeight = 22;
        static constexpr s32 HighlightHeight = 22;
        static constexpr s32 HighlightYOffset = 18;

        s32 measureText(tsl::gfx::Renderer *renderer, const char *text, float fontSize) {
            if (text == nullptr || text[0] == '\0') {
                return 0;
            }
            auto size = renderer->drawString(text, false, 0, 0, fontSize, tsl::Color(0, 0, 0, 0));
            return static_cast<s32>(size.first);
        }

        s32 drawText(
            tsl::gfx::Renderer *renderer,
            const char *text,
            s32 x,
            s32 y,
            float fontSize,
            tsl::Color color,
            s32 maxWidth,
            bool shadow = true
        ) {
            if (text == nullptr || text[0] == '\0' || maxWidth <= 0) {
                return x;
            }
            if (shadow) {
                renderer->drawString(text, false, x + 1, y + 1, fontSize, tsl::Color(0, 0, 0, 15), maxWidth);
            }
            auto size = renderer->drawString(text, false, x, y, fontSize, color, maxWidth);
            return x + static_cast<s32>(size.first);
        }

        size_t utf8CharLen(const char *text) {
            const unsigned char first = static_cast<unsigned char>(text[0]);
            if (first < 0x80) {
                return 1;
            }
            if ((first & 0xE0) == 0xC0) {
                return 2;
            }
            if ((first & 0xF0) == 0xE0) {
                return 3;
            }
            if ((first & 0xF8) == 0xF0) {
                return 4;
            }
            return 1;
        }

        void appendBytes(char *out, size_t outSize, const char *text, size_t length) {
            size_t used = strlen(out);
            for (size_t i = 0; i < length && used + 1 < outSize; i++) {
                out[used++] = text[i];
                out[used] = '\0';
            }
        }

        const char *skipLeadingSpaces(const char *text) {
            while (*text == ' ' || *text == '\t' || *text == '\n' || *text == '\r') {
                text++;
            }
            return text;
        }

        void wrapText(tsl::gfx::Renderer *renderer, const char *text, float fontSize, s32 maxWidth, char *line1, size_t line1Size, char *line2, size_t line2Size) {
            line1[0] = '\0';
            line2[0] = '\0';
            char *lines[] = {line1, line2};
            const size_t sizes[] = {line1Size, line2Size};
            int line = 0;
            const char *cursor = skipLeadingSpaces(text != nullptr ? text : "");

            while (*cursor != '\0' && line < 2) {
                if (*cursor == '\n' || *cursor == '\r') {
                    cursor = skipLeadingSpaces(cursor + 1);
                    line++;
                    continue;
                }

                const size_t charLen = utf8CharLen(cursor);
                char candidate[SentenceSize];
                copyText(candidate, sizeof(candidate), lines[line]);
                appendBytes(candidate, sizeof(candidate), cursor, charLen);
                if (lines[line][0] != '\0' && measureText(renderer, candidate, fontSize) > maxWidth) {
                    line++;
                    cursor = skipLeadingSpaces(cursor);
                    continue;
                }

                appendBytes(lines[line], sizes[line], cursor, charLen);
                cursor += charLen;
            }
        }

        void drawStatusToggles(tsl::gfx::Renderer *renderer, s32 panelX, s32 panelY, s32 panelW) {
            char text[24];
            snprintf(text, sizeof(text), "%s  %s",
                     g_passthrough ? "HUD" : "FOCUS",
                     g_position_top ? "TOP" : "BOT");
            constexpr float size = 11.0F;
            const s32 width = measureText(renderer, text, size);
            const s32 x = panelX + panelW - width - 10;
            drawText(renderer, text, x, panelY + 4, size, tsl::Color(11, 11, 11, 15), width + 8, false);
        }

        void drawPlainParagraph(tsl::gfx::Renderer *renderer, const char *text, s32 x, s32 y, s32 maxWidth) {
            char line1[SentenceSize];
            char line2[SentenceSize];
            wrapText(renderer, text, ParagraphFontSize, maxWidth, line1, sizeof(line1), line2, sizeof(line2));
            drawText(renderer, line1, x, y, ParagraphFontSize, tsl::Color(13, 13, 13, 15), maxWidth);
            drawText(renderer, line2, x, y + ParagraphLineHeight, ParagraphFontSize, tsl::Color(13, 13, 13, 15), maxWidth);
        }

        char spinnerChar() const {
            constexpr char frames[] = "|/-\\";
            return frames[(g_spinner_frame / 6) % 4];
        }

        bool ocrLoading() const {
            return !g_ocr_visual_timed_out && (strncmp(g_target, "Loading", 7) == 0 || g_hud_pending);
        }

        bool savingWord() const {
            return g_hud_saving;
        }

        void compactSpinner(const char *label, char *out, size_t outSize) const {
            snprintf(out, outSize, "%s %c", label, spinnerChar());
        }

        void drawParagraph(tsl::gfx::Renderer *renderer, s32 x, s32 y, s32 maxWidth) {
            if (g_word_count == 0) {
                char loading[16];
                compactSpinner("OCR", loading, sizeof(loading));
                drawPlainParagraph(renderer, ocrLoading() ? g_status : g_result, x, y, maxWidth);
                return;
            }

            s32 cursorX = x;
            s32 cursorY = y;
            s32 previousFuriganaRight = x;
            int line = 0;
            for (size_t i = 0; i < g_word_count; i++) {
                const OcrWord &word = g_words[i];
                const char *surface = word.surface[0] != '\0' ? word.surface : word.base;
                if (surface[0] == '\0') {
                    continue;
                }

                const s32 wordWidth = measureText(renderer, surface, ParagraphFontSize);
                const bool hasFurigana = word.reading[0] != '\0' && strcmp(word.reading, surface) != 0;
                const s32 readingWidth = hasFurigana ? measureText(renderer, word.reading, FuriganaFontSize) : 0;
                if (hasFurigana && cursorX > x) {
                    const s32 readingLeft = cursorX + ((wordWidth - readingWidth) / 2);
                    const s32 extraGap = previousFuriganaRight + FuriganaMinGap - readingLeft;
                    if (extraGap > 0) {
                        cursorX += extraGap;
                    }
                }

                s32 visualRight = cursorX + wordWidth;
                if (hasFurigana) {
                    const s32 readingRight = cursorX + ((wordWidth - readingWidth) / 2) + readingWidth;
                    if (readingRight > visualRight) {
                        visualRight = readingRight;
                    }
                }
                if (cursorX > x && visualRight > x + maxWidth) {
                    line++;
                    if (line >= 2) {
                        break;
                    }
                    cursorX = x;
                    cursorY = y + ParagraphLineHeight;
                    previousFuriganaRight = x;
                }

                const bool selected = static_cast<int>(i) == g_selected_word;
                if (hasFurigana) {
                    const s32 readingX = cursorX + ((wordWidth - readingWidth) / 2);
                    const s32 actualReadingX = readingX > x ? readingX : cursorX;
                    drawText(
                        renderer,
                        word.reading,
                        actualReadingX,
                        cursorY - 17,
                        FuriganaFontSize,
                        selected ? tsl::Color(11, 15, 12, 15) : tsl::Color(10, 12, 10, 15),
                        maxWidth - (cursorX - x),
                        false
                    );
                    previousFuriganaRight = actualReadingX + readingWidth;
                }
                if (selected && wordWidth > 0) {
                    renderer->drawRect(cursorX - 2, cursorY - HighlightYOffset, wordWidth + 4, HighlightHeight, tsl::Color(0, 15, 13, 7));
                }
                cursorX = drawText(
                    renderer,
                    surface,
                    cursorX,
                    cursorY,
                    ParagraphFontSize,
                    selected ? tsl::Color(15, 15, 15, 15) : tsl::Color(13, 13, 13, 15),
                    maxWidth - (cursorX - x)
                );
            }
        }

        void compactStatus(char *out, size_t outSize) {
            if (ocrLoading()) {
                compactSpinner("OCR", out, outSize);
                return;
            }
            if (strstr(g_status, "failed") != nullptr || strstr(g_status, "Failed") != nullptr ||
                strstr(g_status, "Error") != nullptr || strstr(g_status, "error") != nullptr ||
                strstr(g_status, "unavailable") != nullptr || strstr(g_status, "Invalid") != nullptr ||
                strstr(g_status, "Bad ") != nullptr || strstr(g_status, "Could not") != nullptr) {
                copyText(out, outSize, "Error");
                return;
            }
            if (strstr(g_status, "Waiting") != nullptr || strstr(g_status, "request") != nullptr ||
                strstr(g_status, "pending") != nullptr || strstr(g_status, "sent") != nullptr) {
                compactSpinner("OCR", out, outSize);
                return;
            }
            if (savingWord()) {
                compactSpinner("Save", out, outSize);
                return;
            }
            copyText(out, outSize, "Done");
        }

        void formatMetaPrefix(char *out, size_t outSize) {
            out[0] = '\0';
            char totals[64];
            snprintf(totals, sizeof(totals), "W %d", g_saved_word_total);
            appendText(out, outSize, totals);
            appendText(out, outSize, g_hud_saved ? "   Saved" : "   New");
        }

        void truncateUtf8Chars(const char *text, size_t maxChars, char *out, size_t outSize) {
            out[0] = '\0';
            size_t usedChars = 0;
            const char *cursor = text != nullptr ? text : "";
            while (*cursor != '\0' && usedChars < maxChars) {
                const size_t charLen = utf8CharLen(cursor);
                if (strlen(out) + charLen + 1 >= outSize) {
                    break;
                }
                appendBytes(out, outSize, cursor, charLen);
                cursor += charLen;
                usedChars++;
            }
            if (*cursor != '\0' && strlen(out) + 4 < outSize) {
                appendText(out, outSize, "...");
            }
        }

        const OcrWord *selectedWord() {
            if (g_selected_word < 0 || g_selected_word >= static_cast<int>(g_word_count)) {
                return nullptr;
            }
            return &g_words[g_selected_word];
        }

        const char *nextKanjiEntry(const char *cursor, char *kanji, size_t kanjiSize, char *meaning, size_t meaningSize) {
            while (*cursor == ' ') {
                cursor++;
            }
            if (*cursor == '\0') {
                return nullptr;
            }

            kanji[0] = '\0';
            meaning[0] = '\0';
            const size_t kanjiBytes = utf8CharLen(cursor);
            appendBytes(kanji, kanjiSize, cursor, kanjiBytes);
            cursor += kanjiBytes;
            while (*cursor == ' ') {
                cursor++;
            }

            size_t words = 0;
            bool inWord = false;
            while (*cursor != '\0') {
                const bool entryBreak = cursor[0] == ' ' && cursor[1] != '\0' && cursor[1] == ' ' &&
                                        cursor[2] != '\0' && cursor[2] == ' ';
                if (entryBreak) {
                    cursor += 3;
                    break;
                }
                const bool separator = *cursor == ' ' || *cursor == ',' || *cursor == ';';
                if (separator) {
                    if (inWord) {
                        words++;
                        inWord = false;
                        if (words >= 2) {
                            while (*cursor != '\0') {
                                const bool nextBreak = cursor[0] == ' ' && cursor[1] != '\0' && cursor[1] == ' ' &&
                                                       cursor[2] != '\0' && cursor[2] == ' ';
                                if (nextBreak) {
                                    break;
                                }
                                cursor++;
                            }
                            if (*cursor != '\0') {
                                cursor += 3;
                            }
                            break;
                        }
                    }
                    if (meaning[0] != '\0' && meaning[strlen(meaning) - 1] != ' ') {
                        appendText(meaning, meaningSize, " ");
                    }
                    cursor++;
                    continue;
                }
                inWord = true;
                appendBytes(meaning, meaningSize, cursor, utf8CharLen(cursor));
                cursor += utf8CharLen(cursor);
            }
            return cursor;
        }

        void drawMeta(
            tsl::gfx::Renderer *renderer,
            const OcrWord &word,
            s32 x,
            s32 y,
            s32 maxWidth,
            s32 statusWidth
        ) {
            const char *frequency = g_hud_frequency[0] != '\0' ? g_hud_frequency : word.frequency;
            const char *kanji = g_hud_kanji[0] != '\0' ? g_hud_kanji : word.kanji;
            const tsl::Color frequencyColor = word.frequency_value < 0 ? tsl::Color(13, 13, 12, 15)
                : word.frequency_value < 30000                    ? tsl::Color(4, 15, 5, 15)
                : word.frequency_value <= 50000                   ? tsl::Color(15, 14, 3, 15)
                                                                  : tsl::Color(15, 4, 3, 15);
            s32 cursorX = x;
            const s32 rightLimit = x + maxWidth - statusWidth - 18;

            char prefix[96];
            formatMetaPrefix(prefix, sizeof(prefix));
            cursorX = drawText(renderer, prefix, cursorX, y, 12.0F, tsl::Color(11, 11, 11, 15), rightLimit - cursorX, false);
            if (frequency[0] != '\0' && cursorX < rightLimit) {
                char freq[48];
                snprintf(freq, sizeof(freq), "  F %s", frequency);
                cursorX = drawText(renderer, freq, cursorX + 6, y, 15.0F, frequencyColor, rightLimit - cursorX - 6, false);
            }

            const char *entry = kanji;
            while (entry != nullptr && *entry != '\0' && cursorX < rightLimit) {
                char kanjiChar[8];
                char meaning[48];
                entry = nextKanjiEntry(entry, kanjiChar, sizeof(kanjiChar), meaning, sizeof(meaning));
                if (entry == nullptr || kanjiChar[0] == '\0') {
                    break;
                }
                cursorX = drawText(renderer, kanjiChar, cursorX + 12, y, 19.0F, tsl::Color(15, 15, 14, 15), rightLimit - cursorX - 12, false);
                if (meaning[0] != '\0' && cursorX < rightLimit) {
                    cursorX = drawText(renderer, meaning, cursorX + 4, y, 14.0F, tsl::Color(12, 14, 12, 15), rightLimit - cursorX - 4, false);
                }
            }
        }

        void drawTargetRow(tsl::gfx::Renderer *renderer, s32 x, s32 y, s32 maxWidth) {
            char status[16];
            compactStatus(status, sizeof(status));

            const OcrWord *word = selectedWord();
            constexpr float statusSize = 11.0F;
            const s32 rightEdge = x + maxWidth;
            const bool showMeta = word != nullptr && !ocrLoading();
            const s32 metaAreaX = showMeta ? x + (maxWidth * 3 / 5) : rightEdge;
            const s32 metaMaxWidth = rightEdge - metaAreaX;
            const s32 statusWidth = measureText(renderer, status, statusSize);
            const s32 statusX = rightEdge - statusWidth;
            const tsl::Color statusColor = strcmp(status, "Error") == 0 ? tsl::Color(15, 5, 4, 15)
                : (ocrLoading() || savingWord())                   ? tsl::Color(0, 15, 13, 15)
                                                                   : tsl::Color(10, 10, 10, 15);
            drawText(renderer, status, statusX, y, statusSize, statusColor, statusWidth + 4, false);
            if (showMeta) {
                drawMeta(renderer, *word, metaAreaX, y, metaMaxWidth, statusWidth);
            }

            s32 leftMax = statusX - x - 14;
            if (leftMax < 420) {
                leftMax = showMeta ? metaAreaX - x - 18 : maxWidth - 70;
            }
            if (leftMax <= 0) {
                return;
            }

            if (ocrLoading()) {
                char loading[16];
                compactSpinner("OCR", loading, sizeof(loading));
                const char *target = g_target[0] != '\0' && strcmp(g_target, "Loading...") != 0 ? g_target : loading;
                drawText(renderer, target, x, y, 19.0F, tsl::Color(15, 9, 2, 15), leftMax);
                return;
            }

            if (word == nullptr) {
                drawText(renderer, g_target, x, y, 19.0F, tsl::Color(13, 13, 13, 15), leftMax);
                return;
            }

            const char *surface = word->surface[0] != '\0' ? word->surface : word->base;
            s32 cursorX = drawText(renderer, surface, x, y, 22.0F, tsl::Color(15, 8, 1, 15), leftMax);
            if (word->reading[0] != '\0' && strcmp(word->reading, surface) != 0 && cursorX < x + leftMax) {
                char reading[ReadingSize + 4];
                snprintf(reading, sizeof(reading), "(%s)", word->reading);
                cursorX = drawText(
                    renderer,
                    reading,
                    cursorX + 4,
                    y,
                    22.0F,
                    tsl::Color(7, 15, 8, 15),
                    x + leftMax - cursorX - 4,
                    false
                );
            }

            if (cursorX < x + leftMax) {
                cursorX = drawText(renderer, ": ", cursorX + 3, y, 18.0F, tsl::Color(10, 10, 10, 15), x + leftMax - cursorX - 3, false);
            }

            char definition[DefinitionSize];
            truncateUtf8Chars(word->definition[0] != '\0' ? word->definition : "No definition", 60, definition, sizeof(definition));
            if (cursorX < x + leftMax) {
                drawText(
                    renderer,
                    definition,
                    cursorX,
                    y,
                    19.0F,
                    word->definition[0] != '\0' ? tsl::Color(15, 15, 14, 15) : tsl::Color(10, 10, 10, 15),
                    x + leftMax - cursorX
                );
            }
        }
    };

private:
    static constexpr u64 OcrVisualTimeoutNs = 25000000000ULL;

    void updateOcrWatchdog() {
        const bool loading = strncmp(g_target, "Loading", 7) == 0 || g_hud_pending;
        if (!loading) {
            m_loading_started_tick = 0;
            m_loading_generation = g_hud_generation;
            g_ocr_visual_timed_out = false;
            return;
        }

        if (m_loading_started_tick == 0 || m_loading_generation != g_hud_generation) {
            m_loading_started_tick = svcGetSystemTick();
            m_loading_generation = g_hud_generation;
            g_ocr_visual_timed_out = false;
            return;
        }

        if (armTicksToNs(svcGetSystemTick() - m_loading_started_tick) >= OcrVisualTimeoutNs) {
            g_ocr_visual_timed_out = true;
            g_hud_pending = false;
            copyText(g_result, sizeof(g_result), "OCR timed out. Press OCR again.");
            copyText(g_target, sizeof(g_target), "OCR timed out. Press OCR again.");
        }
    }

    u8 m_refresh_tick = 0;
    u64 m_loading_started_tick = 0;
    u32 m_loading_generation = 0;
};

class SwitchOcrOverlay final : public tsl::Overlay {
public:
    void initServices() override {
        g_sd_mounted = R_SUCCEEDED(fsdevMountSdmc());
        if (g_sd_mounted) {
            mkdir("sdmc:/config", 0777);
            mkdir(CONFIG_DIR, 0777);
        } else {
            copyText(g_status, sizeof(g_status), "Could not mount SD card.");
        }
    }

    void exitServices() override {
        if (g_sd_mounted) {
            fsdevUnmountDevice("sdmc");
            g_sd_mounted = false;
        }
    }

    std::unique_ptr<tsl::Gui> loadInitialGui() override {
        return initially<SwitchOcrGui>();
    }
};

int main(int argc, char **argv) {
    return tsl::loop<SwitchOcrOverlay>(argc, argv);
}
