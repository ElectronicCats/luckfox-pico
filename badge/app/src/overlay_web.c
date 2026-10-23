/*
 * File: app/src/overlay_web.c
 * Purpose: HTTP editor, snapshot/preview and MJPEG server for the badge overlay.
 *
 * The server uses only libc and sockets. Each accepted connection runs in a
 * child process so an MJPEG stream cannot block /save or the editor page.
 */

#include <arpa/inet.h>
#include <ctype.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <signal.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#define CONFIG_PATH "/userdata/badge/overlay.ini"
#define CONFIG_DIR "/userdata/badge"
#define EDITOR_PATH "/oem/usr/share/badge/editor.html"
#define ASSET_DIR "/oem/usr/share/badge"
#define LATEST_DIR "/userdata/photos"
#define PREVIEW_PATH "/tmp/badge_preview.jpg"
#define PREVIEW_REQUEST "/tmp/badge_preview.request"
#define CAPTURE_REQUEST "/tmp/badge_capture.request"
#define CAMERA_READY_FILE "/tmp/badge_camera.ready"
#define APP_READY_FILE "/tmp/badge_photo.ready"
#define LISTEN_PORT 80
#define MAX_REQUEST 32768
#define MAX_FILE (4 * 1024 * 1024)

static volatile sig_atomic_t g_running = 1;

static void on_signal(int sig)
{
    (void)sig;
    g_running = 0;
}

static int send_all(int fd, const void *data, size_t len)
{
    const char *p = data;

    while (len > 0) {
        ssize_t n = send(fd, p, len, 0);
        if (n <= 0)
            return -1;
        p += n;
        len -= (size_t)n;
    }
    return 0;
}

static void send_response(int fd, const char *status, const char *type,
                          const char *body, size_t len)
{
    char header[512];
    int n = snprintf(header, sizeof(header),
                     "HTTP/1.1 %s\r\n"
                     "Content-Type: %s\r\n"
                     "Content-Length: %zu\r\n"
                     "Connection: close\r\n"
                     "Cache-Control: no-store\r\n\r\n",
                     status, type, len);
    if (n > 0)
        (void)send_all(fd, header, (size_t)n);
    if (body && len)
        (void)send_all(fd, body, len);
}

static char *load_file(const char *path, size_t *out_len)
{
    FILE *f;
    long size;
    char *buf;

    *out_len = 0;
    f = fopen(path, "rb");
    if (!f)
        return NULL;
    if (fseek(f, 0, SEEK_END) != 0 || (size = ftell(f)) < 0 ||
        size > MAX_FILE || fseek(f, 0, SEEK_SET) != 0) {
        fclose(f);
        return NULL;
    }
    buf = malloc((size_t)size + 1);
    if (!buf) {
        fclose(f);
        return NULL;
    }
    if (fread(buf, 1, (size_t)size, f) != (size_t)size) {
        free(buf);
        fclose(f);
        return NULL;
    }
    fclose(f);
    buf[size] = '\0';
    *out_len = (size_t)size;
    return buf;
}

static void send_file(int fd, const char *path, const char *type)
{
    size_t len = 0;
    char *data = load_file(path, &len);

    if (!data) {
        send_response(fd, "404 Not Found", "text/plain", "not found\n", 10);
        return;
    }
    send_response(fd, "200 OK", type, data, len);
    free(data);
}

static int path_is(const char *path, const char *want)
{
    return strcmp(path, want) == 0;
}

static int has_suffix(const char *s, const char *suffix)
{
    size_t a = strlen(s);
    size_t b = strlen(suffix);
    return a >= b && strcmp(s + a - b, suffix) == 0;
}

static int find_latest_photo(char *out, size_t outsz, time_t *mtime_out,
                             long *size_out)
{
    DIR *dir;
    struct dirent *entry;
    struct stat st;
    char path[512];
    time_t best_time = 0;
    long best_size = 0;
    int found = 0;

    dir = opendir(LATEST_DIR);
    if (!dir)
        return -1;
    while ((entry = readdir(dir)) != NULL) {
        if (!has_suffix(entry->d_name, ".jpg"))
            continue;
        if (snprintf(path, sizeof(path), "%s/%s", LATEST_DIR,
                     entry->d_name) >= (int)sizeof(path))
            continue;
        if (stat(path, &st) != 0 || !S_ISREG(st.st_mode))
            continue;
        if (!found || st.st_mtime > best_time ||
            (st.st_mtime == best_time && st.st_size > best_size)) {
            snprintf(out, outsz, "%s", path);
            best_time = st.st_mtime;
            best_size = (long)st.st_size;
            found = 1;
        }
    }
    closedir(dir);
    if (!found)
        return -1;
    if (mtime_out)
        *mtime_out = best_time;
    if (size_out)
        *size_out = best_size;
    return 0;
}

