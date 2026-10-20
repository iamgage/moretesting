// Hitbloq for Quest - standalone C++ port of the PC mod's core features.
// Design note: everything that touches game types whose exact generated names vary between
// bs-cordl versions is resolved at RUNTIME through il2cpp reflection, so a wrong guess can
// never break the build (it just logs and disables that feature).
#include "scotland2/shared/modloader.h"
#include "beatsaber-hook/shared/utils/hooking.hpp"
#include "beatsaber-hook/shared/utils/il2cpp-utils.hpp"
#include "beatsaber-hook/shared/rapidjson/include/rapidjson/document.h"
#include "beatsaber-hook/shared/rapidjson/include/rapidjson/stringbuffer.h"
#include "beatsaber-hook/shared/rapidjson/include/rapidjson/writer.h"
#include "paper2_scotland2/shared/logger.hpp"
#include "bsml/shared/BSML.hpp"
#include "bsml/shared/BSML-Lite.hpp"
#include "bsml/shared/BSML/MainThreadScheduler.hpp"
#include "bsml/shared/BSML/FloatingScreen/FloatingScreen.hpp"

#include "GlobalNamespace/StandardLevelDetailView.hpp"
#include "GlobalNamespace/BeatmapKey.hpp"
#include "GlobalNamespace/BeatmapCharacteristicSO.hpp"
#include "UnityEngine/GameObject.hpp"
#include "UnityEngine/UI/Button.hpp"
#include "HMUI/ViewController.hpp"
#include "UnityEngine/Transform.hpp"
#include "UnityEngine/MonoBehaviour.hpp"
#include "UnityEngine/Networking/UnityWebRequest.hpp"
#include "UnityEngine/Networking/UnityWebRequestAsyncOperation.hpp"
#include "UnityEngine/Networking/DownloadHandler.hpp"
#include "TMPro/TextMeshProUGUI.hpp"
#include "TMPro/TextAlignmentOptions.hpp"

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstring>
#include <dirent.h>
#include <sys/stat.h>
#include <fstream>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <vector>

static constexpr auto Logger = Paper::ConstLoggerContext("Hitbloq");
#define LOG(...) Paper::Logger::fmtLog<Paper::LogLevel::INF>(__VA_ARGS__)

static modloader::ModInfo modInfo{MOD_ID, VERSION, 0};

// ------------------------------------------------------------------ small helpers
static StringW SW(const std::string& s) { return StringW(s.c_str()); }
static std::string Std(StringW s) { return s ? static_cast<std::string>(s) : std::string(); }

// Run `action` on the main thread once `cond` is true (polled every 0.1s). Only uses ScheduleAfterTime.
static void PollUntil(std::function<bool()> cond, std::function<void()> action) {
    auto step = std::make_shared<std::function<void()>>();
    *step = [cond, action, step]() {
        if (cond()) action();
        else BSML::MainThreadScheduler::ScheduleAfterTime(0.1f, *step);
    };
    BSML::MainThreadScheduler::ScheduleAfterTime(0.1f, *step);
}

// ---- safe rapidjson getters (rapidjson asserts abort on wrong types)
using JV = rapidjson::Value;
static const JV* Mem(const JV& v, const char* k) { return (v.IsObject() && v.HasMember(k)) ? &v[k] : nullptr; }
static double Num(const JV& v, const char* k, double def = 0) { auto m = Mem(v, k); return (m && m->IsNumber()) ? m->GetDouble() : def; }
static int Int(const JV& v, const char* k, int def = -1) { auto m = Mem(v, k); return (m && m->IsNumber()) ? (int)m->GetDouble() : def; }
static std::string Str(const JV& v, const char* k, const std::string& def = "") { auto m = Mem(v, k); return (m && m->IsString()) ? m->GetString() : def; }
static bool BoolOr(const JV& v, const char* k, bool def) { auto m = Mem(v, k); return (m && m->IsBool()) ? m->GetBool() : def; }

// ---- il2cpp reflection helpers
static Il2CppObject* Invoke(Il2CppObject* o, const char* method) {
    if (!o) return nullptr;
    auto m = il2cpp_functions::class_get_method_from_name(o->klass, method, 0);
    if (!m) return nullptr;
    Il2CppException* ex = nullptr;
    auto r = il2cpp_functions::runtime_invoke(m, o, nullptr, &ex);
    return ex ? nullptr : r;
}
static bool UnboxBool(Il2CppObject* boxed) { return boxed && *reinterpret_cast<bool*>(il2cpp_functions::object_unbox(boxed)); }
static int UnboxInt(Il2CppObject* boxed) { return boxed ? *reinterpret_cast<int*>(il2cpp_functions::object_unbox(boxed)) : 0; }
static Il2CppObject* FieldObj(Il2CppObject* o, const char* name) {
    if (!o) return nullptr;
    auto f = il2cpp_functions::class_get_field_from_name(o->klass, name);
    if (!f) return nullptr;
    Il2CppObject* r = nullptr;
    il2cpp_functions::field_get_value(o, f, &r);
    return r;
}
static int FieldInt(Il2CppObject* o, const char* name, int def = 0) {
    if (!o) return def;
    auto f = il2cpp_functions::class_get_field_from_name(o->klass, name);
    if (!f) return def;
    int r = def;
    il2cpp_functions::field_get_value(o, f, &r);
    return r;
}
// string from a field or a property getter
static std::string StringMember(Il2CppObject* o, const char* name) {
    if (!o) return "";
    if (auto s = FieldObj(o, name)) return Std(StringW(reinterpret_cast<Il2CppString*>(s)));
    std::string getter = std::string("get_") + name;
    if (auto s = Invoke(o, getter.c_str())) return Std(StringW(reinterpret_cast<Il2CppString*>(s)));
    return "";
}

// ------------------------------------------------------------------ config
static const char* CONFIG_PATH = "/sdcard/ModData/com.beatgames.beatsaber/Configs/Hitbloq.json";
static struct Config {
    std::string playerId;                   // BeatLeader / ScoreSaber id. Auto-detected on Quest when empty.
    std::string accountType = "auto";       // "auto" (like the PC mod: ScoreSaber if its mod is installed, else BeatLeader), "scoresaber" or "beatleader"
    std::string url = "https://hitbloq.com";
    // where the panel appears (world space). The panel has a grab handle, but these let you fix a bad default without rebuilding.
    float posX = 2.45f, posY = 1.55f, posZ = 2.75f, rotY = 30.0f;
    float arrowUp = 0.55f;   // how far above the leaderboard centre the switch arrows sit (metres)
    float arrowSide = 0.95f; // how far left/right of the leaderboard centre they sit (metres)
    // PlaylistManager loads playlists from here
    std::string playlistDir = "/sdcard/ModData/com.beatgames.beatsaber/Mods/PlaylistManager/Playlists";
    bool autoOpen = true;    // show the Hitbloq leaderboard automatically on Hitbloq-ranked maps (the < arrow goes back to the normal one)
} cfg;

