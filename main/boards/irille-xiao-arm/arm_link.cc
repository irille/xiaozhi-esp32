#include "arm_link.h"

#include <driver/uart.h>
#include <esp_log.h>
#include <esp_timer.h>

#include <cstdio>
#include <cstring>

#include "config.h"

#define TAG "ArmLink"

namespace {

// 工具调用的总等待上界，**由 config.h 推导而不是冻成字面量**：它是决策器那几个
// 超时的函数，改大 ACK 超时或重试次数时必须跟着走，否则这条兜底会从"永不触发的
// 保险"变成主路径，所有请求都误报 LINK。
constexpr int kRequestTimeoutMs =
    ARM_ACK_TIMEOUT_MS * ARM_MAX_ATTEMPTS + ARM_STOP_ACK_WAIT_MS + 1000;
constexpr int kRxPollMs = 100;
// 排空的循环上限：只为防止串口持续来数据时卡住本轮，不是业务约束。
constexpr int kDrainMaxLines = 16;
// 排空时单行的读取期限。9600 8N1 下一行最长约 55 ms（51 字节），给 80 ms 有余。
constexpr int kDrainLineTimeoutMs = 80;

uint32_t NowMs() {
    return static_cast<uint32_t>(esp_timer_get_time() / 1000);
}

}  // namespace

ArmLink::ArmLink() {
    ArmFsmConfig cfg = {};
    cfg.ack_timeout_ms = ARM_ACK_TIMEOUT_MS;
    cfg.done_grace_ms = ARM_DONE_GRACE_MS;
    cfg.fallback_deadline_ms = ARM_FALLBACK_DEADLINE_MS;
    cfg.boot_window_ms = ARM_BOOT_HOME_WINDOW_MS;
    cfg.stop_ack_wait_ms = ARM_STOP_ACK_WAIT_MS;
    cfg.max_attempts = ARM_MAX_ATTEMPTS;
    arm_fsm_init(&fsm_, &cfg);

    mutex_ = xSemaphoreCreateMutex();
    reply_sem_ = xSemaphoreCreateBinary();
}

void ArmLink::Start() {
    if (started_) return;

    uart_config_t cfg = {};
    cfg.baud_rate = ARM_UART_BAUD;
    cfg.data_bits = UART_DATA_8_BITS;
    cfg.parity = UART_PARITY_DISABLE;
    cfg.stop_bits = UART_STOP_BITS_1;
    cfg.flow_ctrl = UART_HW_FLOWCTRL_DISABLE;
    cfg.source_clk = UART_SCLK_DEFAULT;

    // 先把 TX 拉高再交给 UART：悬空的 TX 在 driver 装上前可能被下位机读成起始位。
    gpio_reset_pin(ARM_UART_TX_GPIO);
    gpio_set_direction(ARM_UART_TX_GPIO, GPIO_MODE_OUTPUT);
    gpio_set_level(ARM_UART_TX_GPIO, 1);

    ESP_ERROR_CHECK(uart_driver_install(ARM_UART_PORT, 256, 256, 0, nullptr, 0));
    ESP_ERROR_CHECK(uart_param_config(ARM_UART_PORT, &cfg));
    ESP_ERROR_CHECK(uart_set_pin(ARM_UART_PORT, ARM_UART_TX_GPIO, ARM_UART_RX_GPIO,
                                 UART_PIN_NO_CHANGE, UART_PIN_NO_CHANGE));

    // ⚠️ 开机**不发**任何东西（含 STOP）。12V 同时上电时，任何指令都会落在下位机的
    // 自动归位窗口里，把归位打断成 detach → RELAXED（治具 2026-09-01 实测复现）。
    // 决策器此刻处于 WAIT_BOOT_DONE，本就零下行；这里只把窗口起点对齐到真实时钟。
    xSemaphoreTake(mutex_, portMAX_DELAY);
    fsm_.boot_window_start_ms = NowMs();
    xSemaphoreGive(mutex_);

    xTaskCreate(&ArmLink::RxOwnerTrampoline, "arm_rx", 4096, this, 5, &rx_task_);
    started_ = true;
    ESP_LOGI(TAG, "arm link up on UART%d (TX=%d RX=%d @%d)", ARM_UART_PORT,
             ARM_UART_TX_GPIO, ARM_UART_RX_GPIO, ARM_UART_BAUD);
}

