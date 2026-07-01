# mdreplay TODO — 易用性 / 功能候选

> 来源:`/pax --loop` 易用性演进的产出(已实现+验证过,但用户决定**先撤回、按需再做**)。
> 这些都已严守铁律「零上游耦合 / simple is best / 不过早抽象」做过 scope 把关。
> **动手前先确认要做哪条**;每条独立、可单独实现+验证(`bash test/run_all.sh` 风格全绿 + 字节基线)。

## 痛点背景
- 包发给别人后,**包里零数据**(`datas/` 1GB 是产物被 gitignore;导出器在父目录不进包),
  收件人开箱只能手写 CSV 才能跑 → 出盒即用性差。
- CLI 缺 `--version`、无 dry-run 校验、`--help` 无可复制示例;测试要逐个敲 8 条命令。

---

## P0 — 直击「开箱跑不通」

### 1. 提交微型示例数据 + 即跑 config(`examples/`)
- **做什么**:`examples/datas/*.{book,trade}.csv`(合成、带 `update_id`、~30 行,总 <30KB)+ `examples/config.toml`,
  让人 `mdreplay --config examples/config.toml --realtime 0` 一行跑通(book+trade 两路),不必手写 CSV 或拿 1GB 产物。
- **注意**:父级 `tick_feat_deliverable/.gitignore` 有 `*.csv`,需在 `mdreplay/.gitignore` 加负向规则
  `!/examples/` `!/examples/datas/` `!/examples/datas/*.csv`(实测能覆盖父级,`git add examples/` 可入)。
- **scope**:在内(mdreplay 自有输入契约样例,不绑 venue/口径)。**工作量**:小。

### 2. README 置顶「快速开始(无需数据)」
- **做什么**:build → 跑 `examples/config.toml` → 贴预期 stdout(`loaded 2 replays... done: 120 events`)→ `ls /dev/shm`。
- **scope**:在内。**工作量**:小(与 #1 配套)。

---

## P1 — 该做(低成本 CLI / 打包卫生)

### 3. 统一测试 runner `test/run_all.sh`
- **做什么**:一条脚本顺序跑 `xmake build test && xmake run test` + 全部 `*_e2e.sh` + `mem_check.sh`,
  逐项 PASS/FAIL 汇总、任一失败退非零。纯编排现有测试,不引框架。
- **⚠️ 依赖 #6**:run_all 会调 `mem_check.sh`,而 mem_check 现依赖 `datas/`;不先修 #6,收件人跑 run_all 会失败。
- **scope**:在内。**工作量**:小。

### 4. `--version`
- **做什么**:`mdreplay <ver>  (gconf shm 段契约 v1.2.2)`。写死 `kVersion`/`kGconfContract` 两常量即可。
- **scope**:在内。**工作量**:小。

### 5. `--dry-run`(校验 config + 预演装配,不建段不回放)
- **做什么**:复用 `load_one_replay` 的 load_sources+detect_depth,打印每路装配计划(发现几文件/识别档数/选哪个段/窗口),
  **跳过 `open_output` + `replay`**,退 0=可装配。让人在真正建 shm 段灌数据前先验证 config。
- **落点**:`Config` 加 `bool dry_run`(CLI-only,注明不来自 toml);`load_one_replay` 加 `dry_run` 参数,
  detect_depth 后若 dry 则 log 计划 + return(不 open/不拼接);main 循环后若 dry 则打汇总 + return 0。
- **scope**:在内(只读已有装配逻辑,零新口径;**别膨胀成校验数据内容正确性**——那是下游的事)。**工作量**:中。

### 6. `mem_check.sh` 在 `datas/` 缺失时自给(否则 #3 出盒就红)
- **做什么**:检测到 `datas/` 无 `*.trade.csv` → 用合成数据(见 #7 生成器或内联 awk)造一份小数据自测,
  而非 `die "找不到数据目录"`。让 run_all 出盒即可跑。
- **scope**:在内。**工作量**:小-中。

### 7. 示例数据生成器 `tools/gen_sample_data.sh`
- **做什么**:`gen_sample_data.sh [--out datas] [--rows N] [--depth 1|5] [--symbols ...]` 生成合成 book+trade CSV
  (带 update_id、确定性 `srand` 固定种子、价格小幅游走、通用不绑 venue)。供 #1 静态样例、#6 mem_check、压测用。
- **scope**:在内(demo/test 工具,同 e2e 内联 fixture 的模式)。**工作量**:小。

### 8. `--help` 末尾加可复制示例行
- **做什么**:`print_usage()` 末尾 `开箱即跑: mdreplay --config examples/config.toml --realtime 0`。**工作量**:小。

---

## P2 — 锦上添花

### 9. `discover` 找不到文件时列目录实际内容
- **做什么**:`no *.book.csv under 'dir'` → 附「目录实际有: a.txt, x.book.json, …」+ 期望命名 `*.<kind>.<fmt>`。
  纠 csv/json 写错、命名错(最常见开箱错)。落点 `main.cpp::load_one_replay` 空源分支 + 一个 `list_dir_sample` 小助手。
- **scope**:在内。**工作量**:小。

---

## ❌ 不做(守边界:像功能但越界或过早抽象)
- **venue 专属解析器 / 自动吃交易所 raw 格式**(binance b/a、okx 增量重建):违反零上游耦合。
- **把 `formatted_to_datas.py` 导出器收进包**:它知道 tick_feat 下游口径,属父仓库。
- **parquet/arrow 输入**:违反零第三方解析 + simple,重蹈 arrow ABI 地狱。
- **book+trade 合进单进程单段 / 自动多 kind**:现有「单入单出 + 多 [[replays]] 同钟」是刻意解耦。
- **通用 Sink 注册表 / 段类型插件系统**:过早抽象;三种段三个 if 足够,等 Rule of Three。
- **per-replay 字段加 CLI 覆盖**(`--replays[0].input.dir`):撑爆 CLI 解析、语义混乱。
- **网络 feed / 重连 / 背压队列**:它是文件回放器,不是实盘客户端。
- **内置 shm 消费者 / dashboard / live metrics**:读段是下游的事。
- **数据质量校验**(ts 单调、盘口 gap、跨档一致):超出「搬运记录」职责,属下游口径。
