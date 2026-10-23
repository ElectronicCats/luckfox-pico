/*
 * File: app/src/main.c
 * Purpose: badge_photo entry point: startup order, LED feedback, camera pipeline, and button loop.
 */

#include "aiq.h"
#include "buttons.h"
#include "camera.h"
#include "config.h"
#include "led_engine.h"
#include "log.h"
#include "meta.h"
#include "overlay.h"
#include "photo_path.h"
#include "user_led.h"
#include "ws2812.h"

#include "rk_meta_app_param.h"

#include <errno.h>
#include <fcntl.h>
#include <getopt.h>
#include <poll.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#define DEFAULT_CONFIG_FILE "/etc/badge_photo.conf"
#define USER_CONFIG_FILE "/userdata/badge_photo.conf"
#define READY_FILE "/tmp/badge_photo.ready"
#define CAMERA_READY_FILE "/tmp/badge_camera.ready"
#define SYSTEM_READY_FILE "/tmp/badge_system.ready"
#define PREVIEW_REQUEST "/tmp/badge_preview.request"
#define CAPTURE_REQUEST "/tmp/badge_capture.request"
#define LED_SPI_DEV "/dev/spidev0.0"

typedef struct app_ctx {
    app_config_t cfg;
    badge_meta_t meta;
    aiq_handle_t *aiq;
    camera_ctx_t *camera;
    int led_fd;
    int button_fd;
    button_state_t button;
    led_engine_t led;
} app_ctx_t;

static volatile sig_atomic_t g_running = 1;
static volatile sig_atomic_t g_capture_request = 0;
static volatile sig_atomic_t g_camera_fault = 0;
static void on_signal(int sig)
{
    (void)sig;
    g_running = 0;
}

static void on_capture_signal(int sig)
{
    (void)sig;
    g_capture_request = 1;
}

static void on_camera_fault_signal(int sig)
{
    (void)sig;
    g_camera_fault = 1;
}

static void led_set(app_ctx_t *app, uint8_t r, uint8_t g, uint8_t b)
{
    uint8_t rgb[16 * 3];
    int i;

    if (!app->cfg.led || app->led_fd < 0)
        return;
    for (i = 0; i < app->cfg.led_count && i < 16; i++) {
        rgb[i * 3 + 0] = r;
        rgb[i * 3 + 1] = g;
        rgb[i * 3 + 2] = b;
    }
    if (ws2812_write(app->led_fd, rgb, app->cfg.led_count) != 0)
        LOGD("led: write falló: %s\n", strerror(errno));
}

static void led_off(app_ctx_t *app)
{
    led_set(app, 0, 0, 0);
}

static void led_pulse(app_ctx_t *app, uint8_t r, uint8_t g, uint8_t b,
                      unsigned int ms)
{
    led_set(app, r, g, b);
    usleep(ms * 1000);
    led_off(app);
}

static void led_flash(app_ctx_t *app, uint8_t r, uint8_t g, uint8_t b)
{
    led_pulse(app, r, g, b, 40);
}

/* Boot is 100% ready: two green blinks, 1 s apart, then the LED stays off. */
static void led_boot_ready(app_ctx_t *app)
{
    if (!app->cfg.led || app->led_fd < 0)
        return;

    LOGI("led: boot listo, verde x2\n");
    led_pulse(app, 0, 64, 0, 120);
    usleep(1000 * 1000);
    led_pulse(app, 0, 64, 0, 120);
    led_off(app);
}

/* Photo feedback: flash event and two short white blinks, then off. */
static void led_photo_flash(app_ctx_t *app)
{
    if (!app->cfg.led || app->led_fd < 0)
        return;

    LOGI("evento: flash foto\n");
    led_pulse(app, 64, 64, 64, 80);
    usleep(80 * 1000);
    led_pulse(app, 64, 64, 64, 80);
    led_off(app);
}

