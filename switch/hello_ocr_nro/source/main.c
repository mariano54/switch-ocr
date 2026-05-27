#include <switch.h>

#include <arpa/inet.h>
#include <errno.h>
#include <netinet/in.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

#ifndef SERVER_HOST
#define SERVER_HOST "192.168.0.125"
#endif

#ifndef SERVER_PORT
#define SERVER_PORT 8000
#endif

#define RESPONSE_SIZE 16384
#define JPEG_BUFFER_SIZE (4 * 1024 * 1024)

static char g_status[512] = "Ready.";
static char g_response[RESPONSE_SIZE] = "";

static void set_status(const char *message) {
    snprintf(g_status, sizeof(g_status), "%s", message);
}

static void keep_http_body_only(void) {
    char *body = strstr(g_response, "\r\n\r\n");
    if (body == NULL) {
        return;
    }

    body += 4;
    memmove(g_response, body, strlen(body) + 1);
}

static bool copy_json_string_value(const char *key, char *out, size_t out_size) {
    char pattern[64];
    snprintf(pattern, sizeof(pattern), "\"%s\": \"", key);

    char *cursor = strstr(g_response, pattern);
    if (cursor == NULL || out_size == 0) {
        return false;
    }

    cursor += strlen(pattern);
    size_t used = 0;
    while (*cursor && used + 1 < out_size) {
        if (*cursor == '"' && (cursor == g_response || cursor[-1] != '\\')) {
            break;
        }

        if (*cursor == '\\' && cursor[1]) {
            cursor++;
            if (*cursor == 'n') {
                out[used++] = '\n';
            } else if (*cursor == 'r') {
                out[used++] = '\r';
            } else if (*cursor == 't') {
                out[used++] = '\t';
            } else {
                out[used++] = *cursor;
            }
            cursor++;
            continue;
        }

        out[used++] = *cursor++;
    }
    out[used] = '\0';
    return true;
}

static int send_all(int sock, const void *data, size_t size) {
    const char *cursor = (const char *)data;
    while (size > 0) {
        ssize_t sent = send(sock, cursor, size, 0);
        if (sent < 0) {
            return -1;
        }
        cursor += sent;
        size -= (size_t)sent;
    }
    return 0;
}

static int connect_to_server(void) {
    int sock = socket(AF_INET, SOCK_STREAM, 0);
    if (sock < 0) {
        return -1;
    }

    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_port = htons(SERVER_PORT);

    if (inet_pton(AF_INET, SERVER_HOST, &addr.sin_addr) != 1) {
        close(sock);
        return -1;
    }

    if (connect(sock, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        close(sock);
        return -1;
    }

    return sock;
}

static int http_post(const char *path, const char *content_type, const void *body, size_t body_size) {
    int sock = connect_to_server();
    if (sock < 0) {
        snprintf(g_response, sizeof(g_response), "connect failed: errno=%d", errno);
        return -1;
    }

    char header[512];
    int header_size = snprintf(
        header,
        sizeof(header),
        "POST %s HTTP/1.1\r\n"
        "Host: %s:%d\r\n"
        "Content-Type: %s\r\n"
        "Content-Length: %zu\r\n"
        "Connection: close\r\n"
        "\r\n",
        path,
        SERVER_HOST,
        SERVER_PORT,
        content_type,
        body_size
    );

    if (header_size < 0 || (size_t)header_size >= sizeof(header)) {
        close(sock);
        snprintf(g_response, sizeof(g_response), "request header too large");
        return -1;
    }

    if (send_all(sock, header, (size_t)header_size) != 0 || send_all(sock, body, body_size) != 0) {
        close(sock);
        snprintf(g_response, sizeof(g_response), "send failed: errno=%d", errno);
        return -1;
    }

    size_t used = 0;
    while (used + 1 < sizeof(g_response)) {
        ssize_t received = recv(sock, g_response + used, sizeof(g_response) - used - 1, 0);
        if (received <= 0) {
            break;
        }
        used += (size_t)received;
    }
    g_response[used] = '\0';
    close(sock);
    keep_http_body_only();
    return 0;
}

static void post_hello(void) {
    set_status("Posting /hello...");
    const char *body = "{\"client\":\"switch-homebrew\",\"message\":\"hello from Switch\"}";
    if (http_post("/hello", "application/json", body, strlen(body)) == 0) {
        set_status("POST /hello complete.");
    } else {
        set_status("POST /hello failed.");
    }
}

static void capture_and_post(void) {
    set_status("Capturing JPEG screenshot...");

    Result rc = capsscInitialize();
    if (R_FAILED(rc)) {
        snprintf(g_response, sizeof(g_response), "capsscInitialize failed: 0x%x", rc);
        set_status("Screenshot service unavailable.");
        return;
    }

    void *jpeg = malloc(JPEG_BUFFER_SIZE);
    if (jpeg == NULL) {
        capsscExit();
        snprintf(g_response, sizeof(g_response), "malloc failed for JPEG buffer");
        set_status("Screenshot buffer allocation failed.");
        return;
    }

    u64 jpeg_size = 0;
    rc = capsscCaptureJpegScreenShot(
        &jpeg_size,
        jpeg,
        JPEG_BUFFER_SIZE,
        ViLayerStack_Screenshot,
        1000000000LL
    );

    capsscExit();

    if (R_FAILED(rc)) {
        free(jpeg);
        snprintf(g_response, sizeof(g_response), "capsscCaptureJpegScreenShot failed: 0x%x", rc);
        set_status("Screenshot capture failed.");
        return;
    }

    snprintf(g_status, sizeof(g_status), "Posting /ocr with %llu JPEG bytes...", (unsigned long long)jpeg_size);
    if (http_post("/ocr", "image/jpeg", jpeg, (size_t)jpeg_size) == 0) {
        char display[RESPONSE_SIZE];
        if (copy_json_string_value("display_text", display, sizeof(display))) {
            snprintf(g_response, sizeof(g_response), "%s", display);
        }
        set_status("POST /ocr complete.");
    } else {
        set_status("POST /ocr failed.");
    }

    free(jpeg);
}

static void draw_screen(void) {
    consoleClear();
    printf("\x1b[1;1HSwitch OCR Prototype\n");
    printf("Server: http://%s:%d\n\n", SERVER_HOST, SERVER_PORT);
    printf("+------------------------------+\n");
    printf("|  Hello World OCR Menu        |\n");
    printf("|  A: POST /hello              |\n");
    printf("|  ZL+ZR: capture JPEG -> /ocr |\n");
    printf("|  X: capture JPEG -> /ocr     |\n");
    printf("|  +: exit                     |\n");
    printf("+------------------------------+\n\n");
    printf("Status:\n%s\n\n", g_status);
    printf("Last response:\n%s\n", g_response[0] ? g_response : "(none)");
}

int main(int argc, char **argv) {
    (void)argc;
    (void)argv;

    consoleInit(NULL);
    socketInitializeDefault();

    PadState pad;
    padConfigureInput(1, HidNpadStyleSet_NpadStandard);
    padInitializeDefault(&pad);

    while (appletMainLoop()) {
        padUpdate(&pad);
        u64 down = padGetButtonsDown(&pad);

        if (down & HidNpadButton_Plus) {
            break;
        }
        if (down & HidNpadButton_A) {
            post_hello();
        }
        u64 held = padGetButtons(&pad);
        if ((down & HidNpadButton_X) || ((held & HidNpadButton_ZL) && (down & HidNpadButton_ZR))) {
            capture_and_post();
        }

        draw_screen();
        consoleUpdate(NULL);
    }

    socketExit();
    consoleExit(NULL);
    return 0;
}