void ArmLink::RxOwnerTrampoline(void* arg) {
    static_cast<ArmLink*>(arg)->RxOwnerLoop();
}

// 返回行长；超时返回 0；**超长行返回 -1（整行丢弃）**。
// 截断后再交给决策器是危险的：一条被截成 "DONE" 的长行会被认成动作完成。
int ArmLink::ReadLine(char* out, int cap, int timeout_ms) {
    int len = 0;
    bool overflow = false;
    TickType_t deadline = xTaskGetTickCount() + pdMS_TO_TICKS(timeout_ms);
    for (;;) {
        uint8_t ch = 0;
        TickType_t left = deadline - xTaskGetTickCount();
        if ((int32_t)left <= 0) return 0;
        if (uart_read_bytes(ARM_UART_PORT, &ch, 1, left) != 1) return 0;
        if (ch == '\n') {
            if (overflow) {
                ESP_LOGW(TAG, "rx: oversized line dropped");
                return -1;
            }
            out[len] = '\0';
            return len;
        }
        if (ch == '\r') continue;
        if (len < cap - 1) {
            out[len++] = (char)ch;
        } else {
            overflow = true;  // 继续吞到换行为止，但整行作废
        }
    }
}

// 唯一的 RX owner：只有这里读串口。READY 走事件通道不入应答队列——它不是任何
// 请求的应答，若入队会被下一次请求当成自己的回复读走。
void ArmLink::RxOwnerLoop() {
    char line[ARM_FSM_LINE_MAX];
    for (;;) {
        // **接收时刻**的世代快照：在开始等这一行之前取样。用处理时刻的当前值
        // 取样等于恒真，终态认领的第五条就形同虚设——迟到的旧终态会被新操作冒领。
        xSemaphoreTake(mutex_, portMAX_DELAY);
        uint32_t rx_gen = fsm_.operation_generation;
        xSemaphoreGive(mutex_);

        int n = ReadLine(line, sizeof line, kRxPollMs);

        xSemaphoreTake(mutex_, portMAX_DELAY);
        if (n > 0) {
            ESP_LOGD(TAG, "rx: %s", line);
            FeedLine(line, rx_gen);
        }
        // 每轮都推一次时钟：重发、期限核对、上电窗口超时都靠它。
        Execute(arm_fsm_on_tick(&fsm_, NowMs()));
        xSemaphoreGive(mutex_);
    }
}

void ArmLink::FeedLine(const char* line, uint32_t rx_generation) {
    Execute(arm_fsm_on_line(&fsm_, line, rx_generation, NowMs()));
}

void ArmLink::PostReply(const ArmReply& r) {
    pending_reply_ = r;
    xSemaphoreGive(reply_sem_);
}

// 把已经躺在串口缓冲里的行全部读完并回喂。清场用：旧操作的终态就在里面，
// 必须在 STATUS 回复之前被消费掉，否则它会在清场之后才冒出来、被下一个操作冒领。
// 持锁调用，非阻塞读。
void ArmLink::DrainPending(uint32_t rx_generation) {
    char line[ARM_FSM_LINE_MAX];
    for (int guard = 0; guard < kDrainMaxLines; ++guard) {
        // 先看缓冲里到底有没有数据。ReadLine 的超时是**整行**的期限，传 0 会在
        // 读第一个字节之前就到期返回——那样一行也排不掉，等于没排。
        size_t avail = 0;
        if (uart_get_buffered_data_len(ARM_UART_PORT, &avail) != ESP_OK || avail == 0) {
            break;
        }
        int n = ReadLine(line, sizeof line, kDrainLineTimeoutMs);
        if (n == 0) break;      // 有字节但凑不成整行：剩下的留给 RX owner
        if (n < 0) continue;    // 超长行已整行丢弃，继续排
        ESP_LOGD(TAG, "drain: %s", line);
        FeedLine(line, rx_generation);
    }
}

