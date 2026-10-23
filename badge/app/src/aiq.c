/*
 * File: app/src/aiq.c
 * Purpose: Dynamic librkaiq loader and AIQ lifecycle, including Thunder Boot IQ mapping and fast AE/AWB handoff.
 */

#include "aiq.h"
#include "log.h"

#include <dlfcn.h>
#include <fcntl.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <time.h>
#include <unistd.h>

#include "rk_aiq_user_api2_sysctl.h"

struct aiq_handle {
    void *dl;
    rk_aiq_sys_ctx_t *ctx;
    void *iq_map;
    size_t iq_map_len;
    void *iq_buf;

    XCamReturn (*enumStaticMetas)(int, rk_aiq_static_info_t *);
    XCamReturn (*preInit_scene)(const char *, const char *, const char *);
    XCamReturn (*preInit_iq_addr)(const char *, void *, size_t);
    XCamReturn (*preInit_tb_info)(const char *, const rk_aiq_tb_info_t *);
    rk_aiq_sys_ctx_t *(*init)(const char *, const char *, rk_aiq_error_cb,
                              rk_aiq_metas_cb);
    XCamReturn (*prepare)(const rk_aiq_sys_ctx_t *, uint32_t, uint32_t,
                          rk_aiq_working_mode_t);
    XCamReturn (*start)(const rk_aiq_sys_ctx_t *);
    XCamReturn (*stop)(const rk_aiq_sys_ctx_t *, bool);
    void (*deinit)(rk_aiq_sys_ctx_t *);
};

static XCamReturn aiq_err_cb(rk_aiq_err_msg_t *msg)
{
    if (msg)
        LOGW("aiq: error code %u\n", msg->err_code);
    return XCAM_RETURN_NO_ERROR;
}

static XCamReturn aiq_sof_cb(rk_aiq_metas_t *meta)
{
    (void)meta;
    return XCAM_RETURN_NO_ERROR;
}

static void *sym(void *dl, const char *name)
{
    void *p = dlsym(dl, name);
    if (!p)
        LOGE("aiq: falta símbolo %s: %s\n", name, dlerror());
    return p;
}

static long now_us(void)
{
    struct timespec ts = { 0, 0 };
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (long)(ts.tv_sec * 1000000 + ts.tv_nsec / 1000);
}

static unsigned long cmdline_hex(const char *key)
{
    FILE *f;
    char buf[2048];
    char *p;
    size_t len;

    if (!key)
        return 0;

    f = fopen("/proc/cmdline", "r");
    if (!f)
        return 0;
    len = fread(buf, 1, sizeof(buf) - 1, f);
    fclose(f);
    buf[len] = '\0';

    p = strstr(buf, key);
    if (!p)
        return 0;
    p += strlen(key);
    if (*p == '=')
        p++;
    return strtoul(p, NULL, 16);
}

/*
 * Thunder Boot writes the IQ binary into the meta partition. The reference
 * fastboot_server passes that exact mapping to preInit_iq_addr(), not a copy
 * read from /etc/iqfiles. Map the physical range described by the kernel
 * command line so AIQ sees the same bytes and lifetime.
 */
static void *map_iq_bin(size_t *len, void **map_base, size_t *map_len)
{
    unsigned long addr = cmdline_hex("rk_iqbin_addr");
    unsigned long size = cmdline_hex("rk_iqbin_size");
    unsigned long base;
    unsigned long off;
    size_t maplen;
    long page;
    int fd;
    void *m;

    *len = 0;
    *map_base = NULL;
    *map_len = 0;
    if (!addr || !size)
        return NULL;

    page = sysconf(_SC_PAGESIZE);
    if (page <= 0)
        return NULL;
    base = addr & ~((unsigned long)page - 1);
    off = addr - base;
    maplen = (size_t)off + size;

    fd = open("/dev/mem", O_RDWR | O_SYNC);
    if (fd < 0)
        return NULL;
    m = mmap(NULL, maplen, PROT_READ | PROT_WRITE, MAP_SHARED, fd,
             (off_t)base);
    close(fd);
    if (m == MAP_FAILED)
        return NULL;

    *map_base = m;
    *map_len = maplen;
    *len = size;
    return (char *)m + off;
}

static void *load_iq_file(size_t *len)
{
    static const char *path =
        "/etc/iqfiles/sc3336_CMK-OT2119-PC1_30IRC-F16.bin";
    FILE *f;
    long size;
    void *buf;

    *len = 0;
    f = fopen(path, "rb");
    if (!f)
        return NULL;
    if (fseek(f, 0, SEEK_END) != 0 || (size = ftell(f)) <= 0 ||
        fseek(f, 0, SEEK_SET) != 0) {
        fclose(f);
        return NULL;
    }
    buf = malloc((size_t)size);
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
    *len = (size_t)size;
    return buf;
}

