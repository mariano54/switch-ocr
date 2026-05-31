#pragma once

// Shared key-binding schema for the Switch OCR sysmodule and overlay.
// Bindings are global and stored at sdmc:/config/switch-ocr/keys.json.

#include <switch.h>
#include <cstddef>
#include <cstdio>
#include <cstring>

namespace switchocr {

enum Action {
    Action_Ocr = 0,
    Action_Mine,
    Action_Left,
    Action_Right,
    Action_Count,
};

struct ActionInfo {
    const char *key;    // json key
    const char *label;  // menu label
};

inline const ActionInfo kActions[Action_Count] = {
    {"ocr",   "Trigger OCR"},
    {"mine",  "Mine word"},
    {"left",  "Select left"},
    {"right", "Select right"},
};

struct ButtonInfo {
    const char *name;
    u64 mask;
};

// Buttons the remap menu can assign. Order is the menu cycle order.
inline const ButtonInfo kButtons[] = {
    {"Minus",  HidNpadButton_Minus},
    {"Plus",   HidNpadButton_Plus},
    {"StickL", HidNpadButton_StickL},
    {"StickR", HidNpadButton_StickR},
    {"L",      HidNpadButton_L},
    {"R",      HidNpadButton_R},
    {"ZL",     HidNpadButton_ZL},
    {"ZR",     HidNpadButton_ZR},
    {"A",      HidNpadButton_A},
    {"B",      HidNpadButton_B},
    {"X",      HidNpadButton_X},
    {"Y",      HidNpadButton_Y},
    {"Up",     HidNpadButton_Up},
    {"Down",   HidNpadButton_Down},
    {"Left",   HidNpadButton_Left},
    {"Right",  HidNpadButton_Right},
    {"LS-Up",    HidNpadButton_StickLUp},
    {"LS-Down",  HidNpadButton_StickLDown},
    {"LS-Left",  HidNpadButton_StickLLeft},
    {"LS-Right", HidNpadButton_StickLRight},
    {"RS-Up",    HidNpadButton_StickRUp},
    {"RS-Down",  HidNpadButton_StickRDown},
    {"RS-Left",  HidNpadButton_StickRLeft},
    {"RS-Right", HidNpadButton_StickRRight},
};

inline constexpr size_t kButtonCount = sizeof(kButtons) / sizeof(kButtons[0]);

struct KeyBindings {
    u64 mask[Action_Count];
};

inline KeyBindings defaultBindings() {
    KeyBindings bindings{};
    bindings.mask[Action_Ocr] = HidNpadButton_Minus;
    bindings.mask[Action_Mine] = HidNpadButton_StickR;
    bindings.mask[Action_Left] = HidNpadButton_Left;
    bindings.mask[Action_Right] = HidNpadButton_Right;
    return bindings;
}

inline u64 maskForName(const char *name) {
    for (size_t i = 0; i < kButtonCount; i++) {
        if (strcmp(name, kButtons[i].name) == 0) {
            return kButtons[i].mask;
        }
    }
    return 0;
}

inline const char *nameForMask(u64 mask) {
    for (size_t i = 0; i < kButtonCount; i++) {
        if (kButtons[i].mask == mask) {
            return kButtons[i].name;
        }
    }
    return "?";
}

// Returns the index into kButtons for a mask, or 0 if unknown.
inline size_t buttonIndexForMask(u64 mask) {
    for (size_t i = 0; i < kButtonCount; i++) {
        if (kButtons[i].mask == mask) {
            return i;
        }
    }
    return 0;
}

inline bool readJsonStringField(const char *json, const char *key, char *out, size_t outSize) {
    if (outSize == 0) {
        return false;
    }
    char pattern[24];
    snprintf(pattern, sizeof(pattern), "\"%s\"", key);
    const char *cursor = strstr(json, pattern);
    if (cursor == nullptr) {
        return false;
    }
    cursor += strlen(pattern);
    while (*cursor == ' ' || *cursor == '\t' || *cursor == '\n' || *cursor == '\r') {
        cursor++;
    }
    if (*cursor != ':') {
        return false;
    }
    cursor++;
    while (*cursor == ' ' || *cursor == '\t' || *cursor == '\n' || *cursor == '\r') {
        cursor++;
    }
    if (*cursor != '"') {
        return false;
    }
    cursor++;
    size_t used = 0;
    while (*cursor != '\0' && *cursor != '"' && used + 1 < outSize) {
        out[used++] = *cursor++;
    }
    out[used] = '\0';
    return used > 0;
}

inline KeyBindings parseBindings(const char *json) {
    KeyBindings bindings = defaultBindings();
    char value[16];
    for (int action = 0; action < Action_Count; action++) {
        if (readJsonStringField(json, kActions[action].key, value, sizeof(value))) {
            const u64 mask = maskForName(value);
            if (mask != 0) {
                bindings.mask[action] = mask;
            }
        }
    }
    return bindings;
}

// Button combo (handled by the overlay) that opens the Tesla settings menu.
inline constexpr const char *kSettingsComboLabel = "ZL+ZR+Up";

// Builds the idle/intro help shown on the HUD. Split across the paragraph row
// (actions) and the target row (settings) so it fits the compact panel.
inline void formatHelpResult(const KeyBindings &bindings, char *out, size_t outSize) {
    snprintf(out, outSize,
             "Switch OCR ready.  %s/Capture: OCR    %s/%s: select    %s: save word",
             nameForMask(bindings.mask[Action_Ocr]),
             nameForMask(bindings.mask[Action_Left]),
             nameForMask(bindings.mask[Action_Right]),
             nameForMask(bindings.mask[Action_Mine]));
}

inline void formatHelpTarget(char *out, size_t outSize) {
    snprintf(out, outSize,
             "%s: settings  (live HUD on/off, screen top/bottom, remap keys)",
             kSettingsComboLabel);
}

inline void serializeBindings(const KeyBindings &bindings, char *out, size_t outSize) {
    snprintf(out, outSize,
             "{\"ocr\":\"%s\",\"mine\":\"%s\",\"left\":\"%s\",\"right\":\"%s\"}\n",
             nameForMask(bindings.mask[Action_Ocr]),
             nameForMask(bindings.mask[Action_Mine]),
             nameForMask(bindings.mask[Action_Left]),
             nameForMask(bindings.mask[Action_Right]));
}

} // namespace switchocr
