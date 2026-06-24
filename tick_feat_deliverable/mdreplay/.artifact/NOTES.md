回放系统。输入是booktick或者trade记录文件（json, csv...）。输出放在shm里。
配置输入为 auto 格式则自动识别。也许我应该选一个自动识别的库。

```toml
[input]
format   = "csv"                 # csv | json(csv 先实现,json 以后并列加)
dir      = "formatted_2h"        # 录制文件根目录
date     = "20260623"            # 回放哪一天
venue    = "okx"                 # okx | binance | both
symbols  = ["SOLUSDT"]           # 显式列表;或 ["all"] = 全 subset27
# 文件定位模板(占位符 {venue}/{symbol}/{date}/{ext}),换录制布局只改这里
file_pattern = "{venue}_swap_{symbol}_{date}.{ext}"

[replay]
speed = 0.0                      # 0=尽快(纯逻辑序,完全可复现) | 1.0=原速 |
N=N 倍速
loop  = false                    # 跑完是否从头循环(压测/长跑用)
# 可选:只回放一个时间窗(ns,留空=整天)
venue    = "okx"                 # okx | binance | both
symbols  = ["SOLUSDT"]           # 显式列表;或 ["all"] = 全 subset27
# 文件定位模板(占位符 {venue}/{symbol}/{date}/{ext}),换录制布局只改这里
file_pattern = "{venue}_swap_{symbol}_{date}.{ext}"

[replay]
speed = 0.0                      # 0=尽快(纯逻辑序,完全可复现) | 1.0=原速 | N=N 倍速
loop  = false                    # 跑完是否从头循环(压测/长跑用)
# 可选:只回放一个时间窗(ns,留空=整天)
# start_ns = 0
# end_ns   = 0

[output]
create      = true               # 生产者建段(shm_init);false=attach 既有段
name_suffix = ""                 # 段名后缀,如 "_test" → /shm_..._v2_test,
                                  #   测试时与实盘段隔离,不误写生产环境
on_full     = "block"            # block=背压等消费者(回放专属、保证不丢);
                                  #   drop=丢弃+计数告警(模拟实盘 venue RX 不可阻塞)

[scale]
# px/qty 要编码成 uint32 ×10^scale 落进 board/trade(见 board #81:value×10^scale 不得溢出 u32)
# 录制里是 double 价 → 必须给每 symbol 选好 scale,既保精度又不溢出
default_price = 2
default_qty   = 3
# 按 symbol 覆盖(高价/小数多的币):
# [scale.SOLUSDT]
# price = 2
# qty   = 2

[log]
level        = "info"            # trace|debug|info|warn|error(对应 spdlog)
progress_sec = 5                 # 每 N 秒打一次进度(已发条数 / 当前 ts / lapped 计数)
```
input太自由了。定死文件名称格式。需要支持指定时间范围。回放是不同币混合的，不应当指定币种。另外，也不需要指定交易所。

speed 不符合尝试，0 反而代表高速。

output 配置应该直接指定shm名称。去掉 on_full
