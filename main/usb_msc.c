/*
 * usb_msc.c
 *
 * MVP support for playing audio files from a USB flash drive (MSC class)
 * on ESP32-S3 native USB-OTG (GPIO19 D-, GPIO20 D+).
 *
 * Reuses the existing audio_player pipeline (see webclient.c) -- compressed
 * bytes are read from a file instead of a TCP socket and pushed through the
 * same audio_stream_consumer() used for internet radio.
 *
 * Robustness notes below (install retries, deferred event handling,
 * "reinstall if already plugged in" recovery) come from real-world testing
 * on a sibling project (yoRadio) using the same underlying usb_host_msc
 * driver -- without them the driver can silently fail to see a drive that
 * was already inserted before boot, or fail usb_host_install() right after
 * reset because the native USB PHY isn't ready yet.
 */

#include <stdio.h>
#include <string.h>
#include <strings.h>
#include <dirent.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "esp_err.h"

#include "usb/usb_host.h"
#include "usb/msc_host.h"
#include "usb/msc_host_vfs.h"

#include "usb_msc.h"
#include "app_main.h"     // kmalloc/kcalloc, task priorities, etc.
#include "webclient.h"    // contentType_t (KAUDIO_MPEG etc.)
#include "audio_player.h" // audio_player_start/stop, audio_stream_consumer, player_t
#include "interface.h"    // kprintf, PSTR

// player_config is defined in app_main.c (created before audio_player_init()).
// Declared here directly rather than in app_main.h: app_main.h and
// audio_player.h/audio_renderer.h already include each other in a circular
// way (audio_renderer.h needs output_mode_t from app_main.h), and adding
// another type from audio_player.h into that cycle just flips which side
// breaks depending on which header is included first by a given .c file.
extern player_t *player_config;

static const char *TAG = "usb_msc";

#define USB_MOUNT_POINT     "/usb0"
#define USB_READ_CHUNK      2048
#define USB_INSTALL_ATTEMPTS 5
#define USB_WAIT_MS          8000
#define USB_REINSTALL_CYCLES  2

static msc_host_device_handle_t s_device = NULL;
static msc_host_vfs_handle_t    s_vfs_handle = NULL;
static bool                     s_hostInstalled = false;
static volatile bool            s_ready = false;

static volatile int   s_pendingConnectAddr = -1;
static volatile bool  s_pendingDisconnect  = false;
static msc_host_device_handle_t s_pendingDisconnectHandle = NULL;

static TaskHandle_t s_usbLibTaskHandle = NULL;
static TaskHandle_t s_installTaskHandle = NULL;

static volatile bool s_stopRequested = false;
static TaskHandle_t  s_playTaskHandle = NULL;
static char          s_currentPath[256] = {0};

// --- simple in-memory playlist (built once when USB is mounted) ---
#define USB_PL_MAX       256
#define USB_PL_NAME_MAX  96
static char  *s_playlist[USB_PL_MAX];   // heap-allocated file names (no path)
static int    s_plCount = 0;
static int    s_plIndex = -1;           // currently playing index
static bool   s_plAutoNext = true;      // auto-advance after track ends

static bool ends_with_ci(const char *str, const char *suffix);
static void usb_msc_playlist_free(void);
static void usb_msc_playlist_build(void);
static int  usb_msc_playlist_index_of(const char *path);

// ---------------------------------------------------------------------
// USB Host Library event pump (must be run continuously while installed)
// ---------------------------------------------------------------------
static void usb_lib_task(void *arg)
{
    (void)arg;
    while (1) {
        uint32_t event_flags;
        usb_host_lib_handle_events(portMAX_DELAY, &event_flags);
    }
}

// MSC driver callback -- runs from the MSC driver's own context, so we only
// stash the event here and do the real (blocking, FATFS) work from a normal
// task below. Doing FAT mount/unmount directly in this callback has been a
// source of intermittent lockups on some builds.
static void msc_event_cb(const msc_host_event_t *event, void *arg)
{
    (void)arg;
    if (event->event == MSC_DEVICE_CONNECTED) {
        s_pendingConnectAddr = event->device.address;
    } else if (event->event == MSC_DEVICE_DISCONNECTED) {
        s_pendingDisconnectHandle = event->device.handle;
        s_pendingDisconnect = true;
    }
}

