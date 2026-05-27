#define TESLA_INIT_IMPL
#include <tesla.hpp>

#include <stdio.h>
#include <string.h>
#include <sys/stat.h>

#define CONFIG_DIR "sdmc:/config/switch-ocr"
#define REQUEST_PATH CONFIG_DIR "/request.txt"
#define RESULT_PATH CONFIG_DIR "/result.txt"
#define STATUS_PATH CONFIG_DIR "/status.txt"
#define DEBUG_PATH CONFIG_DIR "/debug.txt"

namespace {

char g_status[512] = "Sysmodule HUD not seen yet. Reboot after installing it.";
char g_result[1024] = "No OCR result yet.";
char g_debug[512] = "No HUD debug yet.";
bool g_sd_mounted = false;
int g_poll_ticks = 0;
tsl::elm::ListItem *g_status_item = nullptr;
tsl::elm::ListItem *g_result_item = nullptr;
tsl::elm::ListItem *g_debug_item = nullptr;
tsl::elm::ListItem *g_request_item = nullptr;

void copyText(char *out, size_t outSize, const char *text) {
    if (outSize == 0) {
        return;
    }
    snprintf(out, outSize, "%s", text != nullptr ? text : "");
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
    return read > 0;
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

void refreshDisplayFiles() {
    char status[sizeof(g_status)];
    if (readText(STATUS_PATH, status, sizeof(status))) {
        copyText(g_status, sizeof(g_status), status);
    }
    char result[sizeof(g_result)];
    if (readText(RESULT_PATH, result, sizeof(result))) {
        copyText(g_result, sizeof(g_result), result);
    }
    char debug[sizeof(g_debug)];
    if (readText(DEBUG_PATH, debug, sizeof(debug))) {
        copyText(g_debug, sizeof(g_debug), debug);
    }

    if (g_status_item != nullptr) {
        g_status_item->setText(g_status);
        g_status_item->invalidate();
    }
    if (g_result_item != nullptr) {
        g_result_item->setText(g_result);
        g_result_item->invalidate();
    }
    if (g_debug_item != nullptr) {
        g_debug_item->setText(g_debug);
        g_debug_item->invalidate();
    }
}

void sendOcrRequest() {
    char request[64];
    snprintf(request, sizeof(request), "%llu", static_cast<unsigned long long>(armGetSystemTick()));
    writeText(REQUEST_PATH, request);
    copyText(g_status, sizeof(g_status), "Overlay sent OCR request. Waiting for sysmodule...");
    writeText(STATUS_PATH, g_status);
    if (g_status_item != nullptr) {
        g_status_item->setText(g_status);
        g_status_item->invalidate();
    }
    if (g_request_item != nullptr) {
        g_request_item->setValue("sent");
        g_request_item->invalidate();
    }
}

} // namespace

class SwitchOcrGui final : public tsl::Gui {
public:
    ~SwitchOcrGui() override {
        g_status_item = nullptr;
        g_result_item = nullptr;
        g_debug_item = nullptr;
        g_request_item = nullptr;
    }

    tsl::elm::Element *createUI() override {
        refreshDisplayFiles();
        auto *frame = new tsl::elm::OverlayFrame("Switch OCR", "Manual test panel");
        auto *list = new tsl::elm::List();

        auto *request = new tsl::elm::ListItem("Send OCR request", "A");
        request->setClickListener([](u64 keys) {
            if (keys & HidNpadButton_A) {
                sendOcrRequest();
                return true;
            }
            return false;
        });
        g_request_item = request;
        list->addItem(request);

        g_status_item = new tsl::elm::ListItem(g_status);
        list->addItem(new tsl::elm::CategoryHeader("Sysmodule status", true));
        list->addItem(g_status_item);
        g_result_item = new tsl::elm::ListItem(g_result);
        list->addItem(new tsl::elm::CategoryHeader("Latest OCR result", true));
        list->addItem(g_result_item);
        g_debug_item = new tsl::elm::ListItem(g_debug);
        list->addItem(new tsl::elm::CategoryHeader("HUD/input debug", true));
        list->addItem(g_debug_item);
        refreshDisplayFiles();
        list->addItem(new tsl::elm::CategoryHeader("Controls", true));
        list->addItem(new tsl::elm::ListItem("Minus hotkey is still handled by sysmodule"));
        list->addItem(new tsl::elm::ListItem("Tesla hotkey", "LS"));

        frame->setContent(list);
        return frame;
    }

    void update() override {
        if (++g_poll_ticks >= 5) {
            g_poll_ticks = 0;
            refreshDisplayFiles();
        }
    }

    bool handleInput(u64 keysDown, u64, const HidTouchState &, HidAnalogStickState, HidAnalogStickState) override {
        if (keysDown & HidNpadButton_Plus) {
            tsl::Overlay::get()->close();
            return true;
        }
        return false;
    }
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
        g_status_item = nullptr;
        g_result_item = nullptr;
        g_debug_item = nullptr;
        g_request_item = nullptr;
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
