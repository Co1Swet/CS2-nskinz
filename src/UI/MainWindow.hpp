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
    explicit MainWindow(AppState& state);
    ~MainWindow();

    void Start();
    void Stop();

private:
    void ThreadMain();
    void ThreadMainImpl();

    AppState& state_;
    std::thread thread_;
    std::atomic_bool stop_requested_ {false};
};
}
