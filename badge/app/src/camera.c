/*
 * File: app/src/camera.c
 * Purpose: Rockit VI/VENC JPEG pipeline: initialization, warmup, latest-frame polling, and capture.
 */

#include "camera.h"
#include "log.h"
#include "overlay.h"
#include "photo_path.h"

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include "rk_mpi_mb.h"
#include "rk_mpi_sys.h"
#include "rk_mpi_venc.h"
#include "rk_mpi_vi.h"
#include "rk_type.h"

#define VENC_CHN 0
#define PREVIEW_VENC_CHN 1
#define VI_DEV 0
#define VI_CHN 0

struct camera_ctx {
    int width;
    int height;
    int quality;
    int sys_init;
    int venc_created;
    int preview_venc_created;
    int vi_dev_enabled;
    int vi_chn_enabled;
    int vi_pipe_started;
    VIDEO_FRAME_INFO_S held;
    int has_held;
    time_t last_frame_time;
};

static int write_atomic(const char *path, const void *data, size_t len)
{
    char tmp[512];
    int fd;
    const unsigned char *p = data;
    size_t left = len;

    if (strncmp(path, "/tmp/", 5) == 0) {
        const char *base = strrchr(path, '/');
        int n;
        base = base ? base + 1 : path;
        n = snprintf(tmp, sizeof(tmp), "/tmp/%s.tmp", base);
        if (n < 0 || (size_t)n >= sizeof(tmp))
            return -1;
    } else if (photo_path_make_temp(path, PHOTO_STAGING_DIR, tmp,
                                    sizeof(tmp)) != 0) {
        return -1;
    }

    fd = open(tmp, O_CREAT | O_WRONLY | O_TRUNC | O_CLOEXEC, 0644);
    if (fd < 0) {
        LOGE("foto: no se pudo crear %s: %s\n", tmp, strerror(errno));
        return -1;
    }

    while (left > 0) {
        ssize_t n = write(fd, p, left);
        if (n < 0) {
            if (errno == EINTR)
                continue;
            LOGE("foto: write falló: %s\n", strerror(errno));
            close(fd);
            unlink(tmp);
            return -1;
        }
        p += n;
        left -= (size_t)n;
    }

    /* Hot path: rely on the kernel/UBIFS writeback. fsync() added ~70-130 ms
     * per photo; the file is already visible atomically after rename(). */
    close(fd);

    if (rename(tmp, path) != 0) {
        LOGE("foto: rename falló: %s\n", strerror(errno));
        unlink(tmp);
        return -1;
    }
    return 0;
}

static int venc_init_ch(camera_ctx_t *ctx, int chn, int quality)
{
    VENC_CHN_ATTR_S attr;
    VENC_JPEG_PARAM_S jpeg;

    memset(&attr, 0, sizeof(attr));
    attr.stVencAttr.enType = RK_VIDEO_ID_JPEG;
    attr.stVencAttr.enPixelFormat = RK_FMT_YUV420SP;
    attr.stVencAttr.u32PicWidth = (RK_U32)ctx->width;
    attr.stVencAttr.u32PicHeight = (RK_U32)ctx->height;
    attr.stVencAttr.u32VirWidth = (RK_U32)ctx->width;
    attr.stVencAttr.u32VirHeight = (RK_U32)ctx->height;
    attr.stVencAttr.u32StreamBufCnt = 2;
    attr.stVencAttr.u32BufSize = (RK_U32)(ctx->width * ctx->height * 3 / 2);
    attr.stVencAttr.enMirror = MIRROR_NONE;
    attr.stVencAttr.stAttrJpege.bSupportDCF = RK_FALSE;
    attr.stVencAttr.stAttrJpege.stMPFCfg.u8LargeThumbNailNum = 0;
    attr.stVencAttr.stAttrJpege.enReceiveMode = VENC_PIC_RECEIVE_SINGLE;

    if (RK_MPI_VENC_CreateChn(chn, &attr) != RK_SUCCESS) {
        LOGE("venc: CreateChn(%d) falló\n", chn);
        return -1;
    }

    memset(&jpeg, 0, sizeof(jpeg));
    jpeg.u32Qfactor = (RK_U32)quality;
    RK_MPI_VENC_SetJpegParam(chn, &jpeg);

    {
        VENC_RECV_PIC_PARAM_S recv;
        memset(&recv, 0, sizeof(recv));
        recv.s32RecvPicNum = -1;
        (void)RK_MPI_VENC_StartRecvFrame(chn, &recv);
    }
    return 0;
}

