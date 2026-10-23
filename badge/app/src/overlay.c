/*
 * File: app/src/overlay.c
 * Purpose: Hardware RGN overlay of the badge logos and text over the JPEG VENC channel.
 *
 * Defaults match the badge reference firmware photo_config.ini:
 *   text  "Hack GDL 2026" at (100, 100)
 *   image electroniccats.bmp at (100, 640)
 *   image hack_gdl.bmp at (1520, 640)
 *
 * Runtime positions, enable flags and image paths can be edited without a
 * terminal in /userdata/badge/overlay.ini (exposed as the MTP storage
 * "Badge Config"). overlay_poll() watches that file and applies changes to
 * the live RGN regions.
 */

#include "overlay.h"
#include "log.h"

#include "../third_party/stb/stb_truetype.h"

#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#include "rk_comm_rgn.h"
#include "rk_comm_video.h"
#include "rk_mpi_rgn.h"
#include "rk_mpi_sys.h"
#include "rk_mpi_venc.h"

#define OVERLAY_DIR "/oem/usr/share/badge/"
#define OVERLAY_CONFIG "/userdata/badge/overlay.ini"
#define OVERLAY_CONFIG_DIR "/userdata/badge"
#define OVERLAY_DEFAULT_CONFIG OVERLAY_DIR "overlay.ini"
#define OVERLAY_COUNT 3
#define OVERLAY_HANDLE_BASE 10

#define BMP_TRANSPARENT_B 0x08
#define BMP_TRANSPARENT_G 0x00
#define BMP_TRANSPARENT_R 0x00

struct overlay_item {
    const char *section;
    const char *default_file;
    int default_x;
    int default_y;
    char image[256];
    char text[128];
    char font[256];
    int font_size;
    uint32_t font_color;
    int is_text;
    int x;
    int y;
    int enabled;
    RGN_HANDLE handle;
    uint8_t *buffer;
    int width;
    int height;
    int attached;
};

static struct overlay_item g_items[OVERLAY_COUNT] = {
    { "logo1", "electroniccats.bmp", 100, 640, { 0 }, { 0 }, { 0 }, 0, 0, 0, 100, 640, 1, 0, NULL, 0, 0, 0 },
    { "logo2", "hack_gdl.bmp", 1520, 640, { 0 }, { 0 }, { 0 }, 0, 0, 0, 1520, 640, 1, 0, NULL, 0, 0, 0 },
    { "text", NULL, 100, 100, { 0 }, { 0 }, { 0 }, 32, 0xfff799, 1, 100, 100, 1, 0, NULL, 0, 0, 0 },
};

static int g_inited;
static time_t g_config_mtime;
static off_t g_config_size;
static int g_suspended;

struct bmp_file_header {
    uint16_t type;
    uint32_t size;
    uint16_t reserved1;
    uint16_t reserved2;
    uint32_t off_bits;
} __attribute__((packed));

struct bmp_info_header {
    uint32_t size;
    int32_t width;
    int32_t height;
    uint16_t planes;
    uint16_t bit_count;
    uint32_t compression;
    uint32_t size_image;
    int32_t x_pels;
    int32_t y_pels;
    uint32_t clr_used;
    uint32_t clr_important;
} __attribute__((packed));

static void item_defaults(struct overlay_item *item)
{
    item->x = item->default_x;
    item->y = item->default_y;
    item->enabled = 1;
    if (strcmp(item->section, "text") == 0) {
        item->is_text = 1;
        item->font_size = 32;
        item->font_color = 0xfff799;
        snprintf(item->text, sizeof(item->text), "Hack GDL 2026");
        snprintf(item->font, sizeof(item->font), "%sfont.ttf", OVERLAY_DIR);
        item->image[0] = '\0';
    } else {
        item->is_text = 0;
        snprintf(item->image, sizeof(item->image), "%s%s", OVERLAY_DIR,
                 item->default_file);
    }
}

