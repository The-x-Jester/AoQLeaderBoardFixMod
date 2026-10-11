/*
 * AoQLeaderboardRedirector — standalone Attack on Quest 0.5.0 native mod
 *
 * Redirects the game's existing LeaderboardMongoDB.UpdatePlayer score upload
 * to a user-hosted Cloudflare Worker. The game's own MessageDatabase_CR
 * coroutine is retained, so UnityWebRequest continues to handle HTTPS.
 *
 * BEFORE BUILDING:
 *   1) Deploy the Worker from the aoq-leaderboard-worker folder.
 *   2) API_BASE_URL is set to the currently deployed Worker URL.
 *   3) Set LEADERBOARD_ID to an allow-listed board in the Worker if needed.
 *
 * Notes:
 *   - This uploads the score passed to LeaderboardMongoDB.UpdatePlayer.
 *   - The Worker upserts the best total for the selected board.
 *   - If the retired API did not fill LeaderboardMongoDB._id, this mod uses a
 *     deterministic nickname hash as a fallback player ID. That fallback is
 *     stable for the same exact nickname but is not account authentication.
 *   - Redirects legacy AddPlayer/ViewPlayer profile calls so the game receives permanent unlock flags.
 *   - Redirects Regiments.DisplayTopTen reads to the Worker wall-compatibility
 *     route, which returns the raw JSON array expected by the original callback.
 *   - Set WALL_PERIOD to "monthly" to show monthly top-ten scores on the same
 *     three regiment panels (then rebuild). Default is lifetime totals.
 */

#include <android/log.h>
#include <dlfcn.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <time.h>

#include "../../AoQ-ModLoader-For-Quest/shared/inline-hook/inlineHook.h"
#include "../../AoQ-ModLoader-For-Quest/shared/utils/utils.h"
#include "../../AoQ-ModLoader-For-Quest/shared/modapi/modapi.h"
#include "../../AoQ-ModLoader-For-Quest/shared/aoqcore/aoq.h"

#define MOD_TAG "AoQLeaderboard"
#define MOD_SO  "libleaderboardredirect.so"
#define MOD_LOGI(...) __android_log_print(ANDROID_LOG_INFO, MOD_TAG, __VA_ARGS__)
#define MOD_LOGW(...) __android_log_print(ANDROID_LOG_WARN, MOD_TAG, __VA_ARGS__)

/* Edit this after deploying your Worker. Do not include a trailing slash. */
#define API_BASE_URL "https://aoq-leaderboard-api.aoq-leaderboards-quest-1011.workers.dev"
/* Must exactly match an ID in BOARDS inside aoq-leaderboard-worker/src/index.js. */
#define LEADERBOARD_ID "titan-kills"
/* "lifetime" keeps the existing wall behavior; use "monthly" for monthly kills. */
#define WALL_PERIOD "monthly"

/* AoQ 0.5.0 RVAs from the supplied dump.cs. */
#define RVA_LeaderboardMongoDB_UpdatePlayer       0x54CA9C
#define RVA_LeaderboardMongoDB_MessageDatabase_CR 0x54C9F0
#define RVA_Regiments_Start                         0x49FFD0
#define RVA_Regiments_DisplayTopTen                  0x4A0234
#define RVA_PhotonNetwork_get_LocalPlayer         0x9A6640
#define RVA_Photon_Player_get_NickName            0xA62818

/* LeaderboardMongoDB._id is the first instance field in dump.cs. */
#define OFF_LeaderboardMongoDB_id 0x0C

static AoqCfgCache g_cfg = {0};
static int g_enabled = 1;
static int g_warned_unconfigured = 0;

/* Set while the game's original UpdatePlayer method builds its coroutine. */
static int g_inside_update = 0;
static int g_upload_score = 0;
static char g_upload_id[160];
static char g_upload_name[160];
static char g_upload_regiment[160];

/* Upload throttling: send at most once per 60 seconds unless the lifetime
 * score advanced by at least 100 kills. Identical scores are never uploaded. */
#define UPLOAD_COOLDOWN_MS 60000LL
#define UPLOAD_SCORE_DELTA 100
static long long g_last_upload_ms = 0;
static int g_last_upload_score = -1;

/* IL2CPP functions. String creation is looked up lazily after libil2cpp loads. */
typedef void *(*il2cpp_string_new_t)(const char *utf8);
typedef void *(*photon_get_local_player_t)(void *method_info);
typedef void *(*player_get_nickname_t)(void *player, void *method_info);

