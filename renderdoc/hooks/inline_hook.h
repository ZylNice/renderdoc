/******************************************************************************
 * The MIT License (MIT)
 *
 * Copyright (c) 2019-2024 Baldur Karlsson
 *
 * Permission is hereby granted, free of charge, to any person obtaining a copy
 * of this software and associated documentation files (the "Software"), to deal
 * in the Software without restriction, including without limitation the rights
 * to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
 * copies of the Software, and to permit persons to whom the Software is
 * furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in
 * all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 * AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 * OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
 * THE SOFTWARE.
 ******************************************************************************/

#pragma once

// Minimal x64 inline (detour-style) function hooking.
//
// Unlike the IAT-based hooks in hooks.cpp, this patches the first bytes of the
// target function itself. This is necessary when the caller resolves function
// pointers through paths that bypass IAT/GetProcAddress interception (e.g.
// anti-cheat protected games that walk export tables manually or cache
// pointers before IAT hooks can be applied).
//
// The hook is installed as a 14-byte absolute indirect jump:
//
//   FF 25 00 00 00 00    jmp qword ptr [rip+0]
//   <8 byte detour address>
//
// The stolen prologue bytes are copied to a freshly allocated trampoline
// followed by a jump back to the remainder of the original function. The
// trampoline is returned so the detour can call onwards.
//
// Safety notes:
//  - If the prologue contains any relative branch (call/jmp/jcc) or an
//    unknown opcode within the first 14 bytes, installation fails safely
//    (returns NULL) instead of producing a broken trampoline.
//  - The patch is not atomic with respect to other threads already executing
//    the target prologue. It should be installed as early as possible, before
//    the target function is first called.
//  - Windows x64 only.

#if defined(_WIN64)

#include <windows.h>
#include <stdint.h>
#include <string.h>