static uint32_t monotonic_ms(void)
{
    struct timespec ts;

    if (clock_gettime(CLOCK_MONOTONIC, &ts) != 0)
        return 0;
    return (uint32_t)(ts.tv_sec * 1000u + ts.tv_nsec / 1000000u);
}

static void mark_pid_file(const char *path)
{
    char pid[32];
    int fd;
    int len;

    fd = open(path, O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0644);
    if (fd < 0)
        return;
    len = snprintf(pid, sizeof(pid), "%d\n", (int)getpid());
    if (len > 0)
        (void)write(fd, pid, (size_t)len);
    close(fd);
}

static void mark_ready(void)
{
    mark_pid_file(READY_FILE);
}

static void mark_camera_ready(void)
{
    mark_pid_file(CAMERA_READY_FILE);
}

static void mark_system_ready(void)
{
    mark_pid_file(SYSTEM_READY_FILE);
}

static int ensure_output_dir(const char *dir)
{
    int i;

    for (i = 0; i < 20; i++) {
        if (photo_path_ensure_dir(dir) == 0)
            return 0;
        LOGW("storage: esperando %s (%d/20)\n", dir, i + 1);
        usleep(500 * 1000);
    }
    return -1;
}

static int camera_open(app_ctx_t *app)
{
    camera_deinit(app->camera);
    app->camera = NULL;
    if (camera_init(&app->camera, app->cfg.width, app->cfg.height,
                    app->cfg.quality) != 0) {
        app->camera = NULL;
        return -1;
    }
    return 0;
}

static int camera_start(app_ctx_t *app)
{
    int attempt;

    if (camera_open(app) != 0)
        return -1;

    for (attempt = 0; attempt < 2; attempt++) {
        if (camera_warmup(app->camera, 2000) == 0)
            return 0;
        LOGW("camera: warmup falló (%d/2), reinicializando\n", attempt + 1);
        camera_deinit(app->camera);
        app->camera = NULL;
        if (camera_open(app) != 0)
            return -1;
    }
    return -1;
}

static int start_aiq(app_ctx_t *app)
{
    int color_mode;
    int cam_hdr;
    int attempt;

    if (!app->cfg.aiq) {
        LOGI("aiq: deshabilitado; usando fast AE del MCU\n");
        return 0;
    }

    color_mode = app->meta.app ? app->meta.app->color_mode : 0;
    cam_hdr = app->meta.app ? app->meta.app->cam_hdr : 0;
    for (attempt = 0; attempt < 3; attempt++) {
        if (aiq_init(&app->aiq, &app->meta, color_mode, cam_hdr,
                     app->cfg.fast_handoff) == 0)
            return 0;
        LOGW("aiq: reintento %d/3\n", attempt + 1);
        sleep(1);
    }
    LOGE("aiq: no se pudo inicializar\n");
    return -1;
}

static int pipeline_restart(app_ctx_t *app)
{
    camera_deinit(app->camera);
    app->camera = NULL;
    aiq_deinit(app->aiq);
    app->aiq = NULL;

    if (start_aiq(app) != 0)
        return -1;
    return camera_start(app);
}

static int camera_recover_only(app_ctx_t *app)
{
    int attempt;

    camera_deinit(app->camera);
    app->camera = NULL;
    if (camera_open(app) != 0)
        return -1;

    for (attempt = 0; attempt < 3; attempt++) {
        if (camera_warmup(app->camera, 1000) == 0)
            return 0;
        usleep(200 * 1000);
    }
    return -1;
}

static void camera_watchdog(app_ctx_t *app)
{
    static time_t last_attempt;
    time_t now = time(NULL);

    if (now - last_attempt < 5)
        return;

    if (!app->camera ||
        now - camera_last_frame_time(app->camera) > 5) {
        LOGW("watchdog: cámara sin frames; recuperando sólo cámara\n");
        last_attempt = now;
        if (camera_recover_only(app) == 0) {
            LOGI("watchdog: cámara recuperada\n");
        } else {
            LOGW("watchdog: recuperación falló; reinicio completo\n");
            (void)pipeline_restart(app);
        }
    }
}