static il2cpp_string_new_t g_string_new = NULL;
static photon_get_local_player_t g_get_local_player = NULL;
static player_get_nickname_t g_get_nickname = NULL;
static void *g_il2cpp_handle = NULL;

static const char *DEFAULT_CONFIG =
    "{\n  \"entries\": [\n"
    "    {\"key\":\"Enabled\",\"type\":\"bool\",\"value\":true,"
    "\"description\":\"Redirect AoQ score uploads to your configured leaderboard API.\"}\n"
    "  ]\n}\n";

static void refresh_config(void)
{
    if (aoq_cfg_refresh(&g_cfg, MOD_SO) == 0)
        g_enabled = aoq_cfg_bool(&g_cfg, "Enabled", 1);
}

static int api_is_configured(void)
{
    return API_BASE_URL[0] != '\0' &&
           strstr(API_BASE_URL, "REPLACE-WITH-YOUR-WORKER") == NULL &&
           strncmp(API_BASE_URL, "https://", 8) == 0;
}

static void resolve_il2cpp(void)
{
    if (!g_il2cpp_handle) {
#ifdef RTLD_NOLOAD
        g_il2cpp_handle = dlopen("libil2cpp.so", RTLD_NOW | RTLD_NOLOAD);
#endif
        if (!g_il2cpp_handle)
            g_il2cpp_handle = dlopen("libil2cpp.so", RTLD_NOW);
    }

    if (g_il2cpp_handle && !g_string_new)
        g_string_new = (il2cpp_string_new_t)dlsym(g_il2cpp_handle, "il2cpp_string_new");

    if (!g_get_local_player)
        g_get_local_player = (photon_get_local_player_t)
            getRealOffset(RVA_PhotonNetwork_get_LocalPlayer);
    if (!g_get_nickname)
        g_get_nickname = (player_get_nickname_t)
            getRealOffset(RVA_Photon_Player_get_NickName);
}

/* Append one Unicode scalar as UTF-8. Returns 0 on output overflow. */
static int append_utf8(uint32_t cp, char *out, size_t cap, size_t *used)
{
    unsigned char bytes[4];
    size_t count;
    if (cp <= 0x7Fu) {
        bytes[0] = (unsigned char)cp; count = 1;
    } else if (cp <= 0x7FFu) {
        bytes[0] = (unsigned char)(0xC0u | (cp >> 6));
        bytes[1] = (unsigned char)(0x80u | (cp & 0x3Fu)); count = 2;
    } else if (cp <= 0xFFFFu) {
        bytes[0] = (unsigned char)(0xE0u | (cp >> 12));
        bytes[1] = (unsigned char)(0x80u | ((cp >> 6) & 0x3Fu));
        bytes[2] = (unsigned char)(0x80u | (cp & 0x3Fu)); count = 3;
    } else if (cp <= 0x10FFFFu) {
        bytes[0] = (unsigned char)(0xF0u | (cp >> 18));
        bytes[1] = (unsigned char)(0x80u | ((cp >> 12) & 0x3Fu));
        bytes[2] = (unsigned char)(0x80u | ((cp >> 6) & 0x3Fu));
        bytes[3] = (unsigned char)(0x80u | (cp & 0x3Fu)); count = 4;
    } else {
        bytes[0] = '?'; count = 1;
    }

    if (*used + count >= cap) return 0;
    for (size_t i = 0; i < count; ++i) out[(*used)++] = (char)bytes[i];
    return 1;
}

/* Unity/IL2CPP String on ARMv7: length at +0x8 and UTF-16 chars at +0xC. */
static int cs_to_utf8(void *cs, char *out, size_t cap)
{
    if (!out || cap == 0) return 0;
    out[0] = '\0';
    if (!cs || cap < 2) return 0;

    int32_t len = *(int32_t *)((char *)cs + 0x8);
    const uint16_t *ch = (const uint16_t *)((char *)cs + 0xC);
    if (len < 0 || len > 2048) return 0;

    size_t used = 0;
    for (int32_t i = 0; i < len; ++i) {
        uint32_t cp = ch[i];
        if (cp >= 0xD800u && cp <= 0xDBFFu && i + 1 < len) {
            uint32_t lo = ch[i + 1];
            if (lo >= 0xDC00u && lo <= 0xDFFFu) {
                cp = 0x10000u + ((cp - 0xD800u) << 10) + (lo - 0xDC00u);
                ++i;
            } else cp = '?';
        } else if (cp >= 0xDC00u && cp <= 0xDFFFu) {
            cp = '?';
        }
        if (!append_utf8(cp, out, cap, &used)) break;
    }
    out[used] = '\0';
    return 1;
}