static int request_preview(void)
{
    struct stat before;
    struct stat after;
    int have_before;
    int fd;
    int i;

    have_before = stat(PREVIEW_PATH, &before) == 0;
    fd = open(PREVIEW_REQUEST, O_CREAT | O_WRONLY | O_TRUNC, 0644);
    if (fd < 0)
        return -1;
    close(fd);

    for (i = 0; i < 150; i++) {
        if (stat(PREVIEW_PATH, &after) == 0 &&
            (!have_before || after.st_mtime != before.st_mtime ||
             after.st_size != before.st_size))
            return 0;
        usleep(20 * 1000);
    }
    return -1;
}

static void serve_preview(int fd)
{
    if (request_preview() != 0) {
        send_response(fd, "503 Service Unavailable", "text/plain",
                      "preview unavailable\n", 20);
        return;
    }
    send_file(fd, PREVIEW_PATH, "image/jpeg");
}

static void serve_stream(int fd)
{
    static const char head[] =
        "HTTP/1.1 200 OK\r\n"
        "Content-Type: multipart/x-mixed-replace; boundary=frame\r\n"
        "Cache-Control: no-store, no-cache\r\n"
        "X-Accel-Buffering: no\r\n"
        "Connection: close\r\n\r\n";
    time_t last_frame = time(NULL);
    int i;

    if (send_all(fd, head, sizeof(head) - 1) != 0)
        return;

    for (i = 0; i < 3600 && g_running; i++) {
        size_t len = 0;
        char *data;
        char part[128];
        int n;

        if (request_preview() != 0) {
            /* Do not leave a silent multipart connection open when the
             * preview pipeline stops producing fresh frames. */
            if (time(NULL) - last_frame >= 6)
                break;
            usleep(250 * 1000);
            continue;
        }
        data = load_file(PREVIEW_PATH, &len);
        if (!data || len == 0) {
            free(data);
            if (time(NULL) - last_frame >= 6)
                break;
            usleep(250 * 1000);
            continue;
        }
        n = snprintf(part, sizeof(part),
                     "--frame\r\nContent-Type: image/jpeg\r\n"
                     "Content-Length: %zu\r\n\r\n", len);
        if (n <= 0 || send_all(fd, part, (size_t)n) != 0 ||
            send_all(fd, data, len) != 0 ||
            send_all(fd, "\r\n", 2) != 0) {
            free(data);
            break;
        }
        free(data);
        last_frame = time(NULL);
        usleep(5 * 1000);
    }
}

static int usb_link_up(void)
{
    DIR *dir;
    struct dirent *entry;
    int up = 0;

    dir = opendir("/sys/class/net");
    if (!dir)
        return 0;
    while ((entry = readdir(dir)) != NULL) {
        char path[320];
        FILE *f;
        int carrier = 0;

        if (strncmp(entry->d_name, "usb", 3) != 0)
            continue;
        snprintf(path, sizeof(path), "/sys/class/net/%s/carrier",
                 entry->d_name);
        f = fopen(path, "r");
        if (f) {
            if (fscanf(f, "%d", &carrier) != 1)
                carrier = 0;
            fclose(f);
            if (carrier) {
                up = 1;
                break;
            }
        }
    }
    closedir(dir);
    return up;
}

static long preview_age_ms(void)
{
    struct stat st;
    time_t now = time(NULL);

    if (stat(PREVIEW_PATH, &st) != 0)
        return -1;
    if (st.st_mtime > now)
        return 0;
    return (long)(now - st.st_mtime) * 1000L;
}

static void serve_status(int fd)
{
    char json[256];
    int n;
    int camera = access(CAMERA_READY_FILE, F_OK) == 0;
    int app = access(APP_READY_FILE, F_OK) == 0;
    int usb = usb_link_up();
    long age = preview_age_ms();

    n = snprintf(json, sizeof(json),
                 "{\"camera\":%d,\"app\":%d,\"usb\":%d,"
                 "\"preview_age_ms\":%ld}", camera, app, usb, age);
    send_response(fd, "200 OK", "application/json", json,
                  n > 0 ? (size_t)n : 0);
}

