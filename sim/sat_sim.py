#!/usr/bin/env python3
"""
Имитатор борта МКА: собирает кадры телеметрии и отправляет их по UDP.

Приложение cubesat телеметрию больше не генерирует — оно только принимает.
Всё, что видно на экране, приходит отсюда.

Формат кадра — в стиле CCSDS Space Packet, big-endian, 288 байт:
    Primary Header (6)  +  payload (280)  +  CRC-16-CCITT (2)
Раскладка полей описана в PROTOCOL.md и должна совпадать с protocol.h.

Примеры запуска
---------------
Обычный пролёт, один кадр в секунду:
    python sat_sim.py

Быстрее и с потерями в канале, как на слабом сигнале:
    python sat_sim.py --rate 5 --drop 0.1 --corrupt 0.05

Разыграть сценарий из файла (аварийные ситуации):
    python sat_sim.py --scenario scenarios/low_battery.jsonl

Переиграть пролёт, записанный приложением:
    python sat_sim.py --replay ../build/.../logs/telemetry_20260927_120000.bin

Проверить, что раскладка кадра совпадает с ожидаемой:
    python sat_sim.py --selftest
"""

from __future__ import annotations

import argparse
import json
import math
import random
import re
import socket
import struct
import sys
import time
from pathlib import Path

# В консоли Windows кодировка по умолчанию не UTF-8, иначе русский текст
# в выводе превращается в мусор.
for stream in (sys.stdout, sys.stderr):
    try:
        stream.reconfigure(encoding="utf-8", errors="replace")
    except (AttributeError, ValueError):
        pass

# --------------------------------------------------------------------------
#  Протокол
# --------------------------------------------------------------------------

HEADER_SIZE = 6
PAYLOAD_SIZE = 280
CRC_SIZE = 2
PACKET_SIZE = HEADER_SIZE + PAYLOAD_SIZE + CRC_SIZE  # 288

APID_HOUSEKEEPING = 0x064
SEQ_MASK = 0x3FFF
DEFAULT_PORT = 5005

# Порядок полей payload. Должен строка в строку соответствовать
# writePayload()/readPayload() в protocol.h.
PAYLOAD_FORMAT = (
    ">"
    "I"                       # timestamp
    "3f 3f f f f B 12f"       # EPS
    "f f f f 12f"             # Thermal
    "3f 3f 6f 4f 3f"          # ADCS
    "B I H H H B"             # OBC
    "f h f B"                 # Comm
    "d d f f I"               # GPS
)


def crc16_ccitt(data: bytes) -> int:
    """CRC-16-CCITT: полином 0x1021, начальное значение 0xFFFF, старшим битом вперёд."""
    crc = 0xFFFF
    for byte in data:
        crc ^= byte << 8
        for _ in range(8):
            if crc & 0x8000:
                crc = ((crc << 1) ^ 0x1021) & 0xFFFF
            else:
                crc = (crc << 1) & 0xFFFF
    return crc


def pack_payload(t: dict) -> bytes:
    eps, th, ad, obc, comm, gps = (
        t["eps"], t["thermal"], t["adcs"], t["obc"], t["comm"], t["gps"],
    )
    values = [
        t["timestamp"],
        *eps["bus_voltage"], *eps["bus_current"],
        eps["battery_voltage"], eps["battery_temp"], eps["battery_current"],
        eps["soc_percent"], *eps["solar_power"],
        th["temp_cpu"], th["temp_battery"], th["temp_antenna"], th["temp_in"],
        *th["temp_solar"],
        *ad["gyro"], *ad["magnetometer"], *ad["sun_sensor"],
        *ad["quaternion"], *ad["reaction_wheel_speed"],
        obc["cpu_load_percent"], obc["memory_used_kb"], obc["reboot_count"],
        obc["watchdog_reset_count"], obc["firmware_version"], obc["last_error_code"],
        comm["tx_power_dbm"], comm["rssi"], comm["bit_error_rate"],
        1 if comm["antenna_deployed"] else 0,
        gps["latitude"], gps["longitude"], gps["altitude_km"], gps["speed_kmh"],
        gps["gps_timestamp"],
    ]
    return struct.pack(PAYLOAD_FORMAT, *values)