static int usable_id(const char *id)
{
    if (!id || !id[0]) return 0;
    if (!strcmp(id, "null") || !strcmp(id, "undefined") || !strcmp(id, "0")) return 0;
    return 1;
}

/* Stable 64-bit nickname-derived fallback when the old service never issued _id. */
static void nickname_fallback_id(const char *name, char *out, size_t cap)
{
    uint64_t h = UINT64_C(14695981039346656037);
    const unsigned char *p = (const unsigned char *)name;
    while (p && *p) {
        h ^= (uint64_t)*p++;
        h *= UINT64_C(1099511628211);
    }
    snprintf(out, cap, "nick-%016llx", (unsigned long long)h);
}

static void url_encode(const char *src, char *dst, size_t cap)
{
    static const char hex[] = "0123456789ABCDEF";
    size_t used = 0;
    if (!cap) return;
    dst[0] = '\0';
    if (!src) return;

    for (const unsigned char *p = (const unsigned char *)src; *p; ++p) {
        unsigned char c = *p;
        int safe = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
                   (c >= '0' && c <= '9') || c == '-' || c == '_' || c == '.' || c == '~';
        size_t need = safe ? 1u : 3u;
        if (used + need >= cap) break;
        if (safe) dst[used++] = (char)c;
        else {
            dst[used++] = '%';
            dst[used++] = hex[(c >> 4) & 0x0F];
            dst[used++] = hex[c & 0x0F];
        }
    }
    dst[used] = '\0';
}

static long long monotonic_ms(void)
{
    struct timespec ts;
    if (clock_gettime(CLOCK_MONOTONIC, &ts) != 0) return 0;
    return (long long)ts.tv_sec * 1000LL + (long long)ts.tv_nsec / 1000000LL;
}

static int should_upload_score(int score)
{
    long long now = monotonic_ms();
    if (g_last_upload_score < 0) {
        g_last_upload_ms = now;
        g_last_upload_score = score;
        return 1;
    }
    if (score == g_last_upload_score) return 0;

    long long elapsed = now > g_last_upload_ms ? now - g_last_upload_ms : 0;
    int delta = score > g_last_upload_score ? score - g_last_upload_score : 0;
    if (elapsed >= UPLOAD_COOLDOWN_MS || delta >= UPLOAD_SCORE_DELTA) {
        g_last_upload_ms = now;
        g_last_upload_score = score;
        return 1;
    }
    return 0;
}

static int prepare_upload_identity(void *leaderboard_obj, int total_kills, void *regiment_arg)
{
    char id[160] = {0};
    char name[160] = {0};
    char regiment[160] = {0};

    if (leaderboard_obj) {
        void *managed_id = AOQ_FIELD(leaderboard_obj, OFF_LeaderboardMongoDB_id, void *);
        (void)cs_to_utf8(managed_id, id, sizeof(id));
    }

    /* UpdatePlayer(int totalKills, string regiment = ""): this argument is
     * an IL2CPP System.String. Empty/null means the original call omitted it. */
    if (regiment_arg) (void)cs_to_utf8(regiment_arg, regiment, sizeof(regiment));

    resolve_il2cpp();
    if (g_get_local_player && g_get_nickname) {
        void *local_player = g_get_local_player(NULL); /* static PhotonNetwork.get_LocalPlayer */
        if (local_player) {
            void *managed_name = g_get_nickname(local_player, NULL);
            (void)cs_to_utf8(managed_name, name, sizeof(name));
        }
    }

    if (!name[0]) {
        /* Avoid uploading all un-named players into a shared "Unknown" record. */
        MOD_LOGW("No local Photon nickname available; leaderboard upload will be skipped this call");
        return 0;
    }
    if (!usable_id(id)) nickname_fallback_id(name, id, sizeof(id));

    snprintf(g_upload_id, sizeof(g_upload_id), "%s", id);
    snprintf(g_upload_name, sizeof(g_upload_name), "%s", name);
    snprintf(g_upload_regiment, sizeof(g_upload_regiment), "%s", regiment);
    g_upload_score = total_kills < 0 ? 0 : total_kills;
    return 1;
}

