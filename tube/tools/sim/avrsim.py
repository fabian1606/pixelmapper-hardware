#!/usr/bin/env python3
"""avrsim - cycle-counting ATmega8 emulator (16 MHz) for testing the LED tube
firmware (tube_fw + tube_boot_rdm) without hardware.

Implements the AVR instructions avr-gcc emits for these images and the
peripherals they use: USART (250 kbaud 8N2, RX with break/frame-error
injection, TX with frame timing), Timer1 (normal + CTC), Timer2 (CTC), WDT
with WDTON (timed sequence, WDRF reset), EEPROM (read, timed write), SPM
self-programming (page buffer, erase/write busy time, RWW section rules),
ADC, GPIO ports with a timeline of PORTD writes (DE/TXD pins), and the boot
reset vector (BOOTRST). Anything unknown or illegal raises SimError.
"""
import random

F_CPU = 16_000_000
BIT = F_CPU // 250_000          # 64 cycles per bit at 250 kbaud
FRAME = 11 * BIT                # 8N2 frame: 704 cycles = 44 us


def us(x):
    return int(round(x * 16))


class SimError(Exception):
    pass


class Reset(Exception):
    pass


# I/O space addresses (data address = io + 0x20)
ADCL, ADCH, ADCSRA, ADMUX = 0x04, 0x05, 0x06, 0x07
UBRRL, UCSRB, UCSRA, UDR = 0x09, 0x0A, 0x0B, 0x0C
PIND, DDRD, PORTD = 0x10, 0x11, 0x12
PINC, DDRC, PORTC = 0x13, 0x14, 0x15
PINB, DDRB, PORTB = 0x16, 0x17, 0x18
EECR, EEDR, EEARL, EEARH = 0x1C, 0x1D, 0x1E, 0x1F
UBRRH_UCSRC, WDTCR = 0x20, 0x21
OCR2, TCNT2, TCCR2 = 0x23, 0x24, 0x25
OCR1AL, OCR1AH, TCNT1L, TCNT1H, TCCR1B, TCCR1A = 0x2A, 0x2B, 0x2C, 0x2D, 0x2E, 0x2F
MCUCSR, SPMCR, TIFR, TIMSK, GICR = 0x34, 0x37, 0x38, 0x39, 0x3B
SPL, SPH, SREG = 0x3D, 0x3E, 0x3F

# UCSRA bits
RXC, TXC, UDRE, FE, DOR = 0x80, 0x40, 0x20, 0x10, 0x08
# UCSRB bits
RXCIE, TXCIE, UDRIE, RXEN, TXEN = 0x80, 0x40, 0x20, 0x10, 0x08
# TIFR / TIMSK bits
OCF2, TOV2, OCF1A, TOV1 = 0x80, 0x40, 0x10, 0x04
# SREG bits
C_, Z_, N_, V_, S_, H_, T_, I_ = 1, 2, 4, 8, 16, 32, 64, 128

VEC_TIMER2_COMP, VEC_TIMER1_COMPA, VEC_USART_RXC = 0x003, 0x006, 0x00B

WDT_CYCLES = [int(F_CPU * 0.016384 * (1 << p)) for p in range(8)]   # 16K WDT clocks @ 1 MHz << WDP
EE_WRITE_CYCLES = us(8500)
SPM_BUSY_CYCLES = us(4500)
PAGE = 64


