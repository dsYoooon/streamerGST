#pragma once

#include <string>

namespace GStreamerWorker {

enum class WorkerState {
    Idle,
    Starting,
    Running,
    Stopping,
    Error
};

inline const char* ToString(WorkerState state) {
    switch (state) {
    case WorkerState::Idle: return "Idle";
    case WorkerState::Starting: return "Starting";
    case WorkerState::Running: return "Running";
    case WorkerState::Stopping: return "Stopping";
    case WorkerState::Error: return "Error";
    }
    return "Unknown";
}

struct WorkerStatus {
    WorkerState state = WorkerState::Idle;
    std::string message;
};

} // namespace GStreamerWorker
