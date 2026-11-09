#include <windows.h>
#include <tlhelp32.h>
#include <tchar.h>
#include <vector>
#include <string>
#include <array>
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdint>
#include <cstdarg>
#include <cstring>

using std::abs;

/* ------------------------------------------------------------------ */
/* data.h — x64 equivalents of the repo's x32 defines                   */
/* ------------------------------------------------------------------ */

/* x32 : #define CInstanceBase 0x16F7754  (global -> +0x18 = main actor)
   x64 : the instance manager is a std::map, not a flat array.
         base + MGR_RVA -> mgr ; mgr+0x38 -> head ; head+8 -> root ;
         node+0x20 = VID (DWORD), node+0x28 = instance (pointer). */
#define MGR_RVA        0xE059B8u   /* aoe99 g_rva_mgr     */
#define NET_RVA        0xE05BA8u   /* aoe99 g_rva_net     */
#define MAINVID_RVA    0x205D50u   /* netGetMainActorVID  */
#define MGTYPE_RVA     0x1B7CC0u   /* chrGetInstanceType -> real method */
#define POS_RVA        0x1E6B20u   /* chrGetPixelPosition -> real method */
#define SENDATTACK_RVA 0x22AE50u   /* CPythonNetworkStream::SendAttackPacket */

/* x32 : CPythonPlayer 0x16F7780, a global. x64 : same, a global at
   base+0xE07A48. WARNING: base+0x0358C0 is NOT this accessor - it is
   a lazy-init returning [base+0xE047B8], a different object. The
   constructor at 0x245EA0 does store `this` into 0xE07A48 (at 0x245F4A),
   and the playerGetTargetVID wrapper (0xD9C7F) loads rcx from 0xE07A48
   then calls 0x23DF60 WITHOUT adjusting this. Verified live: the field
   reads 0 while aoe99 logs "target 0". */
#define PLAYER_PTR_RVA 0xE07A48u   /* instance pointer, NOT a function */
#define GETTARGET_RVA  0x23DF60u   /* mov eax,[rcx+0x1B324]; ret */

/* The source's "go and attack". x32 : x86 cave grafted on the main loop,
   calling OnPressActor(ecx=player, push 1, push vid, push mainActor).
   x64 : CPythonPlayer::PressActor at 0x23EE00, SAME fastcall arguments
   (rcx=player, rdx=mainActor, r8d=vid, r9d=1). Too far -> action state 3
   + vid queued at +0x1B15C ; the run is executed by the player's Update
   (0x247F60, case 0x248015), arrival triggers SetTarget then the attack
   (0x2539F0). Not safe from a foreign thread: std::map mutation,
   singleton TLS, rotation shared with rendering -> main thread.
   Graft point: CPythonApplication::Update (0x16B860) does
   `4C 89 F9` (mov rcx,r15 = player) then `E8 92 4F 0D 00` (call 0x240AE0,
   player Update) at base+0x16BB49. Only the rel32 is rewritten: the E8
   opcode stays, the switch is a single 4-byte store. */
#define PRESSACTOR_RVA 0x23EE00u
#define HOOK_RVA       0x16BB49u   /* call 0x240AE0, preceded by mov rcx,r15 */
#define ORIGUPDATE_RVA 0x240AE0u   /* CPythonPlayer::Update, original callee */

/* x32 : Offset_isDead 0x350 (via the mixed table +0x214), direct bool.
   x64 : 0x838 (RecvDeadPacket) NEVER sets on this build (corpses = 0).
   Reliable signal: animation frame counter at +0x488, 4-byte int.
   ALIVE: loops, climbs ~60/s then drops back to zero (measured peaks 190-217).
   DEAD: climbs straight without looping, 322-714 then freezes.
   Slow net (250): covers instances with no history, ~4 s latency.
   ZERO-GAP path: latch + 7D0 empty = instant death.
   LATCH ON TWO FIELDS. 0x7D0 carries our VID when the MOB strikes back;
   0x320 carries our VID from THE FIRST STRIKE we land ("0 -> our vid at
   the first hit, stays", aoe99 OFF_ATK_A). Without 0x320, a mob killed in
   one hit NEVER retaliated, never latched: its corpse stays "alive" ~4 s
   while 488 climbs to 250. That is the "Attack Bug" video bug: after 6-8
   fast deaths the bot catches up to corpses and hits them. With the latch
   on 0x320 too, every mob we hit latches, and its corpse exits the instant
   7D0 drops back to zero.
   Cost: any live mob we touched that disengages is declared dead and is
   skipped until its slot recycles (512, oldest first). Tradeoff already
   accepted for the zero-gap.
   The 250 net stays for deaths with no field at all (stone, rare edge). */
#define Offset_isDead   0x488u
#define Offset_atkA     0x320u   /* DWORD: 0 -> our vid at the first strike, stays */
#define Offset_atkB     0x7D0u   /* DWORD: 0 <-> our VID, follows the current strike */
#define DEAD_488_THRESH 250      /* slow net */

/* x32 : Offset_name 0x24. Not reversed on x64 ; getName() is never called
   anywhere in the bot logic, the read is kept as is. */
#define Offset_name    0x24u

/* x32 : Offset_setAttackStatus 0x54, read as a short.
   x64 : BYTE at +0x80. Proof: the real SetAttackKeyState(bool) at 0x2528A0
   does `mov BYTE PTR [rsi+0x80], bl` at 0x2528DC (rsi = this, 0x2528A8).
   The player update path compares that same byte twice:
   `cmp BYTE PTR [rsi+0x80], 0x1` at 0x24829D and 0x248314, branching to
   the attack routine 0x23FA20. Neighbour bytes +0x7C..+0x7F/+0x81 are the
   direction keys (SetSingleDIKKeyState, 0x2528F0). */
#define Offset_setAttackStatus 0x80u

/* x32 : Offset_getTargetVid 0x4B004 via the mixed table.
   x64 : direct DWORD, confirmed by GetTargetVID 0x23DF60. */
#define Offset_targetVid       0x1B324u

#define MOB_SLOTS      400       /* original loop: for i < 400 */
#define TYPE_MOB       0
#define TYPE_METIN     2

typedef uintptr_t ADDR;

/* ------------------------------------------------------------------ */
/* KeInterface — same template API as KeInterface.hpp, in-process backend  */
/* ------------------------------------------------------------------ */

#define PAGE_SIZE 0x1000

class KeInterface {
public:
    HANDLE hDriver;

    KeInterface() : hDriver(nullptr) {}
    KeInterface(LPCSTR) : hDriver(nullptr) {}   /* RegistryPath ignored: no driver anymore */

    template <typename type>
    type ReadVirtualMemory(ULONG, ADDR ReadAddress, SIZE_T Size) {
        type Buffer{};
        if (!readable(ReadAddress, Size ? Size : sizeof(type))) return Buffer;
        memcpy(&Buffer, (const void*)ReadAddress, Size ? Size : sizeof(type));
        return Buffer;
    }

