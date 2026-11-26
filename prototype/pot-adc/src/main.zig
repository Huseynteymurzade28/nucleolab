//! Potentiometer voltage reader for the NUCLEO-L452RE.
//!
//! Wiring:  pot outer leg -> 3V3
//!          pot middle leg (wiper) -> A0 (PA0 = ADC1 channel 5)
//!          pot other outer leg -> GND
//!
//! ADC1 measures A0 and VREFINT (internal 1.2 V reference). VREFINT plus the
//! factory calibration value gives the real VDD, so the millivolt result stays
//! correct even if the 3V3 rail isn't exactly 3.300 V.
//! Each reading is sent over USART2 (PA2 -> ST-LINK -> /dev/ttyACM0 on the PC,
//! 115200 8N1) as one text line:  "P <raw> <pot_mv> <vdd_mv>\n"
//! The same values are also kept in the dbg_* variables (watch.sh reads them).
//!
//! Register-level, no libraries. CPU runs on the 16 MHz HSI16 clock: it is
//! more accurate than the default MSI, which matters for the UART baud rate.

fn reg(addr: u32) *volatile u32 {
    return @ptrFromInt(addr);
}

const rcc = 0x4002_1000;
const rcc_cr = rcc + 0x00;
const rcc_cfgr = rcc + 0x08;
const rcc_ahb2enr = rcc + 0x4C;
const rcc_apb1enr1 = rcc + 0x58;

const gpioa_moder = 0x4800_0000;
const gpioa_afrl = 0x4800_0000 + 0x20;

const usart2 = 0x4000_4400;
const usart_cr1 = usart2 + 0x00;
const usart_brr = usart2 + 0x0C;
const usart_isr = usart2 + 0x1C;
const usart_tdr = usart2 + 0x28;

const adc1 = 0x5004_0000;
const adc_isr = adc1 + 0x00;
const adc_cr = adc1 + 0x08;
const adc_cfgr = adc1 + 0x0C;
const adc_smpr1 = adc1 + 0x14;
const adc_sqr1 = adc1 + 0x30;
const adc_dr = adc1 + 0x40;
const adc_ccr = adc1 + 0x300 + 0x08; // common register

/// VREFINT raw value measured by ST at 3.0 V, stored in system memory
const vrefint_cal: *const volatile u16 = @ptrFromInt(0x1FFF_75AA);

const ch_pot = 5; // PA0
const ch_vrefint = 0;

// --- Debug outputs ---------------------------------------------------------------
export var dbg_pot_raw: u32 = 0; // 0..4095
export var dbg_pot_mv: u32 = 0; // wiper voltage in millivolts
export var dbg_vdd_mv: u32 = 0; // measured supply voltage
export var dbg_percent: u32 = 0; // knob position 0..100
export var dbg_count: u32 = 0; // increments every reading (shows it's alive)

fn delay(n: u32) void {
    var i: u32 = 0;
    while (i < n) : (i += 1) asm volatile ("nop");
}

fn clockInit() void {
    reg(rcc_cr).* |= 1 << 8; // HSI16 on
    while (reg(rcc_cr).* & (1 << 10) == 0) {} // HSIRDY
    reg(rcc_cfgr).* = (reg(rcc_cfgr).* & ~@as(u32, 0b11)) | 0b01; // SYSCLK = HSI16
    while ((reg(rcc_cfgr).* >> 2) & 0b11 != 0b01) {}
}

fn uartInit() void {
    reg(rcc_ahb2enr).* |= 1 << 0; // GPIOA
    reg(rcc_apb1enr1).* |= 1 << 17; // USART2
    // PA2 = alternate function 7 (USART2_TX)
    reg(gpioa_afrl).* = (reg(gpioa_afrl).* & ~@as(u32, 0xF << 8)) | (7 << 8);
    reg(gpioa_moder).* = (reg(gpioa_moder).* & ~@as(u32, 0b11 << 4)) | (0b10 << 4);
    reg(usart_brr).* = (16_000_000 + 115_200 / 2) / 115_200; // 139
    reg(usart_cr1).* = (1 << 3) | (1 << 0); // TE + UE
}

