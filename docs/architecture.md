# 架构设计

## 1. 目标模型

真实链路（示意）：

```
┌──────────────┐   虚拟 PCM    ┌──────────────┐   共享内存    ┌──────────────┐
│  App         │  write/poll   │  PVM Linux   │  ring buffer  │  DSP         │
│  (Android)   │ ────────────► │  (宿主/路由)  │ ────────────► │  (算法核)     │
└──────────────┘               └──────────────┘               └──────────────┘
        ▲                              ▲                              ▲
        └────── 控制消息（set_hw_params / start / stop / drain）────────┘
```

三个进程各自模拟一个参与者：

| 进程 | 角色 | 关键行为 |
|---|---|---|
| `gvm_app` | Android 应用 + 虚拟声卡客户端 | 读音乐文件、`open/set_hw_params/write/poll`、感知 `hw_ptr` 与背压 |
| `pvm_router` | Linux 宿主 / 路由层 | 接收 `set_hw_params`，创建共享内存段，转发音频与状态 |
| `dsp_engine` | DSP 算法核 | 按自己的时钟消费，跑算法，回报统计 |

**为什么中间要隔一个 PVM**：真实系统里 Android 在虚拟机里，DSP 是另一个核，
两者不能直接共享内存，必须由宿主转接。仿真保留这一跳，才能体现"两段独立时钟"。

## 2. 数据流与两段 ring

```
[GVM] --ring A--> [PVM] --ring B--> [DSP]
```

- **ring A**（虚拟 PCM）：GVM 是生产者，PVM 是消费者。这一段承载 `appl_ptr`/`hw_ptr` 语义。
- **ring B**（IPC 音频）：PVM 是生产者，DSP 是消费者。这一段体现跨核传输。

两段 ring 的几何参数由 `set_hw_params` 决定：`period_frames`、`period_count`、
`channels`、`rate`、`sample_bytes`。PVM 收到后创建 ring B 并把名字回传给 DSP。

> 简化点：第一版让 PVM 直接把 ring A 的数据搬到 ring B（memcpy），不做重采样。
> 真实系统里这里可能有格式转换、混音、路由矩阵。

## 3. ring buffer 契约（已实现）

见 `include/mcshm_ring.h`。核心不变量：

1. `write_frames` 单调不减，只有生产者写；`read_frames` 单调不减，只有消费者写。
2. `0 <= write_frames - read_frames <= ring_frames` 永远成立。
3. 写指针用 `release` 发布，读指针用 `release` 发布；对侧用 `acquire` 读取后
   才访问载荷，因此数据必须先落盘后发布。
4. 索引 `head % ring_frames`，跨越末尾时拆成两段 memcpy。
5. 单生产者单消费者：不允许多个写者，也不允许多个读者。

**错误语义**：`write` 返回实际写入帧数（0 = 满），`read` 返回实际读取帧数（0 = 空）。
调用者必须处理部分传输，不能假设一次操作完成一个 period——这正是背压的来源。

## 4. PCM 设备模型（M3 计划）

模仿 ALSA，但只做 playback：

```
状态机:  SETUP -> PREPARED -> RUNNING -> (XRUN) -> PREPARED
                            \-> DRAINING -> SETUP
```

| 操作 | 语义 |
|---|---|
| `open` | 打开虚拟设备，分配句柄 |
| `set_hw_params` | 把 period 几何写入控制通道，触发 PVM 建段 |
| `prepare` | 清空 ring，指针归零 |
| `start` | 启动模拟时钟，开始推进 `hw_ptr` |
| `write` | 写入 ring；空间不足时阻塞（或 `-EAGAIN`，取决于 nonblock） |
| `poll` | `POLLOUT`：可写空间 ≥ 1 period；`POLLIN`：有数据可读 |
| `drain` | 等 ring 排空再返回 |
| `close` | 释放句柄，段按引用计数回收 |

**`hw_ptr` 只能由时钟/中断推进**，不能由 `write` 推进——这是最容易做错、也最能
体现理解深度的一点。

## 5. 时钟与背压（M3 计划）

- 模拟时钟用 `clock_nanosleep(CLOCK_MONOTONIC, TIMER_ABSTIME)`，按
  `period_frames / rate` 累加绝对时间点，**不做 `nanosleep(period)` 的累加**，
  否则误差会线性累积成漂移。
- 每个 period 边界触发一次"中断"：推进 `hw_ptr`、唤醒 `poll` 等待者、更新统计。
- 背压：ring 满时 `write` 阻塞在条件变量/`poll` 上，等 `hw_ptr` 前进腾出空间。
- xrun：`hw_ptr` 追上 `write_ptr` 即 underrun，报告并进入恢复流程。

## 6. 真 ALSA 抽象（预留）

计划中的接口（M3 落地）：

```c
struct mcpcm_ops {
    int  (*open)(void *ctx, const struct mcpcm_config *cfg);
    int  (*set_hw_params)(void *ctx, const struct mcpcm_config *cfg);
    int  (*prepare)(void *ctx);
    int  (*start)(void *ctx);
    long (*write)(void *ctx, const void *frames, unsigned long count);
    int  (*poll)(void *ctx, int timeout_ms, unsigned *revents);
    int  (*drain)(void *ctx);
    int  (*stop)(void *ctx);
    void (*close)(void *ctx);
};
```

- `mcpcm_shm`：本项目的仿真实现（基于 `mcshm_ring`）
- `mcpcm_alsa`：把同样的语义映射到 `snd_pcm_*`，用于有条件时对照

上层的 GVM 应用只依赖 `mcpcm_ops`，切换后端不修改业务代码。

## 7. 已知风险

| 风险 | 说明 | 应对 |
|---|---|---|
| 32 通道 | `mini_dsp` 的 biquad 仅单声道、NS 上限 8 通道 | M4 时改造成每声道状态 |
| 960 帧 period | `mini_dsp` 的 FFT 固定 256 点，960 不是其整数倍 | 运行时 FFT 尺寸，或 960 = 4×240 分帧 |
| 格式边界 | shm 里是 `S16_LE`，算法是 float | 只在 DSP 入口做一次转换，全链路只一个转换点 |
| 时间精度 | 主机不是实时系统 | 统计 jitter 而非承诺硬实时，文档里写清边界 |
