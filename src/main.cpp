// CantedSights - NVSE plugin (no ESP). Toggle canted aim with a key; while aiming, the
// first-person weapon node is rolled/offset so an offset sight lines up with the eye.
#include <windows.h>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <cmath>
#include <cstdarg>
#include <cstdlib>
typedef unsigned char UInt8; typedef unsigned short UInt16; typedef unsigned int UInt32;
typedef signed char SInt8; typedef signed short SInt16; typedef signed int SInt32;
typedef unsigned long long UInt64; typedef signed long long SInt64;
#include "nvse/PluginAPI.h"

static const UInt32 kPlayerPtrAddr = 0x011DEA3C;   // PlayerCharacter** (FNV 1.4.0.525)

static int   g_toggleKey = 0x56, g_aimKey = 0x02;
static float g_blendTime = 0.15f, g_rollDeg = 45.f, g_off[3] = {0, 0, 0};
static UInt32 g_node1stOff = 0x694;
static char  g_nodeName[64] = "Weapon";
static bool  g_log = true, g_canted = false;
static char  g_dir[MAX_PATH];

static void Log(const char* fmt, ...) {
    if (!g_log) return;
    char path[MAX_PATH]; snprintf(path, sizeof path, "%sCantedSights.log", g_dir);
    FILE* f = fopen(path, "a"); if (!f) return;
    va_list a; va_start(a, fmt); vfprintf(f, fmt, a); va_end(a); fputc('\n', f); fclose(f);
}

// ---- minimal Gamebryo layout (FNV) ----
// NiObjectNET: name at +0x08. NiAVObject: localRotate +0x34 (3x3 floats), localTranslate +0x58.
// NiNode: children array data ptr +0xA0, count (u16) +0xA8.
static const char* NodeName(void* n) { return *(const char**)((char*)n + 0x08); }
static float* LRot(void* n) { return (float*)((char*)n + 0x34); }
static float* LPos(void* n) { return (float*)((char*)n + 0x58); }

static bool Readable(const void* p, size_t n) {
    MEMORY_BASIC_INFORMATION mbi;
    if (!p || !VirtualQuery(p, &mbi, sizeof mbi)) return false;
    return mbi.State == MEM_COMMIT && !(mbi.Protect & (PAGE_NOACCESS | PAGE_GUARD));
}

static void* FindNode(void* node, const char* name, int depth = 0) {
    if (!node || depth > 24 || !Readable(node, 0xB0)) return nullptr;
    const char* nm = NodeName(node);
    if (nm && Readable(nm, 1) && !_stricmp(nm, name)) return node;
    void** kids = *(void***)((char*)node + 0xA0);
    UInt16 cnt = *(UInt16*)((char*)node + 0xA8);
    if (!kids || cnt == 0 || cnt > 512 || !Readable(kids, cnt * 4)) return nullptr;
    for (UInt16 i = 0; i < cnt; ++i) {
        void* k = kids[i]; if (!k || !Readable(k, 0xB0)) continue;
        // only NiNodes have children; a non-node reads garbage here, guarded by Readable/cnt limits
        if (void* r = FindNode(k, name, depth + 1)) return r;
    }
    return nullptr;
}

static void MatMul(const float* a, const float* b, float* o) {
    for (int r = 0; r < 3; ++r) for (int c = 0; c < 3; ++c)
        o[r * 3 + c] = a[r * 3] * b[c] + a[r * 3 + 1] * b[3 + c] + a[r * 3 + 2] * b[6 + c];
}

// per-node state so animation rewrites don't get compounded
static void* s_node = nullptr;
static float s_baseRot[9], s_basePos[3], s_lastRot[9], s_lastPos[3];
static bool  s_have = false;
static float s_blend = 0.f;
static DWORD s_lastTick = 0;
static bool  s_prevKey = false, s_loggedNode = false;

