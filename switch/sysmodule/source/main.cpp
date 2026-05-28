#include <switch.h>

#define STB_TRUETYPE_IMPLEMENTATION
#include <stb_truetype.h>

#include <algorithm>
#include <atomic>
#include <arpa/inet.h>
#include <cwctype>
#include <dirent.h>
#include <errno.h>
#include <netinet/in.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <unistd.h>

#ifndef SERVER_HOST
#define SERVER_HOST "192.168.0.124"
#endif

#ifndef SERVER_PORT
#define SERVER_PORT 8742
#endif

#define CONFIG_DIR "sdmc:/config/switch-ocr"
#define REQUEST_PATH CONFIG_DIR "/request.txt"
#define RESULT_PATH CONFIG_DIR "/result.txt"
#define RESULT_JSON_PATH CONFIG_DIR "/result.json"
#define TARGET_PATH CONFIG_DIR "/target.txt"
#define STATUS_PATH CONFIG_DIR "/status.txt"
#define HUD_PATH CONFIG_DIR "/hud.json"
#define DEBUG_PATH CONFIG_DIR "/debug.txt"

extern "C" {
extern char *fake_heap_start;
extern char *fake_heap_end;
extern u64 __nx_vi_layer_id;
u32 __nx_applet_type = AppletType_None;
u32 __nx_fs_num_sessions = 1;
u32 __nx_nv_transfermem_size = 0x40000;
}

namespace {

constexpr size_t MaxWords = 48;
constexpr size_t SurfaceSize = 160;
constexpr size_t BaseSize = 160;
constexpr size_t ReadingSize = 128;
constexpr size_t DefinitionSize = 384;
constexpr size_t FrequencySize = 32;
constexpr size_t KanjiSize = 128;
constexpr size_t SentenceSize = 2048;
constexpr size_t ResponseSize = 65536;
constexpr size_t MaxSavedWords = 4096;
constexpr size_t MiningRequestSize = 8192;
constexpr size_t MaxMiningQueue = 8;
constexpr u32 FramebufferWidth = 1280;
constexpr u32 FramebufferHeight = 720;
constexpr u32 LayerWidth = 1920;
constexpr u32 LayerHeight = 1080;
constexpr size_t OcrWorkerStackSize = 0x8000;
constexpr u64 OcrWorkerSleepNs = 50000000;
constexpr u64 MiningWorkerSleepNs = 100000000;
constexpr u64 ScreenshotProbeWorkerSleepNs = 100000000;
constexpr u64 InputPollSleepNs = 20000000;
constexpr u64 OcrRequestTimeoutNs = 20000000000ULL;
constexpr u32 OcrCooldownFrames = 12;
constexpr u32 MineCooldownFrames = 12;
constexpr ViLayerStack ApplicationLayerStack = static_cast<ViLayerStack>(8);
constexpr bool EnableSysmoduleViHud = false;
constexpr size_t AlbumProbeChunkSize = 16 * 1024;
constexpr int AlbumProbeMaxDepth = 6;
constexpr size_t CapsscJpegBufferSize = 4 * 1024 * 1024;
// Retain the old SysDVR/latest-frame OCR path, but do not use it by default.
constexpr bool UseSysDvrOcrFallback = false;
constexpr bool UploadScreenshotProbeWithOcr = false;

struct OcrWord {
    char surface[SurfaceSize];
    char surface_plain[SurfaceSize];
    char base[BaseSize];
    char base_plain[BaseSize];
    char reading[ReadingSize];
    char definition[DefinitionSize];
    char frequency[FrequencySize];
    char kanji[KanjiSize];
    int frequency_value;
    bool selectable;
    bool saved;
    bool saving;
};

struct MiningQueueEntry {
    int action;
    char key[BaseSize];
    char body[MiningRequestSize];
};

struct AlbumProbeFile {
    char path[512];
    time_t mtime;
    off_t size;
};

struct Color {
    union {
        struct {
            u16 r : 4;
            u16 g : 4;
            u16 b : 4;
            u16 a : 4;
        } __attribute__((packed));
        u16 rgba;
    };

    constexpr Color(u16 raw) : rgba(raw) {}
    constexpr Color(u8 red, u8 green, u8 blue, u8 alpha) : r(red), g(green), b(blue), a(alpha) {}
};

char g_heap[0x900000];
char g_request[256];
char g_last_request[256];
char g_response[ResponseSize];
char g_status[512] = "Switch OCR HUD ready. Press Minus to OCR.";
char g_mining_status[160] = "Mining sync pending.";
char g_sentence[SentenceSize] = "No OCR result yet.";
OcrWord g_words[MaxWords] = {};
char g_saved_word_keys[MaxSavedWords][BaseSize] = {};
char g_mining_response[ResponseSize] = "";
char g_mining_body[ResponseSize] = "";
MiningQueueEntry g_mining_queue[MaxMiningQueue] = {};
char g_mining_saving_keys[MaxMiningQueue][BaseSize] = {};
size_t g_word_count = 0;
size_t g_saved_word_key_count = 0;
int g_selected_word = -1;
int g_saved_word_total = 0;
int g_lookup_count = 0;
bool g_sm_initialized = false;
bool g_fs_initialized = false;
bool g_sd_mounted = false;
bool g_hid_initialized = false;
bool g_hidsys_initialized = false;
bool g_capture_button_initialized = false;
bool g_pl_initialized = false;
bool g_hos_version_initialized = false;
bool g_socket_driver_initialized = false;
bool g_nifm_initialized = false;
bool g_nifm_request_ready = false;
NifmRequest g_nifm_request = {};
u32 g_request_count = 0;
std::atomic_bool g_ocr_pending = false;
std::atomic_bool g_ocr_requested = false;
std::atomic_bool g_ocr_worker_busy = false;
std::atomic_bool g_ocr_thread_running = false;
std::atomic<u32> g_ocr_generation = 0;
std::atomic<u32> g_ocr_timeout_generation = 0;
std::atomic<u64> g_ocr_started_tick = 0;
std::atomic_int g_active_ocr_socket = -1;
bool g_ocr_thread_started = false;
Thread g_ocr_thread = {};
std::atomic_uint g_mining_queue_head = 0;
std::atomic_uint g_mining_queue_tail = 0;
std::atomic_uint g_mining_saving_count = 0;
std::atomic_bool g_mining_thread_running = false;
bool g_mining_thread_started = false;
Thread g_mining_thread = {};
std::atomic_bool g_screenshot_probe_thread_running = false;
std::atomic<u32> g_screenshot_probe_generation = 0;
std::atomic<u32> g_screenshot_probe_handled_generation = 0;
bool g_screenshot_probe_thread_started = false;
Thread g_screenshot_probe_thread = {};
AlbumProbeFile g_last_album_probe_file = {};
bool g_album_probe_baselined = false;
u32 g_loading_frame = 0;

enum MiningAction {
    MiningActionNone = 0,
    MiningActionStatus = 1,
    MiningActionMine = 2,
};

u32 next_mining_queue_index(u32 index) {
    return (index + 1) % MaxMiningQueue;
}

bool enqueue_mining_request(const MiningQueueEntry &entry) {
    const u32 tail = g_mining_queue_tail.load(std::memory_order_relaxed);
    const u32 next_tail = next_mining_queue_index(tail);
    if (next_tail == g_mining_queue_head.load(std::memory_order_acquire)) {
        return false;
    }

    g_mining_queue[tail] = entry;
    g_mining_queue_tail.store(next_tail, std::memory_order_release);
    return true;
}

bool dequeue_mining_request(MiningQueueEntry &entry) {
    const u32 head = g_mining_queue_head.load(std::memory_order_relaxed);
    if (head == g_mining_queue_tail.load(std::memory_order_acquire)) {
        return false;
    }

    entry = g_mining_queue[head];
    g_mining_queue_head.store(next_mining_queue_index(head), std::memory_order_release);
    return true;
}

void set_status(const char *message) {
    snprintf(g_status, sizeof(g_status), "%s", message);
}

void copy_text(char *out, size_t out_size, const char *text) {
    if (out_size == 0) {
        return;
    }
    snprintf(out, out_size, "%s", text != nullptr ? text : "");
}

bool append_text(char *out, size_t out_size, const char *text) {
    const size_t used = strlen(out);
    if (used + 1 >= out_size) {
        return false;
    }
    snprintf(out + used, out_size - used, "%s", text != nullptr ? text : "");
    return true;
}

bool append_json_escaped(char *out, size_t out_size, const char *text) {
    size_t used = strlen(out);
    for (const char *cursor = text != nullptr ? text : ""; *cursor != '\0'; cursor++) {
        const char ch = *cursor;
        const char *escaped = nullptr;
        switch (ch) {
            case '"':
                escaped = "\\\"";
                break;
            case '\\':
                escaped = "\\\\";
                break;
            case '\n':
                escaped = "\\n";
                break;
            case '\r':
                escaped = "\\r";
                break;
            case '\t':
                escaped = "\\t";
                break;
            default:
                break;
        }

        if (escaped != nullptr) {
            const size_t length = strlen(escaped);
            if (used + length >= out_size) {
                return false;
            }
            memcpy(out + used, escaped, length);
            used += length;
        } else {
            if (used + 1 >= out_size) {
                return false;
            }
            out[used++] = ch;
        }
        out[used] = '\0';
    }
    return true;
}

void copy_json_escaped(char *out, size_t out_size, const char *text) {
    if (out_size == 0) {
        return;
    }
    out[0] = '\0';
    append_json_escaped(out, out_size, text);
}

void write_text(const char *path, const char *text) {
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

void write_debug(const char *text) {
    write_text(DEBUG_PATH, text);
}

bool read_text(const char *path, char *out, size_t out_size) {
    if (!g_sd_mounted || out_size == 0) {
        return false;
    }

    FILE *file = fopen(path, "r");
    if (file == nullptr) {
        return false;
    }

    size_t read = fread(out, 1, out_size - 1, file);
    fclose(file);
    out[read] = '\0';

    while (read > 0 && (out[read - 1] == '\n' || out[read - 1] == '\r')) {
        out[--read] = '\0';
    }
    return read > 0;
}

bool ensure_services_ready(bool include_hud_services) {
    if (!g_sm_initialized) {
        Result rc = smInitialize();
        if (R_FAILED(rc)) {
            return false;
        }
        g_sm_initialized = true;
    }

    if (!g_fs_initialized) {
        Result rc = fsInitialize();
        if (R_FAILED(rc)) {
            return false;
        }
        g_fs_initialized = true;
    }

    if (!g_sd_mounted) {
        Result rc = fsdevMountSdmc();
        if (R_FAILED(rc)) {
            return false;
        }
        g_sd_mounted = true;
    }

    if (!include_hud_services) {
        return true;
    }

    return true;
}

bool ensure_hid_ready() {
    if (!g_hid_initialized) {
        Result rc = hidInitialize();
        if (R_FAILED(rc)) {
            char debug[128];
            snprintf(debug, sizeof(debug), "HID init failed: 0x%x", rc);
            write_debug(debug);
            return false;
        }
        g_hid_initialized = true;
    }

    if (!g_hidsys_initialized) {
        Result rc = hidsysInitialize();
        if (R_SUCCEEDED(rc)) {
            g_hidsys_initialized = true;
            rc = hidsysEnableAppletToGetInput(true);
        }

        char debug[160];
        snprintf(debug, sizeof(debug), "HID active. hidsys rc=0x%x", rc);
        write_debug(debug);
    }

    if (g_hidsys_initialized && !g_capture_button_initialized && R_SUCCEEDED(hidsysActivateCaptureButton())) {
        g_capture_button_initialized = true;
    }

    return true;
}

bool ensure_font_ready() {
    if (!g_pl_initialized) {
        Result rc = plInitialize(PlServiceType_User);
        if (R_FAILED(rc)) {
            rc = plInitialize(PlServiceType_System);
        }
        if (R_FAILED(rc)) {
            char debug[128];
            snprintf(debug, sizeof(debug), "PL init failed: 0x%x", rc);
            write_debug(debug);
            return false;
        }
        g_pl_initialized = true;
    }

    return true;
}

bool is_whitespace(char value) {
    return value == ' ' || value == '\n' || value == '\r' || value == '\t';
}

void skip_whitespace(const char *&cursor, const char *end) {
    while (cursor < end && is_whitespace(*cursor)) {
        cursor++;
    }
}

bool append_char(char *out, size_t out_size, size_t &used, char value) {
    if (used + 1 >= out_size) {
        return false;
    }
    out[used++] = value;
    out[used] = '\0';
    return true;
}

const char *parse_json_string(const char *cursor, const char *end, char *out, size_t out_size) {
    if (out_size == 0 || cursor >= end || *cursor != '"') {
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
                    append_char(out, out_size, used, escaped);
                    break;
                case 'n':
                    append_char(out, out_size, used, '\n');
                    break;
                case 'r':
                    append_char(out, out_size, used, '\r');
                    break;
                case 't':
                    append_char(out, out_size, used, '\t');
                    break;
                case 'u':
                    append_char(out, out_size, used, '?');
                    for (int i = 0; i < 4 && cursor < end; i++) {
                        cursor++;
                    }
                    break;
                default:
                    append_char(out, out_size, used, escaped);
                    break;
            }
            continue;
        }

        append_char(out, out_size, used, ch);
    }