    template <typename type>
    bool WriteVirtualMemory(ULONG, ADDR WriteAddress, type WriteValue, SIZE_T WriteSize) {
        if (!readable(WriteAddress, WriteSize)) return false;
        memcpy((void*)WriteAddress, &WriteValue, WriteSize);
        return true;
    }

    ADDR AllocMem(ULONG, ULONG size) {
        return (ADDR)VirtualAlloc(nullptr, size, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE);
    }

    bool VirtualProtect(ULONG, ADDR Address, ULONG size, ULONG Protect, PULONG OldProtect) {
        return ::VirtualProtect((void*)Address, size, Protect, OldProtect) != 0;
    }

    void FreeMemory(ULONG, ADDR Address, ULONG) {
        VirtualFree((void*)Address, 0, MEM_RELEASE);
    }

private:
    static bool readable(ADDR p, SIZE_T len) {
        MEMORY_BASIC_INFORMATION mbi;
        if (!p || !len) return false;
        ADDR end = p + len;
        if (end < p) return false;
        while (p < end) {
            if (::VirtualQuery((void*)p, &mbi, sizeof(mbi)) != sizeof(mbi)) return false;
            if (mbi.State != MEM_COMMIT) return false;
            if (mbi.Protect & (PAGE_NOACCESS | PAGE_GUARD)) return false;
            ADDR region = (ADDR)mbi.BaseAddress + mbi.RegionSize;
            if (region <= p) return false;
            p = region;
        }
        return true;
    }
};

static KeInterface Driver;
static DWORD pID = 0;
static ADDR ModuleBase = 0;

/* ------------------------------------------------------------------ */
/* hackMain.h — original structures, Instance as ADDR (x64)             */
/* ------------------------------------------------------------------ */

typedef struct D3DXVECTOR3 {
    float x, y, z;
} D3DXVECTOR3, *LPD3DXVECTOR3;

typedef struct InstanceObj {
    ADDR Instance;
    DWORD VID;
    D3DXVECTOR3 Pos;
    DWORD Type;
    DWORD Distance;
} InstanceObj, *PInstanceObj;

/* ------------------------------------------------------------------ */
/* state lifted from hackMain.cpp / frmMain.h                           */
/* ------------------------------------------------------------------ */

bool FarmBot_FixedRange = false;
D3DXVECTOR3 FarmBot_FixedRangePos;

bool FarmBot_AttackMob = false;
bool FarmBot_AttackMetin = false;
DWORD FarmBot_Distance = false;

/* ------------------------------------------------------------------ */
/* signatures of the real x64 client methods (AOE pointers)             */
/* ------------------------------------------------------------------ */

typedef int  (__fastcall *fn_mainvid)(void* stream);
typedef int  (__fastcall *fn_type)(void* inst);
typedef void (__fastcall *fn_pos)(void* inst, float* out);
typedef void (__fastcall *fn_attack)(void* stream, unsigned char bType, unsigned long vid);

static fn_mainvid fmain   = nullptr;
static fn_type    ftype   = nullptr;
static fn_pos     fpos    = nullptr;
static fn_attack  fattack = nullptr;

static ADDR g_stream = 0;          /* CPythonNetworkStream, read at base+NET_RVA */
static DWORD g_mainvid = 0;
static ADDR g_maininst = 0;

/* Worker -> main thread queue. The worker writes a VID, the cave consumes
   it on the next frame. 0 = nothing to press. */
static volatile LONG kc_press_vid = 0;
static ADDR kc_cave = 0;
static unsigned char kc_orig[5] = { 0 };
static bool kc_hooked = false;

/* Table materialized from the manager tree: this replaces the x32 global
   MobPointer (0x1610958) that getMobList walked as 400 DWORDs.
   getMobList keeps its for (i = 0; i < 400; i++) loop and its sort. */
static ADDR  g_mobarray[MOB_SLOTS];
static DWORD g_mobvid[MOB_SLOTS];
static int   g_mobcount = 0;

/* ------------------------------------------------------------------ */
/* journal                                                              */
/* ------------------------------------------------------------------ */

/* Config, log and signal files live next to the DLL: kdir() returns the
   current module's folder (kc_self set by DllMain). No absolute path to
   edit before compiling anymore. */
static HINSTANCE kc_self = nullptr;

static const char* kdir(void) {
    static char buf[MAX_PATH];
    static bool done = false;
    if (!done) {
        done = true;
        HMODULE hm = (HMODULE)kc_self;
        DWORD n = GetModuleFileNameA(hm, buf, MAX_PATH);
        while (n > 0 && buf[n - 1] != '\\' && buf[n - 1] != '/') n--;
        buf[n] = 0;
    }
    return buf;
}

static void klog(const char* fmt, ...) {
    char path[MAX_PATH];
    snprintf(path, sizeof(path), "%skctest.log", kdir());
    FILE* f = fopen(path, "ab");
    if (!f) return;
    SYSTEMTIME st;
    GetLocalTime(&st);
    fprintf(f, "[%02d:%02d:%02d] ", st.wHour, st.wMinute, st.wSecond);
    va_list ap;
    va_start(ap, fmt);
    vfprintf(f, fmt, ap);
    va_end(ap);
    fprintf(f, "\r\n");
    fclose(f);
}

static bool kfile_exists(const char* path) {
    DWORD a = GetFileAttributesA(path);
    return a != INVALID_FILE_ATTRIBUTES && !(a & FILE_ATTRIBUTE_DIRECTORY);
}

/* The shim returns 0 for an unreadable address; without this guard a stray
   pointer would read "isnil = 0" and pass for a valid node. */
static bool kc_ok(ADDR p, SIZE_T len) {
    MEMORY_BASIC_INFORMATION mbi;
    if (!p || !len) return false;
    ADDR end = p + len;
    if (end < p) return false;
    while (p < end) {
        if (VirtualQuery((void*)p, &mbi, sizeof(mbi)) != sizeof(mbi)) return false;
        if (mbi.State != MEM_COMMIT) return false;
        if (mbi.Protect & (PAGE_NOACCESS | PAGE_GUARD)) return false;
        ADDR region = (ADDR)mbi.BaseAddress + mbi.RegionSize;
        if (region <= p) return false;
        p = region;
    }
    return true;
}

/* ------------------------------------------------------------------ */
/* manager tree walk — replaces the x32 flat array                      */
/* ------------------------------------------------------------------ */