static int load_bmp(const char *path, uint8_t **out, int *width, int *height)
{
    FILE *f;
    long size;
    uint8_t *data;
    struct bmp_file_header file_header;
    struct bmp_info_header info;
    uint8_t *bmp_data;
    uint8_t *argb;
    int pitch;
    int i;
    int j;

    *out = NULL;
    *width = 0;
    *height = 0;

    f = fopen(path, "rb");
    if (!f) {
        LOGW("overlay: no se pudo abrir %s\n", path);
        return -1;
    }
    if (fseek(f, 0, SEEK_END) != 0 || (size = ftell(f)) <= 0 ||
        fseek(f, 0, SEEK_SET) != 0) {
        fclose(f);
        return -1;
    }
    data = malloc((size_t)size);
    if (!data) {
        fclose(f);
        return -1;
    }
    if (fread(data, 1, (size_t)size, f) != (size_t)size) {
        free(data);
        fclose(f);
        return -1;
    }
    fclose(f);

    if (size < (long)(sizeof(file_header) + sizeof(info))) {
        free(data);
        return -1;
    }
    memcpy(&file_header, data, sizeof(file_header));
    memcpy(&info, data + sizeof(file_header), sizeof(info));

    if (file_header.type != 0x4d42 || info.compression != 0 ||
        (info.bit_count != 24 && info.bit_count != 32) ||
        info.width <= 0 || info.height == 0) {
        LOGW("overlay: %s no es un BMP 24/32 bits sin compresión\n", path);
        free(data);
        return -1;
    }

    pitch = ((info.width * info.bit_count + 31) / 32) * 4;
    if (file_header.off_bits + (uint32_t)pitch * abs(info.height) > (uint32_t)size) {
        free(data);
        return -1;
    }
    bmp_data = data + file_header.off_bits;

    argb = malloc((size_t)info.width * abs(info.height) * 4);
    if (!argb) {
        free(data);
        return -1;
    }

    for (i = 0; i < abs(info.height); i++) {
        int src_row = info.height > 0 ? abs(info.height) - i - 1 : i;
        for (j = 0; j < info.width; j++) {
            int src = src_row * pitch + j * (info.bit_count / 8);
            int dst = (i * info.width + j) * 4;
            uint8_t b = bmp_data[src + 0];
            uint8_t g = bmp_data[src + 1];
            uint8_t r = bmp_data[src + 2];
            uint8_t a;

            if (info.bit_count == 32)
                a = bmp_data[src + 3];
            else if (b == BMP_TRANSPARENT_B &&
                     g == BMP_TRANSPARENT_G &&
                     r == BMP_TRANSPARENT_R)
                a = 0;
            else
                a = 0xff;

            argb[dst + 0] = a;
            argb[dst + 1] = r;
            argb[dst + 2] = g;
            argb[dst + 3] = b;
        }
    }

    free(data);
    *out = argb;
    *width = info.width;
    *height = abs(info.height);
    return 0;
}

