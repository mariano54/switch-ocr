#include <switch.h>

#define STB_TRUETYPE_IMPLEMENTATION
#include <stb_truetype.h>

#include <algorithm>
#include <arpa/inet.h>
#include <cwctype>
#include <errno.h>
#include <netinet/in.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <unistd.h>

#ifndef SERVER_HOST
#define SERVER_HOST "192.168.0.124"
#endif

#ifndef SERVER_PORT
#define SERVER_PORT 8000
#endif

#define CONFIG_DIR "sdmc:/config/switch-ocr"
#define REQUEST_PATH CONFIG_DIR "/request.txt"
#define RESULT_PATH CONFIG_DIR "/result.txt"
#define RESULT_JSON_PATH CONFIG_DIR "/result.json"
#define STATUS_PATH CONFIG_DIR "/status.txt"
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
constexpr size_t DefinitionSize = 384;
constexpr size_t FrequencySize = 32;
constexpr size_t KanjiSize = 128;
constexpr size_t SentenceSize = 2048;
constexpr size_t ResponseSize = 16384;
constexpr u32 FramebufferWidth = 1280;
constexpr u32 FramebufferHeight = 720;
constexpr u32 LayerWidth = 1920;
constexpr u32 LayerHeight = 1080;

struct OcrWord {
    char surface[SurfaceSize];
    char surface_plain[SurfaceSize];
    char base[BaseSize];
    char base_plain[BaseSize];
    char definition[DefinitionSize];
    char frequency[FrequencySize];
    char kanji[KanjiSize];
    int frequency_value;
    bool selectable;
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
char g_sentence[SentenceSize] = "No OCR result yet.";
OcrWord g_words[MaxWords] = {};
size_t g_word_count = 0;
int g_selected_word = -1;
bool g_sm_initialized = false;
bool g_fs_initialized = false;
bool g_sd_mounted = false;
bool g_hid_initialized = false;
bool g_hidsys_initialized = false;
bool g_pl_initialized = false;
bool g_socket_driver_initialized = false;
bool g_nifm_initialized = false;
bool g_nifm_request_ready = false;
NifmRequest g_nifm_request = {};
u32 g_request_count = 0;
bool g_ocr_pending = false;
u32 g_loading_frame = 0;

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

void append_limited(char *out, size_t out_size, size_t &used, const char *text, size_t text_size) {
    for (size_t i = 0; i < text_size && text[i] != '\0' && used + 1 < out_size; i++) {
        out[used++] = text[i];
    }
    out[used] = '\0';
}

bool same_text(const char *left, const char *right) {
    return strcmp(left, right) == 0;
}

void append_ruby(char *out, size_t out_size, size_t &used, const char *base, const char *reading) {
    append_text(out, out_size, base);
    used = strlen(out);
    if (reading[0] != '\0' && !same_text(reading, base)) {
        append_char(out, out_size, used, '(');
        append_limited(out, out_size, used, reading, strlen(reading));
        append_char(out, out_size, used, ')');
    }
}

void render_ruby_plain(const char *input, char *out, size_t out_size) {
    if (out_size == 0) {
        return;
    }

    out[0] = '\0';
    size_t used = 0;
    bool in_ruby = false;
    bool reading_rt = false;
    char ruby_base[64] = "";
    char rt_text[64] = "";
    size_t ruby_used = 0;
    size_t rt_used = 0;
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
            } else if (strncmp(cursor, "<ruby", 5) == 0) {
                in_ruby = true;
                ruby_base[0] = '\0';
                rt_text[0] = '\0';
                ruby_used = 0;
                rt_used = 0;
            } else if (strncmp(cursor, "</ruby", 6) == 0) {
                append_ruby(out, out_size, used, ruby_base, rt_text);
                in_ruby = false;
                reading_rt = false;
                ruby_base[0] = '\0';
                rt_text[0] = '\0';
                ruby_used = 0;
                rt_used = 0;
            }

            cursor = tag_end + 1;
            continue;
        }

        if (reading_rt) {
            append_char(rt_text, sizeof(rt_text), rt_used, *cursor);
        } else if (in_ruby) {
            append_char(ruby_base, sizeof(ruby_base), ruby_used, *cursor);
        } else {
            out[used++] = *cursor;
            out[used] = '\0';
        }
        cursor++;
    }

    if (in_ruby) {
        append_ruby(out, out_size, used, ruby_base, rt_text);
    }
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

bool is_selectable_word(const OcrWord &word) {
    return word.definition[0] != '\0' || word.frequency[0] != '\0' || word.kanji[0] != '\0' || word.base[0] != '\0';
}

void select_first_word() {
    g_selected_word = -1;
    int best_frequency_value = -1;
    for (size_t i = 0; i < g_word_count; i++) {
        if (g_words[i].frequency_value > best_frequency_value) {
            best_frequency_value = g_words[i].frequency_value;
            g_selected_word = static_cast<int>(i);
        }
    }
    if (g_selected_word >= 0) {
        return;
    }

    for (size_t i = 0; i < g_word_count; i++) {
        if (g_words[i].selectable) {
            g_selected_word = static_cast<int>(i);
            return;
        }
    }

    if (g_word_count > 0) {
        g_selected_word = 0;
    }
}