static void handle_device_connected(uint8_t addr)
{
    if (msc_host_install_device(addr, &s_device) != ESP_OK) {
        ESP_LOGE(TAG, "msc_host_install_device failed");
        return;
    }

    esp_vfs_fat_mount_config_t mount_config = {
        .format_if_mount_failed = false,
        .max_files = 3,
        .allocation_unit_size = 0,
    };

    if (msc_host_vfs_register(s_device, USB_MOUNT_POINT, &mount_config, &s_vfs_handle) != ESP_OK) {
        ESP_LOGE(TAG, "msc_host_vfs_register (FAT mount) failed");
        msc_host_uninstall_device(s_device);
        s_device = NULL;
        return;
    }

    s_ready = true;
    ESP_LOGI(TAG, "USB flash drive mounted at %s", USB_MOUNT_POINT);

    // MVP: auto-play a fixed test file so the pipeline can be validated
    // without a file browser UI yet. Async so this task stays responsive
    // to a disconnect event while a track is playing.
    usb_msc_playlist_build();
    // optional: auto-start first track
    // if (s_plCount > 0) { char p[128]; snprintf(p,sizeof(p),"%s/%s",USB_MOUNT_POINT,s_playlist[0]); usb_msc_play_file_async(p); }
}

static void handle_device_disconnected(msc_host_device_handle_t handle)
{
    s_ready = false;
    if (s_vfs_handle) {
        msc_host_vfs_unregister(s_vfs_handle);
        s_vfs_handle = NULL;
    }
    if (handle) {
        msc_host_uninstall_device(handle);
    }
    s_device = NULL;
    usb_msc_playlist_free();
    ESP_LOGI(TAG, "USB flash drive removed");
}

// Polls for connect/disconnect events queued by msc_event_cb() and does the
// actual (blocking) mount/unmount work in normal task context.
static void install_task(void *arg)
{
    (void)arg;
    while (1) {
        if (s_pendingConnectAddr >= 0) {
            int addr = s_pendingConnectAddr;
            s_pendingConnectAddr = -1;
            handle_device_connected((uint8_t)addr);
        }
        if (s_pendingDisconnect) {
            s_pendingDisconnect = false;
            handle_device_disconnected(s_pendingDisconnectHandle);
        }
        vTaskDelay(pdMS_TO_TICKS(100));
    }
}

static bool install_stack(void)
{
    const usb_host_config_t host_config = {
        .skip_phy_setup = false,
        .intr_flags = ESP_INTR_FLAG_LEVEL1,
    };

    // Right after reset the native USB PHY is sometimes not ready yet, and
    // usb_host_install() returns ESP_ERR_NOT_FOUND. Retry a few times
    // instead of giving up immediately.
    esp_err_t err = ESP_FAIL;
    for (int i = 1; i <= USB_INSTALL_ATTEMPTS; i++) {
        err = usb_host_install(&host_config);
        if (err == ESP_OK) break;
        ESP_LOGW(TAG, "usb_host_install() attempt %d/%d failed: %s", i, USB_INSTALL_ATTEMPTS, esp_err_to_name(err));
        vTaskDelay(pdMS_TO_TICKS(300));
    }
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "USB Host install failed: %s", esp_err_to_name(err));
        return false;
    }

    const msc_host_driver_config_t msc_config = {
        .create_backround_task = true,
        .task_priority = 5,
        .stack_size = 4096,
        .callback = msc_event_cb,
        .callback_arg = NULL,
    };
    err = msc_host_install(&msc_config);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "MSC host install failed: %s", esp_err_to_name(err));
        usb_host_uninstall();
        return false;
    }

    xTaskCreatePinnedToCore(usb_lib_task, "usb_host_lib", 4096, NULL, 8, &s_usbLibTaskHandle, 1);
    xTaskCreatePinnedToCore(install_task, "usb_install", 4096, NULL, 6, &s_installTaskHandle, 1);

    s_hostInstalled = true;
    return true;
}

