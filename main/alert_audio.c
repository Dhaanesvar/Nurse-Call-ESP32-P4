#include "alert_audio.h"

#include <stdint.h>
#include <stdio.h>
#include <stdbool.h>
#include <string.h>

#include "bsp/esp-bsp.h"
#include "esp_codec_dev.h"
#include "esp_err.h"
#include "esp_log.h"
#include "esp_spiffs.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

// Voice tuning reference values.
#define MP3_READ_BUF_SIZE 8192
#define MP3_OUTPUT_VOLUME_PERCENT 40
#define MP3_OUTPUT_SAMPLE_RATE 48000
#define MP3_ENABLE_DSP_ENHANCER 0
#define MP3_PREAMP_NUM 200
#define MP3_PREAMP_DEN 130
#define MP3_HPF_ALPHA_NUM 1000
#define MP3_HPF_ALPHA_DEN 1000
#define MP3_LIMITER_THRESHOLD 28000
#define MP3_RESAMPLE_MAX_RATIO 4
#define MP3_WRITE_CHUNK_SAMPLES_PER_CH 256
#define MP3_PATH_MAX_LEN 160

#define AUDIO_SAMPLE_RATE_HZ    MP3_OUTPUT_SAMPLE_RATE
#define AUDIO_BITS_PER_SAMPLE   16
#define AUDIO_CHANNELS          1
#define AUDIO_VOLUME_PERCENT    60
#define AUDIO_CHUNK_SAMPLES     MP3_WRITE_CHUNK_SAMPLES_PER_CH

static const char *TAG = "alert_audio";

typedef struct {
    nurse_alert_type_t type;
    const char *voice_text;
    const char *wav_path;
} alert_profile_t;

static const alert_profile_t s_profiles[] = {
    { NURSE_ALERT_NONE, "", NULL },
    {
        NURSE_ALERT_EMERGENCY,
        "Emergency alert from Bed one six zero. Immediate assistance required.",
        "/spiffs/alerts/emergency.wav",
    },
    {
        NURSE_ALERT_CALL,
        "Nurse call from Bed one six zero. Please attend the patient.",
        "/spiffs/alerts/call.wav",
    },
    {
        NURSE_ALERT_CODE_BLUE,
        "Code blue at Bed one six zero. Critical response team required now.",
        "/spiffs/alerts/code_blue.wav",
    },
    {
        NURSE_ALERT_TOILET_EMERGENCY,
        "Toilet emergency at Bed one six zero. Immediate assistance required.",
        "/spiffs/alerts/toilet_emergency.wav",
    },
};

static esp_codec_dev_handle_t s_speaker = NULL;
static TaskHandle_t s_audio_task = NULL;
static volatile nurse_alert_type_t s_requested_alert = NURSE_ALERT_NONE;
static volatile bool s_stop_requested = false;
static bool s_audio_opened = false;
static int s_audio_rate_hz = AUDIO_SAMPLE_RATE_HZ;
static int s_audio_channels = AUDIO_CHANNELS;
static bool s_spiffs_checked = false;
static bool s_spiffs_ready = false;

static void ensure_spiffs_mounted(void)
{
    if(s_spiffs_checked) {
        return;
    }

    esp_vfs_spiffs_conf_t conf = {
        .base_path = "/spiffs",
        .partition_label = "storage",
        .max_files = 5,
        .format_if_mount_failed = true,
    };

    esp_err_t ret = esp_vfs_spiffs_register(&conf);
    s_spiffs_checked = true;

    if(ret == ESP_OK || ret == ESP_ERR_INVALID_STATE) {
        s_spiffs_ready = true;
        ESP_LOGI(TAG, "SPIFFS ready at /spiffs");
        return;
    }

    s_spiffs_ready = false;
    ESP_LOGW(TAG, "SPIFFS unavailable (%s), voice WAV playback disabled", esp_err_to_name(ret));
}

static const alert_profile_t *get_profile(nurse_alert_type_t type)
{
    size_t i;
    for(i = 0; i < sizeof(s_profiles) / sizeof(s_profiles[0]); i++) {
        if(s_profiles[i].type == type) {
            return &s_profiles[i];
        }
    }
    return &s_profiles[0];
}

static bool open_audio_path(int sample_rate_hz, int channels)
{
    if(sample_rate_hz <= 0) {
        sample_rate_hz = AUDIO_SAMPLE_RATE_HZ;
    }
    if(channels != 2) {
        channels = 1;
    }

    if(s_audio_opened && s_audio_rate_hz == sample_rate_hz && s_audio_channels == channels) {
        if(esp_codec_dev_set_out_mute(s_speaker, false) != ESP_CODEC_DEV_OK) {
            ESP_LOGW(TAG, "failed to unmute speaker");
        }
        return true;
    }

    esp_codec_dev_sample_info_t fs = {
        .sample_rate = sample_rate_hz,
        .bits_per_sample = AUDIO_BITS_PER_SAMPLE,
        .channel = channels,
    };

    if(s_speaker == NULL) {
        s_speaker = bsp_audio_codec_speaker_init();
        if(s_speaker == NULL) {
            ESP_LOGE(TAG, "speaker init failed");
            return false;
        }
        if(esp_codec_dev_set_out_vol(s_speaker, AUDIO_VOLUME_PERCENT) != ESP_CODEC_DEV_OK) {
            ESP_LOGW(TAG, "failed to set speaker volume");
        }
    }

    if(s_audio_opened) {
        (void)esp_codec_dev_close(s_speaker);
        s_audio_opened = false;
    }

    if(esp_codec_dev_open(s_speaker, &fs) != ESP_CODEC_DEV_OK) {
        ESP_LOGE(TAG, "speaker open failed");
        return false;
    }
    s_audio_opened = true;
    s_audio_rate_hz = sample_rate_hz;
    s_audio_channels = channels;

    if(esp_codec_dev_set_out_mute(s_speaker, false) != ESP_CODEC_DEV_OK) {
        ESP_LOGW(TAG, "failed to unmute speaker");
    }

    return true;
}

