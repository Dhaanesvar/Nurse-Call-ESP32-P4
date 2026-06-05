#ifndef ALERT_AUDIO_H
#define ALERT_AUDIO_H

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    NURSE_ALERT_NONE = 0,
    NURSE_ALERT_EMERGENCY,
    NURSE_ALERT_CALL,
    NURSE_ALERT_CODE_BLUE,
    NURSE_ALERT_TOILET_EMERGENCY,
} nurse_alert_type_t;

void nurse_audio_init(void);
void nurse_audio_start(nurse_alert_type_t alert_type);
void nurse_audio_stop(void);

#ifdef __cplusplus
}
#endif

#endif