static int render_text(const char *text, const char *font_path, int size,
                       uint32_t rgb, uint8_t **out, int *width, int *height)
{
    FILE *f;
    long len;
    unsigned char *ttf;
    stbtt_fontinfo font;
    float scale;
    int ascent;
    int descent;
    int line_gap;
    int baseline;
    int pad = 8;
    int text_width = 0;
    int w;
    int h;
    uint8_t *buffer;
    int x;
    int i;
    uint8_t r = (rgb >> 16) & 0xff;
    uint8_t g = (rgb >> 8) & 0xff;
    uint8_t b = rgb & 0xff;

    *out = NULL;
    *width = 0;
    *height = 0;
    if (!text || !text[0] || !font_path || size <= 0)
        return -1;

    LOGD("overlay: render '%s' size=%d color=%06x font=%s\n", text, size,
         (unsigned)rgb, font_path);
    f = fopen(font_path, "rb");
    if (!f) {
        LOGW("overlay: no se pudo abrir la fuente %s\n", font_path);
        return -1;
    }
    if (fseek(f, 0, SEEK_END) != 0 || (len = ftell(f)) <= 0 ||
        fseek(f, 0, SEEK_SET) != 0) {
        fclose(f);
        return -1;
    }
    ttf = malloc((size_t)len);
    if (!ttf) {
        fclose(f);
        return -1;
    }
    if (fread(ttf, 1, (size_t)len, f) != (size_t)len) {
        free(ttf);
        fclose(f);
        return -1;
    }
    fclose(f);

    if (!stbtt_InitFont(&font, ttf, stbtt_GetFontOffsetForIndex(ttf, 0))) {
        free(ttf);
        return -1;
    }
    scale = stbtt_ScaleForPixelHeight(&font, (float)size);
    stbtt_GetFontVMetrics(&font, &ascent, &descent, &line_gap);
    baseline = (int)(ascent * scale);

    for (i = 0; text[i]; i++) {
        int advance;
        int lsb;

        stbtt_GetCodepointHMetrics(&font, (int)(unsigned char)text[i], &advance, &lsb);
        text_width += (int)(advance * scale);
    }

    w = text_width + pad * 2;
    h = (int)((ascent - descent) * scale) + pad * 2;
    w = (w + 15) & ~15;
    h = (h + 15) & ~15;
    buffer = calloc((size_t)w * h, 4);
    if (!buffer) {
        free(ttf);
        return -1;
    }

    x = pad;
    for (i = 0; text[i]; i++) {
        int advance;
        int lsb;
        int x0;
        int y0;
        int x1;
        int y1;
        int gw;
        int gh;
        unsigned char *glyph;
        int row;
        int col;

        stbtt_GetCodepointHMetrics(&font, (int)(unsigned char)text[i], &advance, &lsb);
        stbtt_GetCodepointBitmapBox(&font, (int)(unsigned char)text[i], scale, scale,
                                    &x0, &y0, &x1, &y1);
        gw = x1 - x0;
        gh = y1 - y0;
        if (gw > 0 && gh > 0) {
            glyph = stbtt_GetCodepointBitmap(&font, 0, scale,
                                             (int)(unsigned char)text[i],
                                             &gw, &gh, 0, 0);
            for (row = 0; row < gh; row++) {
                for (col = 0; col < gw; col++) {
                    uint8_t alpha = glyph[row * gw + col];
                    int px;
                    int py;
                    int dst;

                    if (!alpha)
                        continue;
                    px = x + x0 + col;
                    py = pad + baseline + y0 + row;
                    if (px < 0 || py < 0 || px >= w || py >= h)
                        continue;
                    dst = (py * w + px) * 4;
                    buffer[dst + 0] = alpha;
                    buffer[dst + 1] = r;
                    buffer[dst + 2] = g;
                    buffer[dst + 3] = b;
                }
            }
            stbtt_FreeBitmap(glyph, NULL);
        }
        x += (int)(advance * scale);
    }

    free(ttf);
    *out = buffer;
    *width = w;
    *height = h;
    return 0;
}

static void fill_chn_attr(const struct overlay_item *item, RGN_CHN_ATTR_S *attr)
{
    memset(attr, 0, sizeof(*attr));
    attr->bShow = item->enabled ? RK_TRUE : RK_FALSE;
    attr->enType = OVERLAY_RGN;
    /* The RV1106 RGN requires even coordinates. Normalize as a last defense
     * for positions edited through MTP; the web editor also snaps to even. */
    attr->unChnAttr.stOverlayChn.stPoint.s32X = item->x & ~1;
    attr->unChnAttr.stOverlayChn.stPoint.s32Y = item->y & ~1;
    attr->unChnAttr.stOverlayChn.u32FgAlpha = 255;
    attr->unChnAttr.stOverlayChn.u32BgAlpha = 0;
}

static void fill_venc_chn(MPP_CHN_S *chn)
{
    memset(chn, 0, sizeof(*chn));
    chn->enModId = RK_ID_VENC;
    chn->s32DevId = 0;
    chn->s32ChnId = 0;
}