static void teardown_stack(void)
{
    if (s_installTaskHandle)  { vTaskDelete(s_installTaskHandle);  s_installTaskHandle = NULL; }
    if (s_usbLibTaskHandle)   { vTaskDelete(s_usbLibTaskHandle);   s_usbLibTaskHandle = NULL; }
    if (s_hostInstalled) {
        msc_host_uninstall();
        usb_host_uninstall();
    }
    s_pendingConnectAddr = -1;
    s_pendingDisconnect = false;
    s_hostInstalled = false;
}

// Runs once at boot: brings up the stack and, if a flash drive was already
// plugged in before we switched to Host mode (so no connect edge was ever
// seen), tears down and re-installs a couple of times to force the
// controller to re-enumerate it.
static void usb_msc_startup_task(void *arg)
{
    (void)arg;

    if (!install_stack()) {
        vTaskDelete(NULL);
        return;
    }

    for (int cycle = 0; cycle <= USB_REINSTALL_CYCLES && !s_ready; cycle++) {
        if (cycle > 0) {
            ESP_LOGW(TAG, "No USB device found, reinstalling stack (attempt %d/%d)...", cycle, USB_REINSTALL_CYCLES);
            teardown_stack();
            vTaskDelay(pdMS_TO_TICKS(300));
            if (!install_stack()) continue;
        }
        uint32_t waited = 0;
        while (!s_ready && waited < USB_WAIT_MS) {
            vTaskDelay(pdMS_TO_TICKS(100));
            waited += 100;
        }
    }

    if (!s_ready) {
        ESP_LOGI(TAG, "No USB flash drive detected yet -- will still be picked up on hot-plug");
    }

    vTaskDelete(NULL);
}

void usb_msc_init(void)
{
    xTaskCreatePinnedToCore(usb_msc_startup_task, "usb_msc_start", 4096, NULL, 5, NULL, 1);
}

bool usb_msc_is_ready(void)
{
    return s_ready;
}

static bool ends_with_ci(const char *str, const char *suffix)
{
    size_t lstr = strlen(str), lsuf = strlen(suffix);
    if (lsuf > lstr) return false;
    return strcasecmp(str + (lstr - lsuf), suffix) == 0;
}



static void usb_msc_playlist_free(void)
{
    for (int i = 0; i < s_plCount; i++) {
        if (s_playlist[i]) {
            free(s_playlist[i]);
            s_playlist[i] = NULL;
        }
    }
    s_plCount = 0;
    s_plIndex = -1;
}

// Scan /usb0 root and build sorted name list in heap (not on stack)
static void usb_msc_playlist_build(void)
{
    usb_msc_playlist_free();
    if (!s_ready) return;

    DIR *dh = opendir(USB_MOUNT_POINT);
    if (!dh) {
        ESP_LOGE(TAG, "playlist: opendir failed");
        return;
    }

    struct dirent *d;
    while ((d = readdir(dh)) != NULL && s_plCount < USB_PL_MAX) {
        if (d->d_type == DT_DIR) continue;
        if (!ends_with_ci(d->d_name, ".mp3") &&
            !ends_with_ci(d->d_name, ".aac") &&
            !ends_with_ci(d->d_name, ".m4a")) {
            continue;
        }
        size_t len = strlen(d->d_name);
        if (len == 0 || len >= USB_PL_NAME_MAX) continue;
        char *copy = (char *)malloc(len + 1);
        if (!copy) break;
        memcpy(copy, d->d_name, len + 1);
        s_playlist[s_plCount++] = copy;
    }
    closedir(dh);

    // Sort by name (case-insensitive bubble – fine for a few hundred entries)
    for (int i = 0; i < s_plCount - 1; i++) {
        for (int j = 0; j < s_plCount - 1 - i; j++) {
            if (strcasecmp(s_playlist[j], s_playlist[j + 1]) > 0) {
                char *tmp = s_playlist[j];
                s_playlist[j] = s_playlist[j + 1];
                s_playlist[j + 1] = tmp;
            }
        }
    }
    ESP_LOGI(TAG, "playlist: %d tracks", s_plCount);
}

