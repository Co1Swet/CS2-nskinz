#pragma once

#include "Core\AppState.hpp"

#include <Windows.h>

#include <atomic>
#include <thread>

namespace nskinzcs2
{
class MainWindow
{
public:
    explicit MainWindow(AppState& state, DWORD external_runtime_process_id = 0);
    ~MainWindow();

    void Start();
    void Stop();

private:
    void ThreadMain();
    void ThreadMainImpl();

    AppState& state_;
    DWORD external_runtime_process_id_ = 0;
    std::thread thread_;
    std::atomic_bool stop_requested_ {false};
};
}
