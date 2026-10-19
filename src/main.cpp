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
        }
    }
    while (!cfg.url.empty() && cfg.url.back() == '/') cfg.url.pop_back();
    std::string out = "{\n  \"playerId\": \"" + cfg.playerId + "\",\n  \"accountType\": \"" + cfg.accountType +
                      "\",\n  \"url\": \"" + cfg.url + "\"\n}\n";
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

static void Draw() {
    if (!headerText || !rankText || !statusText || !boardText) return;
    std::string h = "<b><color=#ffd42a>Hitbloq</color></b>";
    if (!S.pools.empty()) h += "   pool: <color=#aa6eff>" + CurPool() + "</color> (" + Fmt(S.pools[S.poolIdx].second) + " stars)";
    else h += "   <color=#aaaaaa>map not ranked on any pool</color>";
    headerText->set_text(SW(h));
    rankText->set_text(SW(S.rankLine));
    statusText->set_text(SW(S.status));
    boardText->set_text(SW(S.board));
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

static void LoadBoard(int ver) {
    if (S.mapStr.empty() || S.pools.empty()) { S.board.clear(); Draw(); return; }
    std::string url;
    if (S.around) {
        if (!S.registered) { S.board = "<color=#aaaaaa>Not registered yet.</color>"; Draw(); return; }
        url = cfg.url + "/api/leaderboard/" + S.mapStr + "/nearby_scores_extended/" + std::to_string(S.hbId);
    } else url = cfg.url + "/api/leaderboard/" + S.mapStr + "/scores_extended/0";
    S.board = "<color=#aaaaaa>Loading...</color>"; Draw();
    std::string pool = CurPool();
    Http(url, [ver, pool](long, std::string body) {
        if (ver != S.version) return;
        rapidjson::Document d;
        std::string out;
        if (ParseJson(body, d) && d.IsArray()) {
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
        S.board = out.empty() ? "<color=#aaaaaa>No scores yet.</color>" : out;
        Draw();
    });
}

static void LoadForCurrentMap() {
    if (S.pools.empty()) return;
    int ver = S.version;
    auto go = [ver]() { if (ver == S.version) { LoadRank(ver); LoadBoard(ver); } };
    if (S.registered) go();
    else if (!S.registering) CheckRegistered(go);
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
static void BuildUI() {
    if (screen) return;
    int gen = ++uiGen;
    screen = BSML::Lite::CreateFloatingScreen({96.0f, 66.0f}, {0.0f, 1.6f, 3.3f}, {0.0f, 0.0f, 0.0f}, 0.0f, true);
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
    headerText = mk(28.0f, 8.0f, 4.0f, 514);
    rankText   = mk(21.0f, 6.0f, 3.2f, 514);
    boardText  = mk(-1.0f, 40.0f, 2.9f, 257);
    statusText = mk(-22.0f, 6.0f, 3.0f, 514);
    BSML::Lite::CreateUIButton(t, "Pool", {-30.0f, -29.0f}, {24.0f, 8.0f}, []() {
        if (S.pools.size() < 2) return;
        S.poolIdx = (S.poolIdx + 1) % (int)S.pools.size();
        Draw(); ReloadAll();
    });
    BSML::Lite::CreateUIButton(t, "Global / Around", {0.0f, -29.0f}, {30.0f, 8.0f}, []() {
        S.around = !S.around;
        LoadBoard(++S.version);
    });
    BSML::Lite::CreateUIButton(t, "Refresh", {30.0f, -29.0f}, {24.0f, 8.0f}, []() { Refresh(); });

    // show the panel only while the level detail view is on screen
    auto poll = std::make_shared<std::function<void()>>();
    *poll = [poll, gen]() {
        if (!screen || gen != uiGen) return;
        bool vis = detailView && detailView->get_gameObject()->get_activeInHierarchy();
        screen->get_gameObject()->SetActive(vis);
        BSML::MainThreadScheduler::ScheduleAfterTime(0.4f, *poll);
    };
    (*poll)();
}

// ------------------------------------------------------------------ map selection
static const char* DiffName(int d) {
    static const char* n[] = {"Easy", "Normal", "Hard", "Expert", "ExpertPlus"};
    return (d >= 0 && d < 5) ? n[d] : "ExpertPlus";
}

// BeatmapKey member names differ between codegen versions ("levelId" vs "___levelId"); accept both.
template <class K> static void FillKey(const K& k, std::string& levelId, int& diff, std::string& chara) {
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
    int ver = ++S.version;
    if (!ReadKey(view, levelId, diff, chara)) { LOG("could not read BeatmapKey"); Draw(); return; }
    if (levelId.rfind("custom_level_", 0) != 0 || chara.empty()) { Draw(); return; }
    S.mapStr = levelId.substr(13, 40) + "%7C_" + DiffName(diff) + "_Solo" + chara;
    S.status.clear(); Draw();
    Http(cfg.url + "/api/leaderboard/" + S.mapStr + "/info", [ver](long, std::string body) {
        if (ver != S.version) return;
        rapidjson::Document d;
        if (ParseJson(body, d) && d.IsObject() && !Mem(d, "error")) {
            auto sr = Mem(d, "star_rating");
            if (sr && sr->IsObject())
                for (auto& m : sr->GetObject())
                    if (m.value.IsNumber()) S.pools.emplace_back(m.name.GetString(), (float)m.value.GetDouble());
        }
        S.lastPlayedRanked = !S.pools.empty();
        Draw();
        if (S.pools.empty()) return;
        LoadForCurrentMap();
    });
}

// ------------------------------------------------------------------ hooks (resolved by name at runtime)
MAKE_HOOK(RefreshContentHook, nullptr, void, Il2CppObject* self) {
    RefreshContentHook(self);
    if (!self) return;
    detailView = reinterpret_cast<UnityEngine::MonoBehaviour*>(self);
    BuildUI();
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
    il2cpp_functions::Init();
    BSML::Init();
    LoadConfig();
    if (!cfg.playerId.empty()) S.platformId = cfg.playerId;

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
    LOG("Hitbloq loaded");
}