static int take_one(app_ctx_t *app)
{
    char path[512];
    int ret;

    if (!app->camera) {
        LOGW("camera: reintentando inicialización\n");
        if (pipeline_restart(app) != 0) {
            LOGE("camera: init falló\n");
            led_flash(app, 32, 0, 0);
            return -1;
        }
    }

    if (photo_path_next(app->cfg.output_dir, path, sizeof(path)) != 0) {
        LOGE("foto: no se pudo generar nombre en %s\n", app->cfg.output_dir);
        led_flash(app, 32, 0, 0);
        return -1;
    }

    /*
     * Apply any autosaved overlay change and force the RGN display attributes
     * immediately before the capture. This avoids a stale logo position if the
     * editor saved while the encoder was busy or the poll missed a frame.
     */
    overlay_poll();
    overlay_refresh();
    usleep(10 * 1000);

    LOGI("foto: capturando %s\n", path);
    ret = camera_capture(app->camera, path, app->cfg.capture_timeout_ms,
                         app->cfg.retries);
    if (ret != 0) {
        LOGW("foto: recuperando pipeline y reintentando\n");
        if (pipeline_restart(app) == 0)
            ret = camera_capture(app->camera, path,
                                 app->cfg.capture_timeout_ms,
                                 app->cfg.retries);
    }
    if (ret == 0) {
        led_photo_flash(app);
    } else {
        LOGE("foto: captura falló\n");
        led_flash(app, 32, 0, 0);
    }

    return ret;
}

static int reopen_button(app_ctx_t *app)
{
    char path[128];

    buttons_close(app->button_fd);
    app->button_fd = buttons_open(app->cfg.input_name, path, sizeof(path));
    if (app->button_fd < 0) {
        LOGE("input: no se encontró '%s'\n", app->cfg.input_name);
        return -1;
    }
    buttons_state_init(&app->button);
    LOGI("input: %s (%s)\n", path, app->cfg.input_name);
    return 0;
}

static void usage(const char *argv0)
{
    printf("Uso: %s [opciones]\n", argv0);
    printf("  --config PATH            archivo de configuración\n");
    printf("  --output DIR             carpeta de fotos (default /userdata/photos)\n");
    printf("  --width N --height N     resolución (default 2304x1296)\n");
    printf("  --quality N              calidad JPEG 1..100 (default 90)\n");
    printf("  --input NAME             nombre/ruta del input gpio-keys\n");
    printf("  --capture-timeout MS     timeout de captura (default 3000)\n");
    printf("  --retries N              reintentos (default 3)\n");
    printf("  --no-led                 desactivar LED\n");
    printf("  --led-rgb R,G,B          fijar LED y salir (debug)\n");
    printf("  --led-test               probar secuencia LED y salir\n");
    printf("  --once                   tomar una foto y salir\n");
    printf("  --log-level N            0=error 1=warn 2=info 3=debug\n");
    printf("  -h, --help               esta ayuda\n");
}

static int parse_rgb(const char *s, uint8_t *r, uint8_t *g, uint8_t *b)
{
    int rr, gg, bb;
    if (sscanf(s, "%d,%d,%d", &rr, &gg, &bb) != 3)
        return -1;
    if (rr < 0 || rr > 255 || gg < 0 || gg > 255 || bb < 0 || bb > 255)
        return -1;
    *r = (uint8_t)rr;
    *g = (uint8_t)gg;
    *b = (uint8_t)bb;
    return 0;
}