void move_selection(int delta) {
    if (g_word_count == 0) {
        g_selected_word = -1;
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
        char raw_frequency[FrequencySize] = "";
        char raw_kanji[KanjiSize] = "";
        parse_json_field(object_start, object_end, "w", raw_surface, sizeof(raw_surface));
        parse_json_field(object_start, object_end, "b", raw_base, sizeof(raw_base));
        parse_json_field(object_start, object_end, "t", raw_definition, sizeof(raw_definition));
        parse_json_field(object_start, object_end, "f", raw_frequency, sizeof(raw_frequency));
        parse_json_field(object_start, object_end, "k", raw_kanji, sizeof(raw_kanji));

        OcrWord &word = g_words[g_word_count];
        render_ruby_plain(raw_surface, word.surface, sizeof(word.surface));
        strip_ruby_plain(raw_surface, word.surface_plain, sizeof(word.surface_plain));
        render_ruby_plain(raw_base, word.base, sizeof(word.base));
        strip_ruby_plain(raw_base, word.base_plain, sizeof(word.base_plain));
        render_ruby_plain(raw_definition, word.definition, sizeof(word.definition));
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
        }
        word.selectable = is_selectable_word(word);

        if (word.surface[0] != '\0' || word.base[0] != '\0' || word.definition[0] != '\0') {
            append_text(g_sentence, sizeof(g_sentence), word.surface[0] != '\0' ? word.surface : word.base);
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

void apply_ocr_response(char *body_text) {
    g_ocr_pending = false;
    write_text(RESULT_JSON_PATH, body_text);

    if (parse_words_json(body_text)) {
        write_text(RESULT_PATH, g_sentence);
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
    }

    set_status("OCR complete. No selectable words.");
    write_text(STATUS_PATH, g_status);
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

bool request_latest_ocr() {
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

    char *body_text = strstr(g_response, "\r\n\r\n");
    if (body_text == nullptr) {
        snprintf(g_status, sizeof(g_status), "Bad HTTP response: %zu bytes errno=%d", used, receive_errno);
        write_text(STATUS_PATH, g_status);
        return false;
    }
    body_text += 4;

    apply_ocr_response(body_text);
    return true;
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
        s32 max_z = 0;
        if (R_SUCCEEDED(viGetZOrderCountMax(&m_display, &max_z)) && max_z > 0) {
            viSetLayerZ(&m_layer, max_z);
        }

        add_to_layer_stack(&m_layer, ViLayerStack_Default);
        add_to_layer_stack(&m_layer, ViLayerStack_Screenshot);
        add_to_layer_stack(&m_layer, ViLayerStack_Recording);
        add_to_layer_stack(&m_layer, ViLayerStack_Arbitrary);
        add_to_layer_stack(&m_layer, ViLayerStack_LastFrame);
        add_to_layer_stack(&m_layer, ViLayerStack_Null);
        add_to_layer_stack(&m_layer, ViLayerStack_ApplicationForDebug);
        add_to_layer_stack(&m_layer, ViLayerStack_Lcd);

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

        const OcrWord &word = g_words[g_selected_word];
        char line[640];
        char word_label[320];
        const char *surface = word.surface_plain[0] != '\0' ? word.surface_plain : word.surface;
        const char *base = word.base_plain[0] != '\0' ? word.base_plain : surface;
        if (base[0] != '\0' && strcmp(base, surface) != 0) {
            snprintf(word_label, sizeof(word_label), "%s [%s]", surface, base);
        } else {
            snprintf(word_label, sizeof(word_label), "%s", surface);
        }
        snprintf(line, sizeof(line), "%s  %s", word_label, word.definition[0] != '\0' ? word.definition : "No definition");
        if (word.frequency[0] != '\0') {
            append_text(line, sizeof(line), "  Freq ");
            append_text(line, sizeof(line), word.frequency);
        }
        if (word.kanji[0] != '\0') {
            append_text(line, sizeof(line), "  ");
            append_text(line, sizeof(line), word.kanji);
        }
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
            const char *surface = word.surface[0] != '\0' ? word.surface : word.base;
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
        set_status("OCR request received.");
        write_text(STATUS_PATH, g_status);
        g_ocr_pending = true;
        if (!request_latest_ocr()) {
            g_ocr_pending = false;
        }
    }
}

void handle_input(u64 keys_down, u64 keys_held) {
    static u64 last_logged_buttons = 0;
    const u64 interesting_buttons =
        HidNpadButton_Minus | HidNpadButton_Left | HidNpadButton_Right | HidNpadButton_AnyLeft | HidNpadButton_AnyRight;
    const u64 current_buttons = keys_held & interesting_buttons;
    if ((keys_down & interesting_buttons) != 0 || current_buttons != last_logged_buttons) {
        char debug[128];
        snprintf(debug, sizeof(debug), "Input down=0x%llx held=0x%llx", static_cast<unsigned long long>(keys_down),
                 static_cast<unsigned long long>(keys_held));
        write_debug(debug);
        last_logged_buttons = current_buttons;
    }

    if (keys_down & HidNpadButton_Minus) {
        snprintf(g_status, sizeof(g_status), "OCR request %u: connecting...", ++g_request_count);
        write_text(STATUS_PATH, g_status);
        g_ocr_pending = true;
        if (!request_latest_ocr()) {
            g_ocr_pending = false;
        }
    } else if (keys_down & (HidNpadButton_Left | HidNpadButton_AnyLeft)) {
        move_selection(-1);
    } else if (keys_down & (HidNpadButton_Right | HidNpadButton_AnyRight)) {
        move_selection(1);
    }
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

        if (pad_ready) {
            padUpdate(&pad);
            handle_input(padGetButtonsDown(&pad), padGetButtons(&pad));
        }

        if (!renderer_ready) {
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
        if (renderer_ready) {
            g_renderer.draw();
        }

        svcSleepThread(50000000);
    }

    return 0;
}