static void close_audio_path(void)
{
    if(s_speaker != NULL) {
        (void)esp_codec_dev_set_out_mute(s_speaker, true);
        if(s_audio_opened) {
            (void)esp_codec_dev_close(s_speaker);
            s_audio_opened = false;
        }
    }
}

static void write_silence_ms(int ms)
{
    int16_t chunk[AUDIO_CHUNK_SAMPLES] = {0};
    int rate_hz = s_audio_rate_hz > 0 ? s_audio_rate_hz : AUDIO_SAMPLE_RATE_HZ;
    int samples_left = (rate_hz * ms) / 1000;

    while(samples_left > 0 && !s_stop_requested) {
        int n = samples_left > AUDIO_CHUNK_SAMPLES ? AUDIO_CHUNK_SAMPLES : samples_left;
        (void)esp_codec_dev_write(s_speaker, chunk, n * (int)sizeof(int16_t));
        samples_left -= n;
    }
}

static bool play_wav_file(const char *path)
{
    FILE *f;
    uint8_t buf[1024];
    uint8_t header[44];
    bool played_any = false;
    int sample_rate_hz = AUDIO_SAMPLE_RATE_HZ;
    int channels = AUDIO_CHANNELS;
    int bits_per_sample = AUDIO_BITS_PER_SAMPLE;

    if(!s_spiffs_ready || path == NULL) {
        return false;
    }

    f = fopen(path, "rb");
    if(f == NULL) {
        return false;
    }

    if(fread(header, 1, sizeof(header), f) == sizeof(header)) {
        if(memcmp(header, "RIFF", 4) == 0 && memcmp(&header[8], "WAVE", 4) == 0) {
            channels = (int)(header[22] | (header[23] << 8));
            sample_rate_hz = (int)(header[24] | (header[25] << 8) | (header[26] << 16) | (header[27] << 24));
            bits_per_sample = (int)(header[34] | (header[35] << 8));
        } else {
            // Not a canonical WAV header: restart and play as raw PCM with default format.
            (void)fseek(f, 0, SEEK_SET);
        }
    } else {
        fclose(f);
        return false;
    }

    if(bits_per_sample != 16) {
        ESP_LOGW(TAG, "Unsupported WAV depth (%d): %s", bits_per_sample, path);
        fclose(f);
        return false;
    }

    if(!open_audio_path(sample_rate_hz, channels)) {
        fclose(f);
        return false;
    }

    while(!s_stop_requested) {
        size_t n = fread(buf, 1, sizeof(buf), f);
        if(n == 0) {
            break;
        }
        played_any = true;
        (void)esp_codec_dev_write(s_speaker, buf, (int)n);
    }

    fclose(f);
    return played_any;
}

static bool play_alert_once(const alert_profile_t *profile)
{
    ESP_LOGI(TAG, "VOICE SCRIPT: %s", profile->voice_text);

    if(!play_wav_file(profile->wav_path)) {
        ESP_LOGW(TAG, "Missing voice file: %s", profile->wav_path ? profile->wav_path : "(null)");
        write_silence_ms(250);
        return false;
    }

    return true;
}

static void audio_task(void *arg)
{
    (void)arg;

    while(1) {
        ulTaskNotifyTake(pdTRUE, portMAX_DELAY);

        if(s_requested_alert == NURSE_ALERT_NONE) {
            continue;
        }

        while(!s_stop_requested && s_requested_alert != NURSE_ALERT_NONE) {
            const alert_profile_t *profile = get_profile(s_requested_alert);
            if(!play_alert_once(profile)) {
                // Missing/unreadable file: stop looping to avoid log spam.
                s_stop_requested = true;
                break;
            }
            write_silence_ms(400);
        }

        close_audio_path();
        s_requested_alert = NURSE_ALERT_NONE;
        s_stop_requested = false;
    }
}

void nurse_audio_init(void)
{
    if(s_audio_task != NULL) {
        return;
    }

    ensure_spiffs_mounted();
    xTaskCreate(audio_task, "nurse_audio", 4096, NULL, 4, &s_audio_task);
}

void nurse_audio_start(nurse_alert_type_t alert_type)
{
    nurse_audio_init();
    s_requested_alert = alert_type;
    s_stop_requested = false;
    if(s_audio_task != NULL) {
        xTaskNotifyGive(s_audio_task);
    }
}

void nurse_audio_stop(void)
{
    s_stop_requested = true;
    s_requested_alert = NURSE_ALERT_NONE;
    if(s_speaker != NULL) {
        (void)esp_codec_dev_set_out_mute(s_speaker, true);
    }
}