static void LoadConfig() {
    std::ifstream f(CONFIG_PATH);
    if (f) {
        std::string s((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
        rapidjson::Document d;
        d.Parse(s.c_str());
        if (!d.HasParseError() && d.IsObject()) {
            cfg.playerId = Str(d, "playerId", cfg.playerId);
            cfg.accountType = Str(d, "accountType", cfg.accountType);
            cfg.url = Str(d, "url", cfg.url);
            cfg.posX = (float)Num(d, "posX", cfg.posX);
            cfg.posY = (float)Num(d, "posY", cfg.posY);
            cfg.posZ = (float)Num(d, "posZ", cfg.posZ);
            cfg.rotY = (float)Num(d, "rotY", cfg.rotY);
            cfg.arrowUp = (float)Num(d, "arrowUp", cfg.arrowUp);
            cfg.arrowSide = (float)Num(d, "arrowSide", cfg.arrowSide);
            cfg.playlistDir = Str(d, "playlistDir", cfg.playlistDir);
            cfg.autoOpen = BoolOr(d, "autoOpen", cfg.autoOpen);
        }
    }
    while (!cfg.url.empty() && cfg.url.back() == '/') cfg.url.pop_back();
    std::string out = "{\n  \"playerId\": \"" + cfg.playerId + "\",\n  \"accountType\": \"" + cfg.accountType +
                      "\",\n  \"url\": \"" + cfg.url + "\",\n  \"posX\": " + std::to_string(cfg.posX) +
                      ",\n  \"posY\": " + std::to_string(cfg.posY) + ",\n  \"posZ\": " + std::to_string(cfg.posZ) +
                      ",\n  \"rotY\": " + std::to_string(cfg.rotY) + ",\n  \"arrowUp\": " + std::to_string(cfg.arrowUp) + ",\n  \"arrowSide\": " + std::to_string(cfg.arrowSide) + ",\n  \"playlistDir\": \"" + cfg.playlistDir + "\",\n  \"autoOpen\": " + (cfg.autoOpen ? "true" : "false") + "\n}\n";
    std::ofstream o(CONFIG_PATH);
    if (o) o << out;
}

// ------------------------------------------------------------------ http (UnityWebRequest, polled on the main thread)
using HttpCb = std::function<void(long, std::string)>;

// Build a POST request purely through il2cpp reflection (UnityWebRequest.Post / WWWForm are not in this game's metadata).
static Il2CppObject* NewObject(const char* ns, const char* name, int argc, const char* paramHint, std::vector<void*> args) {
    auto k = il2cpp_utils::GetClassFromName(ns, name);
    if (!k) return nullptr;
    const MethodInfo* ctor = nullptr;
    void* iter = nullptr;
    while (auto m = il2cpp_functions::class_get_methods(k, &iter)) {
        if (std::string(il2cpp_functions::method_get_name(m)) != ".ctor" || (int)il2cpp_functions::method_get_param_count(m) != argc) continue;
        if (paramHint && argc > 0) {
            std::string tn = il2cpp_functions::type_get_name(il2cpp_functions::method_get_param(m, 0));
            if (tn.find(paramHint) == std::string::npos) continue;
        }
        ctor = m; break;
    }
    if (!ctor) return nullptr;
    auto o = il2cpp_functions::object_new(k);
    Il2CppException* ex = nullptr;
    il2cpp_functions::runtime_invoke(ctor, o, args.empty() ? nullptr : args.data(), &ex);
    return ex ? nullptr : o;
}
static bool CallMethod(Il2CppObject* o, const char* name, std::vector<void*> args) {
    auto m = il2cpp_functions::class_get_method_from_name(o->klass, name, (int)args.size());
    if (!m) return false;
    Il2CppException* ex = nullptr;
    il2cpp_functions::runtime_invoke(m, o, args.empty() ? nullptr : args.data(), &ex);
    return !ex;
}
static std::string UrlEncode(const std::string& in) {
    static const char* hex = "0123456789ABCDEF";
    std::string o;
    for (unsigned char c : in) {
        if (isalnum(c) || c == '-' || c == '_' || c == '.' || c == '~') o += (char)c;
        else { o += '%'; o += hex[c >> 4]; o += hex[c & 15]; }
    }
    return o;
}
static UnityEngine::Networking::UnityWebRequest* MakePost(const std::string& url, const std::string& body) {
    auto str = [](const std::string& x) { return (void*)il2cpp_functions::string_new(x.c_str()); };
    auto req = NewObject("UnityEngine.Networking", "UnityWebRequest", 2, "String", {str(url), str("POST")});
    auto byteK = il2cpp_utils::GetClassFromName("System", "Byte");
    if (!req || !byteK) return nullptr;
    auto arr = il2cpp_functions::array_new(byteK, body.size());
    std::memcpy(reinterpret_cast<char*>(arr) + 32, body.data(), body.size()); // 64-bit Il2CppArray: header 16 + bounds 8 + length 8
    auto up = NewObject("UnityEngine.Networking", "UploadHandlerRaw", 1, "Byte[]", {arr});
    auto dl = NewObject("UnityEngine.Networking", "DownloadHandlerBuffer", 0, nullptr, {});
    if (!up || !dl) return nullptr;
    CallMethod(req, "set_uploadHandler", {up});
    CallMethod(req, "set_downloadHandler", {dl});
    CallMethod(req, "SetRequestHeader", {str("Content-Type"), str("application/x-www-form-urlencoded")});
    return reinterpret_cast<UnityEngine::Networking::UnityWebRequest*>(req);
}

static void Http(const std::string& url, HttpCb cb, std::optional<std::pair<std::string, std::string>> form = std::nullopt) {
    UnityEngine::Networking::UnityWebRequest* req = nullptr;
    if (form) req = MakePost(url, form->first + "=" + UrlEncode(form->second));
    else req = UnityEngine::Networking::UnityWebRequest::Get(SW(url));
    if (!req) { cb(0, ""); return; }
    req->set_timeout(15);
    auto op = req->SendWebRequest();
    SafePtr<UnityEngine::Networking::UnityWebRequest> sreq(req);
    SafePtr<UnityEngine::Networking::UnityWebRequestAsyncOperation> sop(op);
    PollUntil(
        [sop]() { return !sop || sop->get_isDone(); },
        [sreq, cb]() {
            long code = 0; std::string body;
            if (sreq) {
                code = static_cast<long>(sreq->get_responseCode());
                auto dh = sreq->get_downloadHandler();
                if (dh) body = Std(dh->get_text());
                sreq->Dispose();
            }
            cb(code, body);
        });
}

static bool ParseJson(const std::string& s, rapidjson::Document& d) {
    if (s.size() < 3) return false;
    d.Parse(s.c_str());
    return !d.HasParseError();
}

// ------------------------------------------------------------------ state
static struct State {
    std::string platformId;
    int hbId = -1;
    bool registered = false;
    bool registering = false;
    std::string mapStr;
    std::vector<std::pair<std::string, float>> pools;
    int poolIdx = 0;
    bool around = false;
    int version = 0;
    std::string status, rankLine, board;
    bool lastPlayedRanked = false;
    bool refreshing = false;
    int source = 0;                  // leaderboard shown in the panel: 0 Hitbloq, 1 BeatLeader, 2 ScoreSaber
    bool custom = false;             // selected map is a custom map (BeatLeader / ScoreSaber can show it)
    std::string hash, chara;         // map hash and characteristic ("Standard", ...)
    int diffIdx = 4;                 // 0 Easy .. 4 ExpertPlus
} S;

static SafePtrUnity<BSML::FloatingScreen> screen;
static SafePtrUnity<UnityEngine::MonoBehaviour> detailView;
static SafePtrUnity<TMPro::TextMeshProUGUI> headerText, rankText, statusText, boardText;
static int uiGen = 0;

static std::string Fmt(double v, int dec = 2) { char b[32]; snprintf(b, sizeof b, "%.*f", dec, v); return b; }
static std::string StripTags(const std::string& s) {
    std::string o; bool in = false;
    for (char c : s) { if (c == '<') in = true; else if (c == '>') in = false; else if (!in) o += c; }
    return o;
}
static std::string CurPool() { return S.pools.empty() ? "" : S.pools[S.poolIdx].first; }

// ---- pool browser state (page in the left-hand Mods menu, modelled on the PC mod's pool browser)
static constexpr int POOLS_PER_PAGE = 6;
struct PoolInfo { std::string id, name, tagline, desc, owners; int maps = -1; double pop = -1; };
static struct Browser {
    bool loading = false, loaded = false, downloading = false;
    std::vector<PoolInfo> pools;
    int page = 0, sel = 0, sort = 0; // sort: 0 popularity, 1 name, 2 ranked maps
    std::string status;
} B;
static SafePtrUnity<TMPro::TextMeshProUGUI> pageHeader, pageList, pageDetail, pageStatus;

static std::string Trunc(const std::string& s, size_t max) { // UTF-8 safe
    if (s.size() <= max) return s;
    size_t n = max;
    while (n > 0 && (static_cast<unsigned char>(s[n]) & 0xC0) == 0x80) n--;
    return s.substr(0, n) + "...";
}
static int PageCount() { return std::max(1, ((int)B.pools.size() + POOLS_PER_PAGE - 1) / POOLS_PER_PAGE); }
static const char* SortName() { static const char* n[] = {"Popularity", "Name", "Ranked maps"}; return n[B.sort % 3]; }

static void DrawPage() {
    if (!pageHeader || !pageList || !pageDetail || !pageStatus) return;
    pageHeader->set_text(SW("<b><color=#ffd42a>HITBLOQ</color></b>   <size=70%><color=#aaaaaa>sort: " + std::string(SortName()) + "   page " +
                            std::to_string(B.page + 1) + "/" + std::to_string(PageCount()) + "</color></size>"));
    std::string list, detail;
    if (B.loading) list = "<color=#aaaaaa>Loading pools...</color>";
    else if (B.pools.empty()) list = "<color=#aaaaaa>No pools loaded.\nReopen this page to retry.</color>";
    else for (int i = 0; i < POOLS_PER_PAGE; i++) {
        int idx = B.page * POOLS_PER_PAGE + i;
        if (idx >= (int)B.pools.size()) break;
        bool sel = idx == B.sel;
        list += std::string("<pos=0%><color=#ffd42a>") + std::to_string(i + 1) + "</color><pos=9%>" + (sel ? "<color=#00ff88>" : "<color=#ffffff>") +
                Trunc(StripTags(B.pools[idx].name), 20) + "</color>\n";
    }
    if (B.sel >= 0 && B.sel < (int)B.pools.size()) {
        auto& pl = B.pools[B.sel];
        detail = "<b><color=#ffd42a>" + Trunc(StripTags(pl.name), 28) + "</color></b>\n";
        if (!pl.owners.empty()) detail += "<size=80%><color=#aaaaaa>Owners: " + Trunc(StripTags(pl.owners), 40) + "</color></size>\n";
        if (!pl.tagline.empty()) detail += "<size=85%>" + Trunc(StripTags(pl.tagline), 90) + "</size>\n";
        if (!pl.desc.empty()) detail += "<size=80%><color=#dddddd>" + Trunc(StripTags(pl.desc), 220) + "</color></size>\n";
        if (pl.maps >= 0) detail += "<color=#aa6eff>" + std::to_string(pl.maps) + " ranked maps</color>   ";
        if (pl.pop >= 0) detail += "<color=#ffd42a>" + Fmt(pl.pop, 0) + " popularity</color>";
    }
    pageList->set_text(SW(list));
    pageDetail->set_text(SW(detail));
    pageStatus->set_text(SW(B.status));
}

static void Draw() {
    DrawPage();
    if (!headerText || !rankText || !statusText || !boardText) return;
    std::string h;
    if (S.source == 0) {
        h = "<b><color=#ffd42a>Hitbloq</color></b>";
        if (!S.pools.empty()) h += "   pool: <color=#aa6eff>" + CurPool() + "</color> (" + Fmt(S.pools[S.poolIdx].second) + " stars)";
        else h += "   <color=#aaaaaa>map not ranked on any pool</color>";
    } else if (S.source == 1) h = "<b><color=#ff4d6d>BeatLeader</color></b>   <color=#aaaaaa>global top scores</color>";
    else h = "<b><color=#ffd42a>ScoreSaber</color></b>   <color=#aaaaaa>global top scores</color>";
    headerText->set_text(SW(h));
    rankText->set_text(SW(S.source == 0 ? S.rankLine : std::string()));
    statusText->set_text(SW(S.status));
    boardText->set_text(SW(S.board));
}

// ------------------------------------------------------------------ playlists (same source as the website: /static/hashlists/<pool>.bplist)
static void MkDirs(const std::string& path) {
    std::string cur;
    for (size_t i = 0; i < path.size(); i++) {
        cur += path[i];
        if (path[i] == '/' && cur.size() > 1) mkdir(cur.c_str(), 0777);
    }
    mkdir(path.c_str(), 0777);
}

static std::string SafeFileName(const std::string& in) {
    std::string o;
    for (unsigned char c : in) o += (isalnum(c) || c == '-' || c == '_') ? (char)c : '_';
    return o.empty() ? "pool" : o;
}

// Saves the pool's playlist where PlaylistManager picks it up. PlaylistManager then offers to download the missing songs.
static void DownloadPlaylist(const std::string& poolId, const std::string& shownName) {
    if (poolId.empty()) { S.status = B.status = "<color=red>No pool selected.</color>"; Draw(); return; }
    if (B.downloading) return;
    B.downloading = true;
    S.status = B.status = "Downloading playlist...";
    Draw();
    Http(cfg.url + "/static/hashlists/" + UrlEncode(poolId) + ".bplist", [poolId, shownName](long code, std::string body) {
        B.downloading = false;
        rapidjson::Document d;
        auto songs = ParseJson(body, d) && d.IsObject() ? Mem(d, "songs") : nullptr;
        if (code < 200 || code >= 300 || !songs || !songs->IsArray()) {
            LOG("playlist download failed for {} (http {}, {} bytes)", poolId, code, body.size());
            S.status = B.status = "<color=red>Could not download that playlist.</color>";
            Draw(); return;
        }
        MkDirs(cfg.playlistDir);
        std::string path = cfg.playlistDir + "/Hitbloq_" + SafeFileName(poolId) + ".bplist";
        std::ofstream o(path, std::ios::binary | std::ios::trunc);
        if (o) o.write(body.data(), (std::streamsize)body.size());
        if (!o) {
            LOG("could not write {}", path);
            S.status = B.status = "<color=red>Could not save the playlist file.</color>";
            Draw(); return;
        }
        o.close();
        LOG("saved playlist {} ({} maps) to {}", poolId, (int)songs->Size(), path);
        S.status = B.status = "<color=green>Saved " + Trunc(StripTags(shownName), 24) + " (" + std::to_string((int)songs->Size()) +
                   " maps). Restart the game, then open PlaylistManager.</color>";
        Draw();
    });
}

// The pool list endpoints return slightly different shapes over time, so accept arrays of ids or of objects.
static void AddPool(std::vector<PoolInfo>& out, PoolInfo pi) {
    if (pi.id.empty()) return;
    for (auto& p : out) if (p.id == pi.id) return;
    if (pi.name.empty()) pi.name = pi.id;
    out.push_back(std::move(pi));
}
static std::string FirstStr(const JV& e, std::initializer_list<const char*> keys) {
    for (auto k : keys) { auto v = Str(e, k); if (!v.empty()) return v; }
    return "";
}
static double FirstNum(const JV& e, std::initializer_list<const char*> keys) {
    for (auto k : keys) { auto m = Mem(e, k); if (m && m->IsNumber()) return m->GetDouble(); }
    return -1;
}
static void ParsePoolItem(std::vector<PoolInfo>& out, const JV& e, const char* keyFallback, bool allowString) {
    PoolInfo pi;
    if (e.IsString()) { if (!allowString) return; pi.id = pi.name = e.GetString(); AddPool(out, std::move(pi)); return; }
    if (!e.IsObject()) return;
    pi.id = FirstStr(e, {"_id", "id", "pool_id"});
    if (pi.id.empty() && keyFallback) pi.id = keyFallback;
    pi.name = FirstStr(e, {"shown_name", "name"});
    pi.tagline = FirstStr(e, {"short_description", "tagline"});
    pi.desc = FirstStr(e, {"description", "long_description", "about"});
    pi.maps = (int)FirstNum(e, {"ranked_maps", "map_count", "maps", "ranked_count", "ranked_list_count"});
    pi.pop = FirstNum(e, {"popularity", "players", "player_count"});
    if (auto o = Mem(e, "owners")) {
        if (o->IsArray()) for (auto& x : o->GetArray()) {
            std::string n = x.IsString() ? x.GetString() : (x.IsObject() ? Str(x, "tag", Str(x, "name")) : "");
            if (!n.empty()) pi.owners += (pi.owners.empty() ? "" : ", ") + n;
        }
    }
    AddPool(out, std::move(pi));
}
static void ParsePools(const JV& d, std::vector<PoolInfo>& out) {
    if (d.IsArray()) { for (auto& e : d.GetArray()) ParsePoolItem(out, e, nullptr, true); return; }
    if (!d.IsObject()) return;
    for (auto& m : d.GetObject()) {
        if (m.value.IsArray()) { for (auto& e : m.value.GetArray()) ParsePoolItem(out, e, nullptr, true); }
        else if (m.value.IsObject()) ParsePoolItem(out, m.value, m.name.GetString(), false);
    }
}

static void SortPools() {
    std::string keep = (B.sel >= 0 && B.sel < (int)B.pools.size()) ? B.pools[B.sel].id : "";
    auto lower = [](std::string x) { std::transform(x.begin(), x.end(), x.begin(), [](unsigned char c) { return std::tolower(c); }); return x; };
    std::stable_sort(B.pools.begin(), B.pools.end(), [lower](const PoolInfo& a, const PoolInfo& b) {
        if (B.sort == 1) return lower(a.name) < lower(b.name);
        if (B.sort == 2) return a.maps > b.maps;
        return a.pop > b.pop;
    });
    B.sel = 0;
    for (int i = 0; i < (int)B.pools.size(); i++) if (B.pools[i].id == keep) B.sel = i;
    B.page = B.sel / POOLS_PER_PAGE;
}

static void LoadPools() {
    if (B.loading) return;
    B.loading = true;
    Draw();
    Http(cfg.url + "/api/map_pools_detailed", [](long code, std::string body) {
        rapidjson::Document d;
        std::vector<PoolInfo> out;
        if (ParseJson(body, d)) ParsePools(d, out);
        B.loading = false;
        if (out.empty()) {
            LOG("pool list empty (http {}, {} bytes)", code, body.size());
            B.status = "<color=red>Could not load the pool list.</color>";
        } else {
            B.pools = std::move(out); B.loaded = true; B.sel = 0; B.page = 0; B.status.clear();
            SortPools();
            LOG("loaded {} pools", (int)B.pools.size());
        }
        Draw();
    });
}

// ------------------------------------------------------------------ user / registration
static void CheckRegistered(std::function<void()> done, int attempt = 0);

// is a mod whose file name contains `needle` installed? (checks the scotland2 mod folders)
static bool ModInstalled(const std::string& needle) {
    static const char* dirs[] = {
        "/sdcard/ModData/com.beatgames.beatsaber/Modloader/early_mods",
        "/sdcard/ModData/com.beatgames.beatsaber/Modloader/mods",
        "/sdcard/ModData/com.beatgames.beatsaber/Modloader/late_mods",
        "/sdcard/ModData/com.beatgames.beatsaber/Mods",
    };
    for (auto dir : dirs) {
        DIR* d = opendir(dir);
        if (!d) continue;
        while (auto e = readdir(d)) {
            std::string n = e->d_name;
            std::transform(n.begin(), n.end(), n.begin(), [](unsigned char c) { return std::tolower(c); });
            if (n.find(needle) != std::string::npos) { closedir(d); return true; }
        }
        closedir(d);
    }
    return false;
}

// which leaderboard account to use for registration, in order of preference (same priority as the PC mod: ScoreSaber first)
static std::vector<std::string> AccountOrder() {
    if (cfg.accountType == "scoresaber") return {"scoresaber", "beatleader"};
    if (cfg.accountType == "beatleader") return {"beatleader", "scoresaber"};
    return ModInstalled("scoresaber") ? std::vector<std::string>{"scoresaber", "beatleader"}
                                      : std::vector<std::string>{"beatleader", "scoresaber"};
}

static void TryRegister(std::vector<std::string> order, size_t i) {
    if (i >= order.size()) {
        S.registering = false;
        S.status = "<color=red>Please submit some scores to your BeatLeader or ScoreSaber account first.</color>";
        Draw(); return;
    }
    bool bl = order[i] == "beatleader";
    std::string existsUrl = bl ? "https://api.beatleader.xyz/player/" + S.platformId
                               : "https://scoresaber.com/api/player/" + S.platformId + "/full";
    Http(existsUrl, [order, i, bl](long code, std::string body) {
        if (code == 0 || code >= 500) { // network / server problem, not a missing account
            S.registering = false;
            S.status = "<color=red>Could not reach the leaderboard server, try again later.</color>";
            Draw(); return;
        }
        bool exists = code >= 200 && code < 300 && body.find("Player not found") == std::string::npos;
        if (!exists) { TryRegister(order, i + 1); return; }
        std::string profile = bl ? "https://www.beatleader.xyz/u/" + S.platformId : "https://scoresaber.com/u/" + S.platformId;
        Http("https://hitbloq.com/api/add_user", [](long, std::string b) {
            rapidjson::Document d;
            if (ParseJson(b, d) && Str(d, "status") == "ratelimit") {
                S.registering = false;
                S.status = "<color=red>Please register for Hitbloq on the Discord server.</color>";
                Draw(); return;
            }
            CheckRegistered([]() {}, 1); // poll until the account shows up
        }, std::make_pair(std::string("url"), profile));
    });
}

static void StartRegistration() {
    if (S.registering) return;
    S.registering = true;
    S.status = "Registering Hitbloq account, this may take a while...";
    Draw();
    TryRegister(AccountOrder(), 0);
}

static void CheckRegistered(std::function<void()> done, int attempt) {
    if (S.platformId.empty()) {
        S.status = "<color=red>No player id found. Set playerId in Configs/Hitbloq.json</color>";
        Draw(); return;
    }
    Http("https://hitbloq.com/api/tools/ss_registered/" + S.platformId, [done, attempt](long, std::string body) {
        rapidjson::Document d;
        int id = -1; bool reg = false;
        if (ParseJson(body, d) && d.IsObject()) {
            id = Int(d, "user", -1);
            auto r = Mem(d, "registered");
            reg = r && r->IsBool() && r->GetBool();
        }
        if (id != -1 && reg) {
            S.hbId = id; S.registered = true; S.registering = false;
            if (S.status.find("Regist") != std::string::npos) S.status = "";
            Draw(); done(); return;
        }
        if (id == -1 && attempt == 0) { StartRegistration(); return; }
        if (attempt > 0 && attempt < 40) { // registration in progress, retry every 5s
            S.status = "Registering Hitbloq account, this may take a while...";
            Draw();
            BSML::MainThreadScheduler::ScheduleAfterTime(5.0f, [done, attempt]() { CheckRegistered(done, attempt + 1); });
            return;
        }
        S.registering = false;
        S.status = "<color=red>Hitbloq registration did not finish, try again later.</color>";
        Draw();
    });
}

// ------------------------------------------------------------------ data loading
static void LoadRank(int ver) {
    if (!S.registered || S.pools.empty()) { S.rankLine.clear(); Draw(); return; }
    Http(cfg.url + "/api/player_rank/" + CurPool() + "/" + std::to_string(S.hbId), [ver](long, std::string body) {
        if (ver != S.version) return;
        rapidjson::Document d;
        if (ParseJson(body, d) && Mem(d, "rank")) {
            S.rankLine = "<color=#ffffff>#" + std::to_string(Int(d, "rank", 0)) + "</color>  <color=#aa6eff>" +
                         Fmt(Num(d, "cr")) + "cr</color>  " + std::to_string(Int(d, "ranked_score_count", 0)) + " scores";
        } else S.rankLine = "<color=#aaaaaa>unranked on this pool</color>";
        Draw();
    });
}

static const char* DiffName(int d);
static std::string LowerS(std::string x) { std::transform(x.begin(), x.end(), x.begin(), [](unsigned char c) { return std::tolower(c); }); return x; }
static std::string UpperS(std::string x) { std::transform(x.begin(), x.end(), x.begin(), [](unsigned char c) { return std::toupper(c); }); return x; }
static std::string Commas(double v) {
    std::string n = std::to_string((long long)v), o;
    int c = 0;
    for (int i = (int)n.size() - 1; i >= 0; i--) { o.insert(o.begin(), n[i]); if (++c % 3 == 0 && i > 0 && n[i - 1] != '-') o.insert(o.begin(), ','); }
    return o;
}

// BeatLeader / ScoreSaber top scores for the selected custom map (public endpoints, no account needed)
static void LoadExternalBoard(int ver) {
    if (!S.custom) { S.board.clear(); Draw(); return; }
    bool bl = S.source == 1;
    std::string url = bl ? "https://api.beatleader.xyz/v3/scores/" + LowerS(S.hash) + "/" + DiffName(S.diffIdx) + "/" + S.chara + "/general/global/page?page=1&count=10"
                         : "https://scoresaber.com/api/leaderboard/by-hash/" + UpperS(S.hash) + "/scores?difficulty=" + std::to_string(S.diffIdx * 2 + 1) +
                           "&gameMode=Solo" + S.chara + "&page=1";
    S.board = "<color=#aaaaaa>Loading...</color>"; Draw();
    Http(url, [ver, bl](long code, std::string body) {
        if (ver != S.version || (S.source == 1) != bl) return;
        rapidjson::Document d;
        std::string out;
        bool ok = ParseJson(body, d);
        const JV* arr = ok && d.IsObject() ? Mem(d, bl ? "data" : "scores") : nullptr;
        LOG("{} board: http {}, {} bytes, rows {}", bl ? "BeatLeader" : "ScoreSaber", code, body.size(), arr && arr->IsArray() ? (int)arr->Size() : -1);
        if (arr && arr->IsArray()) {
            int n = 0;
            for (auto& e : arr->GetArray()) {
                if (++n > 10) break;
                auto pl = Mem(e, bl ? "player" : "leaderboardPlayerInfo");
                std::string name = pl ? StripTags(Str(*pl, "name", "?")) : "?";
                if (name.size() > 22) name = name.substr(0, 22);
                bool me = pl && !S.platformId.empty() && Str(*pl, "id") == S.platformId;
                double pp = Num(e, "pp");
                std::string c3, c4 = pp > 0 ? Fmt(pp) + "pp" : "-";
                if (bl) { double acc = Num(e, "accuracy"); c3 = Fmt(acc <= 1.0001 ? acc * 100.0 : acc) + "%"; }
                else c3 = Commas(Num(e, "modifiedScore", Num(e, "baseScore")));
                out += std::string(me ? "<color=#00ff88>" : "") + "<pos=0%>#" + std::to_string(Int(e, "rank", n)) + "<pos=11%>" + name +
                       "<pos=58%><color=#ffd42a>" + c3 + "</color><pos=80%><color=#aa6eff>" + c4 + "</color>" + (me ? "</color>" : "") + "\n";
            }
        }
        if (out.empty()) S.board = (code < 200 || code >= 300) ? (code == 404 ? "<color=#aaaaaa>No scores yet (or map not on this leaderboard).</color>" : "<color=#ff6666>Could not load this leaderboard.</color>")
                                                             : "<color=#aaaaaa>No scores yet.</color>";
        else S.board = std::string("<size=80%><color=#888888><pos=0%>#<pos=11%>PLAYER<pos=58%>") + (bl ? "ACC" : "SCORE") + "<pos=80%>PP</color></size>\n" + out;
        Draw();
    });
}

static void LoadHitbloqBoard(int ver) {
    if (S.mapStr.empty()) { S.board.clear(); Draw(); return; }
    if (S.pools.empty()) { S.board = "<color=#aaaaaa>Not ranked on Hitbloq.\nUse the BeatLeader / ScoreSaber tabs above.</color>"; Draw(); return; }
    std::string url;
    if (S.around) {
        if (!S.registered) { S.board = "<color=#aaaaaa>Not registered yet.</color>"; Draw(); return; }
        url = cfg.url + "/api/leaderboard/" + S.mapStr + "/nearby_scores_extended/" + std::to_string(S.hbId);
    } else url = cfg.url + "/api/leaderboard/" + S.mapStr + "/scores_extended/0";
    S.board = "<color=#aaaaaa>Loading...</color>"; Draw();
    std::string pool = CurPool();
    Http(url, [ver, pool](long code, std::string body) {
        if (ver != S.version) return;
        rapidjson::Document d;
        std::string out;
        bool ok = ParseJson(body, d) && d.IsArray();
        LOG("board loaded for pool {}: http {}, {} bytes, parsed {}", pool, code, body.size(), ok);
        if (ok) {
            int n = 0;
            for (auto& e : d.GetArray()) {
                auto cr = Mem(e, "cr");
                if (!cr || !cr->IsObject() || !cr->HasMember(pool.c_str()) || !(*cr)[pool.c_str()].IsNumber()) continue;
                if (++n > 10) break;
                std::string name = StripTags(Str(e, "username", "?"));
                if (name.size() > 22) name = name.substr(0, 22);
                std::string color = Str(e, "custom_color", "#ffffff");
                if (color.empty() || color[0] != '#') color = "#ffffff";
                bool me = S.registered && Int(e, "user", -2) == S.hbId;
                out += std::string(me ? "<color=#00ff88>" : "") + "<pos=0%>#" + std::to_string(Int(e, "rank", 0)) +
                       "<pos=11%><color=" + color + ">" + name + "</color><pos=62%><color=#ffd42a>" +
                       Fmt(Num(e, "accuracy")) + "%</color><pos=80%><color=#aa6eff>" +
                       Fmt((*cr)[pool.c_str()].GetDouble()) + "cr</color>" + (me ? "</color>" : "") + "\n";
            }
        }
        if (out.empty()) S.board = (code < 200 || code >= 300) ? "<color=#ff6666>Could not load the leaderboard.</color>" : "<color=#aaaaaa>No scores yet.</color>";
        else S.board = "<size=80%><color=#888888><pos=0%>#<pos=11%>PLAYER<pos=62%>ACC<pos=80%>CR</color></size>\n" + out;
        Draw();
    });
}

static void LoadBoard(int ver) {
    if (S.source == 0) LoadHitbloqBoard(ver);
    else LoadExternalBoard(ver);
}

static void LoadForCurrentMap() {
    if (S.pools.empty()) return;
    int ver = S.version;
    // The global leaderboard is public: show it right away, whether or not we know who the player is.
    if (S.source == 0) LoadBoard(ver);
    auto go = [ver]() { if (ver == S.version) { LoadRank(ver); LoadBoard(ver); } };
    if (S.registered) { LoadRank(ver); return; }
    if (S.platformId.empty()) {
        S.rankLine = "<color=#aaaaaa>Not linked: set playerId in Hitbloq.json</color>";
        Draw(); return;
    }
    if (!S.registering) CheckRegistered(go);
}

static void ReloadAll() {
    int ver = ++S.version;
    LoadRank(ver);
    LoadBoard(ver);
}

// ------------------------------------------------------------------ refresh (make Hitbloq pull the new BL/SS score)
static void Refresh() {
    if (S.refreshing || !S.registered) return;
    S.refreshing = true;
    S.status = "Refreshing...";
    Draw();
    Http("https://hitbloq.com/api/update_user/" + std::to_string(S.hbId), [](long, std::string body) {
        rapidjson::Document d;
        if (!ParseJson(body, d) || !d.IsObject()) {
            S.refreshing = false; S.status = "<color=red>Hitbloq servers are not responding.</color>"; Draw(); return;
        }
        auto er = Mem(d, "error");
        if (er && !er->IsNull()) {
            std::string err = er->IsString() ? er->GetString() : "unknown";
            S.refreshing = false;
            S.status = err.find("refreshed too fast") != std::string::npos ? "" : "<color=red>Error: " + err + "</color>";
            Draw(); return;
        }
        long actionId = (long)Num(d, "id", -1);
        auto poll = std::make_shared<std::function<void(int)>>();
        *poll = [actionId, poll](int i) {
            BSML::MainThreadScheduler::ScheduleAfterTime(3.0f, [actionId, poll, i]() {
                Http("https://hitbloq.com/api/actions", [actionId, poll, i](long, std::string b) {
                    rapidjson::Document a;
                    bool pending = false;
                    if (ParseJson(b, a) && a.IsArray())
                        for (auto& e : a.GetArray())
                            if ((long)Num(e, "id", -2) == actionId) pending = true;
                    if (!pending) {
                        S.refreshing = false; S.status = "<color=green>Score refreshed!</color>"; Draw();
                        ReloadAll();
                        BSML::MainThreadScheduler::ScheduleAfterTime(1.5f, []() {
                            if (S.status.find("refreshed") != std::string::npos) { S.status.clear(); Draw(); }
                        });
                    } else if (i + 1 >= 7) {
                        S.refreshing = false; S.status = "<color=red>The action queue is very busy, try again later.</color>"; Draw();
                    } else {
                        S.status = "Refreshing... (" + std::to_string(i + 1) + " of 7 tries)"; Draw();
                        (*poll)(i + 1);
                    }
                });
            });
        };
        (*poll)(0);
    });
}

// ------------------------------------------------------------------ UI
// ---- where the real leaderboard is (found at runtime so the panel/arrows line up with it)
struct LbPoseT { float p[3] = {0, 0, 0}, f[3] = {0, 0, 1}, u[3] = {0, 1, 0}, r[3] = {1, 0, 0}; UnityEngine::Quaternion rot{}; bool ok = false; };
static LbPoseT lbPose;
static bool panelOpen = false; // true = Hitbloq panel is shown instead of the normal leaderboard
static bool wantHitbloq = true; // last choice made with the arrows (like LeaderboardCore remembers the selected leaderboard)
static SafePtrUnity<BSML::FloatingScreen> arrowR, arrowL;

static UnityEngine::Transform* FindLeaderboardTransform() {
    auto rk = il2cpp_utils::GetClassFromName("UnityEngine", "Resources");
    if (!rk) return nullptr;
    auto m = il2cpp_functions::class_get_method_from_name(rk, "FindObjectsOfTypeAll", 1);
    if (!m) return nullptr;
    for (auto name : {"PlatformLeaderboardViewController", "LeaderboardViewController"}) {
        auto k = il2cpp_utils::GetClassFromName("", name);
        if (!k) continue;
        void* args[] = {il2cpp_functions::type_get_object(il2cpp_functions::class_get_type(k))};
        Il2CppException* ex = nullptr;
        auto arr = reinterpret_cast<Il2CppArray*>(il2cpp_functions::runtime_invoke(m, nullptr, args, &ex));
        if (ex || !arr || arr->max_length < 1) continue;
        auto items = reinterpret_cast<Il2CppObject**>(reinterpret_cast<char*>(arr) + 32); // 64-bit Il2CppArray header
        for (int i = 0; i < (int)arr->max_length; i++) { // only the instance that is actually on screen
            auto go = Invoke(items[i], "get_gameObject");
            if (go && UnboxBool(Invoke(go, "get_activeInHierarchy"))) {
                if (auto tr = reinterpret_cast<UnityEngine::Transform*>(Invoke(items[i], "get_transform"))) return tr;
            }
        }
    }
    return nullptr;
}

static void RefreshPose() {
    lbPose.ok = false;
    auto tr = FindLeaderboardTransform();
    if (!tr) { LOG("leaderboard view not found, using config position"); return; }
    auto p = tr->get_position(); auto f = tr->get_forward(); auto u = tr->get_up(); auto r = tr->get_right();
    lbPose.p[0] = p.x; lbPose.p[1] = p.y; lbPose.p[2] = p.z;
    lbPose.f[0] = f.x; lbPose.f[1] = f.y; lbPose.f[2] = f.z;
    lbPose.u[0] = u.x; lbPose.u[1] = u.y; lbPose.u[2] = u.z;
    lbPose.r[0] = r.x; lbPose.r[1] = r.y; lbPose.r[2] = r.z;
    lbPose.rot = tr->get_rotation();
    lbPose.ok = true;
    LOG("leaderboard at ({}, {}, {})", p.x, p.y, p.z);
}

// toward = metres towards the viewer, up/side = metres above / to the right of the leaderboard centre
static void Place(UnityEngine::Transform* tr, float toward, float up, float side) {
    if (!tr) return;
    if (!lbPose.ok) { // fallback: config position
        tr->set_position({cfg.posX + side, cfg.posY + up, cfg.posZ - toward});
        return;
    }
    tr->set_position({lbPose.p[0] - lbPose.f[0] * toward + lbPose.u[0] * up + lbPose.r[0] * side,
                      lbPose.p[1] - lbPose.f[1] * toward + lbPose.u[1] * up + lbPose.r[1] * side,
                      lbPose.p[2] - lbPose.f[2] * toward + lbPose.u[2] * up + lbPose.r[2] * side});
    tr->set_rotation(lbPose.rot);
}
static void PlaceScreens() {
    if (screen) Place(screen->get_transform(), 0.08f, 0.0f, 0.0f);
    if (arrowR) Place(arrowR->get_transform(), 0.14f, cfg.arrowUp, cfg.arrowSide);
    if (arrowL) Place(arrowL->get_transform(), 0.14f, cfg.arrowUp, -cfg.arrowSide);
}

static void BuildUI() {
    if (screen) return;
    int gen = ++uiGen;
    screen = BSML::Lite::CreateFloatingScreen({96.0f, 76.0f}, {cfg.posX, cfg.posY, cfg.posZ}, {0.0f, cfg.rotY, 0.0f}, 0.0f, true);
    LOG("panel created");
    auto t = screen->get_transform();
    auto mk = [&](float y, float h, float size, int align) {
        auto tx = BSML::Lite::CreateText(t, "", {0.0f, y}, {90.0f, h});
        tx->set_fontSize(size);
        tx->set_alignment(static_cast<TMPro::TextAlignmentOptions>(align));
        tx->set_richText(true);
        tx->set_enableWordWrapping(false);
        return tx;
    };
    // TMP alignment ints: 514 = Center, 257 = TopLeft
    headerText = mk(25.0f, 8.0f, 4.0f, 514);
    rankText   = mk(18.5f, 6.0f, 3.2f, 514);
    boardText  = mk(-5.0f, 40.0f, 2.9f, 257);
    statusText = mk(-27.0f, 6.0f, 2.6f, 514);
    // source tabs: Hitbloq / BeatLeader / ScoreSaber
    auto pick = [](int src) { if (S.source == src) return; S.source = src; Draw(); ReloadAll(); };
    BSML::Lite::CreateUIButton(t, "Hitbloq", {-32.0f, 33.0f}, {30.0f, 7.0f}, [pick]() { pick(0); });
    BSML::Lite::CreateUIButton(t, "BeatLeader", {0.0f, 33.0f}, {30.0f, 7.0f}, [pick]() { pick(1); });
    BSML::Lite::CreateUIButton(t, "ScoreSaber", {32.0f, 33.0f}, {30.0f, 7.0f}, [pick]() { pick(2); });
    BSML::Lite::CreateUIButton(t, "Pool", {-36.0f, -33.0f}, {22.0f, 8.0f}, []() {
        if (S.pools.size() < 2) return;
        S.poolIdx = (S.poolIdx + 1) % (int)S.pools.size();
        Draw(); ReloadAll();
    });
    BSML::Lite::CreateUIButton(t, "Global/Near", {-12.0f, -33.0f}, {22.0f, 8.0f}, []() {
        S.around = !S.around;
        ReloadAll();
    });
    BSML::Lite::CreateUIButton(t, "Refresh", {12.0f, -33.0f}, {22.0f, 8.0f}, []() { Refresh(); });
    BSML::Lite::CreateUIButton(t, "Playlist", {36.0f, -33.0f}, {22.0f, 8.0f}, []() {
        DownloadPlaylist(CurPool(), CurPool()); // playlist of the selected pool
    });

    // the left/right switch arrows around the leaderboard, like the PC mod
    arrowR = BSML::Lite::CreateFloatingScreen({16.0f, 14.0f}, {cfg.posX + cfg.arrowSide, cfg.posY + cfg.arrowUp, cfg.posZ}, {0.0f, cfg.rotY, 0.0f}, 0.0f, true);
    BSML::Lite::CreateUIButton(arrowR->get_transform(), ">", {0.0f, 0.0f}, {12.0f, 10.0f}, []() { panelOpen = wantHitbloq = true; });
    arrowL = BSML::Lite::CreateFloatingScreen({16.0f, 14.0f}, {cfg.posX - cfg.arrowSide, cfg.posY + cfg.arrowUp, cfg.posZ}, {0.0f, cfg.rotY, 0.0f}, 0.0f, true);
    BSML::Lite::CreateUIButton(arrowL->get_transform(), "<", {0.0f, 0.0f}, {12.0f, 10.0f}, []() { panelOpen = false; wantHitbloq = false; });

    // visibility: only while the level detail view is on screen
    auto poll = std::make_shared<std::function<void()>>();
    auto last = std::make_shared<int>(-1);
    *poll = [poll, gen, last]() {
        if (!screen || gen != uiGen) return;
        bool detail = detailView && detailView->get_gameObject()->get_activeInHierarchy();
        bool ranked = S.custom;
        bool showR = detail && ranked && !panelOpen;
        bool showL = detail && panelOpen;
        bool showPanel = detail && panelOpen;
        int state = (int)detail * 8 + (int)showR * 4 + (int)showL * 2 + (int)showPanel;
        if (*last != state) {
            *last = state;
            LOG("detail view {}, right arrow {}, left arrow {}, panel {}", detail, showR, showL, showPanel);
            if (detail) { RefreshPose(); PlaceScreens(); }
        } else if (detail && !lbPose.ok && (showR || showL || showPanel)) {
            RefreshPose(); PlaceScreens(); // keep looking until the leaderboard is found
        }
        screen->get_gameObject()->SetActive(showPanel);
        if (arrowR) arrowR->get_gameObject()->SetActive(showR);
        if (arrowL) arrowL->get_gameObject()->SetActive(showL);
        BSML::MainThreadScheduler::ScheduleAfterTime(0.4f, *poll);
    };
    (*poll)();
}

// ------------------------------------------------------------------ pool browser page in the left-hand Mods menu
static void PlaylistPageDidActivate(HMUI::ViewController* self, bool firstActivation, bool addedToHierarchy, bool screenSystemEnabling) {
    LOG("Hitbloq page activated (first: {})", firstActivation);
    if (firstActivation) {
        auto t = self->get_transform();
        auto mk = [&](float x, float y, float w, float h, float size, int align, bool wrap) {
            auto tx = BSML::Lite::CreateText(t, "", {x, y}, {w, h});
            tx->set_fontSize(size);
            tx->set_alignment(static_cast<TMPro::TextAlignmentOptions>(align));
            tx->set_richText(true);
            tx->set_enableWordWrapping(wrap);
            return tx;
        };
        pageHeader = mk(0.0f, 31.0f, 84.0f, 7.0f, 4.0f, 514, false);
        pageList   = mk(-23.0f, 8.0f, 40.0f, 40.0f, 3.2f, 257, false);
        pageDetail = mk(23.0f, 8.0f, 40.0f, 40.0f, 2.8f, 257, true);
        pageStatus = mk(0.0f, -15.0f, 88.0f, 5.0f, 2.6f, 514, false);
        float x0 = -35.0f, dx = 10.0f;
        BSML::Lite::CreateUIButton(t, "<", {x0, -22.0f}, {9.0f, 8.0f}, []() { if (B.page > 0) { B.page--; Draw(); } });
        static const char* numLabels[] = {"1", "2", "3", "4", "5", "6"};
        for (int n = 0; n < POOLS_PER_PAGE; n++)
            BSML::Lite::CreateUIButton(t, numLabels[n], {x0 + dx * (n + 1), -22.0f}, {9.0f, 8.0f}, [n]() {
                int i = B.page * POOLS_PER_PAGE + n;
                if (i < (int)B.pools.size()) { B.sel = i; Draw(); }
            });
        BSML::Lite::CreateUIButton(t, ">", {x0 + dx * (POOLS_PER_PAGE + 1), -22.0f}, {9.0f, 8.0f}, []() { if (B.page + 1 < PageCount()) { B.page++; Draw(); } });
        BSML::Lite::CreateUIButton(t, "Sort", {-22.0f, -31.0f}, {24.0f, 8.0f}, []() { B.sort = (B.sort + 1) % 3; SortPools(); Draw(); });
        BSML::Lite::CreateUIButton(t, "GO TO PLAYLIST!", {18.0f, -31.0f}, {44.0f, 8.0f}, []() {
            if (B.sel >= 0 && B.sel < (int)B.pools.size()) DownloadPlaylist(B.pools[B.sel].id, B.pools[B.sel].name);
        });
    }
    B.status.clear();
    if (!B.loaded) LoadPools(); // also retries after a failed load
    Draw();
}

// ------------------------------------------------------------------ map selection
static const char* DiffName(int d) {
    static const char* n[] = {"Easy", "Normal", "Hard", "Expert", "ExpertPlus"};
    return (d >= 0 && d < 5) ? n[d] : "ExpertPlus";
}

// BeatmapKey member names differ between codegen versions ("levelId" vs "___levelId"); accept both.
template <class K> static void FillKey(K k, std::string& levelId, int& diff, std::string& chara) {
    if constexpr (requires { k.___levelId; }) levelId = Std(k.___levelId);
    else if constexpr (requires { k.levelId; }) levelId = Std(k.levelId);
    if constexpr (requires { k.___difficulty; }) diff = static_cast<int>(k.___difficulty);
    else if constexpr (requires { k.difficulty; }) diff = static_cast<int>(k.difficulty);
    auto fillChar = [&](auto&& c) { if (c) chara = Std(c->get_serializedName()); };
    if constexpr (requires { k.___beatmapCharacteristic; }) fillChar(k.___beatmapCharacteristic);
    else if constexpr (requires { k.beatmapCharacteristic; }) fillChar(k.beatmapCharacteristic);
}
template <class V> static bool ReadKeyT(V* v, std::string& levelId, int& diff, std::string& chara) {
    if constexpr (requires { v->get_beatmapKey(); }) { FillKey(v->get_beatmapKey(), levelId, diff, chara); return true; }
    else if constexpr (requires { v->____beatmapKey; }) { FillKey(v->____beatmapKey, levelId, diff, chara); return true; }
    else return false;
}
static bool ReadKey(Il2CppObject* viewObj, std::string& levelId, int& diff, std::string& chara) {
    return ReadKeyT(reinterpret_cast<GlobalNamespace::StandardLevelDetailView*>(viewObj), levelId, diff, chara);
}

static void OnMapSelected(Il2CppObject* view) {
    std::string levelId, chara; int diff = 4;
    S.mapStr.clear(); S.pools.clear(); S.poolIdx = 0; S.rankLine.clear(); S.board.clear();
    panelOpen = false;
    S.custom = false;
    int ver = ++S.version;
    if (!ReadKey(view, levelId, diff, chara)) { LOG("could not read BeatmapKey"); Draw(); return; }
    if (levelId.rfind("custom_level_", 0) != 0 || chara.empty()) { Draw(); return; }
    S.custom = true; S.hash = levelId.substr(13, 40); S.chara = chara; S.diffIdx = (diff >= 0 && diff < 5) ? diff : 4;
    S.mapStr = levelId.substr(13, 40) + "%7C_" + DiffName(diff) + "_Solo" + chara;
    S.status.clear(); Draw();
    Http(cfg.url + "/api/leaderboard/" + S.mapStr + "/info", [ver](long code, std::string body) {
        if (ver != S.version) return;
        rapidjson::Document d;
        if (ParseJson(body, d) && d.IsObject() && !Mem(d, "error")) {
            auto sr = Mem(d, "star_rating");
            if (sr && sr->IsObject())
                for (auto& m : sr->GetObject())
                    if (m.value.IsNumber()) S.pools.emplace_back(m.name.GetString(), (float)m.value.GetDouble());
        }
        S.lastPlayedRanked = !S.pools.empty();
        LOG("map info: http {}, {} pools", code, (int)S.pools.size());
        if (!S.pools.empty() && cfg.autoOpen && wantHitbloq) panelOpen = true;
        Draw();
        if (S.pools.empty()) { LoadBoard(ver); return; } // not on Hitbloq: shows the message, or BeatLeader/ScoreSaber scores
        if (S.source != 0) LoadBoard(ver);
        LoadForCurrentMap();
    });
}

// ------------------------------------------------------------------ hooks (resolved by name at runtime)
MAKE_HOOK(RefreshContentHook, nullptr, void, Il2CppObject* self) {
    RefreshContentHook(self);
    if (!self) return;
    LOG("level detail refreshed");
    detailView = reinterpret_cast<UnityEngine::MonoBehaviour*>(self);
    BuildUI();
    RefreshPose();
    PlaceScreens();
    OnMapSelected(self);
}

MAKE_HOOK(ResultsDidActivateHook, nullptr, void, Il2CppObject* self, bool a, bool b, bool c) {
    ResultsDidActivateHook(self, a, b, c);
    if (!self || std::string(self->klass->name) != "ResultsViewController") return; // method may resolve to the base class
    auto res = FieldObj(self, "_levelCompletionResults");
    // LevelEndStateType: None = 0, Cleared = 1, Failed = 2
    if (res && FieldInt(res, "levelEndStateType", 0) == 1 && S.lastPlayedRanked && S.registered) {
        // give BeatLeader/ScoreSaber a moment to finish their upload, then ask Hitbloq to pull it
        BSML::MainThreadScheduler::ScheduleAfterTime(3.0f, []() { Refresh(); });
    }
}

// grab the platform (Meta) user id so we know which BeatLeader/ScoreSaber account this is
MAKE_HOOK(GetUserInfoHook, nullptr, Il2CppObject*, Il2CppObject* self, void* token) {
    auto task = GetUserInfoHook(self, token);
    if (cfg.playerId.empty() && S.platformId.empty() && task) {
        auto handle = il2cpp_functions::gchandle_new(task, false);
        PollUntil(
            [handle]() {
                auto t = reinterpret_cast<Il2CppObject*>(il2cpp_functions::gchandle_get_target(handle));
                return !t || UnboxBool(Invoke(t, "get_IsCompleted"));
            },
            [handle]() {
                auto t = reinterpret_cast<Il2CppObject*>(il2cpp_functions::gchandle_get_target(handle));
                if (t) {
                    auto info = Invoke(t, "get_Result");
                    auto id = StringMember(info, "platformUserId");
                    if (!id.empty() && S.platformId.empty()) {
                        S.platformId = id; LOG("platform user id: {}", id);
                        S.status.clear(); Draw();
                        LoadForCurrentMap(); // a map may already be selected and waiting for the id
                    }
                }
                il2cpp_functions::gchandle_free(handle);
            });
    }
    return task;
}

static bool HookByName(const char* klass, const char* method, int argc, const char* label, void (*install)(void*)) {
    auto k = il2cpp_utils::GetClassFromName("", klass);
    if (!k) { LOG("class {} not found, {} disabled", klass, label); return false; }
    auto m = il2cpp_functions::class_get_method_from_name(k, method, argc);
    if (!m || !m->methodPointer) { LOG("{}.{} not found, {} disabled", klass, method, label); return false; }
    install(reinterpret_cast<void*>(m->methodPointer));
    return true;
}

// ------------------------------------------------------------------ entry points
extern "C" __attribute__((visibility("default"))) void setup(CModInfo* info) noexcept {
    *info = modInfo.to_c();
    Paper::Logger::RegisterFileContextId("Hitbloq");
}

extern "C" __attribute__((visibility("default"))) void late_load() noexcept {
    LOG("late_load start (v" VERSION ")");
    il2cpp_functions::Init();
    BSML::Init();
    LoadConfig();
    LOG("config loaded, player id '{}'", cfg.playerId);
    wantHitbloq = cfg.autoOpen;
    if (!cfg.playerId.empty()) S.platformId = cfg.playerId;
    BSML::Register::RegisterMainMenuViewControllerMethod("Hitbloq", "Hitbloq", "Hitbloq map pools and playlists", PlaylistPageDidActivate);
    LOG("Mods menu entry registered");

    HookByName("StandardLevelDetailView", "RefreshContent", 0, "map panel",
               [](void* p) { INSTALL_HOOK_DIRECT(Logger, RefreshContentHook, p); });
    HookByName("ResultsViewController", "DidActivate", 3, "auto refresh",
               [](void* p) { INSTALL_HOOK_DIRECT(Logger, ResultsDidActivateHook, p); });
    bool idHooked = false;
    for (auto cls : {"OculusPlatformUserModel", "PlatformUserModel"})
        for (int argc : {1, 0})
            if (!idHooked)
                idHooked = HookByName(cls, "GetUserInfo", argc, "id detection",
                                      [](void* p) { INSTALL_HOOK_DIRECT(Logger, GetUserInfoHook, p); });
    if (!idHooked) LOG("player id auto-detect unavailable: set playerId in Hitbloq.json");
    LOG("Hitbloq loaded (v" VERSION ")");
}
