#include <errno.h>
#include <libgen.h>
#include <libmtp.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>

static LIBMTP_mtpdevice_t *open_device(void) {
    LIBMTP_Init();

    LIBMTP_raw_device_t *rawdevices = NULL;
    int numrawdevices = 0;
    LIBMTP_error_number_t detect_error = LIBMTP_Detect_Raw_Devices(&rawdevices, &numrawdevices);
    if (detect_error != LIBMTP_ERROR_NONE || numrawdevices < 1) {
        fprintf(stderr, "No MTP device found; libmtp error code %d\n", detect_error);
        return NULL;
    }

    LIBMTP_mtpdevice_t *device = LIBMTP_Open_Raw_Device_Uncached(&rawdevices[0]);
    free(rawdevices);
    return device;
}

static int progress_cb(uint64_t const sent, uint64_t const total, void const *const data) {
    (void)data;
    if (total == 0) {
        return 0;
    }
    fprintf(stderr, "\r%llu/%llu bytes", (unsigned long long)sent, (unsigned long long)total);
    if (sent >= total) {
        fprintf(stderr, "\n");
    }
    return 0;
}

static int find_child(LIBMTP_mtpdevice_t *device, uint32_t storage_id, uint32_t parent_id,
                      const char *name, int want_folder, LIBMTP_file_t *out) {
    LIBMTP_file_t *items = LIBMTP_Get_Files_And_Folders(device, storage_id, parent_id);
    for (LIBMTP_file_t *item = items; item != NULL; item = item->next) {
        int is_folder = item->filetype == LIBMTP_FILETYPE_FOLDER;
        if (item->filename != NULL && strcmp(item->filename, name) == 0 && is_folder == want_folder) {
            out->item_id = item->item_id;
            out->parent_id = item->parent_id;
            out->storage_id = item->storage_id;
            out->filesize = item->filesize;
            out->filetype = item->filetype;
            LIBMTP_destroy_file_t(items);
            return 0;
        }
    }
    LIBMTP_destroy_file_t(items);
    return 1;
}

static uint32_t ensure_folder(LIBMTP_mtpdevice_t *device, uint32_t storage_id, uint32_t parent_id, const char *name) {
    LIBMTP_file_t existing = {0};
    if (find_child(device, storage_id, parent_id, name, 1, &existing) == 0) {
        return existing.item_id;
    }

    uint32_t created = LIBMTP_Create_Folder(device, (char *)name, parent_id, storage_id);
    if (created == 0) {
        fprintf(stderr, "Failed to create folder '%s'\n", name);
        LIBMTP_Dump_Errorstack(device);
        LIBMTP_Clear_Errorstack(device);
    }
    return created;
}

static int send_file(LIBMTP_mtpdevice_t *device, uint32_t storage_id, uint32_t parent_id,
                     const char *local_path, const char *remote_name) {
    struct stat statbuf;
    if (stat(local_path, &statbuf) != 0) {
        perror(local_path);
        return 1;
    }

    LIBMTP_file_t existing = {0};
    if (find_child(device, storage_id, parent_id, remote_name, 0, &existing) == 0) {
        fprintf(stderr, "Deleting existing %s (0x%08x)\n", remote_name, existing.item_id);
        if (LIBMTP_Delete_Object(device, existing.item_id) != 0) {
            fprintf(stderr, "Failed to delete existing file\n");
            LIBMTP_Dump_Errorstack(device);
            LIBMTP_Clear_Errorstack(device);
            return 1;
        }
    }

    LIBMTP_file_t *file = LIBMTP_new_file_t();
    if (file == NULL) {
        fprintf(stderr, "Failed to allocate file metadata\n");
        return 1;
    }

    file->filename = strdup(remote_name);
    file->filesize = (uint64_t)statbuf.st_size;
    file->filetype = LIBMTP_FILETYPE_UNKNOWN;
    file->parent_id = parent_id;
    file->storage_id = storage_id;
    file->modificationdate = statbuf.st_mtime;

    fprintf(stderr, "Uploading %s -> %s\n", local_path, remote_name);
    int ret = LIBMTP_Send_File_From_File(device, local_path, file, progress_cb, NULL);
    if (ret != 0) {
        fprintf(stderr, "Upload failed\n");
        LIBMTP_Dump_Errorstack(device);
        LIBMTP_Clear_Errorstack(device);
        LIBMTP_destroy_file_t(file);
        return 1;
    }

    fprintf(stderr, "Uploaded object 0x%08x\n", file->item_id);
    LIBMTP_destroy_file_t(file);
    return 0;
}

static uint32_t parse_storage_id(const char *value) {
    errno = 0;
    char *end = NULL;
    unsigned long parsed = strtoul(value, &end, 0);
    if (errno != 0 || end == value || *end != '\0' || parsed > UINT32_MAX) {
        fprintf(stderr, "Invalid storage ID: %s\n", value);
        exit(2);
    }
    return (uint32_t)parsed;
}

int main(int argc, char **argv) {
    if (argc != 4) {
        fprintf(stderr, "usage: %s <storage-id> <local-file> <remote-path>\n", argv[0]);
        return 2;
    }

    uint32_t storage_id = parse_storage_id(argv[1]);
    const char *local_path = argv[2];
    char *remote_path = strdup(argv[3]);
    if (remote_path == NULL) {
        perror("strdup");
        return 1;
    }

    LIBMTP_mtpdevice_t *device = open_device();
    if (device == NULL) {
        free(remote_path);
        return 1;
    }

    uint32_t parent_id = LIBMTP_FILES_AND_FOLDERS_ROOT;
    char *saveptr = NULL;
    char *part = strtok_r(remote_path, "/", &saveptr);
    while (part != NULL) {
        char *next = strtok_r(NULL, "/", &saveptr);
        if (next == NULL) {
            int ret = send_file(device, storage_id, parent_id, local_path, part);
            LIBMTP_Release_Device(device);
            free(remote_path);
            return ret;
        }

        parent_id = ensure_folder(device, storage_id, parent_id, part);
        if (parent_id == 0) {
            LIBMTP_Release_Device(device);
            free(remote_path);
            return 1;
        }
        part = next;
    }

    fprintf(stderr, "Remote path must include a filename\n");
    LIBMTP_Release_Device(device);
    free(remote_path);
    return 2;
}
