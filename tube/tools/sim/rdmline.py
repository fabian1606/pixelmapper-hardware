"""RS-485 line model around avrsim.Cpu: a controller that sends DMX / RDM
packets with exact E1.20 timing into the emulated tube and measures the
tube's answer (turnaround, break, MAB, driver enable/shutoff, slot gaps)."""
import dataclasses

from avrsim import BIT, FRAME, Cpu, SimError, flash_image, us

SC_RDM, SC_SUB = 0xCC, 0x01
CC_DISC, CC_GET, CC_SET = 0x10, 0x20, 0x30
CTRL_UID = bytes.fromhex("7FF000000002")


def rdm_packet(dest, cc, pid, pd=b"", tn=0, subdev=0, src=CTRL_UID, port=1, msgcnt=0, length=None, sub_sc=SC_SUB):
    pd = bytes(pd)
    n = 24 + len(pd) if length is None else length
    body = (bytes([SC_RDM, sub_sc, n & 0xFF]) + bytes(dest) + bytes(src) + bytes([tn & 0xFF, port, msgcnt])
            + subdev.to_bytes(2, "big") + bytes([cc]) + pid.to_bytes(2, "big") + bytes([len(pd) & 0xFF]) + pd)
    return body + (sum(body) & 0xFFFF).to_bytes(2, "big")


@dataclasses.dataclass
class Rdm:
    dest: bytes
    src: bytes
    tn: int
    rt: int
    msgcnt: int
    subdev: int
    cc: int
    pid: int
    pd: bytes


def rdm_parse(p):
    if len(p) < 26 or p[0] != SC_RDM or p[1] != SC_SUB:
        raise ValueError(f"no RDM packet: {p.hex()}")
    n = p[2]
    if len(p) != n + 2 or p[23] + 24 != n:
        raise ValueError(f"bad length: {p.hex()}")
    if sum(p[:n]) & 0xFFFF != int.from_bytes(p[n:n + 2], "big"):
        raise ValueError(f"bad checksum: {p.hex()}")
    return Rdm(p[3:9], p[9:15], p[15], p[16], p[17], int.from_bytes(p[18:20], "big"), p[20],
               int.from_bytes(p[21:23], "big"), bytes(p[24:n]))


def dub_decode(raw):
    i = 0
    while i < len(raw) and raw[i] == 0xFE and i < 7:
        i += 1
    if i >= len(raw) or raw[i] != 0xAA or len(raw) - i - 1 != 16:
        return None
    e = raw[i + 1:]
    uid = bytes(e[2 * k] & e[2 * k + 1] for k in range(6))
    cs = (e[12] & e[13]) << 8 | (e[14] & e[15])
    return uid if cs == sum(e[:12]) & 0xFFFF else None


@dataclasses.dataclass
class Answer:
    raw: bytes
    eop: int                     # controller end of packet (cycle)
    sop: int = None              # tube start of packet (break edge / first start bit)
    brk: float = None            # us
    mab: float = None            # us
    turnaround: float = None     # us, EOP -> SOP
    de_before_start: float = None   # us, DE high -> first start bit (break-less answers)
    de_after_end: float = None   # us, end of last stop bit -> DE low
    max_gap: float = None        # us, largest inter-slot time
    de_early: bool = False       # DE went high before EOP + 176 us


