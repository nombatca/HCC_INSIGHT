bool requestStart()
{
    State previous = s_state.load();
    if (previous != State::Off && previous != State::Error) return false;
    if (!s_state.compare_exchange_strong(previous, State::Starting)) return false;
    if (!s_sd_lease.acquire()) {
        snprintf(s_error, sizeof(s_error), "SD đang bận hoặc chưa trả về board");
        s_state.store(previous);
        return false;
    }
    if (!launch(true)) {
        s_sd_lease.release();
        snprintf(s_error, sizeof(s_error), "Không tạo được task");
        s_state.store(State::Error);
        return false;
    }
    return true;
}

bool requestStop()
{
    State previous = s_state.load();
    if (previous != State::Ready && previous != State::Connected && previous != State::Error) return false;
    if (!s_state.compare_exchange_strong(previous, State::Stopping)) return false;
    const bool held = s_sd_lease.held();
    if (!held && !s_sd_lease.acquire()) {
        snprintf(s_error, sizeof(s_error), "SD đang được bộ nhận file sử dụng");
        s_state.store(previous);
        return false;
    }
    if (!launch(false)) {
        if (!held) s_sd_lease.release();
        snprintf(s_error, sizeof(s_error), "Không tạo được task");
        s_state.store(State::Error);
        return false;
    }
    return true;
}