def build_frame(payload: bytes, seq: int, apid: int = APID_HOUSEKEEPING) -> bytes:
    """Заголовок CCSDS + payload + CRC по всему предыдущему."""
    word0 = apid & 0x07FF                 # version=0, type=0 (TM), sec_hdr=0
    word1 = 0xC000 | (seq & SEQ_MASK)      # seq_flags=0b11 — не сегментирован
    data_length_field = len(payload) + CRC_SIZE - 1
    frame = struct.pack(">HHH", word0, word1, data_length_field) + payload
    return frame + struct.pack(">H", crc16_ccitt(frame))


# --------------------------------------------------------------------------
#  Модель борта
# --------------------------------------------------------------------------

class Spacecraft:
    """
    Простая модель: спутник идёт по витку, попадая то на свет, то в тень.
    На свету панели дают мощность и греются, батарея заряжается; в тени
    мощность падает до нуля, панели остывают, батарея разряжается.

    Это не расчёт орбиты, а правдоподобная динамика — чтобы на графиках
    было видно виток, а не просто шум вокруг константы.
    """

    def __init__(self, rng: random.Random, orbit_seconds: float) -> None:
        self.rng = rng
        self.orbit_seconds = max(1.0, orbit_seconds)
        self.start = time.monotonic()
        self.soc = 86.0
        self.panel_temp = [20.0] * 12
        self.reboot_count = 3
        self.watchdog_count = 1

    def drift(self, base: float, span: float) -> float:
        return base + (self.rng.random() - 0.5) * span

    def sample(self, timestamp: int, dt: float) -> dict:
        phase = ((time.monotonic() - self.start) / self.orbit_seconds) % 1.0
        # Освещённость: примерно 60% витка на свету, дальше тень.
        sun = max(0.0, math.sin(phase * 2.0 * math.pi))
        in_sunlight = sun > 0.05

        # --- Солнечные панели: каждая смотрит в свою сторону, поэтому у
        # каждой свой сдвиг по фазе.
        solar_power = []
        for i in range(12):
            facing = max(0.0, math.cos((phase * 2.0 * math.pi) - i * math.pi / 6.0))
            # Панель не может отдавать отрицательную мощность: в тени это ноль,
            # а не шум вокруг нуля.
            solar_power.append(round(max(0.0, self.drift(5.4 * sun * facing, 0.3)), 3))

        # --- Температура панелей тянется к равновесной с инерцией.
        for i in range(12):
            target = 45.0 * sun + (-25.0) * (1.0 - sun)
            self.panel_temp[i] += (target - self.panel_temp[i]) * min(1.0, dt * 0.15)
            self.panel_temp[i] += (self.rng.random() - 0.5) * 0.4

        # --- Баланс энергии: приход с панелей минус постоянное потребление.
        generated = sum(solar_power)
        consumed = 4.2
        battery_current = (generated - consumed) / 7.4  # A, отрицательный = разряд
        self.soc = min(100.0, max(0.0, self.soc + battery_current * dt * 0.35))

        return {
            "timestamp": timestamp,
            "eps": {
                "bus_voltage": [self.drift(3.3, 0.05), self.drift(5.0, 0.08),
                                self.drift(8.4, 0.10)],
                "bus_current": [self.drift(0.4, 0.05) for _ in range(3)],
                "battery_voltage": self.drift(7.4 * (0.94 + self.soc / 100.0 * 0.06), 0.05),
                "battery_temp": self.drift(18.0 + 4.0 * sun, 0.6),
                "battery_current": round(battery_current, 3),
                "soc_percent": int(round(self.soc)),
                "solar_power": solar_power,
            },
            "thermal": {
                "temp_cpu": self.drift(24.0 + 6.0 * sun, 0.5),
                "temp_battery": self.drift(18.0 + 4.0 * sun, 0.6),
                "temp_antenna": self.drift(-8.0 + 20.0 * sun, 1.5),
                "temp_in": self.drift(20.0 + 3.0 * sun, 0.8),
                "temp_solar": [round(v, 2) for v in self.panel_temp],
            },
            "adcs": {
                "gyro": [self.drift(0.0, 0.05) for _ in range(3)],
                "magnetometer": [self.drift(20.0, 5.0) for _ in range(3)],
                # Датчик видит Солнце только если оно в его полусфере.
                "sun_sensor": [
                    round(max(0.0, self.drift(
                        math.cos(phase * 2.0 * math.pi - i * math.pi / 3.0), 0.05)), 3)
                    if in_sunlight else 0.0
                    for i in range(6)
                ],
                "quaternion": [self.drift(0.98, 0.01), self.drift(0.10, 0.02),
                               self.drift(0.05, 0.02), self.drift(0.03, 0.02)],
                "reaction_wheel_speed": [self.drift(1200.0, 100.0) for _ in range(3)],
            },
            "obc": {
                "cpu_load_percent": int(min(100, max(0, round(self.drift(34, 5))))),
                "memory_used_kb": int(max(0, round(self.drift(182, 10)))),
                "reboot_count": self.reboot_count,
                "watchdog_reset_count": self.watchdog_count,
                "firmware_version": 142,  # v1.4.2
                "last_error_code": 0,
            },
            "comm": {
                "tx_power_dbm": self.drift(20.0, 1.0),
                "rssi": int(round(self.drift(-92, 4))),
                "bit_error_rate": abs(self.drift(0.0002, 0.0001)),
                "antenna_deployed": True,
            },
            "gps": {
                "latitude": self.drift(55.75, 0.02),
                "longitude": self.drift(37.62, 0.02),
                "altitude_km": self.drift(412.0, 2.0),
                "speed_kmh": self.drift(27600.0, 50.0),
                "gps_timestamp": timestamp,
            },
        }