static void refresh_mobarray() {
    g_mobcount = 0;
    g_maininst = 0;
    memset(g_mobarray, 0, sizeof(g_mobarray));
    memset(g_mobvid, 0, sizeof(g_mobvid));

    if (!ModuleBase) return;
    if (!kc_ok(ModuleBase + NET_RVA, 8) || !kc_ok(ModuleBase + MGR_RVA, 8)) return;
    g_stream = Driver.ReadVirtualMemory<ADDR>(pID, ModuleBase + NET_RVA, 8);
    ADDR mgr = Driver.ReadVirtualMemory<ADDR>(pID, ModuleBase + MGR_RVA, 8);
    if (!g_stream || !mgr || !kc_ok(g_stream, 8) || !kc_ok(mgr, 8)) return;

    int mv = fmain ? fmain((void*)g_stream) : 0;
    g_mainvid = (mv > 0) ? (DWORD)mv : 0;

    ADDR head = Driver.ReadVirtualMemory<ADDR>(pID, mgr + 0x38, 8);
    if (!head || !kc_ok(head, 8)) return;
    ADDR root = Driver.ReadVirtualMemory<ADDR>(pID, head + 8, 8);
    if (!root || !kc_ok(root, 8)) return;

    ADDR stack[256];
    int sp = 0;
    ADDR cur = root;
    int guard = 0;

    while (guard++ < 6000) {
        if (!cur || !kc_ok(cur, 0x30)) break;
        unsigned char isnil = Driver.ReadVirtualMemory<unsigned char>(pID, cur + 0x19, 1);
        if (isnil) {
            if (sp <= 0) break;
            cur = stack[--sp];
            continue;
        }
        ADDR inst = Driver.ReadVirtualMemory<ADDR>(pID, cur + 0x28, 8);
        DWORD vid = Driver.ReadVirtualMemory<DWORD>(pID, cur + 0x20, 4);
        if (inst && kc_ok(inst, 0x30)) {
            if (g_mainvid && vid == g_mainvid) {
                g_maininst = inst;
            } else if (g_mobcount < MOB_SLOTS) {
                g_mobarray[g_mobcount] = inst;
                g_mobvid[g_mobcount] = vid;
                g_mobcount++;
            }
        }
        ADDR left  = Driver.ReadVirtualMemory<ADDR>(pID, cur, 8);
        ADDR right = Driver.ReadVirtualMemory<ADDR>(pID, cur + 0x10, 8);
        if (right && kc_ok(right, 0x20) &&
            !Driver.ReadVirtualMemory<unsigned char>(pID, right + 0x19, 1) && sp < 250)
            stack[sp++] = right;
        if (left && kc_ok(left, 0x20) &&
            !Driver.ReadVirtualMemory<unsigned char>(pID, left + 0x19, 1))
            cur = left;
        else if (sp > 0)
            cur = stack[--sp];
        else
            break;
    }
}

/* ------------------------------------------------------------------ */
/* hackMain.cpp — original logic                                        */
/* ------------------------------------------------------------------ */

bool CompareInstanceObj(InstanceObj i1, InstanceObj i2) {
    return (i1.Distance < i2.Distance);
}

DWORD getDistance(D3DXVECTOR3 pos_1, D3DXVECTOR3 pos_2) {
    float diff_x = abs(pos_1.x - pos_2.x);
    float diff_y = abs(pos_1.y - pos_2.y);
    DWORD diff_pos = (DWORD)(sqrt((diff_x * diff_x) + (diff_y * diff_y)));
    return diff_pos;
}

DWORD dwGetModuleBaseAddress(DWORD dwProcessID, TCHAR* lpszModuleName)
{
    HANDLE hSnapshot = CreateToolhelp32Snapshot(TH32CS_SNAPMODULE, dwProcessID);
    DWORD dwModuleBaseAddress = 0;
    if (hSnapshot != INVALID_HANDLE_VALUE)
    {
        MODULEENTRY32 ModuleEntry32 = { 0 };
        ModuleEntry32.dwSize = sizeof(MODULEENTRY32);
        if (Module32First(hSnapshot, &ModuleEntry32))
        {
            do
            {
                if (_tcscmp(ModuleEntry32.szModule, lpszModuleName) == 0)
                {
                    dwModuleBaseAddress = (DWORD)(uintptr_t)ModuleEntry32.modBaseAddr;
                    break;
                }
            } while (Module32Next(hSnapshot, &ModuleEntry32));
        }
        CloseHandle(hSnapshot);
    }
    return dwModuleBaseAddress;
}

void init_hack(LPCSTR RegistryPath, DWORD pid) {
    pID = pid;
    Driver = KeInterface(RegistryPath);
    /* x32 : L"bin_aeldra_245.exe". x64 : the client we are injected into. */
    ModuleBase = (ADDR)GetModuleHandleA(nullptr);
    fmain   = (fn_mainvid)(ModuleBase + MAINVID_RVA);
    ftype   = (fn_type)   (ModuleBase + MGTYPE_RVA);
    fpos    = (fn_pos)    (ModuleBase + POS_RVA);
    fattack = (fn_attack) (ModuleBase + SENDATTACK_RVA);
}

ADDR getMainActorInstance() {
    /* x32 : [ModuleBase+CInstanceBase] then +Offset_player(0x18).
       x64 : the main actor is the node whose VID == netGetMainActorVID. */
    return g_maininst;
}

DWORD getVid(ADDR instance) {
    if (!instance) { return 0; }
    /* x32 : mixed table at instance+0x214. x64 : the VID sits on the tree
       node, not in the instance ; it is recovered from the table
       materialized on the same tick as the instance. */
    for (int i = 0; i < g_mobcount; i++)
        if (g_mobarray[i] == instance) return g_mobvid[i];
    if (instance == g_maininst) return g_mainvid;
    return 0;
}

std::string getName(ADDR instance) {
    if (!instance) { return ""; }
    std::array<char, 14> name = Driver.ReadVirtualMemory<std::array<char, 14>>(pID, instance + Offset_name, 14);
    return name.data();
}

DWORD getInstanceType(ADDR instance) {
    if (!instance) { return 0; }
    /* x32 : mixed table. x64 : real method of the instance. */
    return ftype ? (DWORD)ftype((void*)instance) : 0;
}

static ADDR kc_player() {
    if (!ModuleBase || !kc_ok(ModuleBase + PLAYER_PTR_RVA, 8)) return 0;
    ADDR p = Driver.ReadVirtualMemory<ADDR>(pID, ModuleBase + PLAYER_PTR_RVA, 8);
    return (p && kc_ok(p, 0x1B328)) ? p : 0;
}

DWORD getTargetVid() {
    if (!getMainActorInstance()) { return 0; }
    /* x32 : CPythonPlayer + mixed table + 0x4B004.
       x64 : direct DWORD at +0x1B324 (GetTargetVID, 0x23DF60). */
    ADDR p = kc_player();
    if (!p) return 0;
    return Driver.ReadVirtualMemory<DWORD>(pID, p + Offset_targetVid, sizeof(DWORD));
}

D3DXVECTOR3 getPixelPosition(ADDR instance) {
    if (!instance) { return { 0 }; }
    float p[3] = { 0.0f, 0.0f, 0.0f };
    if (fpos) fpos((void*)instance, p);
    return { abs(p[0]), abs(p[1]), 0 };
}

