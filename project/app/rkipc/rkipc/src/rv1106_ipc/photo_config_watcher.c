#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <limits.h>
#include <sys/inotify.h>
#include <sys/stat.h>
#include <time.h>

#include "log.h"
#include "iniparser.h"
#include "param.h"
#include "osd.h"
#include "photo_config_watcher.h"

#define WATCH_DIR           "/userdata/video0"
#define TARGET_FILE         "photo_config.ini"
#define TARGET_FILE_SOURCE  "/root/photo_config.ini"
#define DEST_FILE_SOURCE    "/oem/usr/share/rkipc-300w.ini"
#define DEST_FILE           "/userdata/rkipc.ini"
#define SECTION2            "osd.2"
#define SECTION6            "osd.6"
#define SECTION7            "osd.7"

#define EVENT_MASK (IN_CLOSE_WRITE | IN_MOVED_TO | IN_CREATE)
#define BUF_LEN (1024 * (sizeof(struct inotify_event) + NAME_MAX + 1))

typedef struct {
    const char *key;
    const char *default_value;
} config_schema_t;

static config_schema_t schema_character[] = {
    {"type",         "character"},
    {"enabled",      "0"},
    {"position_x",   "0"},
    {"position_y",   "0"},
    {"display_text", "null"},
};

static config_schema_t schema_image[] = {
    {"type",         "image"},
    {"enabled",      "0"},
    {"position_x",   "0"},
    {"position_y",   "0"},
    {"image_path",   "null"},
};

#define NUM_KEYS_CHARACTER (sizeof(schema_character)/sizeof(schema_character[0]))
#define NUM_KEYS_IMAGE (sizeof(schema_image)/sizeof(schema_image[0]))

static int inotify_fd = -1;
static int watch_fd = -1;
static time_t last_mtime = 0;

/* ===================================== */

static time_t get_mtime(const char *filepath) {
    struct stat st;
    if (stat(filepath, &st) != 0)
        return 0;
    return st.st_mtime;
}

/* ===================================== */

static void set_section(dictionary *src, dictionary *dst, dictionary *dst_source, const char* section, config_schema_t* schema, int num_keys) {
    
    char fullkey[128];

    for (int i = 0; i < num_keys; i++) {

        snprintf(fullkey, sizeof(fullkey),
                 "%s:%s", section, schema[i].key);

        const char *value =
            iniparser_getstring(src, fullkey, NULL);

        if (!value)
            value = schema[i].default_value;

        // Update both destinations
        iniparser_set(dst, fullkey, value);
        iniparser_set(dst_source, fullkey, value);
    }
}

/* ===================================== */

static void process_file(void) {

    char fullpath[PATH_MAX];
    snprintf(fullpath, sizeof(fullpath), "%s/%s", WATCH_DIR, TARGET_FILE);

    time_t mtime = get_mtime(fullpath);
    if (mtime == 0 || mtime == last_mtime)
        return;

    last_mtime = mtime;

    dictionary *src = iniparser_load(fullpath);
    if (!src) {
        LOG_ERROR("Failed to load %s\n", fullpath);
        return;
    }

    // Load destination 1
    dictionary *dst = iniparser_load(DEST_FILE);
    if (!dst)
        dst = dictionary_new(0);

    // Load destination 2 (SOURCE)
    dictionary *dst_source = iniparser_load(DEST_FILE_SOURCE);
    if (!dst_source)
        dst_source = dictionary_new(0);

    set_section(src, dst, dst_source, SECTION2, schema_character, NUM_KEYS_CHARACTER);
    set_section(src, dst, dst_source, SECTION6, schema_image, NUM_KEYS_IMAGE);
    set_section(src, dst, dst_source, SECTION7, schema_image, NUM_KEYS_IMAGE);

    // Save DEST_FILE
    FILE *f = fopen(DEST_FILE, "w");
    if (f) {
        iniparser_dump_ini(dst, f);
        fclose(f);
        LOG_INFO("File %s updated.\n", DEST_FILE);
    } else {
        LOG_ERROR("Could not write %s\n", DEST_FILE);
    }

    // Save DEST_FILE_SOURCE
    FILE *f2 = fopen(DEST_FILE_SOURCE, "w");
    if (f2) {
        iniparser_dump_ini(dst_source, f2);
        fclose(f2);
        LOG_INFO("File %s updated.\n", DEST_FILE_SOURCE);
    } else {
        LOG_ERROR("Could not write %s (read-only?)\n", DEST_FILE_SOURCE);
    }

    rk_param_reload();
    rk_osd_restart();

    iniparser_freedict(src);
    iniparser_freedict(dst);
    iniparser_freedict(dst_source);
}

/* ===================================== */

int photo_config_watcher_init(void) {
    inotify_fd = inotify_init1(IN_NONBLOCK);
    if (inotify_fd < 0) {
        LOG_ERROR("inotify_init error\n");
        return -1;
    }

    watch_fd = inotify_add_watch(inotify_fd, WATCH_DIR, EVENT_MASK);
    if (watch_fd < 0) {
        LOG_ERROR("inotify_add_watch error\n");
        close(inotify_fd);
        inotify_fd = -1;
        return -1;
    }

    LOG_INFO("Watcher started for %s/%s\n",
             WATCH_DIR, TARGET_FILE);

    return 0;
}

/* ===================================== */

void photo_config_watcher_process(void) {
    if (inotify_fd < 0)
        return;

    char buffer[BUF_LEN];

    int length = read(inotify_fd, buffer, BUF_LEN);
    if (length <= 0)
        return;

    int i = 0;
    while (i < length) {

        struct inotify_event *event =
            (struct inotify_event *)&buffer[i];

        if (event->len > 0 &&
            strcmp(event->name, TARGET_FILE) == 0 &&
            (event->mask & EVENT_MASK)) {

            process_file();
        }

        i += sizeof(struct inotify_event) + event->len;
    }
}

/* ===================================== */

void photo_config_watcher_deinit(void) {
    if (watch_fd >= 0)
        inotify_rm_watch(inotify_fd, watch_fd);

    if (inotify_fd >= 0)
        close(inotify_fd);

    inotify_fd = -1;
    watch_fd = -1;
}
