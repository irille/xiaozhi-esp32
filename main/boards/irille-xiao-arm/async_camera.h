#ifndef IRILLE_XIAO_ARM_ASYNC_CAMERA_H_
#define IRILLE_XIAO_ARM_ASYNC_CAMERA_H_

// 继承工具 self.camera.take_photo 的主循环占用治理（宪法 III.2 继承工具条款）。
//
// 上游 mcp_server.cc:111 的 take_photo 回调经 app.Schedule() 在 Application 主任务里
// **同步**跑完 Capture() + Explain()。T035a 实测三次：4,370 / 5,050 / 4,980 ms，
// 而本板的即时应答上界是 1.4 s。拆开看，抓帧 ~120 ms、连接 ~10 ms，**其余 4.2 s
// 几乎全是服务端 VLLM 推理**——21~23 KB 的图在 LAN 上传输是毫秒级，所以「先降配置」
// 那一档在这里降不动：分辨率砍到 320×240 也省不下推理时间。
//
// 于是把 Explain 改成异步派发：主循环只承担抓帧 + 入队，推理在独立任务里等，
// 结果由 self.camera.explain_result 取。**core 一行不改**，全部落在 board 目录。
//
// 主循环占用的实测口径也随之改变：以「抓帧 + 派发」计，推理不在主循环。
//
// **结果不按时间作废**：大模型思考 30–60 s 属正常，结果何时到就何时能取，
// explain_result 在此之前一律回 pending + 已等待秒数。唯一的时间常数是
// CAMERA_EXPLAIN_STALE_S（config.h，默认 90 s），语义只有一条：新的 take_photo 到来
// 而旧任务仍在跑时，旧任务年龄 ≥ 该值即判为陈旧、其结果作废（job id 不匹配即丢）。
//
// ⚠️ 两条诚实的残留：
//   1. 上游 HTTP 走 http_client.cc + esp_tcp.cc，全链路没设过 socket 接收超时——
//      服务端接了连接却不回时，那个读是无界阻塞。board 层治不了（要动 core），但它
//      阻塞的是 worker 不是主循环，**self.arm.stop 不再被挡**，这才是那条上界要保的。
//   2. 因此「陈旧后允许新任务」只能做到**作废旧结果**，做不到**立刻开新任务**：
//      EspVideo::Explain 用成员 encoder_thread_，在旧 worker 还没退出时再进一次会对
//      joinable 的 std::thread 赋值 → std::terminate。所以卡死期间 take_photo 仍如实
//      回「相机忙」，只是不再把那份迟到结果喂给下一次提问。

#include <mutex>
#include <string>

#include "esp_video.h"

class AsyncExplainCamera : public EspVideo {
 public:
    using EspVideo::EspVideo;

    // 兼作准入闸：worker 还在跑就不能再抓帧——那会覆盖基类的 frame_，而 worker
    // 正在读它；更要命的是再进一次 Explain 会对 joinable 的成员线程赋值直接 abort。
    bool Capture() override;

    // 抓帧已在主任务完成；这里只把编码+上传+等应答交给 worker，立即返回。
    std::string Explain(const std::string& question) override;

    // self.camera.explain_result 的实现：pending（带已等待秒数）/ 结果 / 错误。
    std::string PollResult();

 private:
    enum class JobState { kIdle, kRunning, kDone, kFailed };

    static void WorkerEntry(void* arg);
    void RunJob(uint32_t job_id);

    std::mutex mutex_;
    JobState state_ = JobState::kIdle;
    bool worker_alive_ = false;  // worker 任务是否还存在（只有它能进基类 Explain）
    uint32_t job_id_ = 0;        // 判陈旧后自增：worker 回来时对不上号即丢弃结果
    std::string question_;
    std::string result_;
    uint32_t started_at_ms_ = 0;
};

#endif  // IRILLE_XIAO_ARM_ASYNC_CAMERA_H_