int main(int argc, char **argv)
{
    app_ctx_t app;
    const char *config_path = NULL;
    int once = 0;
    int led_debug = 0;
    int led_test = 0;
    uint8_t led_r = 0, led_g = 0, led_b = 0;
    int opt;
    int ret = EXIT_FAILURE;
    unsigned int overlay_ticks = 0;
    time_t last_watchdog = 0;
    static const struct option long_opts[] = {
        { "config", required_argument, NULL, 'c' },
        { "output", required_argument, NULL, 'o' },
        { "width", required_argument, NULL, 'w' },
        { "height", required_argument, NULL, 'H' },
        { "quality", required_argument, NULL, 'q' },
        { "input", required_argument, NULL, 'i' },
        { "capture-timeout", required_argument, NULL, 't' },
        { "retries", required_argument, NULL, 'r' },
        { "no-led", no_argument, NULL, 'L' },
        { "led-rgb", required_argument, NULL, 'R' },
        { "led-test", no_argument, NULL, 'T' },
        { "once", no_argument, NULL, '1' },
        { "log-level", required_argument, NULL, 'l' },
        { "help", no_argument, NULL, 'h' },
        { NULL, 0, NULL, 0 },
    };

    memset(&app, 0, sizeof(app));
    app.led_fd = -1;
    app.button_fd = -1;
    app.meta.mem_fd = -1;
    config_defaults(&app.cfg);

    /* Pre-scan only for --config so the file is loaded before CLI overrides. */
    {
        int i;
        for (i = 1; i < argc; i++) {
            if (strcmp(argv[i], "--config") == 0 && i + 1 < argc) {
                config_path = argv[i + 1];
                i++;
            } else if (strncmp(argv[i], "--config=", 9) == 0) {
                config_path = argv[i] + 9;
            }
        }
    }

    if (config_load_file(&app.cfg, DEFAULT_CONFIG_FILE) != 0)
        fprintf(stderr, "config: %s no disponible\n", DEFAULT_CONFIG_FILE);
    if (config_load_file(&app.cfg, USER_CONFIG_FILE) != 0)
        fprintf(stderr, "config: %s no disponible\n", USER_CONFIG_FILE);
    if (config_path) {
        if (config_load_file(&app.cfg, config_path) != 0) {
            fprintf(stderr, "config: no se pudo cargar %s\n", config_path);
            return EXIT_FAILURE;
        }
    }

    while ((opt = getopt_long(argc, argv, "h", long_opts, NULL)) != -1) {
        switch (opt) {
        case 'c': config_path = optarg; break;
        case 'o': snprintf(app.cfg.output_dir, sizeof(app.cfg.output_dir), "%s", optarg); break;
        case 'w': app.cfg.width = atoi(optarg); break;
        case 'H': app.cfg.height = atoi(optarg); break;
        case 'q': app.cfg.quality = atoi(optarg); break;
        case 'i': snprintf(app.cfg.input_name, sizeof(app.cfg.input_name), "%s", optarg); break;
        case 't': app.cfg.capture_timeout_ms = atoi(optarg); break;
        case 'r': app.cfg.retries = atoi(optarg); break;
        case 'L': app.cfg.led = 0; break;
        case 'R':
            if (parse_rgb(optarg, &led_r, &led_g, &led_b) != 0) {
                fprintf(stderr, "RGB inválido: %s\n", optarg);
                return EXIT_FAILURE;
            }
            led_debug = 1;
            break;
        case 'T': led_test = 1; break;
        case '1': once = 1; break;
        case 'l': app.cfg.log_level = atoi(optarg); break;
        case 'h': usage(argv[0]); return EXIT_SUCCESS;
        default: usage(argv[0]); return EXIT_FAILURE;
        }
    }

    log_init(app.cfg.log_level);

    /*
     * Install handlers before AIQ/VI startup. A test or user can send
     * SIGUSR1 while the pipeline is still settling; without this the default
     * action terminates the process before it ever reaches the main loop.
     */
    unlink(READY_FILE);
    unlink(CAMERA_READY_FILE);
    unlink(SYSTEM_READY_FILE);
    signal(SIGINT, on_signal);
    signal(SIGTERM, on_signal);
    signal(SIGUSR1, on_capture_signal);
    signal(SIGUSR2, on_camera_fault_signal);

    if (config_validate(&app.cfg) != 0) {
        LOGE("config: valores inválidos\n");
        return EXIT_FAILURE;
    }

    if (app.cfg.led) {
        app.led_fd = ws2812_open(LED_SPI_DEV);
        if (app.led_fd < 0)
            LOGW("led: no se pudo abrir %s: %s\n", LED_SPI_DEV, strerror(errno));
        else
            led_off(&app);
    }

    if (led_debug) {
        led_set(&app, led_r, led_g, led_b);
        ws2812_close(app.led_fd);
        return EXIT_SUCCESS;
    }

    if (led_test) {
        led_boot_ready(&app);
        led_photo_flash(&app);
        ws2812_close(app.led_fd);
        return EXIT_SUCCESS;
    }

    if (ensure_output_dir(app.cfg.output_dir) != 0) {
        LOGE("storage: %s no está disponible\n", app.cfg.output_dir);
        led_flash(&app, 32, 0, 0);
        goto out_led;
    }

    if (photo_path_ensure_dir(PHOTO_STAGING_DIR) != 0) {
        LOGE("storage: no se pudo crear %s\n", PHOTO_STAGING_DIR);
        led_flash(&app, 32, 0, 0);
        goto out_led;
    }
    {
        int removed = photo_path_clean_staging(PHOTO_STAGING_DIR);
        if (removed > 0)
            LOGI("storage: %d temporales antiguos eliminados\n", removed);
    }

    if (meta_open(&app.meta) != 0) {
        LOGE("meta: no se pudo abrir el meta de fastboot\n");
        led_flash(&app, 32, 0, 0);
        goto out_led;
    }

    /* Match simple_vi_get_frame_rkaiq: AIQ first, then SYS_Init/VI/VENC. */
    if (start_aiq(&app) != 0) {
        led_flash(&app, 32, 0, 0);
        goto out_camera;
    }

    /* Give the AIQ/ISP worker time to start before opening VI. */
    if (app.cfg.aiq)
        sleep(2);

    if (camera_open(&app) != 0)
        LOGW("camera: init inicial falló; se reintentará al pulsar\n");

    int camera_ok = 0;
    if (app.camera) {
        int attempt;
        int warmed = 0;
        for (attempt = 0; attempt < 3; attempt++) {
            if (camera_warmup(app.camera, 500) == 0) {
                warmed = 1;
                break;
            }
            usleep(200 * 1000);
        }
        if (!warmed && app.cfg.fast_handoff == 0) {
            /*
             * Normal AIQ already loaded the IQ/CCM. The Thunder Boot VI pipe
             * still needs a second open, but the AIQ can be preserved.
             */
            LOGW("camera: warmup falló; reinit cámara sin tocar AIQ\n");
            camera_deinit(app.camera);
            app.camera = NULL;
            if (camera_open(&app) == 0) {
                for (attempt = 0; attempt < 3; attempt++) {
                    if (camera_warmup(app.camera, 1000) == 0) {
                        warmed = 1;
                        break;
                    }
                    usleep(200 * 1000);
                }
                if (warmed && app.aiq) {
                    if (aiq_restart(app.aiq) == 0) {
                        (void)camera_warmup(app.camera, 1000);
                    } else {
                        LOGW("camera: aiq_restart falló; se reintentará completo\n");
                        warmed = 0;
                    }
                }
            }
        }
        if (!warmed) {
            /*
             * Fast-handoff AIQ keeps the MCU Thunder Boot ISP parameters and
             * does not reload the IQ/CCM; restart AIQ+camera so color is correct.
             */
            LOGW("camera: warmup inicial falló; reiniciando AIQ+cámara\n");
            if (pipeline_restart(&app) == 0)
                warmed = 1;
        }
        if (warmed) {
            camera_ok = 1;
            LOGI("camera: pipeline listo\n");
        } else
            LOGW("camera: sin frame inicial; se reintentará al pulsar\n");
    } else {
        LOGW("camera: sin pipeline; se reintentará al pulsar\n");
    }

    if (once) {
        ret = take_one(&app) == 0 ? EXIT_SUCCESS : EXIT_FAILURE;
        goto out_camera;
    }

    if (reopen_button(&app) != 0) {
        led_flash(&app, 32, 0, 0);
        goto out_camera;
    }

    /*
     * The green double-blink is the "all ready" signal. Do not expose the
     * USB gadget, MTP, web UI, or button processing until the camera, LED
     * engine, and input device have completed initialization.
     */
    led_boot_ready(&app);
    led_engine_init(&app.led, app.led_fd, app.cfg.led_count, monotonic_ms());
    mark_ready();
    if (camera_ok) {
        mark_camera_ready();
        mark_system_ready();
    }
    LOGI("sistema: cámara/LED/botones listos%s\n",
         camera_ok ? "" : " (sin cámara; esperando recuperación)");
    LOGI("listo: USER3=KEY_LEFT siguiente rutina, USER4=KEY_DOWN anterior; "
         "USER1/USER2 libres para el usuario\n");

    while (g_running) {
        struct pollfd pfd;
        struct input_event ev;
        int pr;

        led_engine_tick(&app.led, monotonic_ms());

        if (app.camera)
            (void)camera_poll(app.camera);

        {
            time_t now = time(NULL);

            if (g_camera_fault) {
                g_camera_fault = 0;
                LOGW("watchdog: fallo de cámara simulado\n");
                camera_deinit(app.camera);
                app.camera = NULL;
            }
            if (++overlay_ticks >= 2) {
                overlay_ticks = 0;
                overlay_poll();
            }
            if (now != last_watchdog) {
                last_watchdog = now;
                camera_watchdog(&app);
            }
        }

        if (access(PREVIEW_REQUEST, F_OK) == 0) {
            unlink(PREVIEW_REQUEST);
            if (app.camera)
                (void)camera_preview(app.camera);
        }

        if (access(CAPTURE_REQUEST, F_OK) == 0) {
            unlink(CAPTURE_REQUEST);
            (void)take_one(&app);
            continue;
        }

        if (g_capture_request) {
            g_capture_request = 0;
            (void)take_one(&app);
            continue;
        }

        pfd.fd = app.button_fd;
        pfd.events = POLLIN;
        pfd.revents = 0;
        pr = poll(&pfd, 1, 20);
        if (pr < 0) {
            if (errno == EINTR)
                continue;
            LOGE("input: poll falló: %s\n", strerror(errno));
            break;
        }
        if (pr == 0)
            continue;
        if (!(pfd.revents & POLLIN))
            continue;

        for (;;) {
            int r = buttons_read(app.button_fd, &ev);
            if (r == 0)
                break;
            if (r < 0) {
                LOGW("input: se perdió el dispositivo, reintentando\n");
                usleep(500 * 1000);
                if (reopen_button(&app) != 0) {
                    g_running = 0;
                    break;
                }
                break;
            }
            {
                button_action_t action = buttons_process(&app.button, &ev);

                switch (action) {
                case BTN_ACTION_CAPTURE:
                    (void)take_one(&app);
                    break;
                case BTN_ACTION_LED_NEXT:
                    if (led_engine_next(&app.led) == 0)
                        LOGI("led: siguiente '%s'\n",
                             led_engine_pattern_name(&app.led));
                    break;
                case BTN_ACTION_LED_PREV:
                    if (led_engine_prev(&app.led) == 0)
                        LOGI("led: anterior '%s'\n",
                             led_engine_pattern_name(&app.led));
                    break;
                case BTN_ACTION_USER_UP:
                    user_led_on_button_up(&app.led);
                    break;
                case BTN_ACTION_USER_RIGHT:
                    user_led_on_button_right(&app.led);
                    break;
                default:
                    break;
                }
            }
        }
    }

    ret = EXIT_SUCCESS;

out_camera:
    led_set(&app, 0, 0, 0);
    camera_deinit(app.camera);
    aiq_deinit(app.aiq);
    meta_close(&app.meta);
out_led:
    buttons_close(app.button_fd);
    ws2812_close(app.led_fd);
    return ret;
}