DWORD getAttackStatus() {
    if (!getMainActorInstance()) { return 0; }
    /* x32 : short at CPythonPlayer+0x54. x64 : BYTE at +0x80, and the instance
       comes from the global base+0xE07A48 - never from the 0x358C0 accessor,
       which returns another object and made this read garbage. */
    ADDR p = kc_player();
    if (!p) return 0;
    return Driver.ReadVirtualMemory<unsigned char>(pID, p + Offset_setAttackStatus, 1);
}

/* Minimal per-VID history for the 7D0 latch. Replaces aoe99's cs_vid/cs_fl
   reduced to the only bit we need: "this mob already struck back at us".
   Ring of 512, oldest-polled replacement. No attack logic,
   no sweep, no position windows: just the death verdict. */
#define KC_LATCH_N 512
static DWORD kc_l_vid[KC_LATCH_N];
static DWORD kc_l_seen[KC_LATCH_N];
static unsigned char kc_l_latch[KC_LATCH_N];

static int kc_l_slot(DWORD vid, DWORD now) {
    if (!vid) return -1;
    int oldest = 0;
    for (int i = 0; i < KC_LATCH_N; i++)
        if (kc_l_vid[i] == vid) { kc_l_seen[i] = now; return i; }
    for (int i = 0; i < KC_LATCH_N; i++)
        if (!kc_l_vid[i]) {
            kc_l_vid[i] = vid; kc_l_seen[i] = now; kc_l_latch[i] = 0; return i;
        }
    for (int i = 1; i < KC_LATCH_N; i++)
        if (kc_l_seen[i] < kc_l_seen[oldest]) oldest = i;
    kc_l_vid[oldest] = vid; kc_l_seen[oldest] = now; kc_l_latch[oldest] = 0;
    return oldest;
}

bool getIsDead(ADDR instance) {
    if (!instance) { return false; }
    int an = Driver.ReadVirtualMemory<int>(pID, instance + Offset_isDead, sizeof(int));
    if (an >= DEAD_488_THRESH) { return true; }          /* slow net: always */
    /* ZERO-GAP: needs a VID and our own VID. */
    DWORD vid = getVid(instance);
    DWORD mainvid = g_mainvid;
    if (!vid || !mainvid) { return false; }
    DWORD a = Driver.ReadVirtualMemory<DWORD>(pID, instance + Offset_atkA, sizeof(DWORD));
    DWORD b = Driver.ReadVirtualMemory<DWORD>(pID, instance + Offset_atkB, sizeof(DWORD));
    DWORD now = GetTickCount();
    int k = kc_l_slot(vid, now);
    if (k < 0) { return false; }
    /* latch from THE FIRST STRIKE (0x320) as well as the returned hit (0x7D0):
       a mob killed in one hit must latch, otherwise its corpse lingers. */
    if (a == mainvid || b == mainvid) { kc_l_latch[k] = 1; }
    if (kc_l_latch[k] && b == 0) { return true; }        /* fatal hit: out at once */
    return false;
}

/* x32 : x86 code cave (mov ecx / push / call) grafted onto the client's
   main loop via MainThread 0xEE1AB, then restored.
   x64 : same mechanics. The worker only publishes the VID; the cave
   (kc_build_cave) calls the client's real PressActor on the main thread,
   then hands control back to the original Update. The decision stays
   FarmBotThread's, the transport is the source's. */
void PressActor(DWORD vid) {
    if (!vid) { return; }
    if (!kc_hooked) { return; }
    if (kc_press_vid) { return; }              /* one strike already queued */
    /* NO getTargetVid()==vid guard: the source does not have one. tmrBot_Tick
       calls FarmBotThread every second and re-publishes the VID while
       getAttackStatus()==0 ; that re-strike is what restarts/sustains the
       attack on the locked target. The guard blocked every re-strike once
       the target was locked: live mob at dist 53, atkstat 0,
       tgtvid frozen -> permanent stall (kills 1-2 then stops). Source
       behaviour restored: re-publish every tick. */
    klog("press vid=%lu (queued to main thread)", vid);
    InterlockedExchange(&kc_press_vid, (LONG)vid);
}

bool setFixedRange(bool status) {
    if (status) {
        ADDR MainActorInstance = getMainActorInstance();
        if (!MainActorInstance) {
            FarmBot_FixedRange = false;
            FarmBot_FixedRangePos = { 0 };
            return false;
        }
        FarmBot_FixedRangePos = getPixelPosition(MainActorInstance);
        FarmBot_FixedRange = true;
    }
    else {
        FarmBot_FixedRange = false;
        FarmBot_FixedRangePos = { 0 };
    }
    return true;
}

std::vector<InstanceObj> getMobList(bool AttackMob, bool AttackMetin) {
    std::vector<InstanceObj> ObjList;
    ADDR MainActorInstance = getMainActorInstance();
    if (!MainActorInstance) { return ObjList; }
    D3DXVECTOR3 MainActorPos = getPixelPosition(MainActorInstance);
    ADDR MobPtr = (ADDR)g_mobarray;
    for (int i = 0; i < 400; i++) {
        ADDR MobInstance = Driver.ReadVirtualMemory<ADDR>(pID, MobPtr + (i * 8), sizeof(ADDR));
        if (!MobInstance)
            continue;

        if (MobInstance == MainActorInstance)
            continue;

        DWORD MobType = getInstanceType(MobInstance);

        if ((MobType == 0 && !AttackMob) || (MobType == 2 && !AttackMetin) || (MobType != 0 && MobType != 2))
            continue;

        bool IsDead = getIsDead(MobInstance);
        if (IsDead)
            continue;

        DWORD MobVid = getVid(MobInstance);
        if (!MobVid)
            continue;

        D3DXVECTOR3 MobPos = getPixelPosition(MobInstance);
        if (!abs(MobPos.x) || !MobPos.y)
            continue;

        DWORD MobDistance = getDistance(MainActorPos, MobPos);

        ObjList.push_back({ MobInstance, MobVid, MobPos, MobType, MobDistance });
    }
    if (ObjList.size() > 1) {
        std::sort(ObjList.begin(), ObjList.end(), CompareInstanceObj);
    }
    return ObjList;
}

InstanceObj getAttackableMob(DWORD Distance, bool AttackMob, bool AttackMetin) {
    ADDR MainActorInstance = getMainActorInstance();
    if (!MainActorInstance) { return { 0 }; }

    D3DXVECTOR3 MainActorPos = getPixelPosition(MainActorInstance);

    std::vector<InstanceObj> MobList = getMobList(AttackMob, AttackMetin);
    if (!MobList.size()) { return { 0 }; }

    for (InstanceObj Mob : MobList) {
        DWORD MobDistance = Mob.Distance;
        if (FarmBot_FixedRange)
            MobDistance = getDistance(FarmBot_FixedRangePos, Mob.Pos);

        if (MobDistance <= Distance)
            return Mob;
    }
    return { 0 };
}

