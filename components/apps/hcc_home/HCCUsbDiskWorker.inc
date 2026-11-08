void worker(void *arg)
{
    const bool start = arg != nullptr;
    if (start) {
        if (doStart()) {
            s_state.store(State::Ready);
        } else {
            if (s_sd_returned) s_sd_lease.release();
            // Publish Error only after ownership cleanup, so a new request
            // cannot race this worker's lease release.
            s_state.store(State::Error);
        }
    } else {
        if (teardown()) {
            s_sd_lease.release();
            s_state.store(State::Off);
            ESP_LOGI(TAG, "USB DISK tắt, thẻ SD trả lại cho board");
        } else {
            // Keep ownership until a later stop/recovery successfully remounts.
            s_state.store(State::Error);
        }
    }
    vTaskDelete(nullptr);
}