static void serve_capture(int fd)
{
    char before[512] = { 0 };
    char after[512] = { 0 };
    time_t before_time = 0;
    long before_size = 0;
    int fd_req;
    int i;

    (void)find_latest_photo(before, sizeof(before), &before_time,
                            &before_size);
    fd_req = open(CAPTURE_REQUEST, O_CREAT | O_WRONLY | O_TRUNC, 0644);
    if (fd_req < 0) {
        send_response(fd, "500 Internal Server Error", "text/plain",
                      "capture unavailable\n", 20);
        return;
    }
    close(fd_req);

    for (i = 0; i < 250; i++) {
        time_t now_time = 0;
        long now_size = 0;
        if (find_latest_photo(after, sizeof(after), &now_time, &now_size) == 0 &&
            (before[0] == '\0' || strcmp(before, after) != 0 ||
             now_time != before_time || now_size != before_size)) {
            char json[640];
            int n = snprintf(json, sizeof(json),
                             "{\"ok\":true,\"file\":\"%s\"}", after);
            send_response(fd, "200 OK", "application/json",
                          json, n > 0 ? (size_t)n : 0);
            return;
        }
        usleep(20 * 1000);
    }
    send_response(fd, "202 Accepted", "application/json",
                  "{\"ok\":true,\"pending\":true}", 25);
}

static void serve_asset(int fd, const char *name)
{
    static const char *allowed[] = {
        "electroniccats.bmp", "hack_gdl.bmp",
        "electroniccats.png", "hack_gdl.png",
        "font.ttf", NULL
    };
    char path[512];
    int i;
    const char *type = "application/octet-stream";

    for (i = 0; allowed[i]; i++) {
        if (strcmp(name, allowed[i]) == 0)
            break;
    }
    if (!allowed[i]) {
        send_response(fd, "404 Not Found", "text/plain", "not found\n", 10);
        return;
    }
    if (has_suffix(name, ".png"))
        type = "image/png";
    else if (has_suffix(name, ".bmp"))
        type = "image/bmp";
    else if (has_suffix(name, ".ttf"))
        type = "font/ttf";
    snprintf(path, sizeof(path), "%s/%s", ASSET_DIR, name);
    send_file(fd, path, type);
}

static int read_request(int fd, char *request, size_t cap, size_t *body_len)
{
    size_t total = 0;
    size_t content_length = 0;
    char *header_end = NULL;

    while (total < cap - 1) {
        ssize_t n = recv(fd, request + total, cap - 1 - total, 0);
        if (n <= 0)
            break;
        total += (size_t)n;
        request[total] = '\0';
        if (!header_end)
            header_end = strstr(request, "\r\n\r\n");
        if (header_end) {
            char *p = strstr(request, "Content-Length:");
            if (!p)
                p = strstr(request, "content-length:");
            if (p)
                content_length = (size_t)strtoul(p + 15, NULL, 10);
            if (content_length > cap)
                content_length = cap;
            if (total >= (size_t)(header_end - request) + 4 + content_length)
                break;
        }
    }
    if (!header_end)
        return -1;
    *body_len = (size_t)(header_end - request) + 4;
    if (content_length > total - *body_len)
        content_length = total - *body_len;
    request[*body_len + content_length] = '\0';
    *body_len = content_length;
    return 0;
}

