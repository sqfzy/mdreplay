#!/usr/bin/env python3
"""
collect_binance_raw.py — Binance USDⓈ-M 期货实时 book/trade 原始消息收集器

与 collect_okx_raw.py 同构(robust/observable/idempotent), 但走 Binance 协议:
  - 端点 combined stream: wss://fstream.binance.com/stream
  - 订阅 {"method":"SUBSCRIBE","params":[...],"id":1}
  - book 用 <sym>@depth20@100ms(20档全量快照, 无需维护订单簿, ≥15档需求)
  - trade 用 <sym>@trade(逐笔)
  - 心跳: Binance 服务器每 3min 发 ping, aiohttp autoping 自动回 pong(无需主动 ping)

落盘: {out}/binance_{SYM}_{startUTC}.jsonl, 每行:
  {"recv_us": <本地接收微秒>, "stream": "<stream名>", "data": <Binance 原始 data>}

用法:
  uv run --with aiohttp python3 collect_binance_raw.py --dry-run --proxy http://127.0.0.1:7890
  uv run --with aiohttp python3 collect_binance_raw.py --minutes 120 --proxy http://127.0.0.1:7890 --out raw_2h
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

WS_URL = "wss://fstream.binance.com/stream"
DEPTH_STREAM = "@depth20@100ms"            # 20档全量快照, 100ms
TRADE_STREAM = "@trade"                     # 逐笔成交
STATS_INTERVAL_S = 30
RECONNECT_BACKOFF_S = 2

SYMBOLS = [
    "AAVEUSDT", "ADAUSDT", "APTUSDT", "ARBUSDT", "AVAXUSDT", "AXSUSDT", "BCHUSDT",
    "BNBUSDT", "CRVUSDT", "DOGEUSDT", "DOTUSDT", "ENSUSDT", "HBARUSDT", "LTCUSDT",
    "OPUSDT", "ORDIUSDT", "PEOPLEUSDT", "PNUTUSDT", "SANDUSDT", "SOLUSDT", "SUIUSDT",
    "TIAUSDT", "TRUMPUSDT", "UNIUSDT", "WLDUSDT", "XLMUSDT", "XRPUSDT",
]

logger = logging.getLogger("collect_binance")


def _utc_stamp() -> str:
    return datetime.now(timezone.utc).strftime("%Y%m%dT%H%M%SZ")


class RawSink:
    """按 symbol 分文件落盘原始消息; 句柄惰性创建, 周期 flush。"""

    def __init__(self, out_dir: Path, start_stamp: str):
        self._dir = out_dir
        self._stamp = start_stamp
        self._files: dict[str, object] = {}
        self.counts: dict[str, int] = {}

    def write(self, symbol: str, recv_us: int, stream: str, data) -> None:
        f = self._file_for(symbol)
        f.write(json.dumps({"recv_us": recv_us, "stream": stream, "data": data},
                           separators=(",", ":")) + "\n")
        self.counts[symbol] = self.counts.get(symbol, 0) + 1

    def _file_for(self, symbol: str):
        if symbol not in self._files:
            path = self._dir / f"binance_{symbol}_{self._stamp}.jsonl"
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


def _stream_params() -> list[str]:
    params = []
    for s in SYMBOLS:
        params.append(s.lower() + DEPTH_STREAM)
        params.append(s.lower() + TRADE_STREAM)
    return params


async def _subscribe(ws) -> None:
    params = _stream_params()
    await ws.send_str(json.dumps({"method": "SUBSCRIBE", "params": params, "id": 1}))
    logger.info("sent subscribe: %d symbols × 2 streams = %d params", len(SYMBOLS), len(params))


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
    """解析 combined-stream 帧并路由落盘; 内容原样保存。"""
    try:
        msg = json.loads(raw)
    except json.JSONDecodeError:
        logger.warning("non-json frame dropped: %.120s", raw)
        return
    if "result" in msg or "id" in msg:           # 订阅确认/控制帧
        if msg.get("result") not in (None, []):
            logger.debug("control: %s", msg)
        return
    stream = msg.get("stream")
    data = msg.get("data")
    if not stream or data is None:
        return
    symbol = stream.split("@", 1)[0].upper()      # btcusdt@trade → BTCUSDT
    sink.write(symbol, int(time.time() * 1_000_000), stream, data)


async def _run_session(session: aiohttp.ClientSession, sink: RawSink, deadline: float,
                       proxy: str | None) -> None:
    """单次 WS 会话: 订阅 + 收消息直到 deadline 或连接断开(autoping 维持心跳)。"""
    async with session.ws_connect(WS_URL, autoping=True, heartbeat=150, max_msg_size=0, proxy=proxy) as ws:
        await _subscribe(ws)
        async for frame in ws:
            if frame.type == aiohttp.WSMsgType.TEXT:
                _route(frame.data, sink)
            elif frame.type in (aiohttp.WSMsgType.CLOSED, aiohttp.WSMsgType.ERROR):
                logger.warning("ws frame %s, will reconnect", frame.type)
                break
            if time.time() >= deadline:
                logger.info("deadline reached, closing session")
                break


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
                except Exception as e:
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
    ap = argparse.ArgumentParser(description="Binance USDⓈ-M 原始 book/trade 收集器")
    ap.add_argument("--minutes", type=float, default=120.0, help="收集时长(分钟)")
    ap.add_argument("--out", default="raw_2h", help="输出目录")
    ap.add_argument("--proxy", default=None, help="上游 http 代理, 如 http://127.0.0.1:7890")
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