static int attach_item(struct overlay_item *item, int index)
{
    RGN_ATTR_S rgn_attr;
    RGN_CHN_ATTR_S chn_attr;
    MPP_CHN_S chn;
    BITMAP_S bitmap;
    uint8_t *buffer = NULL;
    int width = 0;
    int height = 0;

    if (item->is_text) {
        if (render_text(item->text, item->font, item->font_size,
                        item->font_color, &buffer, &width, &height) != 0)
            return -1;
    } else {
        if (load_bmp(item->image, &buffer, &width, &height) != 0)
            return -1;
    }

    memset(&rgn_attr, 0, sizeof(rgn_attr));
    rgn_attr.enType = OVERLAY_RGN;
    rgn_attr.unAttr.stOverlay.enPixelFmt = RK_FMT_ARGB8888;
    rgn_attr.unAttr.stOverlay.u32CanvasNum = 2;
    rgn_attr.unAttr.stOverlay.stSize.u32Width = width;
    rgn_attr.unAttr.stOverlay.stSize.u32Height = height;

    if (RK_MPI_RGN_Create(item->handle, &rgn_attr) != RK_SUCCESS) {
        LOGW("overlay: RGN_Create(%d) falló\n", item->handle);
        free(buffer);
        return -1;
    }

    fill_venc_chn(&chn);
    fill_chn_attr(item, &chn_attr);
    chn_attr.unChnAttr.stOverlayChn.u32Layer = (RK_U32)index;

    if (RK_MPI_RGN_AttachToChn(item->handle, &chn, &chn_attr) != RK_SUCCESS) {
        LOGW("overlay: AttachToChn(%d) falló\n", item->handle);
        RK_MPI_RGN_Destroy(item->handle);
        free(buffer);
        return -1;
    }

    memset(&bitmap, 0, sizeof(bitmap));
    bitmap.enPixelFormat = RK_FMT_ARGB8888;
    bitmap.u32Width = width;
    bitmap.u32Height = height;
    bitmap.pData = buffer;

    if (RK_MPI_RGN_SetBitMap(item->handle, &bitmap) != RK_SUCCESS) {
        LOGW("overlay: SetBitMap(%d) falló\n", item->handle);
        RK_MPI_RGN_DetachFromChn(item->handle, &chn);
        RK_MPI_RGN_Destroy(item->handle);
        free(buffer);
        return -1;
    }

    item->buffer = buffer;
    item->width = width;
    item->height = height;
    item->attached = 1;
    return 0;
}

static void detach_item(struct overlay_item *item)
{
    MPP_CHN_S chn;

    if (!item->attached)
        return;
    fill_venc_chn(&chn);
    (void)RK_MPI_RGN_DetachFromChn(item->handle, &chn);
    (void)RK_MPI_RGN_Destroy(item->handle);
    free(item->buffer);
    item->buffer = NULL;
    item->attached = 0;
}

static int replace_image(struct overlay_item *item, int index)
{
    uint8_t *buffer = NULL;
    int width = 0;
    int height = 0;
    BITMAP_S bitmap;

    if (item->is_text) {
        if (render_text(item->text, item->font, item->font_size,
                        item->font_color, &buffer, &width, &height) != 0)
            return -1;
    } else {
        if (load_bmp(item->image, &buffer, &width, &height) != 0)
            return -1;
    }

    if (item->attached && width == item->width && height == item->height) {
        memset(&bitmap, 0, sizeof(bitmap));
        bitmap.enPixelFormat = RK_FMT_ARGB8888;
        bitmap.u32Width = width;
        bitmap.u32Height = height;
        bitmap.pData = buffer;
        if (RK_MPI_RGN_SetBitMap(item->handle, &bitmap) != RK_SUCCESS) {
            free(buffer);
            return -1;
        }
        free(item->buffer);
        item->buffer = buffer;
        return 0;
    }

    free(buffer);
    detach_item(item);
    return attach_item(item, index);
}

static char *trim(char *s)
{
    char *end;

    while (*s == ' ' || *s == '\t' || *s == '\r' || *s == '\n')
        s++;
    end = s + strlen(s);
    while (end > s && (end[-1] == ' ' || end[-1] == '\t' ||
                       end[-1] == '\r' || end[-1] == '\n'))
        *--end = '\0';
    return s;
}