static int venc_init(camera_ctx_t *ctx)
{
    if (venc_init_ch(ctx, VENC_CHN, ctx->quality) != 0)
        return -1;
    ctx->venc_created = 1;

    /*
     * Dedicated raw-preview channel: no RGN attached here, so the web canvas
     * can draw the editable overlay without detaching the final-photo RGN.
     */
    if (venc_init_ch(ctx, PREVIEW_VENC_CHN, 55) == 0)
        ctx->preview_venc_created = 1;
    else
        LOGW("venc: preview channel no disponible\n");
    return 0;
}

static int vi_dev_init(camera_ctx_t *ctx)
{
    VI_DEV_ATTR_S dev_attr;
    VI_DEV_BIND_PIPE_S bind_pipe;
    int ret;

    memset(&dev_attr, 0, sizeof(dev_attr));
    memset(&bind_pipe, 0, sizeof(bind_pipe));

    ret = RK_MPI_VI_GetDevAttr(VI_DEV, &dev_attr);
    if (ret == RK_ERR_VI_NOT_CONFIG) {
        if (RK_MPI_VI_SetDevAttr(VI_DEV, &dev_attr) != RK_SUCCESS) {
            LOGE("vi: SetDevAttr falló\n");
            return -1;
        }
    }

    ret = RK_MPI_VI_GetDevIsEnable(VI_DEV);
    if (ret != RK_SUCCESS) {
        if (RK_MPI_VI_EnableDev(VI_DEV) != RK_SUCCESS) {
            LOGE("vi: EnableDev falló\n");
            return -1;
        }
        ctx->vi_dev_enabled = 1;

        bind_pipe.u32Num = 1;
        bind_pipe.PipeId[0] = VI_DEV;
        bind_pipe.bUserStartPipe[0] = RK_FALSE;
        if (RK_MPI_VI_SetDevBindPipe(VI_DEV, &bind_pipe) != RK_SUCCESS) {
            LOGE("vi: SetDevBindPipe falló\n");
            return -1;
        }
    } else {
        ctx->vi_dev_enabled = 1;
    }
    return 0;
}

static int vi_chn_init(camera_ctx_t *ctx)
{
    VI_CHN_ATTR_S attr;
    int ret;

    memset(&attr, 0, sizeof(attr));
    attr.stIspOpt.u32BufCount = 2;
    attr.stIspOpt.enMemoryType = VI_V4L2_MEMORY_TYPE_DMABUF;
    attr.stSize.u32Width = (RK_U32)ctx->width;
    attr.stSize.u32Height = (RK_U32)ctx->height;
    attr.enPixelFormat = RK_FMT_YUV420SP;
    attr.enCompressMode = COMPRESS_MODE_NONE;
    attr.u32Depth = 2;

    ret = RK_MPI_VI_SetChnAttr(VI_DEV, VI_CHN, &attr);
    if (ret != RK_SUCCESS) {
        LOGE("vi: SetChnAttr falló (0x%x)\n", ret);
        return -1;
    }

    ret = RK_MPI_VI_EnableChn(VI_DEV, VI_CHN);
    if (ret != RK_SUCCESS) {
        LOGE("vi: EnableChn falló (0x%x)\n", ret);
        return -1;
    }
    ctx->vi_chn_enabled = 1;
    return 0;
}