namespace InlineHook
{
// length of the absolute indirect jump we write over the function prologue
static const size_t JUMP_LENGTH = 14;

namespace Internal
{
// returns the length of the ModRM + SIB + displacement sequence, where p
// points at the ModRM byte
inline size_t ModRMLength(const uint8_t *p)
{
  uint8_t modrm = p[0];
  uint8_t mod = modrm >> 6;
  uint8_t rm = modrm & 7;

  size_t len = 1;

  if(mod == 3)
    return len;

  bool hasSIB = (rm == 4);
  uint8_t sibBase = 0;

  if(hasSIB)
  {
    uint8_t sib = p[1];
    sibBase = sib & 7;
    len += 1;
  }

  if(mod == 0)
  {
    if(rm == 5 || (hasSIB && sibBase == 5))
      len += 4;    // disp32
  }
  else if(mod == 1)
  {
    len += 1;    // disp8
  }
  else if(mod == 2)
  {
    len += 4;    // disp32
  }

  return len;
}

// Decodes the length of a single x64 instruction for the subset of opcodes
// expected in compiler-generated function prologues. Returns 0 for unknown
// opcodes or control-flow instructions that cannot be safely relocated
// without fixups.
inline size_t InstructionLength(const uint8_t *code)
{
  const uint8_t *p = code;
  bool rexW = false;
  bool op16 = false;

  // skip legacy and REX prefixes
  for(;;)
  {
    uint8_t b = *p;
    if(b == 0xF0 || b == 0xF2 || b == 0xF3 ||    // lock / repne / rep
       b == 0x2E || b == 0x36 || b == 0x3E ||    // segment overrides
       b == 0x26 || b == 0x64 || b == 0x65)
    {
      p++;
      continue;
    }
    if(b == 0x66)
    {
      op16 = true;
      p++;
      continue;
    }
    if(b == 0x67)
    {
      // address-size override: not expected in prologues, bail out
      return 0;
    }
    if((b & 0xF0) == 0x40)
    {
      rexW = (b & 0x08) != 0;
      p++;
      continue;
    }
    break;
  }

  const uint8_t *opcodeStart = p;
  uint8_t op = *p++;
  bool twoByte = false;

  if(op == 0x0F)
  {
    twoByte = true;
    op = *p++;
  }

  size_t modrmLen = 0;
  size_t immLen = 0;

  if(!twoByte)
  {
    // single-byte opcodes that are just one byte (no ModRM, no immediate)
    if((op >= 0x50 && op <= 0x5F) ||    // push/pop r64
       op == 0x90 ||                    // nop
       op == 0x98 || op == 0x99 ||      // cbw/cwde/cdqe, cwd/cdq/cqo
       op == 0x9C || op == 0x9D ||      // pushfq/popfq
       op == 0xC9)                      // leave
    {
      return size_t(p - code);
    }

    if(op == 0x6A)    // push imm8
      immLen = 1;
    else if(op == 0x68)    // push imm32
      immLen = 4;
    else if(op == 0xA8)    // test al, imm8
      immLen = 1;
    else if(op == 0xA9)    // test eax, imm32
      immLen = op16 ? 2 : 4;
    else if(op >= 0xB0 && op <= 0xB7)    // mov r8, imm8
      immLen = 1;
    else if(op >= 0xB8 && op <= 0xBF)    // mov r, imm
      immLen = rexW ? 8 : (op16 ? 2 : 4);
    else if(op == 0x05 || op == 0x0D || op == 0x15 || op == 0x1D || op == 0x25 ||
            op == 0x2D || op == 0x35 || op == 0x3D)    // op eax, imm32
      immLen = op16 ? 2 : 4;
    else if(op == 0x04 || op == 0x0C || op == 0x14 || op == 0x1C || op == 0x24 ||
            op == 0x2C || op == 0x34 || op == 0x3C)    // op al, imm8
      immLen = 1;
    else if(op == 0x80)    // grp1 r/m8, imm8
    {
      modrmLen = ModRMLength(p);
      immLen = 1;
    }
    else if(op == 0x81)    // grp1 r/m, imm32
    {
      modrmLen = ModRMLength(p);
      immLen = op16 ? 2 : 4;
    }
    else if(op == 0x83)    // grp1 r/m, imm8
    {
      modrmLen = ModRMLength(p);
      immLen = 1;
    }
    else if(op == 0xC6)    // mov r/m8, imm8
    {
      modrmLen = ModRMLength(p);
      immLen = 1;
    }
    else if(op == 0xC7)    // mov r/m, imm32
    {
      modrmLen = ModRMLength(p);
      immLen = op16 ? 2 : 4;
    }
    else if(op == 0xF6 || op == 0xF7)    // grp3: test/not/neg/mul/imul/div/idiv
    {
      modrmLen = ModRMLength(p);
      uint8_t reg = (p[0] >> 3) & 7;
      if(reg <= 1)    // test r/m, imm
        immLen = (op == 0xF6) ? 1 : (op16 ? 2 : 4);
    }
    else if(op == 0xFF)    // grp5: inc/dec/call/jmp/push r/m
    {
      uint8_t reg = (p[0] >> 3) & 7;
      // reject indirect call/jmp - control flow can't be relocated safely
      if(reg >= 2 && reg <= 5)
        return 0;
      modrmLen = ModRMLength(p);
    }
    else if(op == 0xFE)    // grp4: inc/dec r/m8
    {
      modrmLen = ModRMLength(p);
    }
    else if((op >= 0x00 && op <= 0x03) || (op >= 0x08 && op <= 0x0B) ||
            (op >= 0x10 && op <= 0x13) || (op >= 0x18 && op <= 0x1B) ||
            (op >= 0x20 && op <= 0x23) || (op >= 0x28 && op <= 0x2B) ||
            (op >= 0x30 && op <= 0x33) || (op >= 0x38 && op <= 0x3B) ||
            (op >= 0x84 && op <= 0x8D) || op == 0x8F || op == 0x63)
    {
      // alu ops, test, xchg, mov, lea, pop r/m, movsxd - all plain ModRM
      modrmLen = ModRMLength(p);
    }
    else
    {
      // anything else: branches (E8/E9/EB/70-7F), ret, int3, string ops, etc.
      return 0;
    }
  }
  else
  {
    // two-byte opcodes (0F xx)
    if(op == 0x05 || op == 0x31 || op == 0xA2)    // syscall / rdtsc / cpuid
    {
      return size_t(p - code);
    }
    else if(op >= 0x80 && op <= 0x8F)    // jcc rel32 - cannot relocate
    {
      return 0;
    }
    else if(op == 0x38 || op == 0x3A)    // three-byte maps
    {
      p++;    // third opcode byte
      modrmLen = ModRMLength(p);
      if(op == 0x3A)
        immLen = 1;
    }
    else
    {
      // movaps/movups/movdqu/nop/setcc/cmov/movzx/movsx/imul/bsr/bsf/etc -
      // all plain ModRM instructions with no immediate
      modrmLen = ModRMLength(p);
    }
  }

  (void)opcodeStart;
  return size_t(p - code) + modrmLen + immLen;
}
}    // namespace Internal

// Installs an inline hook on `target` redirecting execution to `detour`.
// Returns a callable trampoline pointing to the original function, or NULL on
// failure (in which case the target is left unmodified).
inline void *Install(void *target, void *detour)
{
  if(!target || !detour)
    return NULL;

  uint8_t code[32];
  memcpy(code, target, sizeof(code));

  // decode whole instructions until we have at least JUMP_LENGTH bytes
  size_t stolen = 0;
  while(stolen < JUMP_LENGTH)
  {
    size_t len = Internal::InstructionLength(code + stolen);
    if(len == 0 || stolen + len > sizeof(code))
      return NULL;
    stolen += len;
  }

  // build the trampoline: stolen bytes + jump back to target+stolen
  uint8_t *tramp =
      (uint8_t *)VirtualAlloc(NULL, 64, MEM_RESERVE | MEM_COMMIT, PAGE_EXECUTE_READWRITE);
  if(!tramp)
    return NULL;

  memcpy(tramp, code, stolen);
  tramp[stolen + 0] = 0xFF;
  tramp[stolen + 1] = 0x25;
  *(uint32_t *)(tramp + stolen + 2) = 0;
  *(void **)(tramp + stolen + 6) = (uint8_t *)target + stolen;
  FlushInstructionCache(GetCurrentProcess(), tramp, 64);

  // patch the target with an absolute indirect jump to the detour
  DWORD oldProtect = 0;
  if(!VirtualProtect(target, JUMP_LENGTH, PAGE_EXECUTE_READWRITE, &oldProtect))
  {
    VirtualFree(tramp, 0, MEM_RELEASE);
    return NULL;
  }

  uint8_t *t = (uint8_t *)target;
  t[0] = 0xFF;
  t[1] = 0x25;
  *(uint32_t *)(t + 2) = 0;
  *(void **)(t + 6) = detour;

  // pad any remaining stolen bytes with int3 so stale execution fails loudly
  for(size_t i = JUMP_LENGTH; i < stolen; i++)
    t[i] = 0xCC;

  DWORD tmp = 0;
  VirtualProtect(target, JUMP_LENGTH, oldProtect, &tmp);
  FlushInstructionCache(GetCurrentProcess(), target, stolen);

  return tramp;
}

};    // namespace InlineHook

#endif    // defined(_WIN64)