static void *make_managed_upload_url(void)
{
    if (!g_string_new || !api_is_configured()) return NULL;

    char enc_id[512], enc_name[512], enc_board[128], enc_regiment[512], url[3072];
    url_encode(g_upload_id, enc_id, sizeof(enc_id));
    url_encode(g_upload_name, enc_name, sizeof(enc_name));
    url_encode(LEADERBOARD_ID, enc_board, sizeof(enc_board));
    url_encode(g_upload_regiment, enc_regiment, sizeof(enc_regiment));

    /* Match the original C# signature: only add regiment when non-empty. */
    int n;
    if (g_upload_regiment[0]) {
        n = snprintf(url, sizeof(url),
            "%s/UpdatePlayer?id=%s&name=%s&regiment=%s&total=%d&board=%s",
            API_BASE_URL, enc_id, enc_name, enc_regiment, g_upload_score, enc_board);
    } else {
        n = snprintf(url, sizeof(url),
            "%s/UpdatePlayer?id=%s&name=%s&total=%d&board=%s",
            API_BASE_URL, enc_id, enc_name, g_upload_score, enc_board);
    }
    if (n < 0 || (size_t)n >= sizeof(url)) {
        MOD_LOGW("Constructed upload URL was too long; keeping original URL");
        return NULL;
    }
    return g_string_new(url);
}

static void *make_managed_wall_url(int function, const char **regiment_out)
{
    resolve_il2cpp();
    if (!g_string_new || !api_is_configured()) return NULL;

    const char *regiment = NULL;
    switch (function) {
        case 1: regiment = "Scout"; break;
        case 2: regiment = "Military Police"; break;
        case 3: regiment = "Garrison"; break;
        default: return NULL;
    }

    char enc_board[128], enc_regiment[256], url[2048];
    url_encode(LEADERBOARD_ID, enc_board, sizeof(enc_board));
    url_encode(regiment, enc_regiment, sizeof(enc_regiment));
    int n = snprintf(url, sizeof(url),
        "%s/api/v1/leaderboards/%s/wall?limit=10&period=%s&regiment=%s",
        API_BASE_URL, enc_board, WALL_PERIOD, enc_regiment);
    if (n < 0 || (size_t)n >= sizeof(url)) {
        MOD_LOGW("Constructed wall-menu URL was too long; keeping original URL");
        return NULL;
    }
    if (regiment_out) *regiment_out = regiment;
    return g_string_new(url);
}

static int managed_url_contains_update_player(void *uri)
{
    char url[2048];
    if (!cs_to_utf8(uri, url, sizeof(url))) return 0;
    return strstr(url, "/endpoint/UpdatePlayer") != NULL ||
           strstr(url, "/UpdatePlayer") != NULL;
}

/* Rebuild /ViewPlayer and /AddPlayer URLs against our Worker, preserving query string. */
static void *make_managed_profile_url(void *uri, const char **route_out)
{
    if (!g_string_new || !api_is_configured()) return NULL;
    char old_url[2048], new_url[3072];
    if (!cs_to_utf8(uri, old_url, sizeof(old_url))) return NULL;
    const char *route = strstr(old_url, "/ViewPlayer");
    if (!route) route = strstr(old_url, "/AddPlayer");
    if (!route) return NULL;
    int n = snprintf(new_url, sizeof(new_url), "%s%s", API_BASE_URL, route);
    if (n < 0 || (size_t)n >= sizeof(new_url)) return NULL;
    if (route_out) *route_out = route[1] == 'V' ? "ViewPlayer" : "AddPlayer";
    return g_string_new(new_url);
}