int camera_init(camera_ctx_t **out, int width, int height, int quality)
{
    camera_ctx_t *ctx;

    if (!out || width < 16 || height < 16)
        return -1;
    *out = NULL;

    ctx = calloc(1, sizeof(*ctx));
    if (!ctx)
        return -1;
    ctx->width = width;
    ctx->height = height;
    ctx->quality = quality;

    if (RK_MPI_SYS_Init() != RK_SUCCESS) {
        LOGE("rockit: SYS_Init falló\n");
        goto fail;
    }
    ctx->sys_init = 1;

    /* Match fastboot_server: VENC first, then VI, no SYS_Bind. */
    if (venc_init(ctx) != 0)
        goto fail;
    if (vi_dev_init(ctx) != 0)
        goto fail;
    if (vi_chn_init(ctx) != 0)
        goto fail;

    /*
     * Resume the Thunder Boot VI pipe after AIQ has queued its params. This
     * must happen after preInit_tb_info/start: doing it before AIQ makes the
     * ISP wait for params and time out.
     */
    if (RK_MPI_VI_StartPipe(VI_DEV) == RK_SUCCESS)
        ctx->vi_pipe_started = 1;
    else
        LOGW("vi: StartPipe no aplicado\n");

    (void)overlay_init();

    LOGI("camera: %dx%d JPEG q=%d lista (VI mainpath -> VENC send)\n",
         width, height, quality);
    *out = ctx;
    return 0;

fail:
    camera_deinit(ctx);
    return -1;
}

int camera_poll(camera_ctx_t *ctx)
{
    VIDEO_FRAME_INFO_S frame;

    if (!ctx)
        return 0;

    memset(&frame, 0, sizeof(frame));
    if (RK_MPI_VI_GetChnFrame(VI_DEV, VI_CHN, &frame, 20) != RK_SUCCESS)
        return 0;

    if (ctx->has_held)
        RK_MPI_VI_ReleaseChnFrame(VI_DEV, VI_CHN, &ctx->held);
    ctx->held = frame;
    ctx->has_held = 1;
    ctx->last_frame_time = time(NULL);
    return 1;
}

long camera_last_frame_time(const camera_ctx_t *ctx)
{
    return ctx ? (long)ctx->last_frame_time : 0;
}

int camera_warmup(camera_ctx_t *ctx, int timeout_ms)
{
    int wait;

    if (!ctx)
        return -1;

    wait = timeout_ms / 5 + 20;
    while (!ctx->has_held && wait-- > 0) {
        (void)camera_poll(ctx);
        if (!ctx->has_held)
            usleep(5 * 1000);
    }
    if (!ctx->has_held) {
        LOGW("warmup: VI sin frames\n");
        return -1;
    }

    /* Let AE/AWB converge on live frames before declaring the camera ready. */
    wait = 100;
    while (wait-- > 0) {
        (void)camera_poll(ctx);
        usleep(20 * 1000);
    }
    return 0;
}

int camera_capture(camera_ctx_t *ctx, const char *path, int timeout_ms, int retries)
{
    VENC_RECV_PIC_PARAM_S recv;
    VENC_STREAM_S stream;
    VIDEO_FRAME_INFO_S frame;
    int attempt;
    int ret = -1;

    if (!ctx || !path)
        return -1;

    memset(&stream, 0, sizeof(stream));
    stream.pstPack = calloc(1, sizeof(VENC_PACK_S));
    if (!stream.pstPack)
        return -1;

    for (attempt = 0; attempt <= retries; attempt++) {
        int wait = timeout_ms / 5 + 20;
        while (!ctx->has_held && wait-- > 0) {
            (void)camera_poll(ctx);
            if (!ctx->has_held)
                usleep(5 * 1000);
        }
        if (!ctx->has_held) {
            LOGW("captura: VI sin frame nuevo (intento %d)\n", attempt + 1);
            continue;
        }

        frame = ctx->held;
        ctx->has_held = 0;

        memset(&recv, 0, sizeof(recv));
        recv.s32RecvPicNum = 1;
        (void)RK_MPI_VENC_StartRecvFrame(VENC_CHN, &recv);
        if (RK_MPI_VENC_SendFrame(VENC_CHN, &frame, timeout_ms) != RK_SUCCESS) {
            LOGE("captura: VENC_SendFrame falló (intento %d)\n", attempt + 1);
            RK_MPI_VI_ReleaseChnFrame(VI_DEV, VI_CHN, &frame);
            usleep(20 * 1000);
            continue;
        }
        RK_MPI_VI_ReleaseChnFrame(VI_DEV, VI_CHN, &frame);

        if (RK_MPI_VENC_GetStream(VENC_CHN, &stream, timeout_ms) == RK_SUCCESS) {
            void *data = RK_MPI_MB_Handle2VirAddr(stream.pstPack->pMbBlk);
            size_t len = stream.pstPack->u32Len;

            if (data && len > 0 && write_atomic(path, data, len) == 0) {
                if (strncmp(path, "/tmp/", 5) == 0)
                    LOGD("preview: %s (%zu bytes, seq=%u)\n", path, len,
                         stream.u32Seq);
                else
                    LOGI("captura: %s (%zu bytes, seq=%u)\n", path, len,
                         stream.u32Seq);
                ret = 0;
            } else {
                LOGE("captura: no se pudo escribir %s\n", path);
            }
            RK_MPI_VENC_ReleaseStream(VENC_CHN, &stream);
            if (ret == 0)
                break;
        } else {
            LOGW("captura: GetStream timeout (intento %d)\n", attempt + 1);
        }
        usleep(20 * 1000);
    }

    free(stream.pstPack);
    return ret;
}