static void Tick() {
    HWND fg = GetForegroundWindow(); DWORD pid = 0;
    if (fg) GetWindowThreadProcessId(fg, &pid);
    bool focused = (pid == GetCurrentProcessId());

    bool key = focused && (GetAsyncKeyState(g_toggleKey) & 0x8000);
    if (key && !s_prevKey) { g_canted = !g_canted; Log("canted = %d", g_canted); }
    s_prevKey = key;

    DWORD now = GetTickCount(); float dt = s_lastTick ? (now - s_lastTick) / 1000.f : 0.f; s_lastTick = now;
    if (dt > 0.1f) dt = 0.1f;

    void* player = *(void**)kPlayerPtrAddr;
    void* root = player && Readable((char*)player + g_node1stOff, 4) ? *(void**)((char*)player + g_node1stOff) : nullptr;
    void* node = root ? FindNode(root, g_nodeName) : nullptr;

    if (!s_loggedNode) {
        Log("player=%p root=%p rootName=%s weaponNode=%p", player, root,
            (root && Readable(root, 12) && NodeName(root) && Readable(NodeName(root), 1)) ? NodeName(root) : "?", node);
        if (node) s_loggedNode = true;
    }

    bool want = g_canted && focused && node && (GetAsyncKeyState(g_aimKey) & 0x8000);
    float step = g_blendTime > 0.001f ? dt / g_blendTime : 1.f;
    s_blend += want ? step : -step;
    if (s_blend < 0.f) s_blend = 0.f; if (s_blend > 1.f) s_blend = 1.f;

    if (node != s_node) { s_node = node; s_have = false; }
    if (!node) return;

    float* rot = LRot(node); float* pos = LPos(node);
    if (s_have && memcmp(rot, s_lastRot, sizeof s_lastRot) == 0 && memcmp(pos, s_lastPos, sizeof s_lastPos) == 0) {
        // untouched since our last write: restore base before reapplying
        memcpy(rot, s_baseRot, sizeof s_baseRot); memcpy(pos, s_basePos, sizeof s_basePos);
    }
    if (s_blend <= 0.f) { s_have = false; return; }

    memcpy(s_baseRot, rot, sizeof s_baseRot); memcpy(s_basePos, pos, sizeof s_basePos);
    float ang = g_rollDeg * 3.14159265f / 180.f * s_blend, c = cosf(ang), s = sinf(ang);
    const float R[9] = { c, 0, s,  0, 1, 0,  -s, 0, c };   // roll about local Y (barrel axis)
    float out[9]; MatMul(R, s_baseRot, out);
    memcpy(rot, out, sizeof out);
    for (int i = 0; i < 3; ++i) pos[i] = s_basePos[i] + g_off[i] * s_blend;
    memcpy(s_lastRot, rot, sizeof s_lastRot); memcpy(s_lastPos, pos, sizeof s_lastPos);
    s_have = true;
}

static void OnMessage(NVSEMessagingInterface::Message* msg) {
    if (msg->type == NVSEMessagingInterface::kMessage_MainGameLoop) {
        __try { Tick(); } __except (EXCEPTION_EXECUTE_HANDLER) { s_have = false; s_node = nullptr; }
    }
}

static void LoadIni() {
    char path[MAX_PATH]; snprintf(path, sizeof path, "%sCantedSights.ini", g_dir);
    char buf[64];
    auto hex = [&](const char* sec, const char* key, int def) {
        GetPrivateProfileStringA(sec, key, "", buf, sizeof buf, path);
        return buf[0] ? (int)strtol(buf, nullptr, 0) : def; };
    auto flt = [&](const char* sec, const char* key, float def) {
        GetPrivateProfileStringA(sec, key, "", buf, sizeof buf, path);
        return buf[0] ? (float)atof(buf) : def; };
    g_toggleKey = hex("General", "ToggleKey", 0x56); g_aimKey = hex("General", "AimKey", 0x02);
    g_blendTime = flt("General", "BlendTime", 0.15f); g_canted = hex("General", "StartCanted", 0) != 0;
    g_rollDeg = flt("Pose", "RollDegrees", 45.f);
    g_off[0] = flt("Pose", "OffsetX", 0); g_off[1] = flt("Pose", "OffsetY", 0); g_off[2] = flt("Pose", "OffsetZ", 0);
    g_node1stOff = (UInt32)hex("Engine", "Node1stPersonOffset", 0x694);
    GetPrivateProfileStringA("Engine", "WeaponNodeName", "Weapon", g_nodeName, sizeof g_nodeName, path);
    g_log = hex("Engine", "Log", 1) != 0;
}

extern "C" {
__declspec(dllexport) bool NVSEPlugin_Query(const NVSEInterface* nvse, PluginInfo* info) {
    info->infoVersion = PluginInfo::kInfoVersion; info->name = "CantedSights"; info->version = 1;
    return !nvse->isEditor;
}
__declspec(dllexport) bool NVSEPlugin_Load(NVSEInterface* nvse) {
    HMODULE self = nullptr;
    GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                       (LPCSTR)&NVSEPlugin_Load, &self);
    GetModuleFileNameA(self, g_dir, MAX_PATH);
    char* slash = strrchr(g_dir, '\\'); if (slash) slash[1] = 0;
    LoadIni(); Log("CantedSights loaded");
    auto* msg = (NVSEMessagingInterface*)nvse->QueryInterface(kInterface_Messaging);
    if (!msg) return false;
    msg->RegisterListener(nvse->GetPluginHandle(), "NVSE", OnMessage);
    return true;
}
}