void FarmBotThread(DWORD distance) {
    if (!getMainActorInstance()) { return; }
    if (getAttackStatus()) { return; }
    InstanceObj MobObj = getAttackableMob(distance, FarmBot_AttackMob, FarmBot_AttackMetin);
    if (MobObj.VID) {
        PressActor(MobObj.VID);
    }
}

/* ------------------------------------------------------------------ */
/* x64 cave — the source's graft, translated to fastcall                */
/*                                                                      */
/* Entry: via `call` from base+HOOK_RVA, rcx = player (mov rcx,r15),     */
/* [rsp] = return address base+HOOK_RVA+5. Exit: tail-jmp to the         */
/* original Update with rcx restored, so the frame is identical.         */
/*                                                                      */
/* Alignment: entry rsp%16=8 ; 8 pushes (64) -> 8 ; sub rsp,8 -> 0,      */
/* both calls are aligned. Symmetric restoration.                        */
/*                                                                      */
/* Sequence (verified in the disassembler before injection):             */
/*   push rax,rcx,rdx,r8,r9,r10,r11,rbx ; sub rsp,8                      */
/*   ebx = [pending] ; if 0 -> done ; [pending] = 0                      */
/*   rcx = [playerglobal] ; if 0 -> done                                 */
/*   rdx = rcx+8 ; rax = [rcx+8] (secondary interface vtable) ; if 0 -> done */
/*   rcx = rdx ; call [rax+0x110]  (GetMainActorInstance, this=player+8) */
/*   if 0 -> done ; rdx = rax (mainActor)                                */
/*   rcx = [playerglobal] ; r8d = ebx ; r9d = 1 ; call PressActor        */
/* done: add rsp,8 ; pops ; rax = original Update ; jmp rax              */
/* ------------------------------------------------------------------ */

static size_t kc_build_cave(unsigned char* out, volatile LONG* pending,
                            ADDR playerglobal, ADDR pressactor, ADDR origupdate)
{
    size_t n = 0;
    size_t jz[4];
    int njz = 0;

#define EM(...) do { \
        const unsigned char b_[] = { __VA_ARGS__ }; \
        memcpy(out + n, b_, sizeof(b_)); n += sizeof(b_); \
    } while (0)
#define IMM64(pfx, op, val) do { \
        EM(pfx, op); \
        ADDR v_ = (ADDR)(val); \
        memcpy(out + n, &v_, 8); n += 8; \
    } while (0)
#define JZ() do { jz[njz++] = n; EM(0x0F, 0x84, 0x00, 0x00, 0x00, 0x00); } while (0)

    EM(0x50,0x51,0x52,0x41,0x50,0x41,0x51,0x41,0x52,0x41,0x53,0x53);
    EM(0x48,0x83,0xEC,0x08);
    IMM64(0x48,0xB8, pending);
    EM(0x8B,0x18);
    EM(0x85,0xDB);
    JZ();
    EM(0xC7,0x00,0x00,0x00,0x00,0x00);
    IMM64(0x48,0xB9, playerglobal);
    EM(0x48,0x8B,0x09);
    EM(0x48,0x85,0xC9);
    JZ();
    EM(0x48,0x89,0xCA);
    EM(0x48,0x83,0xC2,0x08);
    EM(0x48,0x8B,0x41,0x08);
    EM(0x48,0x85,0xC0);
    JZ();
    EM(0x48,0x89,0xD1);
    EM(0xFF,0x90,0x10,0x01,0x00,0x00);
    EM(0x48,0x85,0xC0);
    JZ();
    EM(0x48,0x89,0xC2);
    IMM64(0x48,0xB9, playerglobal);
    EM(0x48,0x8B,0x09);
    EM(0x41,0x89,0xD8);
    EM(0x41,0xB9,0x01,0x00,0x00,0x00);
    IMM64(0x48,0xB8, pressactor);
    EM(0xFF,0xD0);

    size_t done = n;
    EM(0x48,0x83,0xC4,0x08);
    EM(0x5B,0x41,0x5B,0x41,0x5A,0x41,0x59,0x41,0x58,0x5A,0x59,0x58);
    IMM64(0x48,0xB8, origupdate);
    EM(0xFF,0xE0);

    for (int i = 0; i < njz; ++i) {
        int rel = (int)(done - (jz[i] + 6));
        memcpy(out + jz[i] + 2, &rel, 4);
    }

#undef EM
#undef IMM64
#undef JZ
    return n;   /* 167 bytes; the caller's buffer is 512 */
}

static bool kc_install_hook() {
    if (kc_hooked) return true;
    if (!ModuleBase) return false;

    ADDR site = ModuleBase + HOOK_RVA;
    if (!kc_ok(site, 5)) { klog("hook: site unreadable"); return false; }
    unsigned char cur[5];
    memcpy(cur, (const void*)site, 5);
    if (cur[0] != 0xE8) {
        klog("hook: byte %02X instead of E8 - aborting", cur[0]);
        return false;
    }
    int rel;
    memcpy(&rel, cur + 1, 4);
    ADDR target = site + 5 + (ADDR)(long long)rel;
    if (target != ModuleBase + ORIGUPDATE_RVA) {
        klog("hook: target %llu instead of %llu - aborting",
             (unsigned long long)target, (unsigned long long)(ModuleBase + ORIGUPDATE_RVA));
        return false;
    }
    memcpy(kc_orig, cur, 5);

    unsigned char buf[512];
    size_t len = kc_build_cave(buf, &kc_press_vid, ModuleBase + PLAYER_PTR_RVA,
                               ModuleBase + PRESSACTOR_RVA, ModuleBase + ORIGUPDATE_RVA);

    /* E8 rel32: the cave must fit within +/-2 GB of the site. Search just
       around the image (occupied hints fail, move on). */
    ADDR cave = 0;
    for (int i = 1; i <= 4096 && !cave; ++i) {
        cave = (ADDR)VirtualAlloc((void*)(ModuleBase + (ADDR)i * 0x10000), 512,
                                  MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE);
        if (!cave)
            cave = (ADDR)VirtualAlloc((void*)(ModuleBase - (ADDR)i * 0x10000), 512,
                                      MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE);
    }
    if (!cave) { klog("hook: VirtualAlloc failed %lu", GetLastError()); return false; }

    long long delta = (long long)cave - (long long)(site + 5);
    if (delta > 0x7FFFFFFFll || delta < -0x80000000ll) {
        klog("hook: cave too far away (%lld) - aborting", delta);
        VirtualFree((void*)cave, 0, MEM_RELEASE);
        return false;
    }

    memcpy((void*)cave, buf, len);
    FlushInstructionCache(GetCurrentProcess(), (void*)cave, len);

    DWORD old;
    if (!VirtualProtect((void*)site, 5, PAGE_EXECUTE_READWRITE, &old)) {
        klog("hook: VirtualProtect failed %lu", GetLastError());
        VirtualFree((void*)cave, 0, MEM_RELEASE);
        return false;
    }
    /* The E8 opcode stays: a single 4-byte store (same cache line)
       switches the target, never a torn instruction. */
    int r32 = (int)delta;
    memcpy((void*)(site + 1), &r32, 4);
    VirtualProtect((void*)site, 5, old, &old);
    FlushInstructionCache(GetCurrentProcess(), (void*)site, 5);

    kc_cave = cave;
    kc_hooked = true;
    klog("hook installed: site=%llu cave=%llu len=%u orig=%02X %02X %02X %02X %02X",
         (unsigned long long)site, (unsigned long long)cave, (unsigned)len,
         kc_orig[0], kc_orig[1], kc_orig[2], kc_orig[3], kc_orig[4]);
    return true;
}