// Find index of a full path (or bare name) in the playlist; -1 if not found
static int usb_msc_playlist_index_of(const char *path)
{
    if (!path || s_plCount == 0) return -1;
    const char *name = strrchr(path, '/');
    name = name ? name + 1 : path;
    for (int i = 0; i < s_plCount; i++) {
        if (strcasecmp(s_playlist[i], name) == 0) return i;
    }
    return -1;
}

bool usb_msc_play_file(const char *path)
{
    contentType_t contentType;
    if (ends_with_ci(path, ".mp3")) {
        contentType = KAUDIO_MPEG;
    } else if (ends_with_ci(path, ".aac")) {
        contentType = KAUDIO_AAC;
    } else if (ends_with_ci(path, ".m4a") || ends_with_ci(path, ".mp4")) {
        contentType = KAUDIO_MP4;
    } else {
        ESP_LOGE(TAG, "Unsupported file type: %s", path);
        return false;
    }

    FILE *f = fopen(path, "rb");
    if (!f) {
        ESP_LOGE(TAG, "Could not open %s", path);
        return false;
    }

    ESP_LOGI(TAG, "Playing %s", path);

    // Remember for HTTP monitoring endpoint + playlist position
    strncpy(s_currentPath, path, sizeof(s_currentPath) - 1);
    s_currentPath[sizeof(s_currentPath) - 1] = 0;
    s_plIndex = usb_msc_playlist_index_of(path);
    if (s_plIndex < 0 && s_plCount == 0) {
        // list not built yet – build now
        usb_msc_playlist_build();
        s_plIndex = usb_msc_playlist_index_of(path);
    }

    // --- Интеграция с дисплеем и веб-интерфейсом (как у обычных станций) ---
    {
        // Берём только имя файла без пути
        const char *filename = strrchr(path, '/');
        filename = filename ? filename + 1 : path;

        char display_name[128];
        snprintf(display_name, sizeof(display_name), "USB: %s", filename);

        // Эти события слушают и дисплей, и веб-интерфейс
        kprintf(PSTR("##CLI.NAMESET#: %s\n"), display_name);
        kprintf(PSTR("##CLI.META#: %s\n"), display_name);
        kprintf(PSTR("##CLI.PLAYING#\n"));
    }
    // ----------------------------------------------------------------------

    // Stop whatever is currently playing (radio stream or another local
    // file) before feeding a new one into the same decode pipeline.
    audio_player_stop();

    player_config->media_stream->content_type = contentType;
    player_config->media_stream->eof = false;

    audio_player_start();

    uint8_t *buf = (uint8_t *)kmalloc(USB_READ_CHUNK);
    if (!buf) {
        ESP_LOGE(TAG, "Out of memory");
        fclose(f);
        audio_player_stop();
        kprintf(PSTR("##CLI.STOPPED#\n"));
        return false;
    }

    bool ok = true;
    size_t n;
    s_stopRequested = false;

    while ((n = fread(buf, 1, USB_READ_CHUNK, f)) > 0) {
        if (s_stopRequested) {
            ESP_LOGI(TAG, "Playback stop requested");
            break;
        }
        if (audio_stream_consumer((char *)buf, (ssize_t)n) == -1) {
            ESP_LOGW(TAG, "audio_stream_consumer error, stopping playback");
            ok = false;
            break;
        }
    }

    player_config->media_stream->eof = true;
    free(buf);
    fclose(f);
    audio_player_stop();

    // Сообщаем дисплею и вебу, что воспроизведение закончилось
    kprintf(PSTR("##CLI.STOPPED#\n"));

    s_currentPath[0] = 0;   // no longer playing

    ESP_LOGI(TAG, "Finished %s", path);
    return ok;
}

// --- async wrapper + stop + directory listing, used by the web UI --------

static char s_asyncPath[256];

static void play_task(void *arg)
{
    (void)arg;
    usb_msc_play_file(s_asyncPath);
    s_playTaskHandle = NULL;

    // Auto-next from in-memory playlist (no big stack buffers)
    if (s_plAutoNext && !s_stopRequested && s_plCount > 0) {
        int next = (s_plIndex < 0) ? 0 : (s_plIndex + 1) % s_plCount;
        char next_path[160];
        snprintf(next_path, sizeof(next_path), "%s/%s", USB_MOUNT_POINT, s_playlist[next]);
        ESP_LOGI(TAG, "Auto next [%d/%d]: %s", next + 1, s_plCount, s_playlist[next]);
        vTaskDelay(pdMS_TO_TICKS(200));
        if (!s_stopRequested) {
            usb_msc_play_file_async(next_path);
        }
    }

    vTaskDelete(NULL);
}