int camera_preview(camera_ctx_t *ctx)
{
    VENC_RECV_PIC_PARAM_S recv;
    VENC_STREAM_S stream;
    VIDEO_FRAME_INFO_S frame;
    int wait;

    if (!ctx || !ctx->preview_venc_created)
        return -1;

    wait = 1500 / 5 + 20;
    while (!ctx->has_held && wait-- > 0) {
        (void)camera_poll(ctx);
        if (!ctx->has_held)
            usleep(5 * 1000);
    }
    if (!ctx->has_held)
        return -1;

    frame = ctx->held;
    ctx->has_held = 0;

    memset(&recv, 0, sizeof(recv));
    recv.s32RecvPicNum = 1;
    (void)RK_MPI_VENC_StartRecvFrame(PREVIEW_VENC_CHN, &recv);
    if (RK_MPI_VENC_SendFrame(PREVIEW_VENC_CHN, &frame, 1500) != RK_SUCCESS) {
        RK_MPI_VI_ReleaseChnFrame(VI_DEV, VI_CHN, &frame);
        return -1;
    }
    RK_MPI_VI_ReleaseChnFrame(VI_DEV, VI_CHN, &frame);

    memset(&stream, 0, sizeof(stream));
    stream.pstPack = calloc(1, sizeof(VENC_PACK_S));
    if (!stream.pstPack)
        return -1;
    if (RK_MPI_VENC_GetStream(PREVIEW_VENC_CHN, &stream, 1500) != RK_SUCCESS) {
        free(stream.pstPack);
        return -1;
    }
    {
        void *data = RK_MPI_MB_Handle2VirAddr(stream.pstPack->pMbBlk);
        size_t len = stream.pstPack->u32Len;
        int ret = -1;
        if (data && len > 0 &&
            write_atomic("/tmp/badge_preview.jpg", data, len) == 0) {
            LOGD("preview: /tmp/badge_preview.jpg (%zu bytes, seq=%u)\n",
                 len, stream.u32Seq);
            ret = 0;
        }
        RK_MPI_VENC_ReleaseStream(PREVIEW_VENC_CHN, &stream);
        free(stream.pstPack);
        return ret;
    }
}

void camera_deinit(camera_ctx_t *ctx)
{
    if (!ctx)
        return;

    if (ctx->has_held)
        RK_MPI_VI_ReleaseChnFrame(VI_DEV, VI_CHN, &ctx->held);
    if (ctx->vi_chn_enabled)
        RK_MPI_VI_DisableChn(VI_DEV, VI_CHN);
    overlay_deinit();
    if (ctx->preview_venc_created) {
        RK_MPI_VENC_StopRecvFrame(PREVIEW_VENC_CHN);
        RK_MPI_VENC_DestroyChn(PREVIEW_VENC_CHN);
    }
    if (ctx->venc_created) {
        RK_MPI_VENC_StopRecvFrame(VENC_CHN);
        RK_MPI_VENC_DestroyChn(VENC_CHN);
    }
    if (ctx->vi_dev_enabled)
        RK_MPI_VI_DisableDev(VI_DEV);
    if (ctx->sys_init)
        RK_MPI_SYS_Exit();
    free(ctx);
}