static void kc_uninstall_hook() {
    if (!kc_hooked) return;
    ADDR site = ModuleBase + HOOK_RVA;
    DWORD old;
    if (VirtualProtect((void*)site, 5, PAGE_EXECUTE_READWRITE, &old)) {
        memcpy((void*)(site + 1), kc_orig + 1, 4);
        VirtualProtect((void*)site, 5, old, &old);
        FlushInstructionCache(GetCurrentProcess(), (void*)site, 5);
    }
    kc_hooked = false;
    InterlockedExchange(&kc_press_vid, 0);
    Sleep(150);   /* let a possible in-flight PressActor finish */
    if (kc_cave) { VirtualFree((void*)kc_cave, 0, MEM_RELEASE); kc_cave = 0; }
    klog("hook removed, site restored");
}

/* ------------------------------------------------------------------ */
/* frmMain — the designer's eight controls, rebuilt in native Win32     */
/*                                                                      */
/* The worker stays the only caller of the client's methods. The UI     */
/* publishes requests then mirrors state: no SendMessage from the       */
/* worker, so a modal MessageBox cannot freeze the bot.                 */
/* ------------------------------------------------------------------ */

#define IDC_TXTINITHACK 100
#define IDC_BTNINITHACK 101
#define IDC_LBLDIST     102
#define IDC_TXTDIST     103
#define IDC_CHCKMOB     104
#define IDC_CHCKMETIN   105
#define IDC_CHCKFIXED   106
#define IDC_CHCKBOT     107

static bool  kc_on = false;
static DWORD kc_dist = 50;          /* frmMain: txtFarmDistance default "50" */
static bool  kc_prev_mob = false, kc_prev_metin = false, kc_prev_fixed = false;
static bool  kc_prev_on = false;

/* -1 = no request. setFixedRange calls getPixelPosition, a client method:
   the request is therefore applied by the worker, not the UI. */
static volatile LONG kc_fixed_req   = -1;
static volatile LONG kc_dist_dirty  = 0;
static volatile LONG kc_close       = 0;
static volatile LONG kc_worker_done = 0;

static char kc_mirror[256] = "";

static HWND kc_wnd = nullptr;
static HWND kc_txtInit = nullptr, kc_txtDist = nullptr;
static HWND kc_chkMob = nullptr, kc_chkMetin = nullptr;
static HWND kc_chkFixed = nullptr, kc_chkBot = nullptr;
static HFONT kc_font = nullptr;

/* Convert::ToUInt32 on text already filtered by KeyPress (digits only). */
static bool kc_parse_u32(const char* s, DWORD* out) {
    if (!s || !*s) return false;
    unsigned long long v = 0;
    for (const char* p = s; *p; ++p) {
        if (*p < '0' || *p > '9') return false;
        v = v * 10 + (unsigned long long)(*p - '0');
        if (v > 0xFFFFFFFFull) return false;
    }
    *out = (DWORD)v;
    return true;
}

static void kc_cfg() {
    char cfgpath[MAX_PATH];
    snprintf(cfgpath, sizeof(cfgpath), "%skctest.txt", kdir());
    char buf[256] = "";
    FILE* f = fopen(cfgpath, "rb");
    if (f) {
        size_t n = fread(buf, 1, sizeof(buf) - 1, f);
        buf[n] = 0;
        fclose(f);
    }

    /* The file and the panel write the same state. A read is only applied
       if the content differs from the mirror, otherwise our own rewrite
       would be reapplied in a loop. */
    if (strcmp(buf, kc_mirror) != 0) {
        char work[256];
        strcpy(work, buf);
        for (char* line = strtok(work, "\r\n"); line; line = strtok(nullptr, "\r\n")) {
            char key[64]; long long v;
            if (sscanf(line, " %63[^= ] =%lld", key, &v) != 2) continue;
            if      (!strcmp(key, "on"))    kc_on = (v != 0);
            else if (!strcmp(key, "mob"))   FarmBot_AttackMob = (v != 0);
            else if (!strcmp(key, "metin")) FarmBot_AttackMetin = (v != 0);
            else if (!strcmp(key, "fixed")) InterlockedExchange(&kc_fixed_req, v ? 1 : 0);
            else if (!strcmp(key, "dist")) {
                if (v > 0 && v <= 100000 && (DWORD)v != kc_dist) {
                    kc_dist = (DWORD)v;
                    InterlockedExchange(&kc_dist_dirty, 1);
                }
            }
        }
        strcpy(kc_mirror, buf);
    }

    LONG req = InterlockedExchange(&kc_fixed_req, -1);
    if (req >= 0 && (req != 0) != FarmBot_FixedRange) {
        /* chckFixedRange_CheckedChanged: if setFixedRange fails, uncheck. */
        if (!setFixedRange(req != 0)) FarmBot_FixedRange = false;
    }

    char canon[256];
    snprintf(canon, sizeof(canon), "on=%d\r\nmob=%d\r\nmetin=%d\r\nfixed=%d\r\ndist=%lu\r\n",
             kc_on ? 1 : 0, FarmBot_AttackMob ? 1 : 0, FarmBot_AttackMetin ? 1 : 0,
             FarmBot_FixedRange ? 1 : 0, kc_dist);
    if (strcmp(canon, kc_mirror) != 0) {
        FILE* o = fopen(cfgpath, "wb");
        if (o) {
            fputs(canon, o);
            fclose(o);
            strcpy(kc_mirror, canon);
        }
    }

    if (kc_on != kc_prev_on) {
        klog("bot %s", kc_on ? "ON" : "OFF");
        kc_prev_on = kc_on;
    }
    if (FarmBot_AttackMob != kc_prev_mob || FarmBot_AttackMetin != kc_prev_metin ||
        FarmBot_FixedRange != kc_prev_fixed) {
        klog("cfg mob=%d metin=%d fixed=%d dist=%lu",
             FarmBot_AttackMob, FarmBot_AttackMetin, FarmBot_FixedRange, kc_dist);
        kc_prev_mob = FarmBot_AttackMob;
        kc_prev_metin = FarmBot_AttackMetin;
        kc_prev_fixed = FarmBot_FixedRange;
    }
}