    return nullptr;
}

bool parse_json_field(const char *object_start, const char *object_end, const char *key, char *out, size_t out_size) {
    char key_pattern[16];
    snprintf(key_pattern, sizeof(key_pattern), "\"%s\"", key);

    const char *cursor = object_start;
    while (cursor < object_end) {
        const char *match = strstr(cursor, key_pattern);
        if (match == nullptr || match >= object_end) {
            return false;
        }

        cursor = match + strlen(key_pattern);
        skip_whitespace(cursor, object_end);
        if (cursor >= object_end || *cursor != ':') {
            continue;
        }
        cursor++;
        skip_whitespace(cursor, object_end);
        return parse_json_string(cursor, object_end, out, out_size) != nullptr;
    }

    return false;
}

bool parse_json_int_field(const char *object_start, const char *object_end, const char *key, int &out) {
    char text[24];
    if (parse_json_field(object_start, object_end, key, text, sizeof(text))) {
        char *end = nullptr;
        const long value = strtol(text, &end, 10);
        if (end != text) {
            out = static_cast<int>(value);
            return true;
        }
    }

    char key_pattern[16];
    snprintf(key_pattern, sizeof(key_pattern), "\"%s\"", key);
    const char *cursor = strstr(object_start, key_pattern);
    if (cursor == nullptr || cursor >= object_end) {
        return false;
    }
    cursor += strlen(key_pattern);
    skip_whitespace(cursor, object_end);
    if (cursor >= object_end || *cursor != ':') {
        return false;
    }
    cursor++;
    skip_whitespace(cursor, object_end);
    char *end = nullptr;
    const long value = strtol(cursor, &end, 10);
    if (end == cursor || end > object_end) {
        return false;
    }
    out = static_cast<int>(value);
    return true;
}

void strip_ruby_plain(const char *input, char *out, size_t out_size) {
    if (out_size == 0) {
        return;
    }

    out[0] = '\0';
    size_t used = 0;
    bool reading_rt = false;
    const char *cursor = input;
    while (*cursor != '\0' && used + 1 < out_size) {
        if (*cursor == '<') {
            const char *tag_end = strchr(cursor, '>');
            if (tag_end == nullptr) {
                break;
            }
            if (strncmp(cursor, "<rt", 3) == 0) {
                reading_rt = true;
            } else if (strncmp(cursor, "</rt", 4) == 0) {
                reading_rt = false;
            }
            cursor = tag_end + 1;
            continue;
        }
        if (!reading_rt) {
            out[used++] = *cursor;
            out[used] = '\0';
        }
        cursor++;
    }
}

void extract_ruby_reading(const char *input, char *out, size_t out_size) {
    if (out_size == 0) {
        return;
    }

    out[0] = '\0';
    size_t used = 0;
    bool reading_rt = false;
    const char *cursor = input;
    while (*cursor != '\0' && used + 1 < out_size) {
        if (*cursor == '<') {
            const char *tag_end = strchr(cursor, '>');
            if (tag_end == nullptr) {
                break;
            }
            if (strncmp(cursor, "<rt", 3) == 0) {
                reading_rt = true;
            } else if (strncmp(cursor, "</rt", 4) == 0) {
                reading_rt = false;
            }
            cursor = tag_end + 1;
            continue;
        }
        if (reading_rt) {
            out[used++] = *cursor;
            out[used] = '\0';
        }
        cursor++;
    }
}

bool is_selectable_word(const OcrWord &word) {
    return word.definition[0] != '\0' || word.frequency[0] != '\0' || word.kanji[0] != '\0' || word.base[0] != '\0';
}

void copy_key_normalized(char *out, size_t out_size, const char *text) {
    if (out_size == 0) {
        return;
    }
    size_t used = 0;
    for (const char *cursor = text != nullptr ? text : ""; *cursor != '\0' && used + 1 < out_size; cursor++) {
        char ch = *cursor;
        if (ch >= 'A' && ch <= 'Z') {
            ch = static_cast<char>(ch - 'A' + 'a');
        }
        out[used++] = ch;
    }
    out[used] = '\0';
}

bool saved_key_matches(const char *candidate) {
    if (candidate == nullptr || candidate[0] == '\0') {
        return false;
    }
    char normalized[BaseSize];
    copy_key_normalized(normalized, sizeof(normalized), candidate);
    for (size_t i = 0; i < g_saved_word_key_count; i++) {
        if (strcmp(g_saved_word_keys[i], normalized) == 0) {
            return true;
        }
    }
    return false;
}

bool word_saved_from_cache(const OcrWord &word) {
    const char *base = word.base_plain[0] != '\0' ? word.base_plain : word.base;
    const char *surface = word.surface_plain[0] != '\0' ? word.surface_plain : word.surface;
    return saved_key_matches(base) || saved_key_matches(surface);
}

bool mining_key_saving(const char *key) {
    if (key == nullptr || key[0] == '\0') {
        return false;
    }
    const u32 count = g_mining_saving_count.load(std::memory_order_acquire);
    for (u32 i = 0; i < count && i < MaxMiningQueue; i++) {
        if (strcmp(g_mining_saving_keys[i], key) == 0) {
            return true;
        }
    }
    return false;
}

bool word_saving_from_queue(const OcrWord &word) {
    char key[BaseSize];
    const char *base = word.base_plain[0] != '\0' ? word.base_plain : word.base;
    const char *surface = word.surface_plain[0] != '\0' ? word.surface_plain : word.surface;
    copy_key_normalized(key, sizeof(key), base[0] != '\0' ? base : surface);
    return mining_key_saving(key);
}

void refresh_saved_flags() {
    for (size_t i = 0; i < g_word_count; i++) {
        g_words[i].saved = word_saved_from_cache(g_words[i]);
    }
}

void refresh_saving_flags() {
    for (size_t i = 0; i < g_word_count; i++) {
        g_words[i].saving = word_saving_from_queue(g_words[i]);
    }
}

void record_selected_lookup() {
    if (g_selected_word >= 0 && g_selected_word < static_cast<int>(g_word_count) && g_words[g_selected_word].selectable) {
        g_lookup_count++;
    }
}

void format_selected_word_line(char *line, size_t line_size) {
    if (line_size == 0) {
        return;
    }

    if (g_selected_word < 0 || g_selected_word >= static_cast<int>(g_word_count)) {
        snprintf(line, line_size, "No definition");
        return;
    }

    const OcrWord &word = g_words[g_selected_word];
    char word_label[320];
    const char *surface = word.surface_plain[0] != '\0' ? word.surface_plain : word.surface;
    const char *base = word.base_plain[0] != '\0' ? word.base_plain : surface;
    if (word.reading[0] != '\0' && strcmp(word.reading, surface) != 0) {
        snprintf(word_label, sizeof(word_label), "%s(%s)", surface, word.reading);
    } else {
        snprintf(word_label, sizeof(word_label), "%s", surface);
    }
    if (base[0] != '\0' && strcmp(base, surface) != 0) {
        append_text(word_label, sizeof(word_label), " [");
        append_text(word_label, sizeof(word_label), base);
        append_text(word_label, sizeof(word_label), "]");
    }

    snprintf(line, line_size, "%s: %s", word_label, word.definition[0] != '\0' ? word.definition : "No definition");
    if (word.frequency[0] != '\0') {
        append_text(line, line_size, "  Freq ");
        append_text(line, line_size, word.frequency);
    }
    if (word.kanji[0] != '\0') {
        append_text(line, line_size, "  ");
        append_text(line, line_size, word.kanji);
    }
    append_text(line, line_size, word.saving ? "  Saving" : (word.saved ? "  Saved" : "  New"));
}

void write_hud_state() {
    const OcrWord *word = (g_selected_word >= 0 && g_selected_word < static_cast<int>(g_word_count)) ? &g_words[g_selected_word] : nullptr;
    char frequency[FrequencySize * 2];
    char kanji[KanjiSize * 2];
    char mining_status[320];
    copy_json_escaped(frequency, sizeof(frequency), word != nullptr ? word->frequency : "");
    copy_json_escaped(kanji, sizeof(kanji), word != nullptr ? word->kanji : "");
    copy_json_escaped(mining_status, sizeof(mining_status), g_mining_status);

    char state[880];
    snprintf(
        state,
        sizeof(state),
        "{\"selected\":%d,\"pending\":%s,\"saving\":%s,\"saving_count\":%u,\"f\":\"%s\",\"k\":\"%s\",\"saved\":%s,\"saved_count\":%d,\"lookup_count\":%d,\"m\":\"%s\"}",
        g_selected_word,
        g_ocr_pending.load(std::memory_order_acquire) ? "true" : "false",
        (word != nullptr && word->saving) ? "true" : "false",
        g_mining_saving_count.load(std::memory_order_acquire),
        frequency,
        kanji,
        (word != nullptr && word->saved) ? "true" : "false",
        g_saved_word_total,
        g_lookup_count,
        mining_status
    );
    write_text(HUD_PATH, state);
}

void write_target_line() {
    char line[640];
    format_selected_word_line(line, sizeof(line));
    write_text(TARGET_PATH, line);
    write_hud_state();
}

void set_ocr_error_state(const char *status, const char *result) {
    char status_copy[sizeof(g_status)];
    char result_copy[SentenceSize];
    copy_text(status_copy, sizeof(status_copy), status);
    copy_text(result_copy, sizeof(result_copy), result);
    g_ocr_pending.store(false, std::memory_order_release);
    g_ocr_requested.store(false, std::memory_order_release);
    g_ocr_started_tick.store(0, std::memory_order_release);
    g_word_count = 0;
    g_selected_word = -1;
    copy_text(g_sentence, sizeof(g_sentence), result_copy);
    write_text(RESULT_PATH, g_sentence);
    write_text(TARGET_PATH, "No definition");
    set_status(status_copy);
    write_text(STATUS_PATH, g_status);
    write_hud_state();
}

void select_first_word() {
    g_selected_word = -1;
    int best_frequency_value = -1;
    for (size_t i = 0; i < g_word_count; i++) {
        if (g_words[i].frequency_value >= 0 && g_words[i].frequency_value > best_frequency_value) {
            best_frequency_value = g_words[i].frequency_value;
            g_selected_word = static_cast<int>(i);
        }
    }
    if (g_selected_word >= 0) {
        record_selected_lookup();
        return;
    }

    for (size_t i = 0; i < g_word_count; i++) {
        if (g_words[i].selectable) {
            g_selected_word = static_cast<int>(i);
            record_selected_lookup();
            return;
        }
    }

    if (g_word_count > 0) {
        g_selected_word = 0;
        record_selected_lookup();
    }
}

void move_selection(int delta) {
    if (g_word_count == 0) {
        g_selected_word = -1;
        write_target_line();
        return;
    }

    int cursor = g_selected_word >= 0 ? g_selected_word : 0;
    for (size_t i = 0; i < g_word_count; i++) {
        cursor += delta;
        if (cursor < 0) {
            cursor = static_cast<int>(g_word_count) - 1;
        } else if (cursor >= static_cast<int>(g_word_count)) {
            cursor = 0;
        }

        if (g_words[cursor].selectable) {
            g_selected_word = cursor;
            record_selected_lookup();
            write_target_line();
            return;
        }
    }
}