class Tube:
    def __init__(self, hexes, device_id=None, eeprom=None, seed=1):
        img = flash_image(*hexes)
        ee = bytearray(eeprom if eeprom is not None else b"\xFF" * 512)
        if device_id is not None:
            ee[0:4] = device_id.to_bytes(4, "big")
        self.cpu = Cpu(img, ee, seed=seed)
        self.tn = 0

    @property
    def ms(self):
        return self.cpu.cyc / 16000.0

    def run_ms(self, ms):
        self.cpu.run_until(self.cpu.cyc + us(ms * 1000))

    def run_us(self, x):
        self.cpu.run_until(self.cpu.cyc + us(x))

    # ------------------------------------------------------------------ line
    def _schedule(self, data, brk_us, mab_us, isd_us, lead_us=4, with_break=True):
        c = self.cpu
        t = c.cyc + us(lead_us)
        if with_break:
            c.rx_queue.append((t + int(9.5 * BIT), t, 0x00, True))
            t += us(brk_us) + us(mab_us)
        for i, b in enumerate(data):
            c.rx_queue.append((t + int(9.5 * BIT), t, b, False))
            t += FRAME + (us(isd_us) if i < len(data) - 1 else 0)
        c.rx_queue.sort()
        c._sched()
        return t                                     # EOP: end of the 2nd stop bit of the last slot

    def send_dmx(self, slots, start_code=0x00, brk_us=176, mab_us=12, isd_us=0):
        eop = self._schedule(bytes([start_code]) + bytes(slots), brk_us, mab_us, isd_us)
        self.cpu.run_until(eop + us(50))
        return eop

    def transact(self, packet, wait_us=3000, brk_us=176, mab_us=12, isd_us=0):
        """Send one packet, run until the answer is complete (or wait_us of silence),
        return Answer (raw = bytes the tube sent, b'' if none)."""
        c = self.cpu
        n_tx, n_log = len(c.tx_frames), len(c.portd_log)
        resets = c.reset_count
        eop = self._schedule(packet, brk_us, mab_us, isd_us)
        c.run_until(eop + us(wait_us))
        while c.tx_shift_end is not None or c.tx_buf is not None or self._de_high():
            c.run_until(c.cyc + us(100))
            if c.cyc > eop + us(50000):
                raise SimError("tube keeps driving the line")
        frames = c.tx_frames[n_tx:]
        log = c.portd_log[n_log - 1:] if n_log else c.portd_log
        a = Answer(bytes(b for _, b in frames), eop)
        if c.reset_count != resets:
            a.reset = True
        self._timing(a, frames, log)
        return a

    def _de_high(self):
        c = self.cpu
        return bool(c.ddrd & 0x04 and c.portd & 0x04)

    def _timing(self, a, frames, log):
        de_rise = de_fall = brk_start = brk_end = None
        prev_de = prev_txd_low = False
        for cyc, portd, ddrd, txen in log:
            de = bool(ddrd & 0x04 and portd & 0x04)
            txd_low = de and not txen and bool(ddrd & 0x02) and not (portd & 0x02)
            if de and not prev_de and cyc >= a.eop - us(1):
                if de_rise is None:
                    de_rise = cyc
                    if cyc < a.eop + us(176):
                        a.de_early = True
            if not de and prev_de and de_rise is not None and de_fall is None:
                de_fall = cyc
            if txd_low and not prev_txd_low and brk_start is None and de_rise is not None:
                brk_start = cyc
            if not txd_low and prev_txd_low and brk_start is not None and brk_end is None:
                brk_end = cyc
            prev_de, prev_txd_low = de, txd_low
        if not frames:
            return
        first, last = frames[0][0], frames[-1][0]
        if brk_start is not None and brk_end is not None and brk_end <= first:
            a.sop = brk_start
            a.brk = (brk_end - brk_start) / 16
            a.mab = (first - brk_end) / 16
        else:
            a.sop = first
            if de_rise is not None:
                a.de_before_start = (first - de_rise) / 16
        a.turnaround = (a.sop - a.eop) / 16
        if de_fall is not None:
            a.de_after_end = (de_fall - (last + FRAME)) / 16
        gaps = [(frames[i + 1][0] - frames[i][0] - FRAME) / 16 for i in range(len(frames) - 1)]
        a.max_gap = max(gaps) if gaps else 0.0

    # ------------------------------------------------------------------ RDM helpers
    def rdm(self, dest, cc, pid, pd=b"", **kw):
        self.tn = (self.tn + 1) & 0xFF
        kw.setdefault("tn", self.tn)
        wait = kw.pop("wait_us", 3000)
        a = self.transact(rdm_packet(dest, cc, pid, pd, **kw), wait_us=wait)
        a.req_tn = kw["tn"]
        a.pkt = None
        if a.raw:
            a.pkt = rdm_parse(a.raw)
        return a

    def ram_symbol(self, addr, n):
        return bytes(self.cpu.d[addr:addr + n])