fn uartByte(b: u8) void {
    while (reg(usart_isr).* & (1 << 7) == 0) {} // TXE
    reg(usart_tdr).* = b;
}

fn uartNum(n: u32) void {
    var buf: [10]u8 = undefined;
    var i: usize = buf.len;
    var v = n;
    while (true) {
        i -= 1;
        buf[i] = '0' + @as(u8, @intCast(v % 10));
        v /= 10;
        if (v == 0) break;
    }
    for (buf[i..]) |c| uartByte(c);
}

fn adcInit() void {
    reg(rcc_ahb2enr).* |= (1 << 0) | (1 << 13); // GPIOA + ADC clocks
    reg(gpioa_moder).* |= 0b11; // PA0 analog mode

    // ADC clock = HCLK/1 (synchronous), VREFINT on
    reg(adc_ccr).* = (0b01 << 16) | (1 << 22);

    // Leave deep power-down, start the internal regulator, wait >20 us
    reg(adc_cr).* = 1 << 28;
    delay(1000);

    // Calibrate (ADC must be off), wait until done
    reg(adc_cr).* |= 1 << 31;
    while (reg(adc_cr).* & (1 << 31) != 0) {}
    delay(100);

    // Overrun: keep the newest value. 12-bit, right aligned (defaults)
    reg(adc_cfgr).* = 1 << 12;
    // Longest sample time (640.5 cycles) on both channels: the pot has high
    // source impedance and VREFINT needs >= 4 us
    reg(adc_smpr1).* = (0b111 << (3 * ch_pot)) | (0b111 << (3 * ch_vrefint));

    // Enable and wait for ADRDY
    reg(adc_isr).* = 1;
    reg(adc_cr).* |= 1;
    while (reg(adc_isr).* & 1 == 0) {}
}

fn adcRead(ch: u32) u32 {
    reg(adc_sqr1).* = ch << 6; // one conversion, this channel
    reg(adc_cr).* |= 1 << 2; // ADSTART
    while (reg(adc_isr).* & (1 << 2) == 0) {} // EOC
    return reg(adc_dr).*; // reading DR clears EOC
}

/// Average of 16 readings to smooth out noise
fn adcAvg(ch: u32) u32 {
    var sum: u32 = 0;
    for (0..16) |_| sum += adcRead(ch);
    return sum / 16;
}

fn main() noreturn {
    clockInit();
    uartInit();
    adcInit();
    while (true) {
        const vref = adcAvg(ch_vrefint);
        const raw = adcAvg(ch_pot);
        const vdd = 3000 * @as(u32, vrefint_cal.*) / @max(vref, 1);
        dbg_vdd_mv = vdd;
        dbg_pot_raw = raw;
        dbg_pot_mv = raw * vdd / 4095;
        dbg_percent = raw * 100 / 4095;
        dbg_count +%= 1;

        uartByte('P');
        uartByte(' ');
        uartNum(raw);
        uartByte(' ');
        uartNum(dbg_pot_mv);
        uartByte(' ');
        uartNum(vdd);
        uartByte('\n');
        delay(20_000); // ~100 lines per second in total

    }
}

// --- Startup -------------------------------------------------------------------

extern var _estack: u32;
extern var _sidata: u32;
extern var _sdata: u32;
extern var _edata: u32;
extern var _sbss: u32;
extern var _ebss: u32;

const Handler = *const fn () callconv(.c) void;

export const vector_table: extern struct { sp: *const anyopaque, handlers: [15]Handler } linksection(".isr_vector") = .{
    .sp = &_estack,
    .handlers = blk: {
        var h: [15]Handler = @splat(hang);
        h[0] = @ptrCast(&resetHandler);
        break :blk h;
    },
};

export fn resetHandler() callconv(.c) noreturn {
    const data_len = @intFromPtr(&_edata) - @intFromPtr(&_sdata);
    @memcpy(@as([*]u8, @ptrCast(&_sdata))[0..data_len], @as([*]const u8, @ptrCast(&_sidata))[0..data_len]);
    const bss_len = @intFromPtr(&_ebss) - @intFromPtr(&_sbss);
    @memset(@as([*]u8, @ptrCast(&_sbss))[0..bss_len], 0);
    main();
}

fn hang() callconv(.c) void {
    while (true) {}
}