static HWND kc_ctl(const char* cls, const char* text, DWORD style,
                   int x, int y, int w, int h, int id) {
    HWND c = CreateWindowExA(0, cls, text, WS_CHILD | WS_VISIBLE | style,
                             x, y, w, h, kc_wnd, (HMENU)(INT_PTR)id, kc_self, nullptr);
    if (c && kc_font) SendMessageA(c, WM_SETFONT, (WPARAM)kc_font, TRUE);
    return c;
}

static void kc_ui_tick() {
    if (!kc_wnd) return;

    char quitpath[MAX_PATH];
    snprintf(quitpath, sizeof(quitpath), "%skcquit.txt", kdir());
    if (kc_worker_done || kfile_exists(quitpath)) {
        DestroyWindow(kc_wnd);
        return;
    }

    SendMessageA(kc_chkMob,   BM_SETCHECK, FarmBot_AttackMob   ? BST_CHECKED : BST_UNCHECKED, 0);
    SendMessageA(kc_chkMetin, BM_SETCHECK, FarmBot_AttackMetin ? BST_CHECKED : BST_UNCHECKED, 0);
    SendMessageA(kc_chkFixed, BM_SETCHECK, FarmBot_FixedRange  ? BST_CHECKED : BST_UNCHECKED, 0);
    SendMessageA(kc_chkBot,   BM_SETCHECK, kc_on               ? BST_CHECKED : BST_UNCHECKED, 0);

    if (InterlockedExchange(&kc_dist_dirty, 0) != 0) {
        char b[32];
        snprintf(b, sizeof(b), "%lu", kc_dist);
        SetWindowTextA(kc_txtDist, b);
    }

    /* tmrBot_Tick: the distance is only read if chckBot is checked, and a
       non-convertible text is reset to "50". */
    if (SendMessageA(kc_chkBot, BM_GETCHECK, 0, 0) == BST_CHECKED) {
        char buf[64];
        DWORD d = 50;
        GetWindowTextA(kc_txtDist, buf, sizeof(buf));
        if (!kc_parse_u32(buf, &d)) {
            d = 50;
            SetWindowTextA(kc_txtDist, "50");
        }
        if (d > 100000) d = 100000;   /* kc_dist * 100 must fit a DWORD */
        kc_dist = d;
    }
}

