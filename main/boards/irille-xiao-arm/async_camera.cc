#include "async_camera.h"

#include <esp_log.h>
#include <esp_timer.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

#include <cstdio>
#include <stdexcept>

#include "config.h"

#define TAG "AsyncCamera"

namespace {

// worker 只跑 EspVideo::Explain（JPEG 编码另起 std::thread，不占这里的栈）。
// 主任务跑同一段代码时实测 remain stack = 5,808 B，这里给足余量并在收尾打水位。
constexpr uint32_t kWorkerStack = 10240;
constexpr UBaseType_t kWorkerPrio = 3;

constexpr const char* kAcceptedJson =
    "{\"success\":true,\"result\":\"Image captured; analysis in progress - call "
    "self.camera.explain_result in a second or two to get it. Keep polling: a long answer "
    "can take a while, and the result is never dropped for being slow.\"}";

constexpr const char* kNoJobJson =
    "{\"success\":false,\"result\":\"No analysis pending. Call self.camera.take_photo first.\"}";

uint32_t NowMs() {
    return (uint32_t)(esp_timer_get_time() / 1000);
}

// 异常文本进 JSON 前必须转义：它来自 what()，可能带引号或换行，直接拼会拼出坏 JSON。
std::string ErrorJson(const std::string& what) {
    std::string out = "{\"success\":false,\"result\":\"error: ";
    for (char c : what) {
        switch (c) {
            case '"':  out += "\\\""; break;
            case '\\': out += "\\\\"; break;
            case '\n': out += "\\n";  break;
            case '\r': out += "\\r";  break;
            case '\t': out += "\\t";  break;
            default:
                if ((unsigned char)c < 0x20) {
                    out += ' ';
                } else {
                    out += c;
                }
        }
    }
    out += "\"}";
    return out;
}

}  // namespace

bool AsyncExplainCamera::Capture() {
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (worker_alive_) {
            uint32_t age_s = (NowMs() - started_at_ms_) / 1000;
            if (age_s >= CAMERA_EXPLAIN_STALE_S) {
                // 判为陈旧：多半是服务端失联、worker 卡在无超时的 socket 读上。
                // 能做的是让它的结果作废——自增 job_id_，worker 回来时对不上号即丢弃，
                // 不会把这份迟到的说明喂给下一次提问。
                // **做不到**的是立刻开新任务：EspVideo::Explain 用成员 encoder_thread_，
                // 旧 worker 未退出时再进一次会对 joinable 的 std::thread 赋值 → abort。
                if (state_ != JobState::kIdle) {
                    ESP_LOGW(TAG, "job %u stale after %u s, its result will be discarded",
                             (unsigned)job_id_, (unsigned)age_s);
                    job_id_++;
                    state_ = JobState::kIdle;
                    result_.clear();
                }
            }
            ESP_LOGW(TAG, "capture rejected: previous analysis still running (%u s)",
                     (unsigned)age_s);
            return false;
        }
        // 上一次的结果还没被取走：agent 既然又拍了，那份结果没人要了，直接丢弃——
        // 留着它会让下一次 explain_result 取到**上一张**图的说明，张冠李戴。
        state_ = JobState::kIdle;
        result_.clear();
    }
    return EspVideo::Capture();
}

std::string AsyncExplainCamera::Explain(const std::string& question) {
    std::lock_guard<std::mutex> lock(mutex_);
    // Capture() 已把 busy 挡在门外，走到这里必定没有活着的 worker。
    question_ = question;
    result_.clear();
    started_at_ms_ = NowMs();
    state_ = JobState::kRunning;
    worker_alive_ = true;

    if (xTaskCreate(WorkerEntry, "cam_explain", kWorkerStack, this, kWorkerPrio, nullptr) != pdPASS) {
        state_ = JobState::kIdle;
        worker_alive_ = false;
        ESP_LOGE(TAG, "failed to create explain worker task");
        throw std::runtime_error("Failed to start image analysis");
    }
    return kAcceptedJson;
}

void AsyncExplainCamera::WorkerEntry(void* arg) {
    auto* self = static_cast<AsyncExplainCamera*>(arg);
    uint32_t job_id;
    {
        std::lock_guard<std::mutex> lock(self->mutex_);
        job_id = self->job_id_;
    }
    self->RunJob(job_id);
    vTaskDelete(nullptr);
}

void AsyncExplainCamera::RunJob(uint32_t job_id) {
    std::string question;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        question = question_;
    }

    std::string payload;
    bool ok = true;
    try {
        // 抓帧已在主任务做过，这里是编码 + 上传 + 等应答——那 4.2 s 就在这一行里。
        payload = EspVideo::Explain(question);
    } catch (const std::exception& e) {
        ok = false;
        payload = ErrorJson(e.what());
    }

    ESP_LOGI(TAG, "explain job %u finished (ok=%d) after %u s, worker stack high water = %u B",
             (unsigned)job_id, (int)ok, (unsigned)((NowMs() - started_at_ms_) / 1000),
             (unsigned)uxTaskGetStackHighWaterMark(nullptr));

    std::lock_guard<std::mutex> lock(mutex_);
    worker_alive_ = false;
    if (job_id != job_id_) {
        ESP_LOGW(TAG, "result of job %u discarded: superseded (current %u)",
                 (unsigned)job_id, (unsigned)job_id_);
        return;   // 已判陈旧，state_ 早已回 kIdle，不要改它
    }
    result_ = payload;
    state_ = ok ? JobState::kDone : JobState::kFailed;
}

std::string AsyncExplainCamera::PollResult() {
    std::lock_guard<std::mutex> lock(mutex_);
    switch (state_) {
        case JobState::kIdle:
            return kNoJobJson;

        case JobState::kRunning: {
            // **永不按时间作废**：想久一点是正常的，如实报已等待多久、让 agent 再来问。
            char buf[224];
            std::snprintf(buf, sizeof buf,
                          "{\"success\":false,\"result\":\"pending - still analyzing (waited %u s). "
                          "Ask again in a second or two; the result is kept until it arrives.\"}",
                          (unsigned)((NowMs() - started_at_ms_) / 1000));
            return buf;
        }

        case JobState::kDone:
        case JobState::kFailed: {
            std::string out = result_;
            result_.clear();
            state_ = JobState::kIdle;
            return out;
        }
    }
    return kNoJobJson;
}
