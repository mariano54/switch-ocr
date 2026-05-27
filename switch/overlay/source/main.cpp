#define TESLA_INIT_IMPL
#define TESLA_AUTO_OPEN_OVERLAY
#define TESLA_FULLSCREEN_HUD
#define TESLA_PASS_THROUGH_INPUT
#include <tesla.hpp>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#define CONFIG_DIR "sdmc:/config/switch-ocr"
#define REQUEST_PATH CONFIG_DIR "/request.txt"
#define RESULT_PATH CONFIG_DIR "/result.txt"
#define RESULT_JSON_PATH CONFIG_DIR "/result.json"
#define TARGET_PATH CONFIG_DIR "/target.txt"
#define STATUS_PATH CONFIG_DIR "/status.txt"
#define HUD_PATH CONFIG_DIR "/hud.json"

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

char g_status[512] = "Done";
char g_result[SentenceSize] = "No OCR result yet.";
char g_target[1024] = "No selected word yet.";
char g_result_json[ResultJsonSize] = "";
char g_hud_json[256] = "";
OcrWord g_words[MaxWords] = {};
size_t g_word_count = 0;
int g_selected_word = -1;
bool g_hud_pending = false;
bool g_sd_mounted = false;

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

void writeText(const char *path, const char *text) {
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

void requestOcrOnOpen() {
    char request[64];
    snprintf(request, sizeof(request), "overlay-open %llu", static_cast<unsigned long long>(svcGetSystemTick()));
    writeText(REQUEST_PATH, request);
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
        if (words[i].frequency_value > bestFrequencyValue) {
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
            size_t frequencyUsed = 0;
            for (const char *frequency = rawFrequency; *frequency != '\0' && frequencyUsed + 1 < sizeof(word.frequency); frequency++) {
                if (*frequency >= '0' && *frequency <= '9') {
                    appendChar(word.frequency, sizeof(word.frequency), frequencyUsed, *frequency);
                } else if (frequencyUsed > 0) {
                    break;
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
    if (parseJsonIntField(json, end, "selected", selected)) {
        g_selected_word = (selected >= 0 && selected < static_cast<int>(g_word_count)) ? selected : -1;
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
        copyText(g_result_json, sizeof(g_result_json), resultJson);
        parseWordsJson(g_result_json);
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

} // namespace

class SwitchOcrGui final : public tsl::Gui {
public:
    tsl::elm::Element *createUI() override {
        refreshDisplayFiles();
        return new HudElement();
    }

    void update() override {
        if (++m_refresh_tick >= 15) {
            refreshDisplayFiles();
            m_refresh_tick = 0;
        }
    }

    class HudElement final : public tsl::elm::Element {
    public:
        void draw(tsl::gfx::Renderer *renderer) override {
            renderer->clearScreen();

            constexpr s32 panel_x = 24;
            constexpr s32 panel_y = 592;
            constexpr s32 panel_w = 1232;
            constexpr s32 panel_h = 108;
            renderer->drawRect(panel_x, panel_y, panel_w, panel_h, tsl::Color(0, 0, 0, 8));
            renderer->drawRect(panel_x, panel_y, panel_w, 2, tsl::Color(0, 15, 13, 12));

            drawParagraph(renderer, panel_x + 14, panel_y + 31, panel_w - 28);
            drawTargetRow(renderer, panel_x + 14, panel_y + 86, panel_w - 28);
        }

        void layout(u16 parentX, u16 parentY, u16 parentWidth, u16 parentHeight) override {
            setBoundaries(parentX, parentY, parentWidth, parentHeight);
        }

        tsl::elm::Element *requestFocus(tsl::elm::Element *, tsl::FocusDirection) override {
            return nullptr;
        }

    private:
        static constexpr float ParagraphFontSize = 18.0F;
        static constexpr s32 ParagraphLineHeight = 24;

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

        void drawPlainParagraph(tsl::gfx::Renderer *renderer, const char *text, s32 x, s32 y, s32 maxWidth) {
            char line1[SentenceSize];
            char line2[SentenceSize];
            wrapText(renderer, text, ParagraphFontSize, maxWidth, line1, sizeof(line1), line2, sizeof(line2));
            drawText(renderer, line1, x, y, ParagraphFontSize, tsl::Color(13, 13, 13, 15), maxWidth);
            drawText(renderer, line2, x, y + ParagraphLineHeight, ParagraphFontSize, tsl::Color(13, 13, 13, 15), maxWidth);
        }

        void drawParagraph(tsl::gfx::Renderer *renderer, s32 x, s32 y, s32 maxWidth) {
            if (g_word_count == 0) {
                const char *text = (strncmp(g_target, "Loading", 7) == 0 || g_hud_pending) ? "Loading..." : g_result;
                drawPlainParagraph(renderer, text, x, y, maxWidth);
                return;
            }

            s32 cursorX = x;
            s32 cursorY = y;
            int line = 0;
            for (size_t i = 0; i < g_word_count; i++) {
                const OcrWord &word = g_words[i];
                const char *surface = word.surface[0] != '\0' ? word.surface : word.base;
                if (surface[0] == '\0') {
                    continue;
                }

                const s32 wordWidth = measureText(renderer, surface, ParagraphFontSize);
                if (cursorX > x && cursorX + wordWidth > x + maxWidth) {
                    line++;
                    if (line >= 2) {
                        break;
                    }
                    cursorX = x;
                    cursorY = y + ParagraphLineHeight;
                }

                const bool selected = static_cast<int>(i) == g_selected_word;
                if (selected && wordWidth > 0) {
                    renderer->drawRect(cursorX - 2, cursorY - 19, wordWidth + 4, ParagraphLineHeight, tsl::Color(0, 15, 13, 7));
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
            if (strncmp(g_target, "Loading", 7) == 0 || g_hud_pending || strstr(g_status, "Waiting") != nullptr ||
                strstr(g_status, "request") != nullptr || strstr(g_status, "pending") != nullptr ||
                strstr(g_status, "sent") != nullptr) {
                copyText(out, outSize, "Loading");
                return;
            }
            if (strstr(g_status, "failed") != nullptr || strstr(g_status, "Failed") != nullptr ||
                strstr(g_status, "Error") != nullptr || strstr(g_status, "error") != nullptr ||
                strstr(g_status, "unavailable") != nullptr || strstr(g_status, "Invalid") != nullptr ||
                strstr(g_status, "Bad ") != nullptr || strstr(g_status, "Could not") != nullptr) {
                copyText(out, outSize, "Error");
                return;
            }
            copyText(out, outSize, "Done");
        }

        void formatMeta(const OcrWord &word, char *out, size_t outSize) {
            out[0] = '\0';
            if (word.frequency[0] != '\0') {
                appendText(out, outSize, "Freq ");
                appendText(out, outSize, word.frequency);
            }
            if (word.kanji[0] != '\0') {
                if (out[0] != '\0') {
                    appendText(out, outSize, "   ");
                }
                appendText(out, outSize, word.kanji);
            }
        }

        const OcrWord *selectedWord() {
            if (g_selected_word < 0 || g_selected_word >= static_cast<int>(g_word_count)) {
                return nullptr;
            }
            return &g_words[g_selected_word];
        }

        void drawTargetRow(tsl::gfx::Renderer *renderer, s32 x, s32 y, s32 maxWidth) {
            char status[16];
            compactStatus(status, sizeof(status));

            const OcrWord *word = selectedWord();
            char meta[256] = "";
            if (word != nullptr && strcmp(status, "Loading") != 0) {
                formatMeta(*word, meta, sizeof(meta));
            }

            constexpr float statusSize = 11.0F;
            constexpr float metaSize = 12.0F;
            s32 metaWidth = measureText(renderer, meta, metaSize);
            const s32 maxMetaWidth = 420;
            if (metaWidth > maxMetaWidth) {
                metaWidth = maxMetaWidth;
            }
            const s32 rightEdge = x + maxWidth;
            const s32 metaX = meta[0] != '\0' ? rightEdge - metaWidth : rightEdge;
            const s32 statusWidth = measureText(renderer, status, statusSize);
            const s32 statusX = (meta[0] != '\0' ? metaX : rightEdge) - statusWidth - 16;
            const tsl::Color statusColor = strcmp(status, "Error") == 0 ? tsl::Color(15, 5, 4, 15)
                : strcmp(status, "Loading") == 0                  ? tsl::Color(0, 15, 13, 15)
                                                                   : tsl::Color(10, 10, 10, 15);
            drawText(renderer, status, statusX, y, statusSize, statusColor, statusWidth + 4, false);
            if (meta[0] != '\0') {
                drawText(renderer, meta, metaX, y, metaSize, tsl::Color(10, 10, 10, 15), metaWidth, false);
            }

            s32 leftMax = statusX - x - 14;
            if (leftMax < 420) {
                leftMax = maxWidth - 470;
            }
            if (leftMax <= 0) {
                return;
            }

            if (strcmp(status, "Loading") == 0) {
                drawText(renderer, "Loading", x, y, 22.0F, tsl::Color(15, 9, 2, 15), leftMax);
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
                    y + 2,
                    14.0F,
                    tsl::Color(10, 10, 10, 15),
                    x + leftMax - cursorX - 4,
                    false
                );
            }

            if (cursorX < x + leftMax) {
                cursorX = drawText(renderer, ": ", cursorX + 3, y, 18.0F, tsl::Color(10, 10, 10, 15), x + leftMax - cursorX - 3, false);
            }

            const char *definition = word->definition[0] != '\0' ? word->definition : "No definition";
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
    u8 m_refresh_tick = 0;
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

    void onShow() override {
        requestOcrOnOpen();
    }

    std::unique_ptr<tsl::Gui> loadInitialGui() override {
        return initially<SwitchOcrGui>();
    }
};

int main(int argc, char **argv) {
    return tsl::loop<SwitchOcrOverlay>(argc, argv);
}
