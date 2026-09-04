#ifndef IRILLE_XIAO_ARM_LINK_H_
#define IRILLE_XIAO_ARM_LINK_H_

// 机械臂串口链路的 **IO 与任务胶水**。
//
// 本文件只做三件事：配 UART、跑唯一的 RX owner 任务、把决策器的输出写出去。
// **不含任何判断**——发不发、发什么、几次算成功、终态归谁，全在 arm_link_fsm。
// 那条边界就是可测边界：留在这里的判断只能上板才能测，而上板测的是 25kg 舵机。

#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>
#include <freertos/task.h>

#include "arm_link_fsm.h"

// 供 self.arm.status 序列化用的快照。取快照时持锁，之后不再碰 fsm。
struct ArmStatusSnapshot {
    int         position_known;
    const char* op_state;      // 静态字符串
    uint32_t    op_id;
    int32_t     op_max_ms;     // -1 = 未知
    ArmCode     op_code;       // ARM_CODE_NONE = 无失败原因
    int         has_pos;
    int         joints[6];
    int         arm_moving;
    int         fw_major;
    int         fw_minor;
    int         collision_guard;
    // 下面四个是给**没有串口日志时**的验收断言用的：12V 通电时 USB 必须拔掉
    // （XIAO 5V 脚与 USB 无二极管隔离），「捕获 READY」「复位判定」「RX 丢弃」
    // 这些原本靠 ESP_LOG 观测的项，只能从 status 里读。
    const char* phase;         // 链路阶段
    uint32_t    link_epoch;    // 每见一次 READY +1 ⇒ 对端复位次数
    int         ready_seen;    // 本纪元见过 READY（区分"没见过"与"见过在等 DONE"）
    uint32_t    rx_dropped;    // 撕裂/超长而整行作废的计数，不静默丢
    uint32_t    rx_bytes;      // 串口收到的总字节数（0 = 对端一个字节都没来）
    uint32_t    rx_malformed;  // 凑成整行但不合协议格式的计数
    const char* boot_verdict;  // boot 验证的判决现场（诊断用，见 arm_link_fsm.h）
    const char* last_st;       // 下位机最近一次回报的原始 ST 值（诊断用）
    const char* last_att;      // 同上，原始 ATT 值
};

class ArmLink {
 public:
    ArmLink();

    // 装 UART 并起 RX owner 任务。调用后进入上电归位窗口——此窗口内零下行。
    void Start();

    // 工具调用入口：阻塞到**即时受理应答**为止（上界约 1.4s），不等终态。
    ArmReply Request(const ArmRequest& req);

    void Snapshot(ArmStatusSnapshot* out);

 private:
    uint32_t rx_dropped_ = 0;   // 只在 RX owner 任务里自增，Snapshot 持锁读
    uint32_t rx_bytes_ = 0;     // 同上

    static void RxOwnerTrampoline(void* arg);
    void RxOwnerLoop();

    // 执行决策器的输出。持锁调用。
    void Execute(ArmDecision d);

    // 把一行喂给决策器并执行其决策。**两条喂行路径的唯一入口**——世代取样的纪律
    // 只在这里体现一次，免得两处各写一套（曾经就漂过：一处传接收时刻快照、
    // 一处传处理时刻的当前值，而后者等于让认领条件恒真）。持锁调用。
    void FeedLine(const char* line, uint32_t rx_generation);

    // 排空串口缓冲里已到达的行并回喂决策器。清场用，持锁调用。
    // rx_generation 由调用方在排空**之前**取样——这些行都是在那之前到达的。
    void DrainPending(uint32_t rx_generation);

    // 交付一个回复给正在等待的工具调用。持锁调用。
    void PostReply(const ArmReply& r);

    // 从 UART 读一整行（到 '\n' 为止）。返回行长，超时返回 0，超长行返回 -1。
    int ReadLine(char* out, int cap, int timeout_ms);

    ArmFsm            fsm_;
    SemaphoreHandle_t mutex_ = nullptr;
    SemaphoreHandle_t reply_sem_ = nullptr;
    ArmReply          pending_reply_ = {};
    TaskHandle_t      rx_task_ = nullptr;
    bool              started_ = false;
};

#endif  // IRILLE_XIAO_ARM_LINK_H_