static LRESULT CALLBACK kc_proc(HWND h, UINT m, WPARAM wp, LPARAM lp) {
    switch (m) {
    case WM_COMMAND:
        if (HIWORD(wp) != BN_CLICKED) return 0;
        switch (LOWORD(wp)) {
        case IDC_BTNINITHACK: {
            char buf[64];
            DWORD pid = 0;
            GetWindowTextA(kc_txtInit, buf, sizeof(buf));
            if (!kc_parse_u32(buf, &pid)) {
                MessageBoxA(h, "Invalid PID", "Init Hack", MB_OK | MB_ICONERROR);
                return 0;
            }
            init_hack("\\\\.\\mrbcnm", pid);
            klog("Init Hack pid=%lu base=%llu", pid, (unsigned long long)ModuleBase);
            return 0;
        }
        case IDC_CHCKMOB:
            FarmBot_AttackMob = (SendMessageA((HWND)lp, BM_GETCHECK, 0, 0) == BST_CHECKED);
            klog("UI Attack Mob -> %d", FarmBot_AttackMob);
            return 0;
        case IDC_CHCKMETIN:
            FarmBot_AttackMetin = (SendMessageA((HWND)lp, BM_GETCHECK, 0, 0) == BST_CHECKED);
            klog("UI Attack Metin -> %d", FarmBot_AttackMetin);
            return 0;
        case IDC_CHCKFIXED:
            InterlockedExchange(&kc_fixed_req,
                SendMessageA((HWND)lp, BM_GETCHECK, 0, 0) == BST_CHECKED ? 1 : 0);
            return 0;
        case IDC_CHCKBOT:
            kc_on = (SendMessageA((HWND)lp, BM_GETCHECK, 0, 0) == BST_CHECKED);
            klog("UI Enable Bot -> %d", kc_on);
            return 0;
        }
        return 0;
    case WM_CTLCOLORSTATIC: {
        HDC dc = (HDC)wp;
        SetBkMode(dc, TRANSPARENT);
        SetTextColor(dc, GetSysColor(COLOR_BTNTEXT));
        return (LRESULT)GetSysColorBrush(COLOR_BTNFACE);
    }
    case WM_TIMER:
        if (wp == 1) { kc_ui_tick(); return 0; }
        break;
    case WM_CLOSE:
        /* frmMain: closing the window ends Application.Run, hence the bot. */
        InterlockedExchange(&kc_close, 1);
        DestroyWindow(h);
        return 0;
    case WM_DESTROY:
        kc_wnd = nullptr;
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProcA(h, m, wp, lp);
}

static DWORD WINAPI kc_ui_thread(LPVOID) {
    /* Unique name per injection: a local class outlives the module that
       registered it, a fixed name would fail with 1410 on swap and
       reuse a wndproc pointing into unloaded code. */
    char clsname[64];
    snprintf(clsname, sizeof(clsname), "KCtest_frmMain_%lu", GetTickCount());

    WNDCLASSA wc;
    ZeroMemory(&wc, sizeof(wc));
    wc.lpfnWndProc   = kc_proc;
    wc.hInstance     = kc_self;
    wc.hCursor       = LoadCursorA(nullptr, (LPCSTR)IDC_ARROW);
    wc.hbrBackground = (HBRUSH)(COLOR_BTNFACE + 1);
    wc.lpszClassName = clsname;
    if (!RegisterClassA(&wc)) {
        klog("panel: RegisterClass failed %lu", GetLastError());
        return 0;
    }

    /* WinForms: Microsoft Sans Serif 8.25 pt = 11 px at 96 dpi, giving
       the AutoScaleDimensions(6,13) and the 13/17/20 heights of the designer. */
    kc_font = CreateFontA(-11, 0, 0, 0, FW_NORMAL, 0, 0, 0, DEFAULT_CHARSET,
                          OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY,
                          DEFAULT_PITCH | FF_DONTCARE, "Microsoft Sans Serif");

    /* FormBorderStyle.FixedSingle: no WS_THICKFRAME, no maximize.
       ClientSize(208,143) converted to the outer size. */
    DWORD style = WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX;
    DWORD ex    = WS_EX_TOPMOST | WS_EX_APPWINDOW | WS_EX_CONTROLPARENT;
    RECT rc = { 0, 0, 208, 143 };
    AdjustWindowRectEx(&rc, style, FALSE, ex);

    kc_wnd = CreateWindowExA(ex, clsname, "frmMain", style,
                             60, 60, rc.right - rc.left, rc.bottom - rc.top,
                             nullptr, nullptr, kc_self, nullptr);
    if (!kc_wnd) {
        klog("panel: CreateWindow failed %lu", GetLastError());
        UnregisterClassA(clsname, kc_self);
        return 0;
    }
    HWND hwnd = kc_wnd;

    /* creation order = tab order = the designer's TabIndex 0..7 */
    char pidbuf[32];
    snprintf(pidbuf, sizeof(pidbuf), "%lu", pID);
    kc_txtInit = kc_ctl("EDIT", pidbuf, WS_TABSTOP | ES_AUTOHSCROLL | ES_NUMBER,
                        12, 12, 100, 20, IDC_TXTINITHACK);
    kc_ctl("BUTTON", "Init Hack", WS_TABSTOP | BS_PUSHBUTTON,
           118, 10, 75, 23, IDC_BTNINITHACK);
    kc_ctl("STATIC", "Farm Distance:", SS_LEFT,
           12, 51, 78, 13, IDC_LBLDIST);
    kc_txtDist = kc_ctl("EDIT", "50", WS_TABSTOP | ES_AUTOHSCROLL | ES_NUMBER | ES_CENTER,
                        105, 48, 88, 20, IDC_TXTDIST);
    kc_chkMob   = kc_ctl("BUTTON", "Attack Mob",   WS_TABSTOP | BS_AUTOCHECKBOX,
                         15,  83, 81, 17, IDC_CHCKMOB);
    kc_chkMetin = kc_ctl("BUTTON", "Attack Metin", WS_TABSTOP | BS_AUTOCHECKBOX,
                         105, 83, 86, 17, IDC_CHCKMETIN);
    kc_chkFixed = kc_ctl("BUTTON", "Fixed Range",  WS_TABSTOP | BS_AUTOCHECKBOX,
                         15, 106, 86, 17, IDC_CHCKFIXED);
    kc_chkBot   = kc_ctl("BUTTON", "Enable Bot",   WS_TABSTOP | BS_AUTOCHECKBOX,
                         105, 106, 78, 17, IDC_CHCKBOT);

    char distbuf[32];
    snprintf(distbuf, sizeof(distbuf), "%lu", kc_dist);
    SetWindowTextA(kc_txtDist, distbuf);

    /* SW_SHOWNOACTIVATE: the panel appears without stealing the game's keyboard. */
    ShowWindow(hwnd, SW_SHOWNOACTIVATE);
    SetWindowPos(hwnd, HWND_TOPMOST, 0, 0, 0, 0,
                 SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
    SetTimer(hwnd, 1, 1000, nullptr);   /* tmrBot.Interval = 1000 */
    kc_ui_tick();

    RECT wr;
    GetWindowRect(hwnd, &wr);
    klog("panel hwnd=%llu rect=(%ld,%ld)-(%ld,%ld) client=208x143 controls=%d%d%d%d%d%d",
         (unsigned long long)(uintptr_t)hwnd, wr.left, wr.top, wr.right, wr.bottom,
         kc_txtInit != nullptr, kc_txtDist != nullptr, kc_chkMob != nullptr,
         kc_chkMetin != nullptr, kc_chkFixed != nullptr, kc_chkBot != nullptr);

    MSG msg;
    while (GetMessageA(&msg, nullptr, 0, 0) > 0) {
        if (!IsDialogMessageA(hwnd, &msg)) {
            TranslateMessage(&msg);
            DispatchMessageA(&msg);
        }
    }

    KillTimer(hwnd, 1);
    if (kc_font) { DeleteObject(kc_font); kc_font = nullptr; }
    UnregisterClassA(clsname, kc_self);
    InterlockedExchange(&kc_close, 1);
    klog("panel: message loop ended");
    return 0;
}

/* tmrBot_Tick: Interval = 1000, FarmBotThread(FarmDistance * 100) */
static HANDLE kc_ui_thr = nullptr;

static DWORD WINAPI kc_worker(LPVOID) {
    DWORD last_diag = 0;
    bool hook_tried = false;
    while (!kc_close) {
        char quitpath[MAX_PATH], alivepath[MAX_PATH];
        snprintf(quitpath, sizeof(quitpath), "%skcquit.txt", kdir());
        snprintf(alivepath, sizeof(alivepath), "%skc_alive.txt", kdir());
        if (kfile_exists(quitpath)) { break; }
        FILE* a = fopen(alivepath, "wb");
        if (a) {
            fprintf(a, "%lu %llX\r\n", pID, (unsigned long long)ModuleBase);
            fclose(a);
        }
        if (!hook_tried) {
            hook_tried = true;
            kc_install_hook();   /* outside DllMain: no loader lock */
        }
        kc_cfg();
        refresh_mobarray();

        DWORD now = GetTickCount();
        if (now - last_diag > 5000) {
            last_diag = now;
            ADDR mi = getMainActorInstance();
            std::vector<InstanceObj> l = getMobList(FarmBot_AttackMob, FarmBot_AttackMetin);
            InstanceObj pick = getAttackableMob(kc_dist * 100, FarmBot_AttackMob, FarmBot_AttackMetin);
            klog("state on=%d hook=%d mainvid=%lu inst=%llu tree=%d list=%u atkstat=%lu "
                 "tgtvid=%lu pick vid=%lu type=%lu dist=%lu fixed=%d range=%lu",
                 kc_on, kc_hooked, g_mainvid, (unsigned long long)mi, g_mobcount,
                 (unsigned)l.size(), mi ? getAttackStatus() : 0, getTargetVid(),
                 pick.VID, pick.Type, pick.Distance, FarmBot_FixedRange, kc_dist * 100);
        }

        if (kc_on)
            FarmBotThread(kc_dist * 100);
        Sleep(1000);
    }
    kc_uninstall_hook();
    InterlockedExchange(&kc_worker_done, 1);
    /* The swap FreeLibrarys as soon as kc_alive disappears: the UI must have
       destroyed the window and joined its thread before it is erased. */
    if (kc_ui_thr) {
        WaitForSingleObject(kc_ui_thr, 3000);
        CloseHandle(kc_ui_thr);
        kc_ui_thr = nullptr;
    }
    char alivepath[MAX_PATH];
    snprintf(alivepath, sizeof(alivepath), "%skc_alive.txt", kdir());
    DeleteFileA(alivepath);
    klog("=== KCtest10 exited ===");
    return 0;
}

BOOL WINAPI DllMain(HINSTANCE h, DWORD reason, LPVOID) {
    if (reason == DLL_PROCESS_ATTACH) {
        DisableThreadLibraryCalls(h);
        kc_self = h;
        init_hack("\\\\.\\mrbcnm", GetCurrentProcessId());
        klog("=== KCtest10 loaded === base=%llu mgr=%d stream=%d",
             (unsigned long long)ModuleBase, MGR_RVA, NET_RVA);
        CreateThread(nullptr, 0, kc_worker, nullptr, 0, nullptr);
        kc_ui_thr = CreateThread(nullptr, 0, kc_ui_thread, nullptr, 0, nullptr);
    }
    return TRUE;
}