bool parse_words_json(const char *json) {
    const char *words = strstr(json, "\"words\"");
    if (words == nullptr) {
        return false;
    }

    const char *cursor = strchr(words, '[');
    if (cursor == nullptr) {
        return false;
    }
    cursor++;

    g_word_count = 0;
    g_sentence[0] = '\0';

    while (*cursor != '\0' && *cursor != ']' && g_word_count < MaxWords) {
        const char *object_start = strchr(cursor, '{');
        if (object_start == nullptr) {
            break;
        }

        const char *object_end = strchr(object_start, '}');
        if (object_end == nullptr) {
            break;
        }

        char raw_surface[SurfaceSize] = "";
        char raw_base[BaseSize] = "";
        char raw_definition[DefinitionSize] = "";
        char raw_reading[ReadingSize] = "";
        char raw_frequency[FrequencySize] = "";
        char raw_kanji[KanjiSize] = "";
        parse_json_field(object_start, object_end, "w", raw_surface, sizeof(raw_surface));
        parse_json_field(object_start, object_end, "b", raw_base, sizeof(raw_base));
        parse_json_field(object_start, object_end, "r", raw_reading, sizeof(raw_reading));
        parse_json_field(object_start, object_end, "t", raw_definition, sizeof(raw_definition));
        parse_json_field(object_start, object_end, "f", raw_frequency, sizeof(raw_frequency));
        parse_json_field(object_start, object_end, "k", raw_kanji, sizeof(raw_kanji));

        OcrWord &word = g_words[g_word_count];
        strip_ruby_plain(raw_surface, word.surface, sizeof(word.surface));
        strip_ruby_plain(raw_surface, word.surface_plain, sizeof(word.surface_plain));
        strip_ruby_plain(raw_base, word.base, sizeof(word.base));
        strip_ruby_plain(raw_base, word.base_plain, sizeof(word.base_plain));
        strip_ruby_plain(raw_reading, word.reading, sizeof(word.reading));
        if (word.reading[0] == '\0') {
            extract_ruby_reading(raw_surface, word.reading, sizeof(word.reading));
        }
        strip_ruby_plain(raw_definition, word.definition, sizeof(word.definition));
        copy_text(word.kanji, sizeof(word.kanji), raw_kanji);
        word.frequency_value = -1;
        parse_json_int_field(object_start, object_end, "fv", word.frequency_value);
        if (word.frequency_value >= 0) {
            snprintf(word.frequency, sizeof(word.frequency), "%d", word.frequency_value);
        } else {
            word.frequency[0] = '\0';
            size_t frequency_used = 0;
            for (const char *frequency = raw_frequency; *frequency != '\0' && frequency_used + 1 < sizeof(word.frequency); frequency++) {
                if (*frequency >= '0' && *frequency <= '9') {
                    append_char(word.frequency, sizeof(word.frequency), frequency_used, *frequency);
                } else if (frequency_used > 0) {
                    break;
                }
            }
            if (frequency_used > 0) {
                char *end = nullptr;
                const long parsed_frequency = strtol(word.frequency, &end, 10);
                if (end != word.frequency && parsed_frequency >= 0) {
                    word.frequency_value = static_cast<int>(parsed_frequency);
                }
            }
        }
        word.selectable = is_selectable_word(word);
        word.saved = word_saved_from_cache(word);
        word.saving = word_saving_from_queue(word);

        if (word.surface[0] != '\0' || word.base[0] != '\0' || word.definition[0] != '\0') {
            append_text(g_sentence, sizeof(g_sentence), word.surface_plain[0] != '\0' ? word.surface_plain : word.base_plain);
            g_word_count++;
        }

        cursor = object_end + 1;
    }

    select_first_word();
    return g_word_count > 0;
}

bool copy_json_string(const char *json, const char *key, char *out, size_t out_size) {
    char pattern[64];
    snprintf(pattern, sizeof(pattern), "\"%s\"", key);

    const char *cursor = strstr(json, pattern);
    if (cursor == nullptr) {
        return false;
    }

    const char *end = json + strlen(json);
    cursor += strlen(pattern);
    skip_whitespace(cursor, end);
    if (cursor >= end || *cursor != ':') {
        return false;
    }
    cursor++;
    skip_whitespace(cursor, end);
    return parse_json_string(cursor, end, out, out_size) != nullptr;
}

bool copy_json_error_text(const char *json, char *out, size_t out_size) {
    if (copy_json_string(json, "display_text", out, out_size) || copy_json_string(json, "error", out, out_size)) {
        return true;
    }
    copy_text(out, out_size, "Mac OCR request failed.");
    return false;
}

bool parse_json_bool_field(const char *json, const char *key, bool &out) {
    char pattern[64];
    snprintf(pattern, sizeof(pattern), "\"%s\"", key);

    const char *cursor = strstr(json, pattern);
    if (cursor == nullptr) {
        return false;
    }

    const char *end = json + strlen(json);
    cursor += strlen(pattern);
    skip_whitespace(cursor, end);
    if (cursor >= end || *cursor != ':') {
        return false;
    }
    cursor++;
    skip_whitespace(cursor, end);
    if (cursor + 4 <= end && strncmp(cursor, "true", 4) == 0) {
        out = true;
        return true;
    }
    if (cursor + 5 <= end && strncmp(cursor, "false", 5) == 0) {
        out = false;
        return true;
    }
    return false;
}

bool response_reports_failure(const char *json) {
    bool value = true;
    return (parse_json_bool_field(json, "ok", value) && !value) ||
           (parse_json_bool_field(json, "success", value) && !value);
}

bool ocr_generation_timed_out(u32 generation) {
    return generation != 0 && g_ocr_timeout_generation.load(std::memory_order_acquire) == generation;
}

bool ocr_generation_expired(u32 generation) {
    if (generation == 0 || generation != g_ocr_generation.load(std::memory_order_acquire)) {
        return false;
    }
    const u64 started_tick = g_ocr_started_tick.load(std::memory_order_acquire);
    return started_tick != 0 && armTicksToNs(svcGetSystemTick() - started_tick) >= OcrRequestTimeoutNs;
}

void mark_ocr_timeout(u32 generation) {
    if (!g_ocr_pending.load(std::memory_order_acquire) || !ocr_generation_expired(generation)) {
        return;
    }
    g_ocr_timeout_generation.store(generation, std::memory_order_release);
    set_ocr_error_state("OCR timed out after 20s.", "OCR request timed out after 20 seconds.");
    const int active_socket = g_active_ocr_socket.load(std::memory_order_acquire);
    if (active_socket >= 0) {
        shutdown(active_socket, SHUT_RDWR);
    }
}

void poll_ocr_timeout() {
    if (!g_ocr_pending.load(std::memory_order_acquire)) {
        return;
    }
    const u32 generation = g_ocr_generation.load(std::memory_order_acquire);
    if (!ocr_generation_timed_out(generation)) {
        mark_ocr_timeout(generation);
    }
}

int parse_http_status(const char *response) {
    if (strncmp(response, "HTTP/", 5) != 0) {
        return 0;
    }
    const char *space = strchr(response, ' ');
    return space != nullptr ? static_cast<int>(strtol(space + 1, nullptr, 10)) : 0;
}

bool init_socket_driver() {
    if (g_socket_driver_initialized) {
        return true;
    }
    if (!g_sm_initialized) {
        set_status("BSD unavailable before service init.");
        write_text(STATUS_PATH, g_status);
        return false;
    }

    Result rc = socketInitializeDefault();
    if (R_FAILED(rc)) {
        snprintf(g_status, sizeof(g_status), "Network init failed: 0x%x", rc);
        write_text(STATUS_PATH, g_status);
        return false;
    }

    g_socket_driver_initialized = true;
    return true;
}

bool ensure_nifm_request_ready() {
    if (g_nifm_request_ready) {
        return true;
    }

    if (!g_nifm_initialized) {
        Result rc = nifmInitialize(NifmServiceType_User);
        if (R_FAILED(rc)) {
            rc = nifmInitialize(NifmServiceType_System);
        }
        if (R_FAILED(rc)) {
            snprintf(g_status, sizeof(g_status), "NIFM init failed: 0x%x", rc);
            write_text(STATUS_PATH, g_status);
            return false;
        }
        g_nifm_initialized = true;
    }

    Result rc = nifmCreateRequest(&g_nifm_request, true);
    if (R_FAILED(rc)) {
        snprintf(g_status, sizeof(g_status), "NIFM request failed: 0x%x", rc);
        write_text(STATUS_PATH, g_status);
        return false;
    }

    rc = nifmRequestSubmitAndWait(&g_nifm_request);
    if (R_FAILED(rc)) {
        nifmRequestClose(&g_nifm_request);
        snprintf(g_status, sizeof(g_status), "NIFM wait failed: 0x%x", rc);
        write_text(STATUS_PATH, g_status);
        return false;
    }

    NifmRequestState state = NifmRequestState_Invalid;
    rc = nifmGetRequestState(&g_nifm_request, &state);
    if (R_FAILED(rc) || state != NifmRequestState_Available) {
        nifmRequestClose(&g_nifm_request);
        snprintf(g_status, sizeof(g_status), "NIFM unavailable: rc=0x%x state=%d", rc, state);
        write_text(STATUS_PATH, g_status);
        return false;
    }

    g_nifm_request_ready = true;
    return true;
}

bool wait_for_tcp_connection(int sock) {
    for (u32 attempt = 0; attempt < 50; attempt++) {
        sockaddr_in addr = {};
        addr.sin_family = AF_INET;
        addr.sin_port = htons(SERVER_PORT);
        inet_pton(AF_INET, SERVER_HOST, &addr.sin_addr);

        if (connect(sock, reinterpret_cast<sockaddr *>(&addr), sizeof(addr)) == 0) {
            return true;
        }

        if (errno == EISCONN) {
            return true;
        }

        if (errno != EINPROGRESS && errno != EALREADY && errno != EWOULDBLOCK) {
            return false;
        }

        svcSleepThread(200000000);
    }

    errno = ETIMEDOUT;
    return false;
}

bool send_all(int sock, const void *data, size_t size) {
    const u8 *cursor = static_cast<const u8 *>(data);
    size_t sent_total = 0;
    while (sent_total < size) {
        const ssize_t sent = send(sock, cursor + sent_total, size - sent_total, 0);
        if (sent <= 0) {
            return false;
        }
        sent_total += static_cast<size_t>(sent);
    }
    return true;
}

bool has_jpeg_extension(const char *path) {
    const char *dot = strrchr(path, '.');
    if (dot == nullptr) {
        return false;
    }

    char ext[6] = {};
    size_t index = 0;
    for (const char *cursor = dot; *cursor != '\0' && index + 1 < sizeof(ext); cursor++, index++) {
        ext[index] = static_cast<char>(*cursor >= 'A' && *cursor <= 'Z' ? *cursor - 'A' + 'a' : *cursor);
    }
    return strcmp(ext, ".jpg") == 0 || strcmp(ext, ".jpeg") == 0;
}

bool album_file_is_newer(const AlbumProbeFile &candidate, const AlbumProbeFile &current) {
    if (current.path[0] == '\0') {
        return true;
    }
    if (candidate.mtime != current.mtime) {
        return candidate.mtime > current.mtime;
    }
    if (candidate.size != current.size) {
        return candidate.size > current.size;
    }
    return strcmp(candidate.path, current.path) > 0;
}

bool same_album_file(const AlbumProbeFile &left, const AlbumProbeFile &right) {
    return left.mtime == right.mtime && left.size == right.size && strcmp(left.path, right.path) == 0;
}

void scan_album_dir(const char *path, int depth, AlbumProbeFile &latest) {
    if (depth > AlbumProbeMaxDepth) {
        return;
    }

    DIR *dir = opendir(path);
    if (dir == nullptr) {
        return;
    }

    while (dirent *entry = readdir(dir)) {
        if (strcmp(entry->d_name, ".") == 0 || strcmp(entry->d_name, "..") == 0) {
            continue;
        }

        char child_path[sizeof(latest.path)];
        const int written = snprintf(child_path, sizeof(child_path), "%s/%s", path, entry->d_name);
        if (written <= 0 || static_cast<size_t>(written) >= sizeof(child_path)) {
            continue;
        }

        struct stat st = {};
        if (stat(child_path, &st) != 0) {
            continue;
        }

        if (S_ISDIR(st.st_mode)) {
            scan_album_dir(child_path, depth + 1, latest);
            continue;
        }

        if (!S_ISREG(st.st_mode) || !has_jpeg_extension(child_path)) {
            continue;
        }

        AlbumProbeFile candidate = {};
        copy_text(candidate.path, sizeof(candidate.path), child_path);
        candidate.mtime = st.st_mtime;
        candidate.size = st.st_size;
        if (album_file_is_newer(candidate, latest)) {
            latest = candidate;
        }
    }

    closedir(dir);
}

