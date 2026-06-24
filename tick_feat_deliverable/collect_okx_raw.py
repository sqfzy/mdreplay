#!/usr/bin/env python3
"""
collect_okx_raw.py — OKX swap 实时 book/trade 原始消息收集器

只做一件事: 把 OKX WebSocket 推送的原始消息(books 400档增量 + trades)
原样落盘成 JSONL, 不做任何解析/订单簿重建(重建留给后续 format 步骤)。

设计原则(对齐用户脚本规范): robust(断线重连/单币隔离) + observable(分级日志/
周期 stats) + idempotent(输出文件名带启动 UTC 时间戳, 重跑绝不覆盖)。

用法:
  # dry-run: 连上收 ~8s 验证订阅与落盘, 然后退出
  uv run --with aiohttp python3 collect_okx_raw.py --dry-run
  # 正式收集 30 分钟
  uv run --with aiohttp python3 collect_okx_raw.py --minutes 30 --out raw_okx

落盘: {out}/okx_{SYM}_{startUTC}.jsonl, 每行一条原始 OKX message:
  {"recv_us": <本地接收微秒>, "msg": <OKX 原始 JSON 对象>}
  msg.arg.channel ∈ {books, trades}; books 首条 action=snapshot, 后续 action=update。
"""
from __future__ import annotations

import argparse
import asyncio
import json
import logging
import signal
import sys
import time
from datetime import datetime, timezone
from pathlib import Path

import aiohttp

WS_URL = "wss://ws.okx.com:8443/ws/v5/public"
CHANNELS = ("books", "trades")            # books=400档增量, trades=逐笔成交
PING_INTERVAL_S = 20                       # OKX: 30s 无数据即断, 应用层主动 ping
STATS_INTERVAL_S = 30
RECONNECT_BACKOFF_S = 2

SYMBOLS = [
    "AAVEUSDT", "ADAUSDT", "APTUSDT", "ARBUSDT", "AVAXUSDT", "AXSUSDT", "BCHUSDT",
    "BNBUSDT", "CRVUSDT", "DOGEUSDT", "DOTUSDT", "ENSUSDT", "HBARUSDT", "LTCUSDT",
    "OPUSDT", "ORDIUSDT", "PEOPLEUSDT", "PNUTUSDT", "SANDUSDT", "SOLUSDT", "SUIUSDT",
    "TIAUSDT", "TRUMPUSDT", "UNIUSDT", "WLDUSDT", "XLMUSDT", "XRPUSDT",
]

logger = logging.getLogger("collect_okx")


def _inst_id(symbol: str) -> str:
    """BTCUSDT -> BTC-USDT-SWAP (与 format_parquet_single 的 okex_swap 口径一致)。"""
    return symbol.replace("USDT", "-USDT-SWAP")


def _utc_stamp() -> str:
    return datetime.now(timezone.utc).strftime("%Y%m%dT%H%M%SZ")


class RawSink:
    """按 symbol 分文件落盘原始消息; 句柄惰性创建, 周期 flush。"""

    def __init__(self, out_dir: Path, start_stamp: str):
        self._dir = out_dir
        self._stamp = start_stamp
        self._files: dict[str, object] = {}
        self.counts: dict[str, int] = {}

    def write(self, symbol: str, recv_us: int, msg: dict) -> None:
        f = self._file_for(symbol)
        f.write(json.dumps({"recv_us": recv_us, "msg": msg}, separators=(",", ":")) + "\n")
        self.counts[symbol] = self.counts.get(symbol, 0) + 1

    def _file_for(self, symbol: str):
        if symbol not in self._files:
            path = self._dir / f"okx_{symbol}_{self._stamp}.jsonl"
            self._files[symbol] = path.open("a", encoding="utf-8")
            logger.debug("open sink %s", path)
        return self._files[symbol]

    def flush(self) -> None:
        for f in self._files.values():
            f.flush()

    def close(self) -> None:
        self.flush()
        for f in self._files.values():
            f.close()
        logger.info("sinks closed: %d files, total msgs=%d", len(self._files), sum(self.counts.values()))


def _build_sub_args() -> list[dict]:
    return [{"channel": ch, "instId": _inst_id(s)} for s in SYMBOLS for ch in CHANNELS]


async def _subscribe(ws) -> None:
    args = _build_sub_args()
    await ws.send_str(json.dumps({"op": "subscribe", "args": args}))
    logger.info("sent subscribe: %d symbols × %d channels = %d args", len(SYMBOLS), len(CHANNELS), len(args))


async def _ping_loop(ws) -> None:
    """OKX 应用层心跳: 每 PING_INTERVAL_S 发 'ping' 纯文本。"""
    try:
        while True:
            await asyncio.sleep(PING_INTERVAL_S)
            await ws.send_str("ping")
    except (asyncio.CancelledError, ConnectionResetError):
        pass


async def _stats_loop(sink: RawSink, deadline: float) -> None:
    last = dict(sink.counts)
    while True:
        await asyncio.sleep(STATS_INTERVAL_S)
        sink.flush()
        delta = sum(sink.counts.values()) - sum(last.values())
        active = sum(1 for s in SYMBOLS if sink.counts.get(s, 0) > last.get(s, 0))
        remain = max(0, int(deadline - time.time()))
        logger.info("stats: +%d msgs/%ds, active_syms=%d/%d, total=%d, remain=%ds",
                    delta, STATS_INTERVAL_S, active, len(SYMBOLS), sum(sink.counts.values()), remain)
        last = dict(sink.counts)


