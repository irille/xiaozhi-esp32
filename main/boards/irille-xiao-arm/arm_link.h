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
    static void RxOwnerTrampoline(void* arg);
    void RxOwnerLoop();

    // 执行决策器的输出。持锁调用。
    void Execute(const ArmDecision& d);

    // 排空串口缓冲里已到达的行并回喂决策器。清场用，持锁调用。
    void DrainPending();

    // 从 UART 读一整行（到 '\n' 为止）。返回行长，超时返回 0。
    int ReadLine(char* out, int cap, int timeout_ms);

    ArmFsm            fsm_;
    SemaphoreHandle_t mutex_ = nullptr;
    SemaphoreHandle_t reply_sem_ = nullptr;
    ArmReply          pending_reply_ = {};
    TaskHandle_t      rx_task_ = nullptr;
    bool              started_ = false;
};

#endif  // IRILLE_XIAO_ARM_LINK_H_