static int parse_config(const char *path, struct overlay_item *items)
{
    FILE *f;
    char line[512];
    struct overlay_item *current = NULL;
    int changed = 0;

    f = fopen(path, "r");
    if (!f) {
        LOGW("overlay: no se pudo leer %s\n", path);
        return -1;
    }

    while (fgets(line, sizeof(line), f)) {
        char *s = trim(line);
        char *eq;
        char *key;
        char *value;
        int i;

        if (*s == '\0' || *s == '#' || *s == ';')
            continue;
        if (*s == '[') {
            char *end = strchr(s, ']');
            if (!end)
                continue;
            *end = '\0';
            s = trim(s + 1);
            current = NULL;
            for (i = 0; i < OVERLAY_COUNT; i++) {
                if (strcmp(s, items[i].section) == 0) {
                    current = &items[i];
                    break;
                }
            }
            continue;
        }
        if (!current)
            continue;
        eq = strchr(s, '=');
        if (!eq)
            continue;
        *eq = '\0';
        key = trim(s);
        value = trim(eq + 1);

        if (strcmp(key, "enabled") == 0) {
            current->enabled = atoi(value) ? 1 : 0;
            changed = 1;
        } else if (strcmp(key, "x") == 0) {
            int x = atoi(value);
            current->x = (x < 0 ? 0 : x) & ~1;
            changed = 1;
        } else if (strcmp(key, "y") == 0) {
            int y = atoi(value);
            current->y = (y < 0 ? 0 : y) & ~1;
            changed = 1;
        } else if (strcmp(key, "image") == 0 && value[0]) {
            snprintf(current->image, sizeof(current->image), "%s", value);
            current->is_text = 0;
            changed = 1;
        } else if (strcmp(key, "text") == 0) {
            snprintf(current->text, sizeof(current->text), "%s", value);
            current->is_text = 1;
            LOGI("overlay: config text '%s'\n", current->text);
            changed = 1;
        } else if (strcmp(key, "size") == 0) {
            current->font_size = atoi(value);
            if (current->font_size < 8)
                current->font_size = 8;
            changed = 1;
        } else if (strcmp(key, "color") == 0) {
            current->font_color = (uint32_t)strtoul(value, NULL, 16);
            changed = 1;
        } else if (strcmp(key, "font") == 0 && value[0]) {
            snprintf(current->font, sizeof(current->font), "%s", value);
            changed = 1;
        }
    }

    fclose(f);
    return changed ? 0 : -1;
}

static int copy_file(const char *src, const char *dst)
{
    FILE *in;
    FILE *out;
    char buf[4096];
    size_t n;

    in = fopen(src, "rb");
    if (!in)
        return -1;
    out = fopen(dst, "wb");
    if (!out) {
        fclose(in);
        return -1;
    }
    while ((n = fread(buf, 1, sizeof(buf), in)) > 0) {
        if (fwrite(buf, 1, n, out) != n) {
            fclose(in);
            fclose(out);
            return -1;
        }
    }
    fclose(in);
    fclose(out);
    return 0;
}

static void ensure_config(void)
{
    struct stat st;

    if (mkdir(OVERLAY_CONFIG_DIR, 0755) != 0 && errno != EEXIST)
        LOGW("overlay: no se pudo crear %s\n", OVERLAY_CONFIG_DIR);
    if (stat(OVERLAY_CONFIG, &st) != 0)
        (void)copy_file(OVERLAY_DEFAULT_CONFIG, OVERLAY_CONFIG);
}

static int apply_item(struct overlay_item *item, int index)
{
    RGN_CHN_ATTR_S chn_attr;
    MPP_CHN_S chn;

    if (!item->attached)
        return 0;
    fill_venc_chn(&chn);
    fill_chn_attr(item, &chn_attr);
    chn_attr.unChnAttr.stOverlayChn.u32Layer = (RK_U32)index;
    if (RK_MPI_RGN_SetDisplayAttr(item->handle, &chn, &chn_attr) != RK_SUCCESS) {
        LOGW("overlay: SetDisplayAttr(%d) falló\n", item->handle);
        return -1;
    }
    return 0;
}

void overlay_refresh(void)
{
    int i;

    if (!g_inited)
        return;
    for (i = 0; i < OVERLAY_COUNT; i++)
        (void)apply_item(&g_items[i], i);
}