# --------------------------------------------------------------------------
#  Сценарии
# --------------------------------------------------------------------------

_PATH_ITEM = re.compile(r"^([A-Za-z_][A-Za-z0-9_]*)(?:\[(\d+)\])?$")


def apply_override(telemetry: dict, path: str, value) -> None:
    """
    Подставляет значение по пути вида 'eps.soc_percent' или
    'thermal.temp_solar[4]'. Путь 'eps.solar_power' целиком тоже допустим.
    """
    parts = path.split(".")
    node = telemetry
    for part in parts[:-1]:
        match = _PATH_ITEM.match(part)
        if not match:
            raise ValueError(f"не понимаю путь: {path}")
        name, idx = match.group(1), match.group(2)
        node = node[name] if idx is None else node[name][int(idx)]

    match = _PATH_ITEM.match(parts[-1])
    if not match:
        raise ValueError(f"не понимаю путь: {path}")
    name, idx = match.group(1), match.group(2)
    if name not in node:
        raise KeyError(f"нет такого поля: {path}")
    if idx is None:
        node[name] = value
    else:
        node[name][int(idx)] = value


def load_scenario(path: Path) -> list[dict]:
    """
    Сценарий — файл JSONL: одна строка на кадр. В строке поля-переопределения
    поверх сгенерированных значений, плюс служебные ключи:
        "_repeat": N  — держать это состояние N кадров
        "_note": "текст" — печатается в консоль

    Пример строки:
        {"_note": "разряд батареи", "eps.soc_percent": 12,
         "eps.battery_current": -0.45, "_repeat": 15}

    Пустые строки и строки, начинающиеся с #, игнорируются.
    """
    steps: list[dict] = []
    with path.open("r", encoding="utf-8") as handle:
        for lineno, raw in enumerate(handle, start=1):
            line = raw.strip()
            if not line or line.startswith("#"):
                continue
            try:
                steps.append(json.loads(line))
            except json.JSONDecodeError as exc:
                raise SystemExit(f"{path}:{lineno}: не разобрать JSON — {exc}") from exc
    if not steps:
        raise SystemExit(f"{path}: сценарий пустой")
    return steps


def expand_scenario(steps: list[dict]) -> list[dict]:
    """Разворачивает _repeat в отдельные кадры."""
    frames: list[dict] = []
    for step in steps:
        repeat = int(step.get("_repeat", 1))
        overrides = {k: v for k, v in step.items() if not k.startswith("_")}
        note = step.get("_note")
        for i in range(max(1, repeat)):
            frames.append({"overrides": overrides, "note": note if i == 0 else None})
    return frames


# --------------------------------------------------------------------------
#  Чтение журнала сырых кадров (формат RawLogWriter из rawlog.h)
# --------------------------------------------------------------------------

def read_raw_log(path: Path):
    """Возвращает список (recv_time_ms, frame_bytes) из записанного пролёта."""
    data = path.read_bytes()
    if len(data) < 8 or data[:4] != b"CSTL":
        raise SystemExit(f"{path}: не похоже на журнал кадров (нет метки CSTL)")
    version = struct.unpack_from(">H", data, 4)[0]
    if version != 1:
        raise SystemExit(f"{path}: версия журнала {version} не поддерживается")

    records = []
    offset = 8
    while offset + 12 <= len(data):
        recv_ms, length = struct.unpack_from(">QI", data, offset)
        offset += 12
        if offset + length > len(data):
            print(f"предупреждение: журнал обрывается на записи {len(records) + 1}",
                  file=sys.stderr)
            break
        records.append((recv_ms, data[offset:offset + length]))
        offset += length
    if not records:
        raise SystemExit(f"{path}: в журнале нет кадров")
    return records