static void handle_client(int fd)
{
    char request[MAX_REQUEST];
    char method[8];
    char path[512];
    char *body;
    size_t body_len = 0;
    size_t total_body_offset;
    int is_post;

    if (read_request(fd, request, sizeof(request), &body_len) != 0) {
        send_response(fd, "400 Bad Request", "text/plain", "bad request\n", 12);
        return;
    }
    body = strstr(request, "\r\n\r\n");
    if (!body)
        return;
    body += 4;
    if (sscanf(request, "%7s %511s", method, path) != 2)
        return;
    {
        char *q = strchr(path, '?');
        if (q)
            *q = '\0';
    }
    is_post = strcmp(method, "POST") == 0;
    total_body_offset = body_len;

    if (is_post && path_is(path, "/save")) {
        char *encoded = strstr(body, "config=");
        char *amp;
        char *decoded;
        size_t decoded_len;
        FILE *f;
        char tmp[256];

        if (!encoded) {
            send_response(fd, "400 Bad Request", "text/plain",
                          "missing config\n", 15);
            return;
        }
        encoded += 7;
        amp = strchr(encoded, '&');
        if (amp)
            *amp = '\0';
        decoded = malloc(strlen(encoded) + 1);
        if (!decoded) {
            send_response(fd, "500 Internal Server Error", "text/plain",
                          "out of memory\n", 14);
            return;
        }
        decoded_len = 0;
        {
            const char *s = encoded;
            while (*s && decoded_len < strlen(encoded)) {
                if (*s == '+') {
                    decoded[decoded_len++] = ' ';
                    s++;
                } else if (*s == '%' && s[1] && s[2]) {
                    int hi = isdigit((unsigned char)s[1]) ? s[1] - '0' :
                        (tolower((unsigned char)s[1]) - 'a' + 10);
                    int lo = isdigit((unsigned char)s[2]) ? s[2] - '0' :
                        (tolower((unsigned char)s[2]) - 'a' + 10);
                    if (hi >= 0 && hi < 16 && lo >= 0 && lo < 16) {
                        decoded[decoded_len++] = (char)((hi << 4) | lo);
                        s += 3;
                    } else {
                        decoded[decoded_len++] = *s++;
                    }
                } else {
                    decoded[decoded_len++] = *s++;
                }
            }
        }
        decoded[decoded_len] = '\0';
        mkdir(CONFIG_DIR, 0755);
        snprintf(tmp, sizeof(tmp), "%s.tmp", CONFIG_PATH);
        f = fopen(tmp, "wb");
        if (!f || fwrite(decoded, 1, decoded_len, f) != decoded_len) {
            if (f)
                fclose(f);
            unlink(tmp);
            free(decoded);
            send_response(fd, "500 Internal Server Error", "text/plain",
                          "write failed\n", 13);
            return;
        }
        fclose(f);
        if (rename(tmp, CONFIG_PATH) != 0) {
            unlink(tmp);
            free(decoded);
            send_response(fd, "500 Internal Server Error", "text/plain",
                          "write failed\n", 13);
            return;
        }
        (void)chmod(CONFIG_PATH, 0666);
        free(decoded);
        send_response(fd, "200 OK", "text/plain", "saved\n", 6);
        return;
    }

    if (is_post && path_is(path, "/api/capture")) {
        serve_capture(fd);
        return;
    }
    if (is_post && path_is(path, "/api/preview")) {
        if (request_preview() == 0)
            send_response(fd, "200 OK", "text/plain", "ok\n", 3);
        else
            send_response(fd, "503 Service Unavailable", "text/plain",
                          "preview unavailable\n", 20);
        return;
    }

    if (!is_post && path_is(path, "/")) {
        send_file(fd, EDITOR_PATH, "text/html; charset=utf-8");
        return;
    }
    if (!is_post && path_is(path, "/overlay.ini")) {
        send_file(fd, CONFIG_PATH, "text/plain; charset=utf-8");
        return;
    }
    if (!is_post && path_is(path, "/latest.jpg")) {
        char latest[512];
        if (find_latest_photo(latest, sizeof(latest), NULL, NULL) == 0)
            send_file(fd, latest, "image/jpeg");
        else
            send_response(fd, "404 Not Found", "text/plain", "no photo\n", 8);
        return;
    }
    if (!is_post && path_is(path, "/api/status")) {
        serve_status(fd);
        return;
    }
    if (!is_post && path_is(path, "/preview.jpg")) {
        serve_preview(fd);
        return;
    }
    if (!is_post && path_is(path, "/stream.mjpg")) {
        serve_stream(fd);
        return;
    }
    if (!is_post && strncmp(path, "/assets/", 8) == 0) {
        serve_asset(fd, path + 8);
        return;
    }
    if (!is_post && path_is(path, "/favicon.ico")) {
        send_response(fd, "204 No Content", "text/plain", "", 0);
        return;
    }
    (void)total_body_offset;
    send_response(fd, "404 Not Found", "text/plain", "not found\n", 10);
}

int main(void)
{
    int fd;
    int one = 1;
    struct sockaddr_in addr;

    signal(SIGTERM, on_signal);
    signal(SIGINT, on_signal);
    signal(SIGCHLD, SIG_IGN);

    fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0)
        return 1;
    setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));

    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_ANY);
    addr.sin_port = htons(LISTEN_PORT);
    if (bind(fd, (struct sockaddr *)&addr, sizeof(addr)) != 0) {
        fprintf(stderr, "overlay_web: bind: %s\n", strerror(errno));
        close(fd);
        return 1;
    }
    if (listen(fd, 16) != 0) {
        fprintf(stderr, "overlay_web: listen: %s\n", strerror(errno));
        close(fd);
        return 1;
    }
    printf("overlay_web: listening on 0.0.0.0:%d\n", LISTEN_PORT);

    while (g_running) {
        int client = accept(fd, NULL, NULL);
        pid_t pid;

        if (client < 0) {
            if (errno == EINTR)
                continue;
            break;
        }
        {
            int one = 1;
            (void)setsockopt(client, IPPROTO_TCP, TCP_NODELAY, &one,
                             sizeof(one));
        }
        pid = fork();
        if (pid == 0) {
            close(fd);
            handle_client(client);
            close(client);
            _exit(0);
        }
        close(client);
    }

    close(fd);
    return 0;
}