/* Capture the actual score/name/id while keeping the game's original method behavior. */
MAKE_HOOK(LeaderboardMongoDB_UpdatePlayer,
          RVA_LeaderboardMongoDB_UpdatePlayer,
          void, void *self, int totalKills, void *regiment)
{
    if (!g_enabled || !self) {
        LeaderboardMongoDB_UpdatePlayer(self, totalKills, regiment);
        return;
    }

    int old_inside = g_inside_update;
    int prepared = prepare_upload_identity(self, totalKills, regiment);
    if (!prepared) {
        g_inside_update = 0;
        LeaderboardMongoDB_UpdatePlayer(self, totalKills, regiment);
        g_inside_update = old_inside;
        return;
    }

    if (!should_upload_score(g_upload_score)) {
        /* UpdatePlayer's purpose here is to schedule the legacy score request.
         * Skip that request entirely; the next score update retries the latest
         * cumulative total after the cooldown/threshold is reached. */
        g_inside_update = 0;
        return;
    }

    g_inside_update = 1;
    LeaderboardMongoDB_UpdatePlayer(self, totalKills, regiment);
    g_inside_update = old_inside;
}

/* Rewrite only the score-upload request; preserve Unity's original coroutine and callback. */
MAKE_HOOK(LeaderboardMongoDB_MessageDatabase_CR,
          RVA_LeaderboardMongoDB_MessageDatabase_CR,
          void *, void *self, void *uri, int function, void *callback)
{
    if (g_enabled && api_is_configured()) {
        resolve_il2cpp();
        if (g_inside_update && managed_url_contains_update_player(uri)) {
            void *new_uri = make_managed_upload_url();
            if (new_uri) {
                MOD_LOGI("Redirecting score upload: board=%s score=%d player=%s regiment=%s",
                         LEADERBOARD_ID, g_upload_score, g_upload_name,
                         g_upload_regiment[0] ? g_upload_regiment : "(none)");
                uri = new_uri;
            } else MOD_LOGW("Could not create managed upload URL; keeping original URL");
        } else {
            const char *route = NULL;
            void *new_uri = make_managed_profile_url(uri, &route);
            if (new_uri) {
                MOD_LOGI("Redirecting game profile request: %s", route ? route : "profile");
                uri = new_uri;
            }
        }
    }

    return LeaderboardMongoDB_MessageDatabase_CR(self, uri, function, callback);
}

/* Redirect the Regiment wall-menu query URL while preserving the game's
 * own coroutine and DisplayTopTen callback. function selects the text fields:
 * 1=Scouts, 2=Military Police, 3=Garrison. Worker returns a bare array of
 * {name,kills}, exactly what the original JSON.Parse(value).Values loop reads. */
MAKE_HOOK(Regiments_DisplayTopTen,
          RVA_Regiments_DisplayTopTen,
          void, void *self, int function, void *uri)
{
    if (g_enabled && api_is_configured()) {
        const char *regiment = NULL;
        void *new_uri = make_managed_wall_url(function, &regiment);
        if (new_uri) {
            MOD_LOGI("Redirecting wall-menu read: period=%s function=%d regiment=%s",
                     WALL_PERIOD, function, regiment ? regiment : "(unknown)");
            uri = new_uri;
        }
    }
    Regiments_DisplayTopTen(self, function, uri);
}

/* Load all three monthly top-ten panels as soon as the Regiments component
 * starts, rather than waiting for the wall menu to be opened. */
MAKE_HOOK(Regiments_Start,
          RVA_Regiments_Start,
          void, void *self)
{
    /* The game's original Regiments.Start already requests the three panels.
     * Do not issue another set of three duplicate Worker reads here. */
    Regiments_Start(self);
}

__attribute__((constructor)) void lib_main(void)
{
    MOD_LOGI("loading standalone leaderboard redirector...");
    aoqmm_register(MOD_SO, "AoQ Leaderboard Redirector", "1.3.0", "OpenAI",
                   "Throttles score uploads and redirects AoQ leaderboards/reward profile requests to a Worker/D1 backend.");
    aoqmm_ensure_config(MOD_SO, DEFAULT_CONFIG);
    aoq_init();
    refresh_config();

    resolve_il2cpp();
    INSTALL_HOOK(LeaderboardMongoDB_UpdatePlayer);
    INSTALL_HOOK(LeaderboardMongoDB_MessageDatabase_CR);
    INSTALL_HOOK(Regiments_DisplayTopTen);
    INSTALL_HOOK(Regiments_Start);

    MOD_LOGI("hooks installed; Enabled=%d Board=%s WallPeriod=%s UploadCooldown=60s ScoreDelta=100 AutoLoad=0",
             g_enabled, LEADERBOARD_ID, WALL_PERIOD);
    if (!api_is_configured())
        MOD_LOGW("API_BASE_URL is still a placeholder; edit it after deploying the backend");
}