class Cpu:
    def __init__(self, flash, eeprom=None, boot_start=0x1800, bootrst=True, seed=1):
        if len(flash) != 8192:
            raise SimError("flash image must be 8192 bytes")
        self.flash = bytearray(flash)
        self.eeprom = bytearray(eeprom if eeprom is not None else b"\xFF" * 512)
        self.boot_start = boot_start
        self.bootrst = bootrst
        self.rng = random.Random(seed)
        self.d = bytearray(0x460)
        self.cyc = 0
        self.dec = [None] * 4096
        self.trace_pc = False
        # line / pin history for the test harness
        self.tx_frames = []           # (start cycle, byte)
        self.portd_log = []           # (cycle, PORTD, DDRD, TXEN)
        self.resets = []              # (cycle, cause)
        self.reset_count = 0
        self.power_on()

    def __getstate__(self):
        st = dict(self.__dict__)
        st["dec"] = [None] * 4096                     # closures are rebuilt lazily
        return st

    # ------------------------------------------------------------------ reset
    def power_on(self):
        for a in range(0x60, 0x460):
            self.d[a] = self.rng.randrange(256)
        self._reset(0x01)             # PORF

    def _reset(self, cause_bit):
        for a in range(0, 0x60):
            self.d[a] = 0
        self.sreg = 0
        self.sp = 0
        self.temp = 0                 # 16-bit timer TEMP register
        self.mcucsr = cause_bit
        # USART
        self.ucsra = UDRE
        self.ucsrb = 0
        self.ucsrc = 0x86
        self.ubrr = 0
        self.rx_fifo = []             # [(byte, fe)]
        self.rx_queue = []            # [(complete cycle, start cycle, byte, fe)] sorted
        self.rxen_since = None
        self.tx_shift_end = None      # cycle when the current frame ends
        self.tx_buf = None            # byte waiting in UDR
        # timers
        self.tccr1a = self.tccr1b = 0
        self.t1 = 0
        self.t1_cyc = self.cyc
        self.t1_rem = 0
        self.ocr1a = 0
        self.tccr2 = 0
        self.t2 = 0
        self.t2_cyc = self.cyc
        self.ocr2 = 0
        self.tifr = 0
        self.timsk = 0
        # WDT (WDTON: always on, WDP = 0 after reset)
        self.wdp = 0
        self.wdce_cyc = -100
        self.wdt_deadline = self.cyc + WDT_CYCLES[0]
        # EEPROM (a running write completes across a reset)
        ee_busy = getattr(self, "ee_done", None)
        self.eecr = 0x02 if ee_busy is not None else 0
        self.eear = 0
        self.eedr = 0
        self.eemwe_cyc = -100
        self.ee_done = ee_busy
        # SPM
        self.spmcr = 0
        self.spmcr_cyc = -100
        self.spm_done = None
        self.rwwsb = False
        self.pagebuf = [0xFFFF] * (PAGE // 2)
        # ADC
        self.admux = self.adcsra = 0
        self.adc = 0
        self.adc_done = None
        self.adc_first = True
        # ports
        self.portb = self.ddrb = self.portc = self.ddrc = self.portd = self.ddrd = 0
        self.gicr = 0
        self.irq_block = 0            # one instruction after RETI / SEI
        self.pc = (self.boot_start >> 1) if self.bootrst else 0
        self.reset_count += 1
        self.resets.append((self.cyc, cause_bit))
        self._log_portd()
        self._sched()

    # ------------------------------------------------------------------ helpers
    def _sched(self):
        ev = [self.wdt_deadline]
        if self.rx_queue:
            ev.append(self.rx_queue[0][0])
        if self.tx_shift_end is not None:
            ev.append(self.tx_shift_end)
        if self.ee_done is not None:
            ev.append(self.ee_done)
        if self.spm_done is not None:
            ev.append(self.spm_done)
        if self.adc_done is not None:
            ev.append(self.adc_done)
        n1 = self._t1_next()
        if n1 is not None:
            ev.append(n1)
        n2 = self._t2_next()
        if n2 is not None:
            ev.append(n2)
        self.next_ev = min(ev)

    def _log_portd(self):
        self.portd_log.append((self.cyc, self.portd, self.ddrd, bool(self.ucsrb & TXEN)))

    # Timer1 ---------------------------------------------------------------
    def _t1_ps(self):
        cs = self.tccr1b & 7
        return {0: 0, 1: 1, 2: 8, 3: 64, 4: 256, 5: 1024}.get(cs, None)

    def _t1_ctc(self):
        return bool(self.tccr1b & 0x08) and not (self.tccr1a & 3)

    def _t1_sync(self):
        ps = self._t1_ps()
        if ps is None:
            raise SimError("Timer1 external clock not supported")
        if ps == 0:
            self.t1_cyc = self.cyc
            return
        total = self.cyc - self.t1_cyc + self.t1_rem
        ticks, self.t1_rem = divmod(total, ps)
        self.t1_cyc = self.cyc
        if not ticks:
            return
        if self._t1_ctc():
            top = self.ocr1a
            if self.t1 <= top:
                to_match = top - self.t1          # ticks until TCNT1 == TOP
                if ticks > to_match:              # TOP -> 0 transition happened
                    self.tifr |= OCF1A
                    rest = ticks - to_match - 1
                    self.t1 = rest % (top + 1)
                else:
                    self.t1 += ticks
            else:                                 # TOP lowered below TCNT1: run to 0xFFFF
                to_ovf = 0xFFFF - self.t1
                if ticks > to_ovf:
                    self.tifr |= TOV1
                    self.t1 = 0
                    ticks -= to_ovf + 1
                    self._t1_ticks_ctc(ticks)
                else:
                    self.t1 += ticks
        else:
            if (self.ocr1a - self.t1) % 0x10000 < ticks:
                self.tifr |= OCF1A
            v = self.t1 + ticks
            if v > 0xFFFF:
                self.tifr |= TOV1
            self.t1 = v & 0xFFFF

    def _t1_ticks_ctc(self, ticks):
        top = self.ocr1a
        if ticks > top:
            self.tifr |= OCF1A
            self.t1 = (ticks - top - 1) % (top + 1)
        else:
            self.t1 = ticks

    def _t1_next(self):
        ps = self._t1_ps()
        if not ps:
            return None
        if self._t1_ctc():
            if self.t1 <= self.ocr1a:
                ticks = self.ocr1a - self.t1 + 1
            else:
                ticks = 0x10000 - self.t1
        else:
            ticks = 0x10000 - self.t1
        return self.t1_cyc + ticks * ps - self.t1_rem

    # Timer2 ---------------------------------------------------------------
    def _t2_ps(self):
        cs = self.tccr2 & 7
        return {0: 0, 1: 1, 2: 8, 3: 32, 4: 64, 5: 128, 6: 256, 7: 1024}[cs]

    def _t2_sync(self):
        ps = self._t2_ps()
        if ps == 0:
            self.t2_cyc = self.cyc
            return
        ticks = (self.cyc - self.t2_cyc) // ps
        if not ticks:
            return
        self.t2_cyc += ticks * ps
        ctc = bool(self.tccr2 & 0x08)
        if ctc:
            top = self.ocr2
            if self.t2 <= top:
                to_match = top - self.t2
                if ticks > to_match:
                    self.tifr |= OCF2
                    self.t2 = (ticks - to_match - 1) % (top + 1)
                else:
                    self.t2 += ticks
            else:
                raise SimError("Timer2 CTC with TCNT2 > OCR2")
        else:
            v = self.t2 + ticks
            if v > 0xFF:
                self.tifr |= TOV2
            self.t2 = v & 0xFF

    def _t2_next(self):
        ps = self._t2_ps()
        if not ps:
            return None
        if self.tccr2 & 0x08:
            ticks = self.ocr2 - self.t2 + 1
        else:
            ticks = 0x100 - self.t2
        return self.t2_cyc + ticks * ps

    # events ---------------------------------------------------------------
    def _service(self):
        c = self.cyc
        if c >= self.wdt_deadline:
            self.mcucsr |= 0x08
            cause = self.mcucsr
            self.wdt_reset_event(cause)
            return
        while self.rx_queue and self.rx_queue[0][0] <= c:
            done, start, byte, fe = self.rx_queue.pop(0)
            if self.ucsrb & RXEN and self.rxen_since is not None and self.rxen_since <= start:
                if len(self.rx_fifo) >= 2:
                    self.ucsra |= DOR
                else:
                    self.rx_fifo.append((byte, fe))
        if self.tx_shift_end is not None and c >= self.tx_shift_end:
            if self.tx_buf is not None:
                b = self.tx_buf
                self.tx_buf = None
                self._tx_start(b, self.tx_shift_end)
            else:
                self.tx_shift_end = None
                self.ucsra |= TXC
        if self.ee_done is not None and c >= self.ee_done:
            self.ee_done = None
            self.eecr &= ~0x02
        if self.spm_done is not None and c >= self.spm_done:
            self.spm_done = None
            self.spmcr &= ~0x01
        if self.adc_done is not None and c >= self.adc_done:
            self.adc_done = None
            self.adcsra &= ~0x40            # ADSC
            self.adcsra |= 0x10             # ADIF
            self.adc = self.rng.randrange(1024)
        self._t1_sync()
        self._t2_sync()
        self._sched()

    def wdt_reset_event(self, cause):
        self._reset(0)
        self.mcucsr = cause | 0x08

    def _tx_start(self, b, start):
        self.tx_frames.append((start, b))
        stop = 2 if self.ucsrc & 0x08 else 1
        self.tx_shift_end = start + (1 + 8 + stop) * BIT

    # ------------------------------------------------------------------ data memory
    def rd(self, a):
        if a >= 0x60:
            if a >= 0x460:
                raise SimError(f"read beyond RAM 0x{a:04X} at pc 0x{self.pc * 2:04X}")
            return self.d[a]
        if a < 0x20:
            return self.d[a]
        return self.io_rd(a - 0x20)

    def wr(self, a, v):
        if a >= 0x60:
            if a >= 0x460:
                raise SimError(f"write beyond RAM 0x{a:04X} at pc 0x{self.pc * 2:04X}")
            self.d[a] = v
        elif a < 0x20:
            self.d[a] = v
        else:
            self.io_wr(a - 0x20, v)

    def io_rd(self, io):
        if io == SREG:
            return self.sreg
        if io == SPL:
            return self.sp & 0xFF
        if io == SPH:
            return self.sp >> 8
        if io == UCSRA:
            v = self.ucsra & ~(RXC | FE | UDRE)
            if self.tx_buf is None:
                v |= UDRE
            if self.rx_fifo:
                v |= RXC
                if self.rx_fifo[0][1]:
                    v |= FE
            return v
        if io == UDR:
            if self.rx_fifo:
                b, _ = self.rx_fifo.pop(0)
                if not self.rx_fifo:
                    self.ucsra &= ~DOR
                return b
            return 0
        if io == UCSRB:
            return self.ucsrb
        if io == TIFR:
            self._t1_sync()
            self._t2_sync()
            return self.tifr
        if io == TIMSK:
            return self.timsk
        if io == TCNT1L:
            self._t1_sync()
            self.temp = self.t1 >> 8
            return self.t1 & 0xFF
        if io == TCNT1H:
            return self.temp
        if io == TCNT2:
            self._t2_sync()
            return self.t2
        if io == TCCR1B:
            return self.tccr1b
        if io == MCUCSR:
            return self.mcucsr
        if io == WDTCR:
            return 0x08 | self.wdp
        if io == EECR:
            if self.eecr & 0x04 and self.cyc - self.eemwe_cyc > 4:
                self.eecr &= ~0x04
            return self.eecr
        if io == EEDR:
            return self.eedr
        if io == SPMCR:
            v = self.spmcr
            if self.rwwsb:
                v |= 0x40
            return v
        if io == PORTB:
            return self.portb
        if io == DDRB:
            return self.ddrb
        if io == PORTC:
            return self.portc
        if io == DDRC:
            return self.ddrc
        if io == PORTD:
            return self.portd
        if io == DDRD:
            return self.ddrd
        if io == PIND:
            return (self.portd & self.ddrd) | 0x01
        if io in (PINB, PINC):
            return 0
        if io == ADCSRA:
            return self.adcsra
        if io == ADMUX:
            return self.admux
        if io == ADCL:
            self.temp = self.adc >> 8
            return self.adc & 0xFF
        if io == ADCH:
            return self.temp
        if io == GICR:
            return self.gicr
        raise SimError(f"read of unsupported I/O 0x{io:02X} at pc 0x{self.pc * 2:04X}")

    def io_wr(self, io, v):
        c = self.cyc
        if io == SREG:
            self.sreg = v
        elif io == SPL:
            self.sp = (self.sp & 0xFF00) | v
        elif io == SPH:
            self.sp = (self.sp & 0x00FF) | (v << 8)
        elif io == UDR:
            if not (self.ucsrb & TXEN):
                return
            if self.tx_shift_end is None:
                self._tx_start(v, c)
            elif self.tx_buf is None:
                self.tx_buf = v
            else:
                raise SimError("UDR written while not empty")
            self._sched()
        elif io == UCSRA:
            if v & TXC:
                self.ucsra &= ~TXC
            self.ucsra = (self.ucsra & ~0x03) | (v & 0x03)
            if v & 0x02:
                raise SimError("U2X not expected")
        elif io == UCSRB:
            old = self.ucsrb
            self.ucsrb = v
            if (v & RXEN) and not (old & RXEN):
                self.rxen_since = c
            if not (v & RXEN):
                self.rx_fifo = []
                self.rxen_since = None
                self.ucsra &= ~DOR
            if (old ^ v) & TXEN:
                if not (v & TXEN) and (self.tx_shift_end is not None or self.tx_buf is not None):
                    raise SimError("TXEN cleared during transmission")
                self._log_portd()
        elif io == UBRRL:
            self.ubrr = (self.ubrr & 0xF00) | v
        elif io == UBRRH_UCSRC:
            if v & 0x80:
                self.ucsrc = v
            else:
                self.ubrr = (self.ubrr & 0xFF) | ((v & 0x0F) << 8)
        elif io == TIFR:
            self._t1_sync()
            self._t2_sync()
            self.tifr &= ~v
        elif io == TIMSK:
            self.timsk = v
        elif io == TCCR1A:
            self._t1_sync()
            self.tccr1a = v
            self._sched()
        elif io == TCCR1B:
            self._t1_sync()
            self.tccr1b = v
            self.t1_rem = 0
            self._sched()
        elif io == TCNT1H:
            self.temp = v
        elif io == TCNT1L:
            self._t1_sync()
            self.t1 = (self.temp << 8) | v
            self.t1_rem = 0
            self._sched()
        elif io == OCR1AH:
            self.temp = v
        elif io == OCR1AL:
            self._t1_sync()
            self.ocr1a = (self.temp << 8) | v
            self._sched()
        elif io == TCCR2:
            self._t2_sync()
            self.tccr2 = v
            self.t2_cyc = c
            self._sched()
        elif io == OCR2:
            self._t2_sync()
            self.ocr2 = v
            self._sched()
        elif io == TCNT2:
            self._t2_sync()
            self.t2 = v
            self._sched()
        elif io == MCUCSR:
            self.mcucsr = v & 0x0F if v else 0
        elif io == WDTCR:
            if v & 0x10 and v & 0x08:           # WDCE | WDE: open the 4-cycle window
                self.wdce_cyc = c
            elif c - self.wdce_cyc <= 4:
                self.wdp = v & 7
                self.wdt_deadline = c + WDT_CYCLES[self.wdp]
                self._sched()
            # other writes: ignored (WDTON, no timed sequence)
        elif io == EECR:
            mwe = bool(self.eecr & 0x04) and c - self.eemwe_cyc <= 4
            if v & 0x02 and mwe:                    # EEWE within 4 cycles after EEMWE
                if self.ee_done is not None:
                    raise SimError("EEPROM write while busy")
                self.eeprom[self.eear] = self.eedr
                self.ee_done = c + EE_WRITE_CYCLES
                self.eecr |= 0x02
                self.eecr &= ~0x04
                self._sched()
            elif v & 0x04 and not mwe:              # EEMWE: open the 4-cycle window
                self.eemwe_cyc = c
                self.eecr |= 0x04
            if v & 0x01:                            # EERE
                if self.ee_done is not None:
                    raise SimError(f"EEPROM read while a write is in progress (pc 0x{self.pc * 2:04X})")
                self.eedr = self.eeprom[self.eear]
                self.cyc += 4
            self.eecr = (self.eecr & 0x06) | (v & 0x08)
        elif io == EEDR:
            self.eedr = v
        elif io == EEARL:
            self.eear = (self.eear & 0x100) | v
        elif io == EEARH:
            self.eear = (self.eear & 0xFF) | ((v & 1) << 8)
        elif io == SPMCR:
            self.spmcr = (self.spmcr & 0x40) | (v & 0x9F)
            self.spmcr_cyc = c
        elif io == PORTB:
            self.portb = v
        elif io == DDRB:
            self.ddrb = v
        elif io == PORTC:
            self.portc = v
        elif io == DDRC:
            self.ddrc = v
        elif io == PORTD:
            self.portd = v
            self._log_portd()
        elif io == DDRD:
            self.ddrd = v
            self._log_portd()
        elif io == ADMUX:
            self.admux = v
        elif io == ADCSRA:
            if v & 0x10:
                self.adcsra &= ~0x10
            self.adcsra = (self.adcsra & 0x50) | (v & ~0x10 & 0xFF)
            if v & 0x40 and self.adc_done is None:
                ps = 1 << max(1, v & 7)
                self.adc_done = c + ps * (25 if self.adc_first else 13)
                self.adc_first = False
                self._sched()
            if not v & 0x80:
                self.adc_first = True
        elif io == GICR:
            if v & 0x03:
                raise SimError("IVSEL/IVCE not expected")
            self.gicr = v
        else:
            raise SimError(f"write of unsupported I/O 0x{io:02X} = 0x{v:02X} at pc 0x{self.pc * 2:04X}")

    # ------------------------------------------------------------------ flash
    def lpm(self, addr):
        if addr < self.boot_start and (self.rwwsb or self.spm_done is not None):
            raise SimError(f"LPM 0x{addr:04X} from the RWW section while it is busy/disabled "
                           f"(pc 0x{self.pc * 2:04X})")
        return self.flash[addr]

    def do_spm(self):
        if self.cyc - self.spmcr_cyc > 4 or not self.spmcr & 0x01:
            raise SimError(f"SPM without SPMEN within 4 cycles at pc 0x{self.pc * 2:04X}")
        if (self.pc - 1) * 2 < self.boot_start:
            raise SimError("SPM executed outside the boot section")
        z = self.d[30] | (self.d[31] << 8)
        op = self.spmcr & 0x1E
        if self.spm_done is not None:
            raise SimError("SPM while a previous SPM operation is busy")
        page = z & ~(PAGE - 1)
        if op == 0:                                     # fill page buffer
            self.pagebuf[(z >> 1) & (PAGE // 2 - 1)] = self.d[0] | (self.d[1] << 8)
            self.spmcr &= ~0x01
        elif op == 0x02:                                # page erase
            if page >= self.boot_start:
                raise SimError(f"page erase in the boot section 0x{page:04X}")
            self.flash[page:page + PAGE] = b"\xFF" * PAGE
            self._invalidate(page)
            self.rwwsb = True
            self.spm_done = self.cyc + SPM_BUSY_CYCLES
        elif op == 0x04:                                # page write
            if page >= self.boot_start:
                raise SimError(f"page write in the boot section 0x{page:04X}")
            for i, w in enumerate(self.pagebuf):
                self.flash[page + 2 * i] &= w & 0xFF        # flash programming can only clear bits
                self.flash[page + 2 * i + 1] &= w >> 8
            self.pagebuf = [0xFFFF] * (PAGE // 2)
            self._invalidate(page)
            self.rwwsb = True
            self.spm_done = self.cyc + SPM_BUSY_CYCLES
        elif op == 0x10:                                # RWW section read enable
            self.rwwsb = False
            self.pagebuf = [0xFFFF] * (PAGE // 2)
            self.spmcr &= ~0x01
        else:
            raise SimError(f"unsupported SPM operation SPMCR=0x{self.spmcr:02X}")
        self._sched()

    def _invalidate(self, page):
        for w in range(page >> 1, (page + PAGE) >> 1):
            self.dec[w] = None

    # ------------------------------------------------------------------ stack
    def push(self, v):
        self.wr(self.sp, v)
        self.sp = (self.sp - 1) & 0xFFFF

    def pop(self):
        self.sp = (self.sp + 1) & 0xFFFF
        return self.rd(self.sp)

    def push_pc(self, pc):
        self.push(pc & 0xFF)
        self.push(pc >> 8)

    def pop_pc(self):
        hi = self.pop()
        lo = self.pop()
        return (hi << 8) | lo

    # ------------------------------------------------------------------ run
    def run_until(self, until):
        dec = self.dec
        while self.cyc < until:
            pc = self.pc
            if pc >= 4096:
                raise SimError(f"PC out of flash 0x{pc * 2:05X}")
            if pc * 2 < self.boot_start and (self.rwwsb or self.spm_done is not None):
                raise SimError(f"executing RWW section 0x{pc * 2:04X} while busy/disabled")
            ins = dec[pc]
            if ins is None:
                ins = dec[pc] = self.decode(pc)
            fn, n = ins
            self.pc = pc + n
            self.cyc += fn(self)
            if self.cyc >= self.next_ev:
                self._service()
            if self.sreg & I_:
                if self.irq_block:
                    self.irq_block -= 1
                else:
                    self._irq()

    def _irq(self):
        vec = None
        f = self.tifr & self.timsk
        if f & OCF2 and self.tccr2 & 7:
            vec, clr = VEC_TIMER2_COMP, OCF2
        elif f & OCF1A:
            vec, clr = VEC_TIMER1_COMPA, OCF1A
        elif self.ucsrb & RXCIE and self.rx_fifo:
            vec, clr = VEC_USART_RXC, 0
        elif f & ~(OCF2 | OCF1A):
            raise SimError(f"unsupported interrupt TIFR&TIMSK=0x{f:02X}")
        if vec is None:
            return
        if self.gicr & 0x02:
            raise SimError("IVSEL set")
        self.tifr &= ~clr
        self.push_pc(self.pc)
        self.sreg &= ~I_
        self.pc = vec
        self.cyc += 4

    # ------------------------------------------------------------------ decoder
    def word(self, pc):
        return self.flash[2 * pc] | (self.flash[2 * pc + 1] << 8)

    def ins_len(self, pc):
        op = self.word(pc)
        if (op & 0xFE0F) in (0x9000, 0x9200) or (op & 0xFE0C) == 0x940C:
            return 2
        return 1

    def decode(self, pc):
        op = self.word(pc)
        return decode(op, pc, self)


# ---------------------------------------------------------------------- flags
def _add_flags(s, a, b, r, carry_in=0):
    r8 = r & 0xFF
    f = s & ~(C_ | Z_ | N_ | V_ | S_ | H_)
    if r > 0xFF:
        f |= C_
    if r8 == 0:
        f |= Z_
    if r8 & 0x80:
        f |= N_
    if (~(a ^ b) & (a ^ r8)) & 0x80:
        f |= V_
    if ((a & 0x0F) + (b & 0x0F) + carry_in) > 0x0F:
        f |= H_
    if bool(f & N_) != bool(f & V_):
        f |= S_
    return f


def _sub_flags(s, a, b, r, keepz=False):
    """r = a - b (- c) as a Python int (may be negative)."""
    r8 = r & 0xFF
    f = s & ~(C_ | Z_ | N_ | V_ | S_ | H_)
    if r < 0:
        f |= C_
    if keepz:
        if r8 == 0 and (s & Z_):
            f |= Z_
    elif r8 == 0:
        f |= Z_
    if r8 & 0x80:
        f |= N_
    if ((a ^ b) & (a ^ r8)) & 0x80:
        f |= V_
    if ((~a & b) | (b & r8) | (r8 & ~a)) & 0x08:
        f |= H_
    if bool(f & N_) != bool(f & V_):
        f |= S_
    return f


def _logic_flags(s, r):
    f = s & ~(Z_ | N_ | V_ | S_)
    if r == 0:
        f |= Z_
    if r & 0x80:
        f |= N_ | S_
    return f


def _signed_k(k, bits):
    return k - (1 << bits) if k & (1 << (bits - 1)) else k


# ---------------------------------------------------------------------- decode
def decode(op, pc, cpu):
    d5 = (op >> 4) & 0x1F
    r5 = (op & 0x0F) | ((op >> 5) & 0x10)
    d4 = 16 + ((op >> 4) & 0x0F)
    K8 = ((op >> 4) & 0xF0) | (op & 0x0F)

    if op == 0x0000:
        return (lambda c: 1), 1
    hi4 = op >> 12
    if (op & 0xFF00) == 0x0100:                          # MOVW
        dd, rr = ((op >> 4) & 0x0F) * 2, (op & 0x0F) * 2

        def f(c):
            c.d[dd] = c.d[rr]
            c.d[dd + 1] = c.d[rr + 1]
            return 1
        return f, 1
    if (op & 0xFF00) == 0x0200:                          # MULS
        dd, rr = 16 + ((op >> 4) & 0x0F), 16 + (op & 0x0F)

        def f(c):
            a, b = c.d[dd], c.d[rr]
            a -= 256 if a & 0x80 else 0
            b -= 256 if b & 0x80 else 0
            r = (a * b) & 0xFFFF
            c.d[0], c.d[1] = r & 0xFF, r >> 8
            c.sreg = (c.sreg & ~(C_ | Z_)) | (C_ if r & 0x8000 else 0) | (Z_ if r == 0 else 0)
            return 2
        return f, 1
    if (op & 0xFF88) == 0x0300:                          # MULSU
        dd, rr = 16 + ((op >> 4) & 7), 16 + (op & 7)

        def f(c):
            a, b = c.d[dd], c.d[rr]
            a -= 256 if a & 0x80 else 0
            r = (a * b) & 0xFFFF
            c.d[0], c.d[1] = r & 0xFF, r >> 8
            c.sreg = (c.sreg & ~(C_ | Z_)) | (C_ if r & 0x8000 else 0) | (Z_ if r == 0 else 0)
            return 2
        return f, 1
    top6 = op & 0xFC00
    if top6 in (0x0400, 0x1400):                         # CPC / CP
        keep = top6 == 0x0400

        def f(c):
            a, b = c.d[d5], c.d[r5]
            r = a - b - (c.sreg & C_ if keep else 0)
            c.sreg = _sub_flags(c.sreg, a, b, r, keepz=keep)
            return 1
        return f, 1
    if top6 in (0x0800, 0x1800):                         # SBC / SUB
        keep = top6 == 0x0800

        def f(c):
            a, b = c.d[d5], c.d[r5]
            r = a - b - (c.sreg & C_ if keep else 0)
            c.sreg = _sub_flags(c.sreg, a, b, r, keepz=keep)
            c.d[d5] = r & 0xFF
            return 1
        return f, 1
    if top6 in (0x0C00, 0x1C00):                         # ADD / ADC
        withc = top6 == 0x1C00

        def f(c):
            a, b = c.d[d5], c.d[r5]
            ci = c.sreg & C_ if withc else 0
            r = a + b + ci
            c.sreg = _add_flags(c.sreg, a, b, r, ci)
            c.d[d5] = r & 0xFF
            return 1
        return f, 1
    if top6 == 0x1000:                                   # CPSE
        def f(c):
            if c.d[d5] == c.d[r5]:
                n = c.ins_len(c.pc)
                c.pc += n
                return 1 + n
            return 1
        return f, 1
    if top6 == 0x2000:                                   # AND
        def f(c):
            r = c.d[d5] & c.d[r5]
            c.d[d5] = r
            c.sreg = _logic_flags(c.sreg, r)
            return 1
        return f, 1
    if top6 == 0x2400:                                   # EOR
        def f(c):
            r = c.d[d5] ^ c.d[r5]
            c.d[d5] = r
            c.sreg = _logic_flags(c.sreg, r)
            return 1
        return f, 1
    if top6 == 0x2800:                                   # OR
        def f(c):
            r = c.d[d5] | c.d[r5]
            c.d[d5] = r
            c.sreg = _logic_flags(c.sreg, r)
            return 1
        return f, 1
    if top6 == 0x2C00:                                   # MOV
        def f(c):
            c.d[d5] = c.d[r5]
            return 1
        return f, 1
    if hi4 == 0x3:                                       # CPI
        def f(c):
            a = c.d[d4]
            c.sreg = _sub_flags(c.sreg, a, K8, a - K8)
            return 1
        return f, 1
    if hi4 in (0x4, 0x5):                                # SBCI / SUBI
        keep = hi4 == 0x4

        def f(c):
            a = c.d[d4]
            r = a - K8 - (c.sreg & C_ if keep else 0)
            c.sreg = _sub_flags(c.sreg, a, K8, r, keepz=keep)
            c.d[d4] = r & 0xFF
            return 1
        return f, 1
    if hi4 == 0x6:                                       # ORI
        def f(c):
            r = c.d[d4] | K8
            c.d[d4] = r
            c.sreg = _logic_flags(c.sreg, r)
            return 1
        return f, 1
    if hi4 == 0x7:                                       # ANDI
        def f(c):
            r = c.d[d4] & K8
            c.d[d4] = r
            c.sreg = _logic_flags(c.sreg, r)
            return 1
        return f, 1
    if (op & 0xD000) == 0x8000:                          # LDD/STD Y+q / Z+q
        q = ((op >> 8) & 0x20) | ((op >> 7) & 0x18) | (op & 7)
        base = 28 if op & 0x0008 else 30
        store = bool(op & 0x0200)
        if store:
            def f(c):
                c.wr(((c.d[base] | (c.d[base + 1] << 8)) + q) & 0xFFFF, c.d[d5])
                return 2
        else:
            def f(c):
                c.d[d5] = c.rd(((c.d[base] | (c.d[base + 1] << 8)) + q) & 0xFFFF)
                return 2
        return f, 1
    if (op & 0xFE0F) == 0x9000:                          # LDS
        k = cpu.word(pc + 1)

        def f(c):
            c.d[d5] = c.rd(k)
            return 2
        return f, 2
    if (op & 0xFE0F) == 0x9200:                          # STS
        k = cpu.word(pc + 1)

        def f(c):
            c.wr(k, c.d[d5])
            return 2
        return f, 2
    if (op & 0xFC00) == 0x9000:                          # LD/ST/LPM/PUSH/POP variants
        store = bool(op & 0x0200)
        low = op & 0x0F
        if not store and low in (0x4, 0x5):              # LPM Rd, Z(+)
            inc = low == 0x5

            def f(c):
                z = c.d[30] | (c.d[31] << 8)
                c.d[d5] = c.lpm(z)
                if inc:
                    z = (z + 1) & 0xFFFF
                    c.d[30], c.d[31] = z & 0xFF, z >> 8
                return 3
            return f, 1
        if low == 0xF:
            if store:
                def f(c):
                    c.push(c.d[d5])
                    return 2
            else:
                def f(c):
                    c.d[d5] = c.pop()
                    return 2
            return f, 1
        modes = {0x1: (30, 1), 0x2: (30, -1), 0x9: (28, 1), 0xA: (28, -1), 0xC: (26, 0), 0xD: (26, 1), 0xE: (26, -1)}
        if low not in modes:
            raise SimError(f"unsupported opcode 0x{op:04X} at 0x{pc * 2:04X}")
        base, mode = modes[low]

        def f(c):
            p = c.d[base] | (c.d[base + 1] << 8)
            if mode < 0:
                p = (p - 1) & 0xFFFF
            if store:
                c.wr(p, c.d[d5])
            else:
                c.d[d5] = c.rd(p)
            if mode > 0:
                p = (p + 1) & 0xFFFF
            if mode:
                c.d[base], c.d[base + 1] = p & 0xFF, p >> 8
            return 2
        return f, 1
    if (op & 0xFE08) == 0x9400 and (op & 0x0F) < 8:     # one-operand group
        low = op & 0x0F
        if low == 0x0:                                   # COM
            def f(c):
                r = 0xFF - c.d[d5]
                c.d[d5] = r
                c.sreg = _logic_flags(c.sreg, r) | C_
                return 1
            return f, 1
        if low == 0x1:                                   # NEG
            def f(c):
                a = c.d[d5]
                r = (-a) & 0xFF
                c.d[d5] = r
                s = c.sreg & ~(C_ | Z_ | N_ | V_ | S_ | H_)
                if r:
                    s |= C_
                else:
                    s |= Z_
                if r & 0x80:
                    s |= N_
                if r == 0x80:
                    s |= V_
                if (r | a) & 0x08:
                    s |= H_
                if bool(s & N_) != bool(s & V_):
                    s |= S_
                c.sreg = s
                return 1
            return f, 1
        if low == 0x2:                                   # SWAP
            def f(c):
                v = c.d[d5]
                c.d[d5] = ((v << 4) | (v >> 4)) & 0xFF
                return 1
            return f, 1
        if low in (0x3, 0xA):                            # INC / DEC (0xA handled below)
            pass
        if low == 0x3:
            def f(c):
                r = (c.d[d5] + 1) & 0xFF
                c.d[d5] = r
                s = c.sreg & ~(Z_ | N_ | V_ | S_)
                if r == 0:
                    s |= Z_
                if r & 0x80:
                    s |= N_
                if r == 0x80:
                    s |= V_
                if bool(s & N_) != bool(s & V_):
                    s |= S_
                c.sreg = s
                return 1
            return f, 1
        if low in (0x5, 0x6, 0x7):                       # ASR / LSR / ROR
            kind = low

            def f(c):
                v = c.d[d5]
                cin = c.sreg & C_
                if kind == 0x5:
                    r = (v >> 1) | (v & 0x80)
                elif kind == 0x6:
                    r = v >> 1
                else:
                    r = (v >> 1) | (0x80 if cin else 0)
                c.d[d5] = r
                s = c.sreg & ~(C_ | Z_ | N_ | V_ | S_)
                if v & 1:
                    s |= C_
                if r == 0:
                    s |= Z_
                if r & 0x80:
                    s |= N_
                if bool(s & N_) != bool(s & C_):
                    s |= V_
                if bool(s & N_) != bool(s & V_):
                    s |= S_
                c.sreg = s
                return 1
            return f, 1
        raise SimError(f"unsupported opcode 0x{op:04X} at 0x{pc * 2:04X}")
    if (op & 0xFE0F) == 0x940A:                          # DEC
        def f(c):
            r = (c.d[d5] - 1) & 0xFF
            c.d[d5] = r
            s = c.sreg & ~(Z_ | N_ | V_ | S_)
            if r == 0:
                s |= Z_
            if r & 0x80:
                s |= N_
            if r == 0x7F:
                s |= V_
            if bool(s & N_) != bool(s & V_):
                s |= S_
            c.sreg = s
            return 1
        return f, 1
    if (op & 0xFF0F) == 0x9408:                          # BSET / BCLR
        bit = (op >> 4) & 7
        clear = bool(op & 0x0080)

        def f(c):
            if clear:
                c.sreg &= ~(1 << bit)
            else:
                c.sreg |= 1 << bit
                if bit == 7:
                    c.irq_block = 1                       # SEI: next instruction runs first
            return 1
        return f, 1
    if op == 0x9508:                                     # RET
        def f(c):
            c.pc = c.pop_pc()
            return 4
        return f, 1
    if op == 0x9518:                                     # RETI
        def f(c):
            c.pc = c.pop_pc()
            c.sreg |= I_
            c.irq_block = 1
            return 4
        return f, 1
    if op == 0x95A8:                                     # WDR
        def f(c):
            c.wdt_deadline = c.cyc + WDT_CYCLES[c.wdp]
            c._sched()
            return 1
        return f, 1
    if op == 0x95C8:                                     # LPM (r0)
        def f(c):
            c.d[0] = c.lpm(c.d[30] | (c.d[31] << 8))
            return 3
        return f, 1
    if op == 0x95E8:                                     # SPM
        def f(c):
            c.do_spm()
            return 1
        return f, 1
    if op == 0x9409:                                     # IJMP
        def f(c):
            c.pc = c.d[30] | (c.d[31] << 8)
            return 2
        return f, 1
    if op == 0x9509:                                     # ICALL
        def f(c):
            c.push_pc(c.pc)
            c.pc = c.d[30] | (c.d[31] << 8)
            return 3
        return f, 1
    if (op & 0xFE00) == 0x9600:                          # ADIW / SBIW
        dd = 24 + ((op >> 3) & 6)
        K = ((op >> 2) & 0x30) | (op & 0x0F)
        sub = bool(op & 0x0100)

        def f(c):
            v = c.d[dd] | (c.d[dd + 1] << 8)
            r = (v - K if sub else v + K) & 0xFFFF
            c.d[dd], c.d[dd + 1] = r & 0xFF, r >> 8
            s = c.sreg & ~(C_ | Z_ | N_ | V_ | S_)
            h7, r15 = bool(v & 0x8000), bool(r & 0x8000)
            if sub:
                if r15 and not h7:
                    s |= C_
                if h7 and not r15:
                    s |= V_
            else:
                if h7 and not r15:
                    s |= C_
                if r15 and not h7:
                    s |= V_
            if r == 0:
                s |= Z_
            if r15:
                s |= N_
            if bool(s & N_) != bool(s & V_):
                s |= S_
            c.sreg = s
            return 2
        return f, 1
    if (op & 0xFC00) == 0x9800:                          # CBI / SBIC / SBI / SBIS
        io = (op >> 3) & 0x1F
        bit = op & 7
        kind = (op >> 8) & 3

        def f(c):
            if kind == 0:
                c.io_wr(io, c.io_rd(io) & ~(1 << bit) & 0xFF)
                return 2
            if kind == 2:
                c.io_wr(io, c.io_rd(io) | (1 << bit))
                return 2
            v = bool(c.io_rd(io) & (1 << bit))
            if v == (kind == 3):
                n = c.ins_len(c.pc)
                c.pc += n
                return 1 + n
            return 1
        return f, 1
    if (op & 0xFC00) == 0x9C00:                          # MUL
        def f(c):
            r = c.d[d5] * c.d[r5]
            c.d[0], c.d[1] = r & 0xFF, r >> 8
            c.sreg = (c.sreg & ~(C_ | Z_)) | (C_ if r & 0x8000 else 0) | (Z_ if r == 0 else 0)
            return 2
        return f, 1
    if (op & 0xF800) == 0xB000:                          # IN
        io = ((op >> 5) & 0x30) | (op & 0x0F)

        def f(c):
            c.d[d5] = c.io_rd(io)
            return 1
        return f, 1
    if (op & 0xF800) == 0xB800:                          # OUT
        io = ((op >> 5) & 0x30) | (op & 0x0F)

        def f(c):
            c.io_wr(io, c.d[d5])
            return 1
        return f, 1
    if hi4 in (0xC, 0xD):                                # RJMP / RCALL
        k = _signed_k(op & 0x0FFF, 12)
        call = hi4 == 0xD

        def f(c):
            if call:
                c.push_pc(c.pc)
            c.pc = (c.pc + k) & 0x0FFF
            return 3 if call else 2
        return f, 1
    if hi4 == 0xE:                                       # LDI
        def f(c):
            c.d[d4] = K8
            return 1
        return f, 1
    if (op & 0xF800) == 0xF000:                          # BRBS / BRBC
        k = _signed_k((op >> 3) & 0x7F, 7)
        bit = 1 << (op & 7)
        want = not (op & 0x0400)

        def f(c):
            if bool(c.sreg & bit) == want:
                c.pc = (c.pc + k) & 0x0FFF
                return 2
            return 1
        return f, 1
    if (op & 0xFC08) == 0xF800:                          # BLD / BST
        bit = op & 7
        if op & 0x0200:
            def f(c):
                c.sreg = (c.sreg | T_) if c.d[d5] & (1 << bit) else (c.sreg & ~T_)
                return 1
        else:
            def f(c):
                c.d[d5] = (c.d[d5] | (1 << bit)) if c.sreg & T_ else (c.d[d5] & ~(1 << bit) & 0xFF)
                return 1
        return f, 1
    if (op & 0xFC08) == 0xFC00:                          # SBRC / SBRS
        bit = 1 << (op & 7)
        want = bool(op & 0x0200)

        def f(c):
            if bool(c.d[d5] & bit) == want:
                n = c.ins_len(c.pc)
                c.pc += n
                return 1 + n
            return 1
        return f, 1
    raise SimError(f"unsupported opcode 0x{op:04X} at 0x{pc * 2:04X}")


# ---------------------------------------------------------------------- images
def read_ihex(path):
    mem, base = {}, 0
    for line in open(path):
        line = line.strip()
        if not line.startswith(":"):
            continue
        r = bytes.fromhex(line[1:])
        n, a, t = r[0], (r[1] << 8) | r[2], r[3]
        if (sum(r) & 0xFF) != 0:
            raise SimError(f"{path}: bad record checksum")
        if t == 0:
            for i in range(n):
                mem[base + a + i] = r[4 + i]
        elif t == 2:
            base = ((r[4] << 8) | r[5]) << 4
        elif t == 4:
            base = ((r[4] << 8) | r[5]) << 16
        elif t == 1:
            break
    return mem


def flash_image(*hexes):
    img = bytearray(b"\xFF" * 8192)
    for h in hexes:
        for a, b in read_ihex(h).items():
            if a >= 8192:
                raise SimError(f"{h}: address 0x{a:04X} beyond flash")
            img[a] = b
    return img
