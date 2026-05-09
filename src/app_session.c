#include "app_session.h"

#include "app_rtsa.h"

esp_err_t app_session_start(void)
{
    return ESP_OK;
}

esp_err_t app_session_send_audio(const void *data, size_t size, uint32_t pts)
{
    return app_rtsa_send_audio(data, size, pts);
}

bool app_session_is_audio_send_ready(void)
{
    return app_rtsa_is_audio_send_ready();
}

esp_err_t app_session_set_audio_receive_callbacks(app_session_audio_info_cb_t info_cb,
                                                  app_session_audio_frame_cb_t frame_cb,
                                                  void *ctx)
{
    return app_rtsa_set_audio_receive_callbacks(info_cb, frame_cb, ctx);
}