bool find_latest_album_jpeg(AlbumProbeFile &out) {
    out = {};
    scan_album_dir("sdmc:/Nintendo/Album", 0, out);
    return out.path[0] != '\0' && out.size > 0;
}

int open_screenshot_probe_socket(size_t body_size, const char *source) {
    if (!init_socket_driver()) {
        write_debug("Screenshot probe: socket init failed");
        return -1;
    }
    ensure_nifm_request_ready();

    int sock = socket(AF_INET, SOCK_STREAM, 0);
    if (sock < 0) {
        char debug[128];
        snprintf(debug, sizeof(debug), "Screenshot probe: socket failed errno=%d", errno);
        write_debug(debug);
        return -1;
    }

    timeval timeout = {};
    timeout.tv_sec = 5;
    setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout));
    setsockopt(sock, SOL_SOCKET, SO_SNDTIMEO, &timeout, sizeof(timeout));

    sockaddr_in addr = {};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(SERVER_PORT);
    if (inet_pton(AF_INET, SERVER_HOST, &addr.sin_addr) != 1 || !wait_for_tcp_connection(sock)) {
        char debug[128];
        snprintf(debug, sizeof(debug), "Screenshot probe: connect failed errno=%d", errno);
        close(sock);
        write_debug(debug);
        return -1;
    }

    char header[512];
    const int header_size = snprintf(
        header,
        sizeof(header),
        "POST /screenshot-probe HTTP/1.1\r\n"
        "Host: %s:%d\r\n"
        "Content-Type: image/jpeg\r\n"
        "Accept: application/json\r\n"
        "X-SwitchOCR-Probe-Source: %s\r\n"
        "Content-Length: %zu\r\n"
        "Connection: close\r\n"
        "\r\n",
        SERVER_HOST,
        SERVER_PORT,
        source,
        body_size
    );
    if (header_size < 0 || static_cast<size_t>(header_size) >= sizeof(header) || !send_all(sock, header, static_cast<size_t>(header_size))) {
        char debug[128];
        snprintf(debug, sizeof(debug), "Screenshot probe: header send failed errno=%d", errno);
        close(sock);
        write_debug(debug);
        return -1;
    }

    return sock;
}

int finish_screenshot_probe_upload(int sock) {
    char response[512] = {};
    size_t used = 0;
    while (used + 1 < sizeof(response)) {
        const ssize_t received = recv(sock, response + used, sizeof(response) - used - 1, 0);
        if (received <= 0) {
            break;
        }
        used += static_cast<size_t>(received);
        response[used] = '\0';
    }
    close(sock);
    return parse_http_status(response);
}

bool ensure_hos_version_ready() {
    if (g_hos_version_initialized) {
        return true;
    }
    if (hosversionGet() != 0) {
        g_hos_version_initialized = true;
        return true;
    }

    Result rc = setsysInitialize();
    if (R_FAILED(rc)) {
        char debug[128];
        snprintf(debug, sizeof(debug), "Capssc probe: setsys init failed: 0x%x", rc);
        write_debug(debug);
        return false;
    }

    SetSysFirmwareVersion firmware = {};
    rc = setsysGetFirmwareVersion(&firmware);
    setsysExit();
    if (R_FAILED(rc)) {
        char debug[128];
        snprintf(debug, sizeof(debug), "Capssc probe: firmware version failed: 0x%x", rc);
        write_debug(debug);
        return false;
    }

    hosversionSet(MAKEHOSVERSION(firmware.major, firmware.minor, firmware.micro));
    g_hos_version_initialized = true;
    return true;
}

bool capture_capssc_jpeg(void **out_jpeg, size_t &out_jpeg_size) {
    *out_jpeg = nullptr;
    out_jpeg_size = 0;
    if (!ensure_hos_version_ready()) {
        return false;
    }

    Result rc = capsscInitialize();
    if (R_FAILED(rc)) {
        char debug[128];
        snprintf(debug, sizeof(debug), "Capssc: init failed: 0x%x", rc);
        write_debug(debug);
        return false;
    }

    void *jpeg = malloc(CapsscJpegBufferSize);
    if (jpeg == nullptr) {
        capsscExit();
        write_debug("Capssc: JPEG buffer allocation failed");
        return false;
    }

    u64 jpeg_size = 0;
    rc = capsscCaptureJpegScreenShot(&jpeg_size, jpeg, CapsscJpegBufferSize, ViLayerStack_Screenshot, 1000000000LL);
    capsscExit();
    if (R_FAILED(rc) || jpeg_size == 0) {
        char debug[128];
        snprintf(debug, sizeof(debug), "Capssc: capture failed: 0x%x size=%llu", rc, static_cast<unsigned long long>(jpeg_size));
        free(jpeg);
        write_debug(debug);
        return false;
    }

    *out_jpeg = jpeg;
    out_jpeg_size = static_cast<size_t>(jpeg_size);
    return true;
}

bool upload_capssc_probe_jpeg(const void *jpeg, size_t jpeg_size) {
    if (jpeg == nullptr || jpeg_size == 0) {
        return false;
    }

    int sock = open_screenshot_probe_socket(jpeg_size, "capssc");
    if (sock < 0) {
        return false;
    }

    if (!send_all(sock, jpeg, jpeg_size)) {
        char debug[160];
        snprintf(debug, sizeof(debug), "Capssc probe: upload failed errno=%d", errno);
        close(sock);
        write_debug(debug);
        return false;
    }

    const int http_status = finish_screenshot_probe_upload(sock);
    char debug[192];
    snprintf(debug, sizeof(debug), "Capssc probe: uploaded %llu bytes, HTTP %d", static_cast<unsigned long long>(jpeg_size), http_status);
    write_debug(debug);
    return http_status >= 200 && http_status < 300;
}

bool capture_capssc_probe() {
    void *jpeg = nullptr;
    size_t jpeg_size = 0;
    if (!capture_capssc_jpeg(&jpeg, jpeg_size)) {
        return false;
    }

    const bool uploaded = upload_capssc_probe_jpeg(jpeg, jpeg_size);
    free(jpeg);
    return uploaded;
}

bool upload_album_probe_file(const AlbumProbeFile &file) {
    FILE *fp = fopen(file.path, "rb");
    if (fp == nullptr) {
        char debug[160];
        snprintf(debug, sizeof(debug), "Album probe: open failed errno=%d", errno);
        write_debug(debug);
        return false;
    }

    int sock = open_screenshot_probe_socket(static_cast<size_t>(file.size), "album");
    if (sock < 0) {
        fclose(fp);
        return false;
    }

    u8 buffer[AlbumProbeChunkSize];
    bool ok = true;
    while (!feof(fp)) {
        const size_t read = fread(buffer, 1, sizeof(buffer), fp);
        if (read > 0 && !send_all(sock, buffer, read)) {
            ok = false;
            break;
        }
        if (ferror(fp)) {
            ok = false;
            break;
        }
    }
    fclose(fp);

    if (!ok) {
        char debug[160];
        snprintf(debug, sizeof(debug), "Album probe: upload failed errno=%d", errno);
        close(sock);
        write_debug(debug);
        return false;
    }

    const int http_status = finish_screenshot_probe_upload(sock);
    char debug[192];
    snprintf(debug, sizeof(debug), "Album probe: uploaded %llu bytes, HTTP %d", static_cast<unsigned long long>(file.size), http_status);
    write_debug(debug);
    return http_status >= 200 && http_status < 300;
}

void poll_album_probe() {
    AlbumProbeFile latest = {};
    if (!find_latest_album_jpeg(latest)) {
        if (!g_album_probe_baselined) {
            write_debug("Album probe: no album JPEG baseline");
            g_album_probe_baselined = true;
        }
        return;
    }

    if (!g_album_probe_baselined) {
        g_last_album_probe_file = latest;
        g_album_probe_baselined = true;
        write_debug("Album probe: baseline ready");
        return;
    }

    if (same_album_file(latest, g_last_album_probe_file)) {
        return;
    }

    if (upload_album_probe_file(latest)) {
        g_last_album_probe_file = latest;
    }
}

bool poll_queued_screenshot_probe() {
    const u32 desired_generation = g_screenshot_probe_generation.load(std::memory_order_acquire);
    if (desired_generation == g_screenshot_probe_handled_generation.load(std::memory_order_acquire)) {
        return false;
    }
    g_screenshot_probe_handled_generation.store(desired_generation, std::memory_order_release);
    capture_capssc_probe();
    poll_album_probe();
    return true;
}

void screenshot_probe_worker_entry(void *) {
    while (g_screenshot_probe_thread_running.load(std::memory_order_acquire)) {
        if (!g_album_probe_baselined) {
            poll_album_probe();
        }
        poll_queued_screenshot_probe();

        svcSleepThread(ScreenshotProbeWorkerSleepNs);
    }
}

bool start_screenshot_probe_worker() {
    if (g_screenshot_probe_thread_started) {
        return true;
    }

    g_screenshot_probe_thread_running.store(true, std::memory_order_release);
    Result rc = threadCreate(&g_screenshot_probe_thread, screenshot_probe_worker_entry, nullptr, nullptr, 0x10000, 0x2e, -2);
    if (R_FAILED(rc)) {
        g_screenshot_probe_thread_running.store(false, std::memory_order_release);
        char debug[128];
        snprintf(debug, sizeof(debug), "Screenshot probe worker failed: 0x%x", rc);
        write_debug(debug);
        return false;
    }

    rc = threadStart(&g_screenshot_probe_thread);
    if (R_FAILED(rc)) {
        threadClose(&g_screenshot_probe_thread);
        g_screenshot_probe_thread_running.store(false, std::memory_order_release);
        char debug[128];
        snprintf(debug, sizeof(debug), "Screenshot probe start failed: 0x%x", rc);
        write_debug(debug);
        return false;
    }

    g_screenshot_probe_thread_started = true;
    return true;
}

void stop_screenshot_probe_worker() {
    if (!g_screenshot_probe_thread_started) {
        return;
    }

    g_screenshot_probe_thread_running.store(false, std::memory_order_release);
    threadWaitForExit(&g_screenshot_probe_thread);
    threadClose(&g_screenshot_probe_thread);
    g_screenshot_probe_thread_started = false;
}

void queue_screenshot_probe() {
    if (!g_screenshot_probe_thread_started) {
        write_debug("Screenshot probe: worker unavailable");
        return;
    }
    const u32 generation = g_screenshot_probe_generation.fetch_add(1, std::memory_order_acq_rel) + 1;
    char debug[128];
    snprintf(debug, sizeof(debug), "Screenshot probe: queued %u", generation);
    write_debug(debug);
}

void apply_ocr_response(char *body_text) {
    g_ocr_pending.store(false, std::memory_order_release);
    g_ocr_requested.store(false, std::memory_order_release);
    g_ocr_started_tick.store(0, std::memory_order_release);
    write_text(RESULT_JSON_PATH, body_text);

    if (response_reports_failure(body_text)) {
        char display_text[SentenceSize];
        copy_json_error_text(body_text, display_text, sizeof(display_text));
        set_ocr_error_state("OCR request failed.", display_text);
        return;
    }

    if (parse_words_json(body_text)) {
        write_text(RESULT_PATH, g_sentence);
        write_target_line();
        set_status("OCR complete. Use D-pad Left/Right.");
        write_text(STATUS_PATH, g_status);
        return;
    }

    char display_text[SentenceSize];
    if (copy_json_string(body_text, "display_text", display_text, sizeof(display_text)) ||
        copy_json_string(body_text, "error", display_text, sizeof(display_text))) {
        copy_text(g_sentence, sizeof(g_sentence), display_text);
        g_word_count = 0;
        g_selected_word = -1;
        write_text(RESULT_PATH, g_sentence);
        write_text(TARGET_PATH, "No definition");
        write_hud_state();
        set_status("OCR complete. No selectable words.");
        write_text(STATUS_PATH, g_status);
        return;
    }

    set_ocr_error_state("Invalid OCR response from Mac.", "Invalid OCR response from Mac.");
}