int aiq_init(aiq_handle_t **out, badge_meta_t *meta, int color_mode,
             int cam_hdr, int fast_handoff)
{
    aiq_handle_t *h;
    rk_aiq_static_info_t info;
    const char *sensor;
    long t0;
    int ret = -1;

    if (!out || !meta)
        return -1;
    *out = NULL;
    (void)meta;

    h = calloc(1, sizeof(*h));
    if (!h)
        return -1;

    h->dl = dlopen("/oem/usr/lib/librkaiq.so", RTLD_LAZY);
    if (!h->dl)
        h->dl = dlopen("/usr/lib/librkaiq.so", RTLD_LAZY);
    if (!h->dl) {
        LOGE("aiq: no se pudo cargar librkaiq.so: %s\n", dlerror());
        goto fail;
    }

    h->enumStaticMetas = sym(h->dl, "rk_aiq_uapi2_sysctl_enumStaticMetas");
    h->preInit_scene = sym(h->dl, "rk_aiq_uapi2_sysctl_preInit_scene");
    h->preInit_iq_addr = sym(h->dl, "rk_aiq_uapi2_sysctl_preInit_iq_addr");
    h->preInit_tb_info = sym(h->dl, "rk_aiq_uapi2_sysctl_preInit_tb_info");
    h->init = sym(h->dl, "rk_aiq_uapi2_sysctl_init");
    h->prepare = sym(h->dl, "rk_aiq_uapi2_sysctl_prepare");
    h->start = sym(h->dl, "rk_aiq_uapi2_sysctl_start");
    h->stop = sym(h->dl, "rk_aiq_uapi2_sysctl_stop");
    h->deinit = sym(h->dl, "rk_aiq_uapi2_sysctl_deinit");

    if (!h->enumStaticMetas || !h->preInit_scene || !h->preInit_iq_addr ||
        !h->preInit_tb_info || !h->init || !h->prepare || !h->start ||
        !h->stop || !h->deinit)
        goto fail;

    memset(&info, 0, sizeof(info));
    if (h->enumStaticMetas(0, &info) != XCAM_RETURN_NO_ERROR) {
        LOGE("aiq: enumStaticMetas falló\n");
        goto fail;
    }
    sensor = info.sensor_info.sensor_name;
    LOGI("aiq: sensor '%s'\n", sensor);

    if (h->preInit_scene(sensor, "normal",
                         color_mode ? "night" : "day") < 0) {
        LOGE("aiq: preInit_scene falló\n");
        goto fail;
    }

    {
        size_t iq_len = 0;
        void *iq = map_iq_bin(&iq_len, &h->iq_map, &h->iq_map_len);

        if (!iq) {
            iq = load_iq_file(&iq_len);
            h->iq_buf = iq;
        }
        if (iq && iq_len) {
            if (h->preInit_iq_addr(sensor, iq, iq_len) < 0)
                LOGE("aiq: preInit_iq_addr falló (%zu bytes)\n", iq_len);
            else
                LOGI("aiq: IQ bin %zu bytes\n", iq_len);
        } else {
            LOGW("aiq: sin IQ bin para preInit_iq_addr\n");
        }
    }

    if (fast_handoff) {
        rk_aiq_tb_info_t tb;

        memset(&tb, 0, sizeof(tb));
        tb.magic = sizeof(tb) - 2;
        tb.is_pre_aiq = true;
        tb.prd_type = RK_AIQ_PRD_TYPE_TB_BATIPC;
        tb.is_start_once = false;
        tb.iq_bin_mode = RK_AIQ_META_FULL_IQ_BIN;
        tb.rtt_share_addr = NULL;
        if (h->preInit_tb_info(sensor, &tb) < 0)
            LOGE("aiq: preInit_tb_info falló\n");
        else
            LOGI("aiq: fast AE/AWB handoff activo\n");
    } else {
        LOGI("aiq: fast handoff desactivado; AIQ normal\n");
    }

    {
        char hdr_str[16];
        snprintf(hdr_str, sizeof(hdr_str), "%d",
                 cam_hdr == 5 ? RK_AIQ_WORKING_MODE_ISP_HDR2
                              : RK_AIQ_WORKING_MODE_NORMAL);
        setenv("HDR_MODE", hdr_str, 1);
    }

    t0 = now_us();
    h->ctx = h->init(sensor, "/etc/iqfiles/", aiq_err_cb, aiq_sof_cb);
    if (!h->ctx) {
        LOGE("aiq: init falló\n");
        goto fail;
    }

    if (h->prepare(h->ctx, 0, 0,
                   cam_hdr == 5 ? RK_AIQ_WORKING_MODE_ISP_HDR2
                                : RK_AIQ_WORKING_MODE_NORMAL) !=
        XCAM_RETURN_NO_ERROR) {
        LOGE("aiq: prepare falló\n");
        goto fail;
    }

    if (h->start(h->ctx) != XCAM_RETURN_NO_ERROR) {
        LOGE("aiq: start falló\n");
        goto fail;
    }

    LOGI("aiq: listo en %ld us\n", now_us() - t0);
    *out = h;
    return 0;

fail:
    if (h->ctx && h->stop)
        h->stop(h->ctx, false);
    if (h->ctx && h->deinit)
        h->deinit(h->ctx);
    if (h->dl)
        dlclose(h->dl);
    if (h->iq_map)
        munmap(h->iq_map, h->iq_map_len);
    free(h->iq_buf);
    free(h);
    (void)ret;
    return -1;
}

int aiq_restart(aiq_handle_t *h)
{
    if (!h || !h->ctx || !h->stop || !h->start)
        return -1;
    if (h->stop(h->ctx, false) != XCAM_RETURN_NO_ERROR)
        return -1;
    if (h->start(h->ctx) != XCAM_RETURN_NO_ERROR)
        return -1;
    LOGI("aiq: reinicio runtime OK\n");
    return 0;
}

void aiq_deinit(aiq_handle_t *h)
{
    if (!h)
        return;
    if (h->ctx && h->stop)
        h->stop(h->ctx, false);
    if (h->ctx && h->deinit)
        h->deinit(h->ctx);
    if (h->dl)
        dlclose(h->dl);
    if (h->iq_map)
        munmap(h->iq_map, h->iq_map_len);
    free(h->iq_buf);
    free(h);
}