void usb_msc_play_file_async(const char *path)
{
    // Only one local-file playback task at a time: stop the current one
    // (if any) and wait briefly for it to exit before starting the new one.
    if (s_playTaskHandle) {
        usb_msc_request_stop();
        for (int i = 0; i < 50 && s_playTaskHandle; i++) {
            vTaskDelay(pdMS_TO_TICKS(20));
        }
    }

    strncpy(s_asyncPath, path, sizeof(s_asyncPath) - 1);
    s_asyncPath[sizeof(s_asyncPath) - 1] = 0;

    xTaskCreatePinnedToCore(play_task, "usb_play", 6144, NULL, 5, &s_playTaskHandle, 1);
}

void usb_msc_request_stop(void)
{
    s_stopRequested = true;
    audio_player_stop();
}

bool usb_msc_list_files(char *buf, size_t buflen)
{
    if (!s_ready) return false;

    DIR *dh = opendir(USB_MOUNT_POINT);
    if (!dh) {
        ESP_LOGE(TAG, "opendir(%s) failed", USB_MOUNT_POINT);
        return false;
    }

    size_t used = 0;
    buf[0] = 0;
    used += snprintf(buf + used, buflen - used, "[");

    struct dirent *d;
    bool first = true;
    while ((d = readdir(dh)) != NULL) {
        if (d->d_type == DT_DIR) continue;
        if (!ends_with_ci(d->d_name, ".mp3") &&
            !ends_with_ci(d->d_name, ".aac") &&
            !ends_with_ci(d->d_name, ".m4a")) {
            continue;
        }
        // +4 for possible comma, quotes and margin
        if (used + strlen(d->d_name) + 4 >= buflen) {
            ESP_LOGW(TAG, "usb_msc_list_files: buffer too small, truncating list");
            break;
        }
        used += snprintf(buf + used, buflen - used, "%s\"%s\"", first ? "" : ",", d->d_name);
        first = false;
    }
    closedir(dh);

    used += snprintf(buf + used, buflen - used, "]");
    return true;
}

const char *usb_msc_current_path(void)
{
    if (s_currentPath[0] == 0) return NULL;
    return s_currentPath;
}

bool usb_msc_is_playing(void)
{
    return (s_playTaskHandle != NULL) || (s_currentPath[0] != 0);
}

bool usb_msc_play_first(void)
{
    if (!s_ready) return false;
    if (s_plCount == 0) usb_msc_playlist_build();
    if (s_plCount == 0) return false;
    char path[160];
    snprintf(path, sizeof(path), "%s/%s", USB_MOUNT_POINT, s_playlist[0]);
    s_stopRequested = false;
    usb_msc_play_file_async(path);
    return true;
}

static bool usb_msc_play_at(int index)
{
    if (!s_ready) return false;
    if (s_plCount == 0) usb_msc_playlist_build();
    if (s_plCount == 0) return false;
    if (index < 0) index += s_plCount;
    index %= s_plCount;
    char path[160];
    snprintf(path, sizeof(path), "%s/%s", USB_MOUNT_POINT, s_playlist[index]);
    s_stopRequested = false;
    usb_msc_play_file_async(path);
    return true;
}

bool usb_msc_play_next(void)
{
    int next = (s_plIndex < 0) ? 0 : s_plIndex + 1;
    return usb_msc_play_at(next);
}

bool usb_msc_play_prev(void)
{
    int prev = (s_plIndex < 0) ? 0 : s_plIndex - 1;
    return usb_msc_play_at(prev);
}

int usb_msc_playlist_count(void)
{
    if (s_plCount == 0 && s_ready) usb_msc_playlist_build();
    return s_plCount;
}

const char *usb_msc_playlist_name(int index)
{
    if (index < 0 || index >= s_plCount) return NULL;
    return s_playlist[index];
}

int usb_msc_playlist_current_index(void)
{
    return s_plIndex;
}