size_t expected_http_response_size(const char *response, size_t used) {
    const char *headers_end = strstr(response, "\r\n\r\n");
    if (headers_end == nullptr) {
        return 0;
    }

    const char *content_length = strstr(response, "Content-Length:");
    if (content_length == nullptr || content_length > headers_end) {
        return 0;
    }

    content_length += strlen("Content-Length:");
    while (*content_length == ' ') {
        content_length++;
    }

    const long body_size = strtol(content_length, nullptr, 10);
    if (body_size < 0) {
        return 0;
    }

    const size_t header_size = static_cast<size_t>((headers_end + 4) - response);
    const size_t total_size = header_size + static_cast<size_t>(body_size);
    return total_size <= sizeof(g_response) ? total_size : used;
}

bool request_capssc_ocr(u32 request_generation) {
    set_status("Capturing Switch screenshot for OCR.");
    write_text(STATUS_PATH, g_status);

    void *jpeg = nullptr;
    size_t jpeg_size = 0;
    if (!capture_capssc_jpeg(&jpeg, jpeg_size)) {
        set_status("Switch screenshot capture failed; see debug.txt.");
        write_text(STATUS_PATH, g_status);
        return false;
    }

    if (request_generation != g_ocr_generation.load(std::memory_order_acquire)) {
        free(jpeg);
        return true;
    }
    if (ocr_generation_timed_out(request_generation) || ocr_generation_expired(request_generation)) {
        free(jpeg);
        mark_ocr_timeout(request_generation);
        return false;
    }

    if (!init_socket_driver()) {
        free(jpeg);
        return false;
    }
    ensure_nifm_request_ready();

    int sock = socket(AF_INET, SOCK_STREAM, 0);
    if (sock < 0) {
        free(jpeg);
        snprintf(g_status, sizeof(g_status), "TCP socket failed: errno=%d result=0x%x", errno, socketGetLastResult());
        write_text(STATUS_PATH, g_status);
        return false;
    }
    g_active_ocr_socket.store(sock, std::memory_order_release);

    timeval timeout = {};
    timeout.tv_sec = 8;
    setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout));
    setsockopt(sock, SOL_SOCKET, SO_SNDTIMEO, &timeout, sizeof(timeout));

    sockaddr_in addr = {};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(SERVER_PORT);
    if (inet_pton(AF_INET, SERVER_HOST, &addr.sin_addr) != 1 || !wait_for_tcp_connection(sock)) {
        close(sock);
        g_active_ocr_socket.store(-1, std::memory_order_release);
        free(jpeg);
        snprintf(g_status, sizeof(g_status), "OCR upload connect failed: errno=%d result=0x%x", errno, socketGetLastResult());
        write_text(STATUS_PATH, g_status);
        return false;
    }

    char header[512];
    const int header_size = snprintf(
        header,
        sizeof(header),
        "POST /ocr-upload HTTP/1.1\r\n"
        "Host: %s:%d\r\n"
        "Content-Type: image/jpeg\r\n"
        "Accept: application/json\r\n"
        "X-SwitchOCR-Source: capssc\r\n"
        "Content-Length: %zu\r\n"
        "Connection: close\r\n"
        "\r\n",
        SERVER_HOST,
        SERVER_PORT,
        jpeg_size
    );
    if (header_size < 0 || static_cast<size_t>(header_size) >= sizeof(header)) {
        close(sock);
        g_active_ocr_socket.store(-1, std::memory_order_release);
        free(jpeg);
        set_status("OCR upload header build failed.");
        write_text(STATUS_PATH, g_status);
        return false;
    }
    if (!send_all(sock, header, static_cast<size_t>(header_size))) {
        close(sock);
        g_active_ocr_socket.store(-1, std::memory_order_release);
        free(jpeg);
        snprintf(g_status, sizeof(g_status), "OCR upload header send failed: errno=%d result=0x%x", errno, socketGetLastResult());
        write_text(STATUS_PATH, g_status);
        return false;
    }
    if (!send_all(sock, jpeg, jpeg_size)) {
        const int send_errno = errno;
        const Result send_result = socketGetLastResult();
        free(jpeg);

        timeval quick_timeout = {};
        quick_timeout.tv_sec = 1;
        setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, &quick_timeout, sizeof(quick_timeout));

        size_t error_response_size = 0;
        g_response[0] = '\0';
        while (error_response_size + 1 < sizeof(g_response)) {
            const ssize_t received = recv(sock, g_response + error_response_size, sizeof(g_response) - error_response_size - 1, 0);
            if (received <= 0) {
                break;
            }
            error_response_size += static_cast<size_t>(received);
            g_response[error_response_size] = '\0';
            const size_t expected_size = expected_http_response_size(g_response, error_response_size);
            if (expected_size > 0 && error_response_size >= expected_size) {
                break;
            }
        }
        close(sock);
        g_active_ocr_socket.store(-1, std::memory_order_release);

        const int http_status = parse_http_status(g_response);
        char *body_text = strstr(g_response, "\r\n\r\n");
        if (body_text != nullptr) {
            write_text(RESULT_JSON_PATH, body_text + 4);
        }
        if (http_status > 0) {
            snprintf(g_status, sizeof(g_status), "OCR upload body send failed: HTTP %d errno=%d", http_status, send_errno);
        } else {
            snprintf(g_status, sizeof(g_status), "OCR upload body send failed: errno=%d result=0x%x", send_errno, send_result);
        }
        write_text(STATUS_PATH, g_status);
        return false;
    }
    free(jpeg);

    set_status("Switch screenshot uploaded. Waiting for OCR...");
    write_text(STATUS_PATH, g_status);

    size_t used = 0;
    int receive_errno = 0;
    while (used + 1 < sizeof(g_response)) {
        ssize_t received = recv(sock, g_response + used, sizeof(g_response) - used - 1, 0);
        if (received < 0) {
            receive_errno = errno;
            if (used > 0) {
                break;
            }
            close(sock);
            g_active_ocr_socket.store(-1, std::memory_order_release);
            snprintf(g_status, sizeof(g_status), "OCR upload receive failed: errno=%d result=0x%x", errno, socketGetLastResult());
            write_text(STATUS_PATH, g_status);
            return false;
        }
        if (received == 0) {
            break;
        }
        used += static_cast<size_t>(received);
        g_response[used] = '\0';

        const size_t expected_size = expected_http_response_size(g_response, used);
        if (expected_size > 0 && used >= expected_size) {
            break;
        }
    }
    g_response[used] = '\0';
    close(sock);
    g_active_ocr_socket.store(-1, std::memory_order_release);

    char *body_text = strstr(g_response, "\r\n\r\n");
    if (body_text == nullptr) {
        snprintf(g_status, sizeof(g_status), "Bad OCR upload response: %zu bytes errno=%d", used, receive_errno);
        write_text(STATUS_PATH, g_status);
        return false;
    }
    body_text += 4;

    if (request_generation != g_ocr_generation.load(std::memory_order_acquire)) {
        return true;
    }
    if (ocr_generation_timed_out(request_generation) || ocr_generation_expired(request_generation)) {
        mark_ocr_timeout(request_generation);
        return false;
    }

    const int http_status = parse_http_status(g_response);
    if (http_status < 200 || http_status >= 300) {
        char display_text[SentenceSize];
        char status[128];
        copy_json_error_text(body_text, display_text, sizeof(display_text));
        snprintf(status, sizeof(status), "OCR upload failed: HTTP %d.", http_status);
        write_text(RESULT_JSON_PATH, body_text);
        set_ocr_error_state(status, display_text);
        return true;
    }

    apply_ocr_response(body_text);
    return true;
}

bool request_latest_ocr(u32 request_generation) {
    if (!init_socket_driver()) {
        return false;
    }

    const bool nifm_ready = ensure_nifm_request_ready();
    NifmRequestState nifm_state = NifmRequestState_Invalid;
    Result nifm_state_rc = 0;
    Result nifm_request_rc = 0;
    if (nifm_ready) {
        nifm_state_rc = nifmGetRequestState(&g_nifm_request, &nifm_state);
        nifm_request_rc = nifmGetResult(&g_nifm_request);
    }

    int sock = socket(AF_INET, SOCK_STREAM, 0);
    if (sock < 0) {
        snprintf(
            g_status,
            sizeof(g_status),
            "TCP socket failed: errno=%d result=0x%x",
            errno,
            socketGetLastResult()
        );
        write_text(STATUS_PATH, g_status);
        return false;
    }
    g_active_ocr_socket.store(sock, std::memory_order_release);

    bool socket_registered = false;
    int register_errno = 0;
    Result register_result = 0;
    if (nifm_ready) {
        if (socketNifmRequestRegisterSocketDescriptor(&g_nifm_request, sock) == 0) {
            socket_registered = true;
        } else {
            register_errno = errno;
            register_result = socketGetLastResult();
        }
    }

    sockaddr_in addr = {};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(SERVER_PORT);
    if (inet_pton(AF_INET, SERVER_HOST, &addr.sin_addr) != 1) {
        if (socket_registered) {
            socketNifmRequestUnregisterSocketDescriptor(&g_nifm_request, sock);
        }
        close(sock);
        g_active_ocr_socket.store(-1, std::memory_order_release);
        set_status("Invalid OCR server IP.");
        write_text(STATUS_PATH, g_status);
        return false;
    }

    snprintf(
        g_status,
        sizeof(g_status),
        "TCP connect: nifm=%d state=%d state_rc=0x%x req=0x%x reg_errno=%d reg=0x%x",
        nifm_ready ? 1 : 0,
        nifm_state,
        nifm_state_rc,
        nifm_request_rc,
        register_errno,
        register_result
    );
    write_text(STATUS_PATH, g_status);
    if (!wait_for_tcp_connection(sock)) {
        if (socket_registered) {
            socketNifmRequestUnregisterSocketDescriptor(&g_nifm_request, sock);
        }
        close(sock);
        g_active_ocr_socket.store(-1, std::memory_order_release);
        snprintf(
            g_status,
            sizeof(g_status),
            "TCP connect failed: errno=%d result=0x%x nifm_state=%d reg_errno=%d reg=0x%x",
            errno,
            socketGetLastResult(),
            nifm_state,
            register_errno,
            register_result
        );
        write_text(STATUS_PATH, g_status);
        return false;
    }

    const char *body = "{}";
    char header[512];
    const int header_size = snprintf(
        header,
        sizeof(header),
        "POST /ocr-latest HTTP/1.1\r\n"
        "Host: %s:%d\r\n"
        "Content-Type: application/json\r\n"
        "Content-Length: 2\r\n"
        "Connection: close\r\n"
        "\r\n",
        SERVER_HOST,
        SERVER_PORT
    );

    if (header_size < 0 || static_cast<size_t>(header_size) >= sizeof(header) ||
        send(sock, header, static_cast<size_t>(header_size), 0) < 0 ||
        send(sock, body, strlen(body), 0) < 0) {
        if (socket_registered) {
            socketNifmRequestUnregisterSocketDescriptor(&g_nifm_request, sock);
        }
        close(sock);
        g_active_ocr_socket.store(-1, std::memory_order_release);
        snprintf(g_status, sizeof(g_status), "TCP send failed: errno=%d result=0x%x", errno, socketGetLastResult());
        write_text(STATUS_PATH, g_status);
        return false;
    }

    set_status("TCP OCR request sent. Waiting for Mac...");
    write_text(STATUS_PATH, g_status);

    size_t used = 0;
    int receive_errno = 0;
    while (used + 1 < sizeof(g_response)) {
        ssize_t received = recv(sock, g_response + used, sizeof(g_response) - used - 1, 0);
        if (received < 0) {
            receive_errno = errno;
            if (used > 0) {
                break;
            }
            if (socket_registered) {
                socketNifmRequestUnregisterSocketDescriptor(&g_nifm_request, sock);
            }
            close(sock);
            g_active_ocr_socket.store(-1, std::memory_order_release);
            snprintf(g_status, sizeof(g_status), "TCP receive failed: errno=%d result=0x%x", errno, socketGetLastResult());
            write_text(STATUS_PATH, g_status);
            return false;
        }
        if (received == 0) {
            break;
        }
        used += static_cast<size_t>(received);
        g_response[used] = '\0';

        const size_t expected_size = expected_http_response_size(g_response, used);
        if (expected_size > 0 && used >= expected_size) {
            break;
        }
    }
    g_response[used] = '\0';

    if (socket_registered) {
        socketNifmRequestUnregisterSocketDescriptor(&g_nifm_request, sock);
    }
    close(sock);
    g_active_ocr_socket.store(-1, std::memory_order_release);

    char *body_text = strstr(g_response, "\r\n\r\n");
    if (body_text == nullptr) {
        snprintf(g_status, sizeof(g_status), "Bad HTTP response: %zu bytes errno=%d", used, receive_errno);
        write_text(STATUS_PATH, g_status);
        return false;
    }
    body_text += 4;

    if (request_generation != g_ocr_generation.load(std::memory_order_acquire)) {
        return true;
    }
    if (ocr_generation_timed_out(request_generation) || ocr_generation_expired(request_generation)) {
        mark_ocr_timeout(request_generation);
        return false;
    }

    const int http_status = parse_http_status(g_response);
    if (http_status < 200 || http_status >= 300) {
        char display_text[SentenceSize];
        char status[128];
        copy_json_error_text(body_text, display_text, sizeof(display_text));
        snprintf(status, sizeof(status), "OCR request failed: HTTP %d.", http_status);
        write_text(RESULT_JSON_PATH, body_text);
        set_ocr_error_state(status, display_text);
        return true;
    }

    apply_ocr_response(body_text);
    return true;
}

