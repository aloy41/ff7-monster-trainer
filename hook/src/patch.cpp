#include "patch.h"
#include "log.h"
#include <windows.h>
#include <string.h>

namespace patch {
namespace {

// One RWX page for all stubs; each stub is < 48 bytes.
uint8_t* g_pool = nullptr;
size_t g_used = 0;
constexpr size_t POOL = 4096;

uint8_t* alloc_stub(size_t n) {
    if (!g_pool) {
        g_pool = static_cast<uint8_t*>(VirtualAlloc(nullptr, POOL, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE));
        if (!g_pool) return nullptr;
    }
    if (g_used + n > POOL) return nullptr;
    uint8_t* p = g_pool + g_used;
    g_used += (n + 15) & ~size_t(15);
    return p;
}

void emit_rel(uint8_t*& p, uint8_t op, uintptr_t target) {
    *p = op;
    int32_t rel = static_cast<int32_t>(target - (reinterpret_cast<uintptr_t>(p) + 5));
    memcpy(p + 1, &rel, 4);
    p += 5;
}

void emit_abs32(uint8_t*& p, const uint8_t* op, size_t oplen, uint32_t imm) {
    memcpy(p, op, oplen);
    memcpy(p + oplen, &imm, 4);
    p += oplen + 4;
}

bool write_call(uintptr_t site, uintptr_t target) {
    DWORD old;
    if (!VirtualProtect(reinterpret_cast<void*>(site), 5, PAGE_EXECUTE_READWRITE, &old)) return false;
    uint8_t buf[5];
    buf[0] = 0xE8;
    int32_t rel = static_cast<int32_t>(target - (site + 5));
    memcpy(buf + 1, &rel, 4);
    memcpy(reinterpret_cast<void*>(site), buf, 5);
    VirtualProtect(reinterpret_cast<void*>(site), 5, old, &old);
    FlushInstructionCache(GetCurrentProcess(), reinterpret_cast<void*>(site), 5);
    return true;
}

bool check_site(uintptr_t site, uintptr_t expected_target, const char* name, uintptr_t& callee) {
    callee = call_target(site);
    const uint8_t op = *reinterpret_cast<const uint8_t*>(site);
    if (op != 0xE8 || !callee) {
        tlog("PATCH FAIL %s: %08X is %02X, not a call", name, (unsigned)site, op);
        return false;
    }
    if (expected_target && callee != expected_target) {
        tlog("PATCH NOTE %s: %08X calls %08X (expected %08X) - another hook is there, chaining onto it", name,
             (unsigned)site, (unsigned)callee, (unsigned)expected_target);
    }
    return true;
}

}  // namespace

uintptr_t call_target(uintptr_t site) {
    const uint8_t* p = reinterpret_cast<const uint8_t*>(site);
    if (p[0] != 0xE8 && p[0] != 0xE9) return 0;
    int32_t rel;
    memcpy(&rel, p + 1, 4);
    return site + 5 + rel;
}

bool hook_call_after(uintptr_t site, uintptr_t expected_target, AfterFn after, const char* name) {
    uintptr_t callee;
    if (!check_site(site, expected_target, name, callee)) return false;
    // stub: pop [saved_ret]; call callee; pushad; push eax; call after; add esp,4; popad; push [saved_ret]; ret
    uint8_t* stub = alloc_stub(48);
    uint32_t* saved = reinterpret_cast<uint32_t*>(alloc_stub(4));
    if (!stub || !saved) return false;
    uint8_t* p = stub;
    static const uint8_t POP_M[] = {0x8F, 0x05};
    static const uint8_t PUSH_M[] = {0xFF, 0x35};
    emit_abs32(p, POP_M, 2, reinterpret_cast<uint32_t>(saved));
    emit_rel(p, 0xE8, callee);
    *p++ = 0x60;  // pushad
    *p++ = 0x50;  // push eax
    emit_rel(p, 0xE8, reinterpret_cast<uintptr_t>(after));
    *p++ = 0x83; *p++ = 0xC4; *p++ = 0x04;  // add esp, 4
    *p++ = 0x61;  // popad
    emit_abs32(p, PUSH_M, 2, reinterpret_cast<uint32_t>(saved));
    *p++ = 0xC3;  // ret
    if (!write_call(site, reinterpret_cast<uintptr_t>(stub))) return false;
    tlog("hooked %s: call at %08X -> stub -> %08X, then after()", name, (unsigned)site, (unsigned)callee);
    return true;
}

bool hook_call_after_args(uintptr_t site, uintptr_t expected_target, AfterArgsFn after, const char* name) {
    uintptr_t callee;
    if (!check_site(site, expected_target, name, callee)) return false;
    // stub: pop [saved_ret]; call callee; pushad; lea ecx,[esp+32]; push ecx; push eax; call after; add esp,8; popad;
    //       push [saved_ret]; ret      (after popping our return address, [esp] is the first argument)
    uint8_t* stub = alloc_stub(48);
    uint32_t* saved = reinterpret_cast<uint32_t*>(alloc_stub(4));
    if (!stub || !saved) return false;
    uint8_t* p = stub;
    static const uint8_t POP_M[] = {0x8F, 0x05};
    static const uint8_t PUSH_M[] = {0xFF, 0x35};
    emit_abs32(p, POP_M, 2, reinterpret_cast<uint32_t>(saved));
    emit_rel(p, 0xE8, callee);
    *p++ = 0x60;                                      // pushad
    *p++ = 0x8D; *p++ = 0x4C; *p++ = 0x24; *p++ = 0x20;  // lea ecx, [esp+32]
    *p++ = 0x51;                                      // push ecx (args)
    *p++ = 0x50;                                      // push eax (result)
    emit_rel(p, 0xE8, reinterpret_cast<uintptr_t>(after));
    *p++ = 0x83; *p++ = 0xC4; *p++ = 0x08;            // add esp, 8
    *p++ = 0x61;                                      // popad
    emit_abs32(p, PUSH_M, 2, reinterpret_cast<uint32_t>(saved));
    *p++ = 0xC3;
    if (!write_call(site, reinterpret_cast<uintptr_t>(stub))) return false;
    tlog("hooked %s: call at %08X -> stub -> %08X, then after(eax, args)", name, (unsigned)site, (unsigned)callee);
    return true;
}

bool hook_call_before_args(uintptr_t site, uintptr_t expected_target, BeforeArgsFn before, const char* name) {
    uintptr_t callee;
    if (!check_site(site, expected_target, name, callee)) return false;
    // stub: pushad; lea ecx,[esp+36]; push ecx; call before; add esp,4; popad; jmp callee
    // (at stub entry [esp] = return address, [esp+4] = first argument; pushad adds 32)
    uint8_t* stub = alloc_stub(32);
    if (!stub) return false;
    uint8_t* p = stub;
    *p++ = 0x60;
    *p++ = 0x8D; *p++ = 0x4C; *p++ = 0x24; *p++ = 0x24;  // lea ecx, [esp+36]
    *p++ = 0x51;
    emit_rel(p, 0xE8, reinterpret_cast<uintptr_t>(before));
    *p++ = 0x83; *p++ = 0xC4; *p++ = 0x04;
    *p++ = 0x61;
    emit_rel(p, 0xE9, callee);
    if (!write_call(site, reinterpret_cast<uintptr_t>(stub))) return false;
    tlog("hooked %s: call at %08X -> before(args) -> %08X", name, (unsigned)site, (unsigned)callee);
    return true;
}

bool hook_call_before(uintptr_t site, uintptr_t expected_target, BeforeFn before, const char* name) {
    uintptr_t callee;
    if (!check_site(site, expected_target, name, callee)) return false;
    // stub: pushad; call before; popad; jmp callee   (callee sees the original return address and args)
    uint8_t* stub = alloc_stub(24);
    if (!stub) return false;
    uint8_t* p = stub;
    *p++ = 0x60;
    emit_rel(p, 0xE8, reinterpret_cast<uintptr_t>(before));
    *p++ = 0x61;
    emit_rel(p, 0xE9, callee);
    if (!write_call(site, reinterpret_cast<uintptr_t>(stub))) return false;
    tlog("hooked %s: call at %08X -> before() -> %08X", name, (unsigned)site, (unsigned)callee);
    return true;
}

}  // namespace patch
