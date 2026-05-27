#define TESLA_INIT_IMPL
#include <tesla.hpp>

#include <stdio.h>
#include <string.h>
#include <sys/stat.h>

#define CONFIG_DIR "sdmc:/config/switch-ocr"
#define REQUEST_PATH CONFIG_DIR "/request.txt"
#define RESULT_PATH CONFIG_DIR "/result.txt"
#define STATUS_PATH CONFIG_DIR "/status.txt"

namespace {

char g_status[512] = "Sysmodule HUD not seen yet. Reboot after installing it.";
char g_result[1024] = "No OCR result yet.";
bool g_sd_mounted = false;

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
}

void sendOcrRequest() {
    char request[64];
    snprintf(request, sizeof(request), "%llu", static_cast<unsigned long long>(armGetSystemTick()));
    writeText(REQUEST_PATH, request);
    copyText(g_status, sizeof(g_status), "Overlay sent OCR request. Waiting for sysmodule...");
    writeText(STATUS_PATH, g_status);
}

} // namespace

class SwitchOcrGui final : public tsl::Gui {
public:
    tsl::elm::Element *createUI() override {
        refreshDisplayFiles();
        auto *frame = new tsl::elm::OverlayFrame("Switch OCR v3", "A OCR, X close");
        auto *list = new tsl::elm::List();

        auto *request = new tsl::elm::ListItem("Transcribe", "A");
        request->setClickListener([](u64 keys) {
            if (keys & HidNpadButton_A) {
                sendOcrRequest();
                tsl::Overlay::get()->close();
                return true;
            }
            return false;
        });
        list->addItem(request);

        list->addItem(new tsl::elm::CategoryHeader("Sysmodule status", true));
        list->addItem(new tsl::elm::ListItem(g_status));
        list->addItem(new tsl::elm::CategoryHeader("Latest OCR result", true));
        list->addItem(new tsl::elm::ListItem(g_result));
        list->addItem(new tsl::elm::CategoryHeader("Controls", true));
        list->addItem(new tsl::elm::ListItem("A writes request and closes this overlay"));
        list->addItem(new tsl::elm::ListItem("X closes this overlay"));
        list->addItem(new tsl::elm::ListItem("Plus closes this Tesla overlay when focused"));
        list->addItem(new tsl::elm::ListItem("Sysmodule hotkey: Minus OCR"));

        frame->setContent(list);
        return frame;
    }

    bool handleInput(u64 keysDown, u64, const HidTouchState &, HidAnalogStickState, HidAnalogStickState) override {
        if (keysDown & (HidNpadButton_X | HidNpadButton_Plus)) {
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