void ocr_worker_entry(void *) {
    u32 handled_generation = 0;
    while (g_ocr_thread_running.load(std::memory_order_acquire)) {
        const u32 desired_generation = g_ocr_generation.load(std::memory_order_acquire);
        if (desired_generation == handled_generation) {
            svcSleepThread(OcrWorkerSleepNs);
            continue;
        }

        g_ocr_worker_busy.store(true, std::memory_order_release);
        handled_generation = desired_generation;
        bool request_ok = request_capssc_ocr(handled_generation);
        if (!request_ok && UseSysDvrOcrFallback) {
            request_ok = request_latest_ocr(handled_generation);
        }
        if (!request_ok) {
            if (handled_generation == g_ocr_generation.load(std::memory_order_acquire)) {
                if (ocr_generation_timed_out(handled_generation)) {
                    set_ocr_error_state("OCR timed out after 20s.", "OCR request timed out after 20 seconds.");
                } else {
                    set_ocr_error_state(g_status, g_status);
                }
            }
        }
        if (handled_generation == g_ocr_generation.load(std::memory_order_acquire)) {
            g_ocr_requested.store(false, std::memory_order_release);
        } else {
            set_status("Starting Switch screenshot OCR request.");
            write_text(STATUS_PATH, g_status);
        }
        g_ocr_worker_busy.store(false, std::memory_order_release);
    }
}

bool start_ocr_worker() {
    if (g_ocr_thread_started) {
        return true;
    }

    g_ocr_thread_running.store(true, std::memory_order_release);
    Result rc = threadCreate(&g_ocr_thread, ocr_worker_entry, nullptr, nullptr, OcrWorkerStackSize, 0x2c, -2);
    if (R_FAILED(rc)) {
        g_ocr_thread_running.store(false, std::memory_order_release);
        snprintf(g_status, sizeof(g_status), "OCR worker thread failed: 0x%x", rc);
        write_text(STATUS_PATH, g_status);
        return false;
    }

    rc = threadStart(&g_ocr_thread);
    if (R_FAILED(rc)) {
        threadClose(&g_ocr_thread);
        g_ocr_thread_running.store(false, std::memory_order_release);
        snprintf(g_status, sizeof(g_status), "OCR worker start failed: 0x%x", rc);
        write_text(STATUS_PATH, g_status);
        return false;
    }

    g_ocr_thread_started = true;
    return true;
}

void stop_ocr_worker() {
    if (!g_ocr_thread_started) {
        return;
    }

    g_ocr_thread_running.store(false, std::memory_order_release);
    threadWaitForExit(&g_ocr_thread);
    threadClose(&g_ocr_thread);
    g_ocr_thread_started = false;
}

bool queue_ocr_request(const char *source) {
    if (!g_ocr_thread_started) {
        set_status("OCR worker unavailable.");
        write_text(STATUS_PATH, g_status);
        return false;
    }

    const bool worker_busy = g_ocr_worker_busy.load(std::memory_order_acquire);
    const u32 request_generation = g_ocr_generation.fetch_add(1, std::memory_order_acq_rel) + 1;
    g_ocr_requested.store(true, std::memory_order_release);
    g_ocr_pending.store(true, std::memory_order_release);
    g_ocr_timeout_generation.store(0, std::memory_order_release);
    g_ocr_started_tick.store(svcGetSystemTick(), std::memory_order_release);
    g_word_count = 0;
    g_selected_word = -1;
    g_sentence[0] = '\0';
    write_text(RESULT_PATH, "");
    write_text(RESULT_JSON_PATH, "");
    write_text(TARGET_PATH, "Loading...");
    write_hud_state();
    g_request_count = request_generation;
    if (worker_busy) {
        const int active_socket = g_active_ocr_socket.load(std::memory_order_acquire);
        if (active_socket >= 0) {
            shutdown(active_socket, SHUT_RDWR);
        }
        snprintf(g_status, sizeof(g_status), "OCR request %u replacing current from %s.", g_request_count, source);
    } else {
        snprintf(g_status, sizeof(g_status), "OCR request %u started from %s.", g_request_count, source);
    }
    write_text(STATUS_PATH, g_status);
    // The OCR worker now uploads its own caps:sc JPEG. Keep the probe path disabled for diagnostics.
    if (UploadScreenshotProbeWithOcr) {
        queue_screenshot_probe();
    }
    return true;
}

void cache_saved_key(const char *candidate) {
    char normalized[BaseSize];
    copy_key_normalized(normalized, sizeof(normalized), candidate);
    if (normalized[0] == '\0' || saved_key_matches(normalized) || g_saved_word_key_count >= MaxSavedWords) {
        return;
    }
    copy_text(g_saved_word_keys[g_saved_word_key_count++], BaseSize, normalized);
}

bool parse_saved_words_json(const char *json) {
    int saved_count = -1;
    if (parse_json_int_field(json, json + strlen(json), "saved_count", saved_count)) {
        g_saved_word_total = saved_count;
    }

    const char *saved_words = strstr(json, "\"saved_words\"");
    if (saved_words == nullptr) {
        refresh_saved_flags();
        return false;
    }

    const char *end = json + strlen(json);
    const char *cursor = strchr(saved_words, '[');
    if (cursor == nullptr) {
        refresh_saved_flags();
        return false;
    }
    cursor++;

    g_saved_word_key_count = 0;
    while (cursor < end && *cursor != ']' && g_saved_word_key_count < MaxSavedWords) {
        skip_whitespace(cursor, end);
        if (cursor >= end || *cursor == ']') {
            break;
        }
        if (*cursor != '"') {
            cursor++;
            continue;
        }

        char raw_key[BaseSize] = "";
        const char *next = parse_json_string(cursor, end, raw_key, sizeof(raw_key));
        if (next == nullptr) {
            break;
        }
        cache_saved_key(raw_key);
        cursor = next;
    }

    if (g_saved_word_total < static_cast<int>(g_saved_word_key_count)) {
        g_saved_word_total = static_cast<int>(g_saved_word_key_count);
    }
    refresh_saved_flags();
    return true;
}

void copy_mining_key_for_word(char *out, size_t out_size, const OcrWord &word) {
    const char *base = word.base_plain[0] != '\0' ? word.base_plain : word.base;
    const char *surface = word.surface_plain[0] != '\0' ? word.surface_plain : word.surface;
    copy_key_normalized(out, out_size, base[0] != '\0' ? base : surface);
}

bool start_mining_save(const char *key) {
    const u32 count = g_mining_saving_count.load(std::memory_order_acquire);
    if (key == nullptr || key[0] == '\0' || mining_key_saving(key) || count >= MaxMiningQueue) {
        return false;
    }
    copy_text(g_mining_saving_keys[count], BaseSize, key);
    g_mining_saving_count.store(count + 1, std::memory_order_release);
    refresh_saving_flags();
    return true;
}

void finish_mining_save(const char *key) {
    const u32 count = g_mining_saving_count.load(std::memory_order_acquire);
    for (u32 i = 0; i < count && i < MaxMiningQueue; i++) {
        if (strcmp(g_mining_saving_keys[i], key) != 0) {
            continue;
        }
        if (i + 1 < count) {
            copy_text(g_mining_saving_keys[i], BaseSize, g_mining_saving_keys[count - 1]);
        }
        g_mining_saving_keys[count - 1][0] = '\0';
        g_mining_saving_count.store(count - 1, std::memory_order_release);
        refresh_saving_flags();
        return;
    }
}

void mark_word_saved_by_key(const char *key) {
    cache_saved_key(key);
    refresh_saved_flags();
    if (g_saved_word_total < static_cast<int>(g_saved_word_key_count)) {
        g_saved_word_total = static_cast<int>(g_saved_word_key_count);
    }
}

bool build_mine_word_body(const OcrWord &word, char *out, size_t out_size) {
    char surface[SurfaceSize * 2];
    char reading[ReadingSize * 2];
    char base[BaseSize * 2];
    char definition[DefinitionSize * 2];
    char sentence[SentenceSize * 2];
    copy_json_escaped(surface, sizeof(surface), word.surface_plain[0] != '\0' ? word.surface_plain : word.surface);
    copy_json_escaped(reading, sizeof(reading), word.reading);
    copy_json_escaped(base, sizeof(base), word.base_plain[0] != '\0' ? word.base_plain : word.base);
    copy_json_escaped(definition, sizeof(definition), word.definition);
    copy_json_escaped(sentence, sizeof(sentence), g_sentence);

    const int written = snprintf(
        out,
        out_size,
        "{\"provider\":\"issen\",\"surface\":\"%s\",\"reading\":\"%s\",\"base\":\"%s\",\"definition\":\"%s\",\"sentence\":\"%s\",\"language\":\"japanese\"}",
        surface,
        reading,
        base,
        definition,
        sentence
    );
    return written > 0 && static_cast<size_t>(written) < out_size;
}

bool request_server_json(const char *method, const char *path, const char *body, char *body_out, size_t body_out_size) {
    if (!init_socket_driver()) {
        return false;
    }
    ensure_nifm_request_ready();

    int sock = socket(AF_INET, SOCK_STREAM, 0);
    if (sock < 0) {
        return false;
    }

    sockaddr_in addr = {};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(SERVER_PORT);
    if (inet_pton(AF_INET, SERVER_HOST, &addr.sin_addr) != 1 || !wait_for_tcp_connection(sock)) {
        close(sock);
        return false;
    }

    const char *request_body = body != nullptr ? body : "";
    const size_t request_body_size = strlen(request_body);
    char header[512];
    const int header_size = snprintf(
        header,
        sizeof(header),
        "%s %s HTTP/1.1\r\n"
        "Host: %s:%d\r\n"
        "Content-Type: application/json\r\n"
        "Accept: application/json\r\n"
        "Content-Length: %zu\r\n"
        "Connection: close\r\n"
        "\r\n",
        method,
        path,
        SERVER_HOST,
        SERVER_PORT,
        request_body_size
    );

    if (header_size < 0 || static_cast<size_t>(header_size) >= sizeof(header) ||
        send(sock, header, static_cast<size_t>(header_size), 0) < 0 ||
        (request_body_size > 0 && send(sock, request_body, request_body_size, 0) < 0)) {
        close(sock);
        return false;
    }

    size_t used = 0;
    while (used + 1 < sizeof(g_mining_response)) {
        ssize_t received = recv(sock, g_mining_response + used, sizeof(g_mining_response) - used - 1, 0);
        if (received < 0) {
            close(sock);
            return false;
        }
        if (received == 0) {
            break;
        }
        used += static_cast<size_t>(received);
        g_mining_response[used] = '\0';
        const size_t expected_size = expected_http_response_size(g_mining_response, used);
        if (expected_size > 0 && used >= expected_size) {
            break;
        }
    }
    close(sock);
    g_mining_response[used] = '\0';

    char *body_text = strstr(g_mining_response, "\r\n\r\n");
    if (body_text == nullptr) {
        return false;
    }
    body_text += 4;

    const int http_status = parse_http_status(g_mining_response);
    if (body_out_size > 0) {
        snprintf(body_out, body_out_size, "%s", body_text);
    }
    return http_status >= 200 && http_status < 300;
}