# --------------------------------------------------------------------------
#  Передача
# --------------------------------------------------------------------------

def corrupt(frame: bytes, rng: random.Random) -> bytes:
    """Портит один бит в payload — приёмник должен поймать это по CRC."""
    if len(frame) <= HEADER_SIZE + CRC_SIZE:
        return frame
    pos = rng.randrange(HEADER_SIZE, len(frame) - CRC_SIZE)
    mutable = bytearray(frame)
    mutable[pos] ^= 1 << rng.randrange(8)
    return bytes(mutable)


def run_live(args: argparse.Namespace, sock: socket.socket, target) -> None:
    rng = random.Random(args.seed)
    craft = Spacecraft(rng, args.orbit_seconds)

    scenario = None
    if args.scenario:
        scenario = expand_scenario(load_scenario(Path(args.scenario)))
        print(f"сценарий: {args.scenario}, кадров {len(scenario)}"
              f"{' (по кругу)' if args.loop else ''}")

    period = 1.0 / args.rate
    seq = 0
    sent = 0
    dropped = 0
    damaged = 0
    step = 0
    previous = time.monotonic()

    while args.count == 0 or step < args.count:
        now = time.monotonic()
        dt = now - previous
        previous = now

        telemetry = craft.sample(int(time.time()), dt if dt > 0 else period)

        note = None
        if scenario:
            if step >= len(scenario):
                if not args.loop:
                    break
            entry = scenario[step % len(scenario)]
            note = entry["note"]
            for path, value in entry["overrides"].items():
                apply_override(telemetry, path, value)

        frame = build_frame(pack_payload(telemetry), seq, args.apid)

        if len(frame) != PACKET_SIZE:
            raise SystemExit(f"внутренняя ошибка: кадр {len(frame)} байт вместо {PACKET_SIZE}")

        mark = ""
        if rng.random() < args.drop:
            # Кадр пропал в канале. Счётчик всё равно двигаем — по разрыву
            # в нём приёмник и поймёт, что была потеря.
            dropped += 1
            mark = "  [ПОТЕРЯН]"
        else:
            out = frame
            if rng.random() < args.corrupt:
                out = corrupt(frame, rng)
                damaged += 1
                mark = "  [ИСКАЖЁН]"
            sock.sendto(out, target)
            sent += 1

        if note:
            print(f"--- {note}")
        if not args.quiet:
            eps = telemetry["eps"]
            print(f"seq={seq:5d}  SOC={eps['soc_percent']:3d}%  "
                  f"I={eps['battery_current']:+6.3f} A  "
                  f"Pсолн={sum(eps['solar_power']):5.1f} Вт  "
                  f"Tcpu={telemetry['thermal']['temp_cpu']:5.1f}°C{mark}")

        seq = (seq + 1) & SEQ_MASK
        step += 1
        time.sleep(period)

    print(f"\nотправлено {sent}, потеряно {dropped}, искажено {damaged}")


def run_replay(args: argparse.Namespace, sock: socket.socket, target) -> None:
    records = read_raw_log(Path(args.replay))
    print(f"переигрываю {len(records)} кадров из {args.replay}"
          f" (скорость x{args.speed})")

    sent = 0
    for i, (recv_ms, frame) in enumerate(records):
        sock.sendto(frame, target)
        sent += 1
        if not args.quiet:
            print(f"кадр {i + 1}/{len(records)}  {len(frame)} байт")
        if i + 1 < len(records):
            gap_ms = records[i + 1][0] - recv_ms
            delay = max(0.0, gap_ms / 1000.0) / max(0.01, args.speed)
            time.sleep(min(delay, 10.0))  # на всякий случай не ждём вечность

    print(f"\nпереиграно {sent} кадров")