def _route(raw: str, sink: RawSink) -> None:
    """解析一条 WS 文本帧并路由落盘; 仅识别 instId 以分文件, 内容原样保存。"""
    if raw == "pong":
        return
    try:
        msg = json.loads(raw)
    except json.JSONDecodeError:
        logger.warning("non-json frame dropped: %.120s", raw)
        return
    if "event" in msg:                       # subscribe/error 等控制帧
        if msg.get("event") == "error":
            logger.error("OKX control error: %s", msg)
        else:
            logger.debug("control: %s", msg)
        return
    inst = msg.get("arg", {}).get("instId")
    if not inst:
        return
    symbol = inst.replace("-USDT-SWAP", "USDT")
    sink.write(symbol, int(time.time() * 1_000_000), msg)


async def _run_session(session: aiohttp.ClientSession, sink: RawSink, deadline: float,
                       proxy: str | None) -> None:
    """单次 WS 会话: 订阅 + 收消息直到 deadline 或连接断开。"""
    async with session.ws_connect(WS_URL, heartbeat=None, max_msg_size=0, proxy=proxy) as ws:
        await _subscribe(ws)
        ping = asyncio.create_task(_ping_loop(ws))
        try:
            async for frame in ws:
                if frame.type == aiohttp.WSMsgType.TEXT:
                    _route(frame.data, sink)
                elif frame.type in (aiohttp.WSMsgType.CLOSED, aiohttp.WSMsgType.ERROR):
                    logger.warning("ws frame %s, will reconnect", frame.type)
                    break
                if time.time() >= deadline:
                    logger.info("deadline reached, closing session")
                    break
        finally:
            ping.cancel()


async def collect(out_dir: Path, duration_s: int, proxy: str | None) -> int:
    out_dir.mkdir(parents=True, exist_ok=True)
    sink = RawSink(out_dir, _utc_stamp())
    deadline = time.time() + duration_s
    logger.info("collect start: %d symbols, duration=%ds, out=%s, proxy=%s",
                len(SYMBOLS), duration_s, out_dir, proxy or "direct")

    stats = asyncio.create_task(_stats_loop(sink, deadline))
    timeout = aiohttp.ClientTimeout(total=None, sock_read=None, sock_connect=15)
    try:
        async with aiohttp.ClientSession(timeout=timeout) as session:
            while time.time() < deadline:
                try:
                    await _run_session(session, sink, deadline, proxy)
                except aiohttp.ClientError as e:
                    logger.warning("session error: %s", e)
                except Exception as e:                       # 单次会话异常不应终止整体
                    logger.exception("unexpected session error: %s", e)
                if time.time() < deadline:
                    await asyncio.sleep(RECONNECT_BACKOFF_S)
    finally:
        stats.cancel()
        sink.close()

    received = sum(sink.counts.values())
    silent = [s for s in SYMBOLS if sink.counts.get(s, 0) == 0]
    if silent:
        logger.warning("symbols with ZERO msgs: %s", ", ".join(silent))
    logger.info("collect done: total_msgs=%d, files=%d", received, len(sink.counts))
    return received


def _parse_args(argv: list[str]) -> argparse.Namespace:
    ap = argparse.ArgumentParser(description="OKX swap 原始 book/trade 收集器")
    ap.add_argument("--minutes", type=float, default=30.0, help="收集时长(分钟)")
    ap.add_argument("--out", default="raw_okx", help="输出目录")
    ap.add_argument("--proxy", default=None, help="上游 http 代理, 如 http://127.0.0.1:7890; 缺省直连")
    ap.add_argument("--dry-run", action="store_true", help="连上收 ~8s 验证后退出")
    ap.add_argument("--log-level", default="INFO")
    a = ap.parse_args(argv)
    if a.minutes <= 0:
        ap.error("--minutes 必须 > 0")
    return a


def main(argv: list[str]) -> int:
    a = _parse_args(argv)
    logging.basicConfig(level=a.log_level, format="%(asctime)s %(levelname)s %(message)s")
    duration_s = 8 if a.dry_run else int(a.minutes * 60)
    out_dir = Path(a.out)
    logger.info("config: dry_run=%s, duration=%ds, out=%s", a.dry_run, duration_s, out_dir)

    loop = asyncio.new_event_loop()
    asyncio.set_event_loop(loop)
    for sig in (signal.SIGINT, signal.SIGTERM):
        try:
            loop.add_signal_handler(sig, lambda: [t.cancel() for t in asyncio.all_tasks(loop)])
        except NotImplementedError:
            pass
    try:
        received = loop.run_until_complete(collect(out_dir, duration_s, a.proxy))
    finally:
        loop.close()

    if received == 0:
        logger.error("收到 0 条消息, 检查网络/订阅")
        return 1
    if a.dry_run:
        logger.info("dry-run OK: 收到 %d 条消息, 落盘验证通过", received)
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