void apply_mining_response(const MiningQueueEntry &request, const char *body_text, bool http_ok) {
    parse_saved_words_json(body_text);

    bool response_ok = http_ok;
    parse_json_bool_field(body_text, "ok", response_ok);
    if (!response_ok) {
        char error[128];
        if (!copy_json_string(body_text, "error", error, sizeof(error))) {
            copy_text(error, sizeof(error), "Mining server unavailable");
        }
        snprintf(g_mining_status, sizeof(g_mining_status), "%s", error);
        write_hud_state();
        return;
    }

    if (request.action == MiningActionMine) {
        bool already_saved = false;
        parse_json_bool_field(body_text, "already_saved", already_saved);
        if (already_saved) {
            copy_text(g_mining_status, sizeof(g_mining_status), "Already saved.");
        } else {
            copy_text(g_mining_status, sizeof(g_mining_status), "Word saved.");
        }
        mark_word_saved_by_key(request.key);
        write_target_line();
        return;
    }

    copy_text(g_mining_status, sizeof(g_mining_status), "Mining sync ready.");
    write_hud_state();
}

bool queue_mining_status_request(const char *source) {
    (void)source;
    if (!g_mining_thread_started) {
        return false;
    }
    MiningQueueEntry request = {};
    request.action = MiningActionStatus;
    return enqueue_mining_request(request);
}

bool queue_mine_selected_word() {
    if (g_selected_word < 0 || g_selected_word >= static_cast<int>(g_word_count)) {
        copy_text(g_mining_status, sizeof(g_mining_status), "No word selected.");
        write_hud_state();
        return false;
    }
    if (g_words[g_selected_word].saved) {
        copy_text(g_mining_status, sizeof(g_mining_status), "Already saved.");
        write_target_line();
        return false;
    }
    if (!g_mining_thread_started) {
        copy_text(g_mining_status, sizeof(g_mining_status), "Mining worker unavailable.");
        write_hud_state();
        return false;
    }

    MiningQueueEntry request = {};
    request.action = MiningActionMine;
    const OcrWord &word = g_words[g_selected_word];
    copy_mining_key_for_word(request.key, sizeof(request.key), word);
    if (request.key[0] == '\0') {
        copy_text(g_mining_status, sizeof(g_mining_status), "Could not mine word.");
        write_hud_state();
        return false;
    }
    if (mining_key_saving(request.key)) {
        copy_text(g_mining_status, sizeof(g_mining_status), "Already saving.");
        write_target_line();
        return false;
    }
    if (!build_mine_word_body(word, request.body, sizeof(request.body))) {
        copy_text(g_mining_status, sizeof(g_mining_status), "Could not mine word.");
        write_hud_state();
        return false;
    }

    if (!start_mining_save(request.key)) {
        copy_text(g_mining_status, sizeof(g_mining_status), "Save queue full.");
        write_hud_state();
        return false;
    }
    if (!enqueue_mining_request(request)) {
        finish_mining_save(request.key);
        copy_text(g_mining_status, sizeof(g_mining_status), "Save queue full.");
        write_hud_state();
        return false;
    }

    copy_text(g_mining_status, sizeof(g_mining_status), "Save queued.");
    write_hud_state();
    return true;
}

void mining_worker_entry(void *) {
    while (g_mining_thread_running.load(std::memory_order_acquire)) {
        MiningQueueEntry request = {};
        if (!dequeue_mining_request(request)) {
            svcSleepThread(MiningWorkerSleepNs);
            continue;
        }

        if (request.action == MiningActionMine) {
            copy_text(g_mining_status, sizeof(g_mining_status), "Saving word...");
            write_hud_state();
        }

        g_mining_body[0] = '\0';
        const bool http_ok = request.action == MiningActionStatus
            ? request_server_json("GET", "/mining/status", "", g_mining_body, sizeof(g_mining_body))
            : request_server_json("POST", "/mine-word", request.body, g_mining_body, sizeof(g_mining_body));
        if (request.action == MiningActionMine) {
            finish_mining_save(request.key);
        }
        if (g_mining_body[0] == '\0') {
            copy_text(g_mining_status, sizeof(g_mining_status), "Mining server unavailable.");
            write_hud_state();
        } else {
            apply_mining_response(request, g_mining_body, http_ok);
        }
    }
}

bool start_mining_worker() {
    if (g_mining_thread_started) {
        return true;
    }

    g_mining_thread_running.store(true, std::memory_order_release);
    Result rc = threadCreate(&g_mining_thread, mining_worker_entry, nullptr, nullptr, 0x10000, 0x2d, -2);
    if (R_FAILED(rc)) {
        g_mining_thread_running.store(false, std::memory_order_release);
        copy_text(g_mining_status, sizeof(g_mining_status), "Mining worker unavailable.");
        write_hud_state();
        return false;
    }

    rc = threadStart(&g_mining_thread);
    if (R_FAILED(rc)) {
        threadClose(&g_mining_thread);
        g_mining_thread_running.store(false, std::memory_order_release);
        copy_text(g_mining_status, sizeof(g_mining_status), "Mining worker failed.");
        write_hud_state();
        return false;
    }

    g_mining_thread_started = true;
    return true;
}

void stop_mining_worker() {
    if (!g_mining_thread_started) {
        return;
    }
    g_mining_thread_running.store(false, std::memory_order_release);
    threadWaitForExit(&g_mining_thread);
    threadClose(&g_mining_thread);
    g_mining_thread_started = false;
}

ssize_t decode_utf8_codepoint(u32 *out, const u8 *input) {
    if (input[0] < 0x80) {
        *out = input[0];
        return 1;
    }
    if ((input[0] & 0xE0) == 0xC0) {
        *out = ((input[0] & 0x1F) << 6) | (input[1] & 0x3F);
        return 2;
    }
    if ((input[0] & 0xF0) == 0xE0) {
        *out = ((input[0] & 0x0F) << 12) | ((input[1] & 0x3F) << 6) | (input[2] & 0x3F);
        return 3;
    }
    if ((input[0] & 0xF8) == 0xF0) {
        *out = ((input[0] & 0x07) << 18) | ((input[1] & 0x3F) << 12) |
               ((input[2] & 0x3F) << 6) | (input[3] & 0x3F);
        return 4;
    }
    return -1;
}

Result add_to_layer_stack(ViLayer *layer, ViLayerStack stack) {
    const struct {
        u32 stack;
        u64 layer_id;
    } in = {stack, layer->layer_id};

    return serviceDispatchIn(viGetSession_IManagerDisplayService(), 6000, in);
}

Result set_layer_topmost(ViDisplay *display, ViLayer *layer) {
    s32 max_z = 0;
    Result rc = viGetZOrderCountMax(display, &max_z);
    if (R_FAILED(rc) || max_z <= 0) {
        return rc;
    }

    rc = viSetLayerZ(layer, max_z);
    if (R_FAILED(rc) && max_z > 1) {
        rc = viSetLayerZ(layer, max_z - 1);
    }
    return rc;
}

Result add_to_visible_layer_stacks(ViLayer *layer) {
    constexpr ViLayerStack stacks[] = {
        ViLayerStack_Default,
        ViLayerStack_Screenshot,
        ViLayerStack_Recording,
        ViLayerStack_Arbitrary,
        ViLayerStack_LastFrame,
        ViLayerStack_Null,
        ViLayerStack_ApplicationForDebug,
        ViLayerStack_Lcd,
        ApplicationLayerStack,
    };

    Result first_failure = 0;
    bool added = false;
    for (ViLayerStack stack : stacks) {
        Result rc = add_to_layer_stack(layer, stack);
        if (R_SUCCEEDED(rc)) {
            added = true;
        } else if (R_SUCCEEDED(first_failure)) {
            first_failure = rc;
        }
    }
    return added ? 0 : first_failure;
}

class HudRenderer {
public:
    Result init() {
        if (m_initialized) {
            return 0;
        }

        Result rc = viInitialize(ViServiceType_Manager);
        if (R_FAILED(rc)) {
            return rc;
        }
        rc = viOpenDefaultDisplay(&m_display);
        if (R_FAILED(rc)) {
            return rc;
        }
        rc = viGetDisplayVsyncEvent(&m_display, &m_vsync_event);
        if (R_FAILED(rc)) {
            return rc;
        }

        __nx_vi_layer_id = 0;
        rc = viCreateManagedLayer(&m_display, static_cast<ViLayerFlags>(0), 0, &__nx_vi_layer_id);
        if (R_FAILED(rc)) {
            return rc;
        }
        rc = viCreateLayer(&m_display, &m_layer);
        if (R_FAILED(rc)) {
            return rc;
        }

        viSetLayerScalingMode(&m_layer, ViScalingMode_FitToLayer);
        Result stack_rc = add_to_visible_layer_stacks(&m_layer);
        if (R_FAILED(stack_rc)) {
            return stack_rc;
        }
        rc = set_layer_topmost(&m_display, &m_layer);
        if (R_FAILED(rc)) {
            return rc;
        }

        rc = viSetLayerSize(&m_layer, LayerWidth, LayerHeight);
        if (R_FAILED(rc)) {
            return rc;
        }
        rc = viSetLayerPosition(&m_layer, 0, 0);
        if (R_FAILED(rc)) {
            return rc;
        }
        rc = nwindowCreateFromLayer(&m_window, &m_layer);
        if (R_FAILED(rc)) {
            return rc;
        }
        rc = framebufferCreate(&m_framebuffer, &m_window, FramebufferWidth, FramebufferHeight, PIXEL_FORMAT_RGBA_4444, 2);
        if (R_FAILED(rc)) {
            return rc;
        }

        if (ensure_font_ready()) {
            rc = init_font();
            if (R_SUCCEEDED(rc)) {
                m_font_ready = true;
            } else {
                char debug[128];
                snprintf(debug, sizeof(debug), "HUD font init failed: 0x%x", rc);
                write_debug(debug);
            }
        } else {
            write_debug("HUD active without font.");
        }

        m_initialized = true;
        return 0;
    }

    void draw() {
        if (!m_initialized) {
            return;
        }

        m_current_framebuffer = framebufferBegin(&m_framebuffer, nullptr);
        clear_screen();

        constexpr s32 panel_x = 24;
        constexpr s32 panel_y = 532;
        constexpr s32 panel_w = 1232;
        constexpr s32 panel_h = 164;
        draw_rect(panel_x, panel_y, panel_w, panel_h, Color(0, 0, 0, 11));
        draw_rect(panel_x, panel_y, panel_w, 4, Color(0, 15, 13, 15));

        if (!m_font_ready) {
            draw_rect(panel_x + 22, panel_y + 38, 520, 20, Color(15, 15, 15, 15));
            draw_rect(panel_x + 22, panel_y + 84, 720, 22, Color(0, 15, 13, 15));
            eventWait(&m_vsync_event, UINT64_MAX);
            framebufferEnd(&m_framebuffer);
            m_current_framebuffer = nullptr;
            return;
        }

        draw_words_line(panel_x + 22, panel_y + 42, 20.0F, panel_w - 44);
        draw_selected_word(panel_x + 22, panel_y + 92, panel_w - 44);
        draw_string(g_status, panel_x + 23, panel_y + 139, 16.0F, Color(0, 0, 0, 15), panel_w - 44);
        draw_string(g_status, panel_x + 22, panel_y + 138, 16.0F, Color(12, 12, 12, 15), panel_w - 44);

        eventWait(&m_vsync_event, UINT64_MAX);
        framebufferEnd(&m_framebuffer);
        m_current_framebuffer = nullptr;
    }

private:
    Result init_font() {
        PlFontData font_data;
        Result rc = plGetSharedFontByType(&font_data, PlSharedFontType_Standard);
        if (R_FAILED(rc)) {
            return rc;
        }

        u8 *font_buffer = static_cast<u8 *>(font_data.address);
        return stbtt_InitFont(&m_font, font_buffer, stbtt_GetFontOffsetForIndex(font_buffer, 0)) ? 0 : MAKERESULT(Module_Libnx, LibnxError_BadInput);
    }

    u32 pixel_offset(s32 x, s32 y) const {
        u32 pos = ((y & 127) / 16) + (x / 32 * 8) + ((y / 16 / 8) * (((FramebufferWidth / 2) / 16 * 8)));
        pos *= 16 * 16 * 4;
        pos += ((y % 16) / 8) * 512 + ((x % 32) / 16) * 256 +
               ((y % 8) / 2) * 64 + ((x % 16) / 8) * 32 + (y % 2) * 16 + (x % 8) * 2;
        return pos / 2;
    }