void ArmLink::Execute(ArmDecision d) {
    if (d.action == ARM_ACT_DRAIN_AND_SEND) {
        // 这些行都是在排空开始**之前**到达的，用那一刻的世代取样。
        uint32_t rx_gen = fsm_.operation_generation;
        DrainPending(rx_gen);
        // 排空可能已经改变了状态（收到 READY 或那条终态）——**不能**继续执行排空前
        // 的旧决策，否则可能在 WAIT_BOOT_DONE 下发出 STATUS、破坏零下行。重新问一次。
        d = arm_fsm_on_tick(&fsm_, NowMs());
        // "只排空一次"是决策器的策略（quiesce_probe_sent 在保证），这里只做防御，
        // 不让 IO 层递归。
        if (d.action == ARM_ACT_DRAIN_AND_SEND) d.action = ARM_ACT_SEND;
    }

    switch (d.action) {
        case ARM_ACT_SEND: {
            char buf[ARM_FSM_LINE_MAX + 2];
            int n = std::snprintf(buf, sizeof buf, "%s\n", d.line);
            uart_write_bytes(ARM_UART_PORT, buf, n);
            ESP_LOGD(TAG, "tx: %s", d.line);
            break;
        }
        case ARM_ACT_SEND_STOP: {
            // 三连发：下位机发送期间关中断会丢起始位，连发把最坏延迟从约 300ms
            // 压到一个字节时间。下位机对 STOP 幂等。
            // 一次写入即可：TX 环形缓冲 256B，而 9600 8N1 下一帧 "STOP\n" 要 5.2ms
            // ——三帧在第一帧排空前就已入队，线上本来就是连续 15 字节。原先那个
            // vTaskDelay(5ms) 在 FREERTOS_HZ=100 下算出来是 0 tick，不产生任何间隔，
            // 只是在持锁状态下白做两次调度切换，而 RX owner 正等着这把锁去收 ACK。
            char burst[5 * ARM_STOP_REPEAT];
            for (int i = 0; i < ARM_STOP_REPEAT; ++i) {
                std::memcpy(burst + i * 5, "STOP\n", 5);
            }
            uart_write_bytes(ARM_UART_PORT, burst, sizeof burst);
            ESP_LOGI(TAG, "tx: STOP x%d", ARM_STOP_REPEAT);
            break;
        }
        case ARM_ACT_REPLY:
            PostReply(d.reply);
            break;
        case ARM_ACT_DRAIN_AND_SEND:  // 上面已降级，不会走到
        case ARM_ACT_NONE:
        default:
            break;
    }
}

ArmReply ArmLink::Request(const ArmRequest& req) {
    xSemaphoreTake(mutex_, portMAX_DELAY);
    // 清掉可能残留的一次 give（上一轮超时后迟到的 REPLY）
    xSemaphoreTake(reply_sem_, 0);
    ArmDecision d = arm_fsm_on_request(&fsm_, &req, NowMs());
    if (d.action == ARM_ACT_REPLY) {
        ArmReply r = d.reply;
        xSemaphoreGive(mutex_);
        return r;   // 本地即可裁定（互斥、锁定态、缓存状态）
    }
    Execute(d);
    xSemaphoreGive(mutex_);

    if (xSemaphoreTake(reply_sem_, pdMS_TO_TICKS(kRequestTimeoutMs)) == pdTRUE) {
        return pending_reply_;
    }

    // 兜底：决策器的重发与超时逻辑应当在上界内产生 REPLY，走到这里说明 RX owner
    // 任务出了问题。如实报链路故障，绝不谎报成功。
    ESP_LOGE(TAG, "request timed out waiting for decision reply");
    ArmReply r = {};
    r.ok = 0;
    r.code = ARM_CODE_LINK;
    r.max_ms = -1;
    return r;
}

void ArmLink::Snapshot(ArmStatusSnapshot* out) {
    xSemaphoreTake(mutex_, portMAX_DELAY);
    out->position_known = fsm_.position_known;
    out->op_state = arm_fsm_op_state_name(fsm_.op_state);
    out->op_id = fsm_.op_id;
    out->op_max_ms = fsm_.op_max_ms;
    out->op_code = fsm_.op_code;
    out->has_pos = fsm_.has_pos;
    std::memcpy(out->joints, fsm_.joints, sizeof out->joints);
    out->arm_moving = fsm_.arm_moving;
    out->fw_major = fsm_.fw_major;
    out->fw_minor = fsm_.fw_minor;
    out->collision_guard = fsm_.collision_guard;
    xSemaphoreGive(mutex_);
}