int overlay_init(void)
{
    int i;
    int attached = 0;

    if (g_inited)
        return 0;

    ensure_config();
    for (i = 0; i < OVERLAY_COUNT; i++) {
        item_defaults(&g_items[i]);
        g_items[i].handle = OVERLAY_HANDLE_BASE + i;
    }

    /* Read the live configuration before creating regions. */
    (void)parse_config(OVERLAY_CONFIG, g_items);

    for (i = 0; i < OVERLAY_COUNT; i++) {
        if (attach_item(&g_items[i], i) == 0) {
            attached++;
            if (g_items[i].is_text)
                LOGI("overlay: text '%s' %dx%d en (%d,%d) enabled=%d\n",
                     g_items[i].text, g_items[i].width, g_items[i].height,
                     g_items[i].x, g_items[i].y, g_items[i].enabled);
            else
                LOGI("overlay: %s %dx%d en (%d,%d) enabled=%d\n",
                     g_items[i].image, g_items[i].width, g_items[i].height,
                     g_items[i].x, g_items[i].y, g_items[i].enabled);
        }
    }

    g_inited = attached > 0;
    {
        struct stat st;

        if (stat(OVERLAY_CONFIG, &st) == 0) {
            g_config_mtime = st.st_mtime;
            g_config_size = st.st_size;
        }
    }
    return attached;
}

void overlay_deinit(void)
{
    int i;

    if (!g_inited)
        return;

    for (i = 0; i < OVERLAY_COUNT; i++)
        detach_item(&g_items[i]);
    g_inited = 0;
    g_suspended = 0;
}

void overlay_poll(void)
{
    struct stat st;
    struct overlay_item parsed[OVERLAY_COUNT];
    int i;
    int changed = 0;

    if (!g_inited || g_suspended)
        return;

    ensure_config();
    if (stat(OVERLAY_CONFIG, &st) != 0)
        return;
    if (st.st_mtime == g_config_mtime && st.st_size == g_config_size)
        return;

    g_config_mtime = st.st_mtime;
    g_config_size = st.st_size;

    memcpy(parsed, g_items, sizeof(parsed));
    if (parse_config(OVERLAY_CONFIG, parsed) != 0) {
        LOGW("overlay: configuración inválida; se conserva la anterior\n");
        return;
    }

    for (i = 0; i < OVERLAY_COUNT; i++) {
        if (strcmp(parsed[i].image, g_items[i].image) != 0 ||
            strcmp(parsed[i].text, g_items[i].text) != 0 ||
            strcmp(parsed[i].font, g_items[i].font) != 0 ||
            parsed[i].font_size != g_items[i].font_size ||
            parsed[i].font_color != g_items[i].font_color ||
            parsed[i].is_text != g_items[i].is_text) {
            struct overlay_item tmp = g_items[i];

            snprintf(tmp.image, sizeof(tmp.image), "%s", parsed[i].image);
            snprintf(tmp.text, sizeof(tmp.text), "%s", parsed[i].text);
            snprintf(tmp.font, sizeof(tmp.font), "%s", parsed[i].font);
            tmp.font_size = parsed[i].font_size;
            tmp.font_color = parsed[i].font_color;
            tmp.is_text = parsed[i].is_text;
            if (replace_image(&tmp, i) == 0) {
                g_items[i].buffer = tmp.buffer;
                g_items[i].width = tmp.width;
                g_items[i].height = tmp.height;
                g_items[i].attached = tmp.attached;
                snprintf(g_items[i].image, sizeof(g_items[i].image), "%s",
                         parsed[i].image);
                snprintf(g_items[i].text, sizeof(g_items[i].text), "%s",
                         parsed[i].text);
                snprintf(g_items[i].font, sizeof(g_items[i].font), "%s",
                         parsed[i].font);
                g_items[i].font_size = parsed[i].font_size;
                g_items[i].font_color = parsed[i].font_color;
                g_items[i].is_text = parsed[i].is_text;
                changed = 1;
            } else {
                LOGW("overlay: no se pudo cargar %s; se conserva la anterior\n",
                     parsed[i].image);
            }
        }
        if (parsed[i].x != g_items[i].x || parsed[i].y != g_items[i].y ||
            parsed[i].enabled != g_items[i].enabled) {
            g_items[i].x = parsed[i].x;
            g_items[i].y = parsed[i].y;
            g_items[i].enabled = parsed[i].enabled;
            apply_item(&g_items[i], i);
            changed = 1;
        }
    }

    if (changed) {
        LOGI("overlay: configuración aplicada en vivo\n");
        for (i = 0; i < OVERLAY_COUNT; i++)
            LOGI("overlay: %s (%d,%d) enabled=%d text='%s' size=%d\n",
                 g_items[i].section, g_items[i].x, g_items[i].y,
                 g_items[i].enabled, g_items[i].text, g_items[i].font_size);
    }
}
