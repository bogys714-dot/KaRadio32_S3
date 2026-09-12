/*
 * usb_msc.h
 *
 * MVP support for playing audio files from a USB flash drive (MSC class)
 * on ESP32-S3 native USB-OTG (GPIO19 D-, GPIO20 D+).
 *
 * This reuses the existing audio_player/audio_renderer pipeline that is
 * already used for internet radio streaming (see webclient.c) -- the only
 * difference is that compressed audio bytes come from a file on the FAT
 * volume instead of from a TCP socket.
 *
 * Status: MVP / proof of concept.
 *  - No file browser UI yet.
 *  - Only one MSC device / one FAT volume (mounted at "/usb0").
 *  - On device connect, it auto-detects the file configured in
 *    USB_MSC_TEST_FILE and plays it once, purely to validate the pipeline.
 */

#ifndef MAIN_INCLUDE_USB_MSC_H_
#define MAIN_INCLUDE_USB_MSC_H_

#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

// Fixed test file used for the MVP (no UI / file browser yet)
#define USB_MSC_TEST_FILE   "/usb0/test.mp3"

/**
 * @brief Start the USB Host stack + MSC driver.
 *
 * Call this once from app_main() after audio_player_init()/renderer_init()
 * have already been called (so player_config is ready to use).
 * Safe to call even if no flash drive is ever connected: it just waits.
 */
void usb_msc_init(void);

/**
 * @brief Returns true if a USB flash drive is currently mounted at /usb0.
 */
bool usb_msc_is_ready(void);

/**
 * @brief Play a single audio file (mp3/aac/m4a) from the mounted USB drive.
 *
 * Stops whatever is currently playing (radio stream or another local file),
 * then streams the file's compressed bytes into the existing audio_player
 * pipeline via audio_stream_consumer(), exactly like webclient.c does for
 * network streams. Blocks until the file finishes playing or an error
 * occurs, so call it from its own task if you don't want to block the
 * caller (usb_msc.c already does this for the MVP auto-play).
 *
 * @param path Absolute VFS path, e.g. "/usb0/test.mp3"
 * @return true if playback completed (or started) without a hard error
 */
bool usb_msc_play_file(const char *path);

/**
 * @brief Play a file asynchronously (spawns a task and returns immediately).
 *
 * Use this from request handlers (web/websocket) where you must not block
 * the caller for the whole duration of the track. Only one such task runs
 * at a time; a new call while one is already playing will stop the current
 * one first (see usb_msc_request_stop()).
 *
 * @param path Absolute VFS path, e.g. "/usb0/track.mp3"
 */
void usb_msc_play_file_async(const char *path);

/**
 * @brief Ask whatever usb_msc_play_file()/usb_msc_play_file_async() is
 * currently doing to stop as soon as possible (checked between reads).
 * Also stops the underlying audio_player.
 */
void usb_msc_request_stop(void);

/**
 * @brief List playable files (.mp3/.aac/.m4a) in the root of the mounted
 * USB drive as a JSON array of strings, e.g. ["track1.mp3","track2.mp3"].
 *
 * @param buf     Destination buffer
 * @param buflen  Size of buf
 * @return true on success (buf is valid JSON), false if not mounted or on
 *         truncation
 */
bool usb_msc_list_files(char *buf, size_t buflen);

/**
 * @brief Return the full path of the currently playing USB file, or NULL
 * if nothing is playing / last playback has finished.
 */
const char *usb_msc_current_path(void);

/**
 * @brief true if a USB file is currently being played (or play task running)
 */
bool usb_msc_is_playing(void);

/**
 * @brief Start the first track from the in-memory playlist (builds list if needed).
 * @return true if playback was started
 */
bool usb_msc_play_first(void);

/**
 * @brief Skip to the next / previous track in the in-memory playlist
 * (wraps around at the ends). No-op (returns false) if nothing is mounted
 * or the playlist is empty.
 */
bool usb_msc_play_next(void);
bool usb_msc_play_prev(void);

/**
 * @brief Number of tracks currently in the in-memory playlist (builds the
 * list from the mounted drive's root folder if it hasn't been built yet).
 */
int usb_msc_playlist_count(void);

/**
 * @brief Bare filename (no path) of the track at `index` in the playlist,
 * or NULL if index is out of range / nothing is mounted.
 */
const char *usb_msc_playlist_name(int index);

/**
 * @brief Index of the currently playing track in the playlist, or -1 if
 * nothing is playing / not found.
 */
int usb_msc_playlist_current_index(void);


#ifdef __cplusplus
}
#endif

#endif /* MAIN_INCLUDE_USB_MSC_H_ */