    u8 blend_channel(u8 src, u8 dst, u8 alpha) const {
        const u8 one_minus_alpha = 0x0F - alpha;
        return static_cast<u8>((dst * alpha + src * one_minus_alpha) / 0x0F);
    }

    void set_pixel(s32 x, s32 y, Color color) {
        if (m_current_framebuffer == nullptr || x < 0 || y < 0 || x >= static_cast<s32>(FramebufferWidth) ||
            y >= static_cast<s32>(FramebufferHeight)) {
            return;
        }
        static_cast<Color *>(m_current_framebuffer)[pixel_offset(x, y)] = color;
    }

    void set_pixel_blend(s32 x, s32 y, Color color) {
        if (m_current_framebuffer == nullptr || x < 0 || y < 0 || x >= static_cast<s32>(FramebufferWidth) ||
            y >= static_cast<s32>(FramebufferHeight)) {
            return;
        }

        Color src = static_cast<Color *>(m_current_framebuffer)[pixel_offset(x, y)];
        Color out(0);
        out.r = blend_channel(src.r, color.r, color.a);
        out.g = blend_channel(src.g, color.g, color.a);
        out.b = blend_channel(src.b, color.b, color.a);
        out.a = std::min<u8>(src.a + color.a, 0x0F);
        set_pixel(x, y, out);
    }

    void clear_screen() {
        std::fill_n(static_cast<Color *>(m_current_framebuffer), m_framebuffer.fb_size / sizeof(Color), Color(0x0000));
    }

    void draw_rect(s32 x, s32 y, s32 width, s32 height, Color color) {
        for (s32 yy = y; yy < y + height; yy++) {
            for (s32 xx = x; xx < x + width; xx++) {
                set_pixel_blend(xx, yy, color);
            }
        }
    }

    s32 text_width(const char *text, float font_size, s32 max_width) {
        if (text == nullptr || text[0] == '\0') {
            return 0;
        }

        s32 width_px = 0;
        const float scale = stbtt_ScaleForPixelHeight(&m_font, font_size);
        while (*text != '\0') {
            u32 codepoint = 0;
            const ssize_t width = decode_utf8_codepoint(&codepoint, reinterpret_cast<const u8 *>(text));
            if (width <= 0 || codepoint == '\n') {
                break;
            }
            text += width;

            int advance = 0;
            int lsb = 0;
            stbtt_GetCodepointHMetrics(&m_font, codepoint, &advance, &lsb);
            const s32 advance_px = static_cast<s32>(advance * scale);
            if (max_width > 0 && width_px + advance_px > max_width) {
                break;
            }
            width_px += advance_px;
        }
        return width_px;
    }

    s32 draw_string(const char *text, s32 x, s32 y, float font_size, Color color, s32 max_width) {
        if (text == nullptr || text[0] == '\0') {
            return x;
        }

        s32 cursor_x = x;
        const float scale = stbtt_ScaleForPixelHeight(&m_font, font_size);
        while (*text != '\0') {
            u32 codepoint = 0;
            const ssize_t width = decode_utf8_codepoint(&codepoint, reinterpret_cast<const u8 *>(text));
            if (width <= 0) {
                break;
            }
            text += width;

            if (codepoint == '\n') {
                break;
            }

            int advance = 0;
            int lsb = 0;
            stbtt_GetCodepointHMetrics(&m_font, codepoint, &advance, &lsb);
            const s32 advance_px = static_cast<s32>(advance * scale);
            if (max_width > 0 && cursor_x + advance_px > x + max_width) {
                break;
            }

            int bmp_width = 0;
            int bmp_height = 0;
            int xoff = 0;
            int yoff = 0;
            u8 *bitmap = stbtt_GetCodepointBitmap(&m_font, scale, scale, codepoint, &bmp_width, &bmp_height, &xoff, &yoff);
            if (bitmap != nullptr && !std::iswspace(static_cast<wint_t>(codepoint))) {
                for (s32 by = 0; by < bmp_height; by++) {
                    for (s32 bx = 0; bx < bmp_width; bx++) {
                        Color pixel = color;
                        pixel.a = static_cast<u8>((bitmap[bmp_width * by + bx] >> 4) * (color.a / 15.0F));
                        if (pixel.a != 0) {
                            set_pixel_blend(cursor_x + xoff + bx, y + yoff + by, pixel);
                        }
                    }
                }
            }
            stbtt_FreeBitmap(bitmap, nullptr);
            cursor_x += advance_px;
        }
        return cursor_x;
    }

    void draw_selected_word(s32 x, s32 y, s32 max_width) {
        if (g_ocr_pending) {
            const char *dots[] = {".", "..", "..."};
            char message[160];
            snprintf(message, sizeof(message), "OCR pending%s", dots[(g_loading_frame++ / 8) % 3]);
            draw_string(message, x + 1, y + 1, 20.0F, Color(0, 0, 0, 15), max_width);
            draw_string(message, x, y, 20.0F, Color(0, 15, 13, 15), max_width);
            return;
        }

        if (g_selected_word < 0 || g_selected_word >= static_cast<int>(g_word_count)) {
            draw_string("No definition", x + 1, y + 1, 20.0F, Color(0, 0, 0, 15), max_width);
            draw_string("No definition", x, y, 20.0F, Color(13, 13, 13, 15), max_width);
            return;
        }

        char line[640];
        format_selected_word_line(line, sizeof(line));
        draw_string(line, x + 1, y + 1, 20.0F, Color(0, 0, 0, 15), max_width);
        draw_string(line, x, y, 20.0F, Color(15, 15, 15, 15), max_width);
    }

    void draw_words_line(s32 x, s32 y, float font_size, s32 max_width) {
        if (g_word_count == 0) {
            draw_string(g_sentence, x + 1, y + 1, font_size, Color(0, 0, 0, 15), max_width);
            draw_string(g_sentence, x, y, font_size, Color(15, 15, 15, 15), max_width);
            return;
        }

        s32 cursor_x = x;
        for (size_t i = 0; i < g_word_count; i++) {
            const OcrWord &word = g_words[i];
            const char *surface = word.surface_plain[0] != '\0' ? word.surface_plain : word.base_plain;
            const s32 remaining_width = max_width - (cursor_x - x);
            if (remaining_width <= 0) {
                break;
            }

            const s32 width = text_width(surface, font_size, remaining_width);
            if (static_cast<int>(i) == g_selected_word && width > 0) {
                draw_rect(cursor_x - 2, y - 22, width + 4, 28, Color(0, 15, 13, 9));
            }
            draw_string(surface, cursor_x + 1, y + 1, font_size, Color(0, 0, 0, 15), remaining_width);
            cursor_x = draw_string(surface, cursor_x, y, font_size, Color(15, 15, 15, 15), remaining_width);
        }
    }

    bool m_initialized = false;
    ViDisplay m_display = {};
    ViLayer m_layer = {};
    Event m_vsync_event = {};
    NWindow m_window = {};
    Framebuffer m_framebuffer = {};
    void *m_current_framebuffer = nullptr;
    stbtt_fontinfo m_font = {};
    bool m_font_ready = false;
};

HudRenderer g_renderer;

void poll_request_file() {
    if (read_text(REQUEST_PATH, g_request, sizeof(g_request)) && strcmp(g_request, g_last_request) != 0) {
        copy_text(g_last_request, sizeof(g_last_request), g_request);
        queue_ocr_request("overlay");
    }
}

bool capture_button_pressed() {
    static bool initialized = false;
    static bool previous_down = false;

    if (!g_capture_button_initialized) {
        return false;
    }

    HidCaptureButtonState state = {};
    if (hidGetCaptureButtonStates(&state, 1) == 0) {
        return false;
    }

    const bool down = state.buttons != 0;
    const bool pressed = initialized && down && !previous_down;
    previous_down = down;
    initialized = true;
    return pressed;
}

void handle_input(u64 keys_down, u64 keys_held) {
    static u64 previous_held = 0;
    static u32 ocr_cooldown = 0;
    static u32 mine_cooldown = 0;
    const u64 interesting_buttons =
        HidNpadButton_Minus | HidNpadButton_StickR | HidNpadButton_Left | HidNpadButton_Right | HidNpadButton_AnyLeft | HidNpadButton_AnyRight;
    const u64 current_held = keys_held & interesting_buttons;
    const bool minus_pressed = ((keys_down | (current_held & ~previous_held)) & HidNpadButton_Minus) != 0;
    const bool capture_pressed = capture_button_pressed();
    const bool mine_pressed = ((keys_down | (current_held & ~previous_held)) & HidNpadButton_StickR) != 0;
    const bool ocr_pending = g_ocr_pending.load(std::memory_order_acquire);

    if (ocr_cooldown > 0) {
        ocr_cooldown--;
    }
    if (mine_cooldown > 0) {
        mine_cooldown--;
    }

    if ((minus_pressed || capture_pressed) && ocr_cooldown == 0) {
        ocr_cooldown = OcrCooldownFrames;
        queue_ocr_request(capture_pressed && !minus_pressed ? "Capture" : "Minus");
    } else if (!ocr_pending && mine_pressed && mine_cooldown == 0) {
        mine_cooldown = MineCooldownFrames;
        queue_mine_selected_word();
    } else if (!ocr_pending && (keys_down & (HidNpadButton_Left | HidNpadButton_AnyLeft))) {
        move_selection(-1);
    } else if (!ocr_pending && (keys_down & (HidNpadButton_Right | HidNpadButton_AnyRight))) {
        move_selection(1);
    }

    previous_held = current_held;
}

} // namespace

extern "C" void __libnx_initheap(void) {
    fake_heap_start = g_heap;
    fake_heap_end = g_heap + sizeof(g_heap);
}

extern "C" void __appInit(void) {
    ensure_services_ready(false);
}

extern "C" void __appExit(void) {
    stop_screenshot_probe_worker();
    stop_mining_worker();
    stop_ocr_worker();
    if (g_socket_driver_initialized) {
        socketExit();
    }
    if (g_nifm_request_ready) {
        nifmRequestClose(&g_nifm_request);
    }
    if (g_nifm_initialized) {
        nifmExit();
    }
    if (g_hidsys_initialized) {
        hidsysExit();
    }
    if (g_pl_initialized) {
        plExit();
    }
    if (g_hid_initialized) {
        hidExit();
    }
    if (g_sd_mounted) {
        fsdevUnmountDevice("sdmc");
    }
    if (g_fs_initialized) {
        fsExit();
    }
    if (g_sm_initialized) {
        smExit();
    }
}

int main(int argc, char **argv) {
    (void)argc;
    (void)argv;

    while (!ensure_services_ready(false)) {
        svcSleepThread(1000000000);
    }

    mkdir("sdmc:/config", 0777);
    mkdir(CONFIG_DIR, 0777);
    set_status("Switch OCR sysmodule polling requests.");
    write_text(STATUS_PATH, g_status);
    write_text(TARGET_PATH, "No definition");
    write_hud_state();
    start_ocr_worker();
    start_screenshot_probe_worker();
    if (start_mining_worker()) {
        queue_mining_status_request("startup");
    }

    PadState pad;
    bool pad_ready = false;
    bool renderer_ready = false;

    while (true) {
        if (!ensure_services_ready(false)) {
            svcSleepThread(1000000000);
            continue;
        }

        if (!pad_ready && ensure_hid_ready()) {
            padConfigureInput(8, HidNpadStyleSet_NpadStandard | HidNpadStyleTag_NpadSystemExt);
            padInitializeAny(&pad);
            padUpdate(&pad);
            pad_ready = true;
            write_debug("Input polling active.");
        }

        poll_request_file();
        poll_ocr_timeout();

        if (pad_ready) {
            padUpdate(&pad);
            handle_input(padGetButtonsDown(&pad), padGetButtons(&pad));
        }

        if (EnableSysmoduleViHud && !renderer_ready) {
            Result rc = g_renderer.init();
            if (R_SUCCEEDED(rc)) {
                renderer_ready = true;
                set_status("Switch OCR HUD renderer active.");
                write_text(STATUS_PATH, g_status);
                write_debug("HUD renderer active.");
            } else {
                snprintf(g_status, sizeof(g_status), "HUD renderer init failed: 0x%x", rc);
                write_text(STATUS_PATH, g_status);
                write_debug(g_status);
            }
        }
        if (EnableSysmoduleViHud && renderer_ready) {
            g_renderer.draw();
        }

        svcSleepThread(InputPollSleepNs);
    }

    return 0;
}