def run_selftest() -> int:
    """Проверяет, что раскладка кадра та, которую ждёт protocol.h."""
    ok = True

    size = struct.calcsize(PAYLOAD_FORMAT)
    print(f"размер payload: {size} (ожидается {PAYLOAD_SIZE})")
    ok &= size == PAYLOAD_SIZE

    craft = Spacecraft(random.Random(1), 600.0)
    telemetry = craft.sample(1_700_000_000, 1.0)
    frame = build_frame(pack_payload(telemetry), 42)
    print(f"размер кадра:   {len(frame)} (ожидается {PACKET_SIZE})")
    ok &= len(frame) == PACKET_SIZE

    # CRC по всему кадру, включая само поле CRC, обнуляет остаток — свойство
    # этого варианта CRC, удобно для проверки на приёмной стороне.
    apid = struct.unpack_from(">H", frame, 0)[0] & 0x07FF
    seq = struct.unpack_from(">H", frame, 2)[0] & SEQ_MASK
    declared = struct.unpack_from(">H", frame, 4)[0] + 1
    print(f"APID: 0x{apid:03X} (ожидается 0x{APID_HOUSEKEEPING:03X})")
    print(f"счётчик: {seq} (ожидается 42)")
    print(f"длина Data Field: {declared} (ожидается {PAYLOAD_SIZE + CRC_SIZE})")
    ok &= apid == APID_HOUSEKEEPING
    ok &= seq == 42
    ok &= declared == PAYLOAD_SIZE + CRC_SIZE

    computed = crc16_ccitt(frame[:-CRC_SIZE])
    stored = struct.unpack_from(">H", frame, len(frame) - CRC_SIZE)[0]
    print(f"CRC: посчитан 0x{computed:04X}, в кадре 0x{stored:04X}")
    ok &= computed == stored

    print("\nOK" if ok else "\nНЕ СОШЛОСЬ")
    return 0 if ok else 1


# --------------------------------------------------------------------------

def main() -> int:
    parser = argparse.ArgumentParser(
        description="Имитатор борта МКА: отправляет кадры телеметрии по UDP.",
        formatter_class=argparse.RawDescriptionHelpFormatter,
    )
    parser.add_argument("--host", default="127.0.0.1", help="куда отправлять (по умолчанию 127.0.0.1)")
    parser.add_argument("--port", type=int, default=DEFAULT_PORT, help=f"порт (по умолчанию {DEFAULT_PORT})")
    parser.add_argument("--rate", type=float, default=1.0, help="кадров в секунду (по умолчанию 1)")
    parser.add_argument("--count", type=int, default=0, help="сколько кадров отправить, 0 — без ограничения")
    parser.add_argument("--apid", type=lambda v: int(v, 0), default=APID_HOUSEKEEPING,
                        help="APID пакета; поставь другой, чтобы приёмник отверг кадры")
    parser.add_argument("--drop", type=float, default=0.0,
                        help="доля теряемых кадров, 0..1 — приёмник увидит пропуски в счётчике")
    parser.add_argument("--corrupt", type=float, default=0.0,
                        help="доля искажаемых кадров, 0..1 — приёмник поймает их по CRC")
    parser.add_argument("--orbit-seconds", type=float, default=600.0,
                        help="длительность витка в секундах (реальная — около 5400)")
    parser.add_argument("--seed", type=int, default=None, help="зерно ГСЧ для повторяемости")
    parser.add_argument("--scenario", help="файл сценария JSONL с переопределениями полей")
    parser.add_argument("--loop", action="store_true", help="повторять сценарий по кругу")
    parser.add_argument("--replay", help="переиграть журнал кадров, записанный приложением")
    parser.add_argument("--speed", type=float, default=1.0, help="ускорение воспроизведения журнала")
    parser.add_argument("--quiet", action="store_true", help="не печатать каждый кадр")
    parser.add_argument("--selftest", action="store_true", help="проверить раскладку кадра и выйти")
    args = parser.parse_args()

    if args.selftest:
        return run_selftest()

    if struct.calcsize(PAYLOAD_FORMAT) != PAYLOAD_SIZE:
        raise SystemExit(
            f"раскладка payload разошлась с protocol.h: "
            f"{struct.calcsize(PAYLOAD_FORMAT)} вместо {PAYLOAD_SIZE} байт"
        )

    if args.scenario and args.replay:
        raise SystemExit("--scenario и --replay вместе не имеют смысла")

    target = (args.host, args.port)
    sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    print(f"передаю на {args.host}:{args.port}")

    try:
        if args.replay:
            run_replay(args, sock, target)
        else:
            run_live(args, sock, target)
    except KeyboardInterrupt:
        print("\nостановлено")
    finally:
        sock.close()
    return 0


if __name__ == "__main__":
    sys.exit(main())
