/*
 * libmapbuilder.so - Map Builder for Attack on Quest (0.5.0). Local only: nothing is shared yet.
 * Replaces the Lego controls while it is on: don't run both at once.
 *
 *   Left X  tap    place the selected piece at the flare gun
 *   Left X  hold   place a platform (size and spot from config)
 *   Left Y  tap    undo the last piece you placed
 *   Left Y  hold   CLEAR: hides the map, deletes floors, lights, particles and audio (keeps player, UI, networked objects)
 *   Right A tap    next piece: cube, sphere, cylinder, capsule, then every scene object the probe found
 *   Right A hold   previous piece
 *   Right B tap    copy the material of the scene object nearest the gun onto the last piece
 *   Right B hold   delete the piece nearest the gun (only pieces you placed)
 *   Left stick  tap   next MATERIAL for the last piece   (first choice is the piece's original)
 *   Left stick  hold  next TEXTURE  for the last piece   (first choice is the original)
 *   Right stick tap   next SHADER   for the last piece   (first choice is the original)
 *   Right stick hold  reset the last piece to its original look
 *
 * A transparent outline shows where the piece will go, sized to the piece.
 * The status panel at the top of the view lists what is selected.
 */

#include <math.h>                       /* before the modloader headers: utils.h's log() macro breaks math.h otherwise */
#include <android/log.h>
#include <dlfcn.h>
#include <stdarg.h>
#include <sys/stat.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "../../AoQ-ModLoader-For-Quest/shared/aoqcore/aoq.h"
#include "../../AoQ-ModLoader-For-Quest/shared/inline-hook/inlineHook.h"
#include "../../AoQ-ModLoader-For-Quest/shared/utils/utils.h"
#include "../../AoQ-ModLoader-For-Quest/shared/modapi/modapi.h"
#include "../../AoQ-ModLoader-For-Quest/shared/aoqnet.h"

#define SO          "libmapbuilder.so"
#define LOGI(...)   __android_log_print(ANDROID_LOG_INFO, "MapBuilder", __VA_ARGS__)
#define LOGE(...)   __android_log_print(ANDROID_LOG_ERROR, "MapBuilder", __VA_ARGS__)

#define BTN_ONE   0x00000001
#define BTN_TWO   0x00000002
#define BTN_LSTK  0x00008000            /* PrimaryThumbstick: left stick click (0x100 is the left menu button) */
#define BTN_RSTK  0x00800000            /* SecondaryThumbstick: right stick click */
#define CTRL_L    0x1
#define CTRL_R    0x2
#define HOLD_SECS 0.5f
#define BARREL_FRESH 0.5f
#define MAX_PIECES   512
#define MAX_TEMPL    32                 /* scene objects copied at scene start (templates stay inactive) */
#define MAX_MATS     96
#define MAX_TEXS     64
#define N_PRIM       4
#define N_SHADERS    8
#define SCAN_DELAY   3.0f

/* ── RVAs (dump.cs) ───────────────────────────────────────────────────────── */
#define RVA_GameSetupController_Awake  0x4E5AD4
#define RVA_PlayerManager_Awake        0x42A07C
#define RVA_NetworkFlareGun_Update     0x47B53C
#define RVA_FireFlare_Update           0x4E36F0
#define RVA_OVRManager_Update          0xB07180
#define RVA_OVRInput_Get               0xAF7A08
#define RVA_Time_get_time              0xFD3EE0
#define RVA_FindObjectsOfType_Type     0xCEBFC4
#define RVA_Material_get_mainTexture   0xC78B4C
#define RVA_Material_set_mainTexture   0xC78CFC
#define RVA_Material_get_shader        0xC786F8
#define RVA_Renderer_get_bounds        0xCF76E4
#define RVA_Resources_LoadAll          0xFCCE94   /* Resources.LoadAll(string, Type): the game's prefabs */
#define OFF_NetFlare_barrelEnd         0x14
#define OFF_FireFlare_barrelEnd        0x1C
#define OFF_PM_player                  0x0C

typedef int   (*Get_t)(int btn, int ctrl, void *mi);
typedef float (*Time_t)(void *mi);
typedef void *(*Find_t)(void *type, void *mi);
typedef void *(*GetP_t)(void *self, void *mi);
typedef void  (*SetP_t)(void *self, void *v, void *mi);
typedef struct { AoqVec3 c, e; } Bnd;
typedef Bnd  (*Bounds_t)(void *self, void *mi);
typedef void *(*LoadAll_t)(void *path, void *type, void *mi);
static LoadAll_t fn_loadall;
static GetP_t   fn_get_shader;
static Get_t    fn_Get;
static Time_t   fn_time;
static Find_t   fn_find;
static GetP_t   fn_get_maintex;
static SetP_t   fn_set_maintex;
static Bounds_t fn_bounds;
static float now_s(void) { return fn_time ? fn_time(NULL) : 0.0f; }

/* ── Config ───────────────────────────────────────────────────────────────── */
static AoqCfgCache g_cfg = {0};
static int   cfg_on = 1;
static float cfg_px = 0.0f, cfg_py = -0.5f, cfg_pz = 0.0f;
static float cfg_pw = 40.0f, cfg_pd = 40.0f, cfg_ph = 1.0f;
static float cfg_stud = 0.25f;
static int   cfg_outline = 1;
static int   cfg_test = 0;
static float cfg_tx = 0.0f, cfg_ty = 0.0f, cfg_tz = 0.0f, cfg_tspace = 4.0f;
static int   cfg_tbatch = 8;
static int   cfg_test_net = 0;                   /* also copy the networked prefabs (they may run AI locally) */

static void cfg_read(void)
{
    if (aoq_cfg_refresh(&g_cfg, SO) != 0) return;
    cfg_on      = aoq_cfg_bool(&g_cfg, "Enabled", 1);
    cfg_px      = aoq_cfg_flt(&g_cfg, "PlatformX", 0.0f);
    cfg_py      = aoq_cfg_flt(&g_cfg, "PlatformY", -0.5f);
    cfg_pz      = aoq_cfg_flt(&g_cfg, "PlatformZ", 0.0f);
    cfg_pw      = aoq_cfg_flt(&g_cfg, "PlatformWidth", 40.0f);
    cfg_pd      = aoq_cfg_flt(&g_cfg, "PlatformDepth", 40.0f);
    cfg_ph      = aoq_cfg_flt(&g_cfg, "PlatformHeight", 1.0f);
    cfg_stud    = aoq_cfg_flt(&g_cfg, "Stud Size", 0.25f);
    cfg_outline = aoq_cfg_bool(&g_cfg, "Outline", 1);
    cfg_test    = aoq_cfg_bool(&g_cfg, "Spawn Test", 0);
    cfg_tx      = aoq_cfg_flt(&g_cfg, "TestX", 0.0f);
    cfg_ty      = aoq_cfg_flt(&g_cfg, "TestY", 0.0f);
    cfg_tz      = aoq_cfg_flt(&g_cfg, "TestZ", 60.0f);
    cfg_tspace  = aoq_cfg_flt(&g_cfg, "TestSpacing", 4.0f);
    cfg_tbatch  = aoq_cfg_int(&g_cfg, "TestPerFrame", 8);
    cfg_test_net = aoq_cfg_bool(&g_cfg, "Test Networked Prefabs", 0);
}

static const char *DEFAULT_CONFIG =
    "{\n  \"entries\": [\n"
    "    {\"key\":\"Enabled\",\"type\":\"bool\",\"value\":true,\"description\":\"Turn the map builder on.\"},\n"
    "    {\"key\":\"PlatformX\",\"type\":\"float\",\"value\":0.0,\"description\":\"Platform centre, X.\"},\n"
    "    {\"key\":\"PlatformY\",\"type\":\"float\",\"value\":-0.5,\"description\":\"Platform centre, Y.\"},\n"
    "    {\"key\":\"PlatformZ\",\"type\":\"float\",\"value\":0.0,\"description\":\"Platform centre, Z.\"},\n"
    "    {\"key\":\"PlatformWidth\",\"type\":\"float\",\"value\":40.0,\"description\":\"Platform width (X), metres.\"},\n"
    "    {\"key\":\"PlatformDepth\",\"type\":\"float\",\"value\":40.0,\"description\":\"Platform depth (Z), metres.\"},\n"
    "    {\"key\":\"PlatformHeight\",\"type\":\"float\",\"value\":1.0,\"description\":\"Platform thickness, metres.\"},\n"
    "    {\"key\":\"Stud Size\",\"type\":\"float\",\"value\":0.25,\"description\":\"Size of a primitive: one stud.\"},\n"
    "    {\"key\":\"Outline\",\"type\":\"bool\",\"value\":true,\"description\":\"Show a transparent outline where the piece will go.\"},\n"
    "    {\"key\":\"Spawn Test\",\"type\":\"bool\",\"value\":false,\"description\":\"Spawn every scene object the probe found, in a grid, and write spawn_test.txt. Set false after.\"},\n"
    "    {\"key\":\"TestX\",\"type\":\"float\",\"value\":0.0,\"description\":\"Spawn test grid origin, X.\"},\n"
    "    {\"key\":\"TestY\",\"type\":\"float\",\"value\":0.0,\"description\":\"Spawn test grid origin, Y.\"},\n"
    "    {\"key\":\"TestZ\",\"type\":\"float\",\"value\":60.0,\"description\":\"Spawn test grid origin, Z.\"},\n"
    "    {\"key\":\"TestSpacing\",\"type\":\"float\",\"value\":4.0,\"description\":\"Metres between grid cells.\"},\n"
    "    {\"key\":\"TestPerFrame\",\"type\":\"int\",\"value\":8,\"description\":\"Objects spawned per frame (lower = smoother).\"},\n"
    "    {\"key\":\"Test Networked Prefabs\",\"type\":\"bool\",\"value\":false,\"description\":\"Also copy the networked prefabs (titans, flares, player). They may run their AI locally.\"}"
    "  ]\n}\n";

/* ── Helpers ──────────────────────────────────────────────────────────────── */
static void obj_name(void *obj, char *out, int max)
{
    out[0] = 0;
    if (!aoqnet_valid(obj) || !an_obj_get_name) return;
    void *s = an_obj_get_name(obj, NULL);
    if (!s) return;
    unsigned len = *(unsigned *)((char *)s + 0x8);
    uint16_t *ch = (uint16_t *)((char *)s + 0xC);
    if (len > (unsigned)(max - 1)) len = (unsigned)(max - 1);
    for (unsigned i = 0; i < len; i++) out[i] = (ch[i] >= 32 && ch[i] < 127) ? (char)ch[i] : '?';
    out[len] = 0;
}

static void *g_pm = NULL;

static int under_player(void *go)
{
    void *root = aoq_alive(g_pm) ? AOQ_FIELD(g_pm, OFF_PM_player, void *) : NULL;
    void *cur = go;
    for (int i = 0; i < 12 && aoqnet_valid(cur); i++) { if (cur == root) return 1; cur = aoqnet_parent(cur); }
    return 0;
}

static int name_kept(const char *n)
{
    static const char *keep[] = { "Hand", "Canvas", "HUD", "UI", "Camera", "Player", "Blade", "Rig", "Gun", "Flare", "Menu",
                                  "Text", "OVR", "Armature", "mixamorig", "TitanTransforms", "TitanTargets", "Refills",
                                  "ReloadLevel", "HitLocation", "Damage", "GroundPos", "Trigger", "AbnormalTitan",
                                  "MapPiece", "MapOutline" };
    for (size_t i = 0; i < sizeof(keep) / sizeof(keep[0]); i++) if (strstr(n, keep[i])) return 1;
    return 0;
}

static int is_floor(const char *n)
{
    if (strstr(n, "GroundPos")) return 0;
    return strstr(n, "Floor") != NULL || strstr(n, "Ground") != NULL;
}

/* kept objects: networked, under the player, or named on purpose */
static int protected_go(void *go)
{
    if (!aoqnet_valid(go)) return 1;
    if (aoqnet_valid(aoqnet_view(go))) return 1;
    if (under_player(go)) return 1;
    char n[128];
    obj_name(go, n, sizeof(n));
    return name_kept(n);
}

static void *find_type(const char *ns, const char *name, int *count)
{
    void *type = aoqnet_type_object(ns, name);
    void *arr = (fn_find && type) ? fn_find(type, NULL) : NULL;
    if (count) *count = arr ? *(int *)((char *)arr + 0x0C) : 0;
    return arr;
}

/* ── Pieces the builder placed ────────────────────────────────────────────── */
typedef struct { void *go, *rend; void *orig_mat; AoqVec3 pos; int used; } Piece;
static Piece g_pieces[MAX_PIECES];
static int   g_npieces = 0;

static int is_piece(void *go)
{
    for (int i = 0; i < MAX_PIECES; i++) if (g_pieces[i].used && g_pieces[i].go == go) return 1;
    return 0;
}

static int piece_add(void *go, void *rend, AoqVec3 pos)
{
    if (!aoqnet_valid(go)) return 0;
    for (int i = 0; i < MAX_PIECES; i++) if (!g_pieces[i].used) {
        void *orig = aoqnet_valid(rend) ? an_rend_get_shared(rend, NULL) : NULL;
        g_pieces[i] = (Piece){ go, rend, orig, pos, 1 };
        g_npieces++;
        return i;
    }
    return -1;
}

static void piece_remove(int i)
{
    if (!g_pieces[i].used) return;
    if (aoqnet_valid(g_pieces[i].go)) aoqnet_destroy_local(g_pieces[i].go);
    memset(&g_pieces[i], 0, sizeof(Piece));
    g_npieces--;
}

static int last_piece(void)
{
    for (int i = MAX_PIECES - 1; i >= 0; i--) if (g_pieces[i].used && aoqnet_valid(g_pieces[i].go)) return i;
    return -1;
}

/* ── Scene catalog, built once per scene ──────────────────────────────────── */
typedef struct { char name[64]; void *tmpl; AoqVec3 size; AoqVec3 off; } Templ;   /* off: pivot -> geometry centre */
static Templ g_tmpl[MAX_TEMPL];
static int   g_ntmpl = 0;
static void *g_mats[MAX_MATS];   static int g_nmats = 0;   /* shared materials in the scene */
static void *g_texs[MAX_TEXS];   static int g_ntexs = 0;   /* main textures in the scene */
static int   g_scanned = 0;
static float g_scene_t = -1.0f;

static void add_mat(void *m)
{
    if (!aoqnet_valid(m) || g_nmats >= MAX_MATS) return;
    for (int i = 0; i < g_nmats; i++) if (g_mats[i] == m) return;
    g_mats[g_nmats++] = m;
}

static void add_tex(void *t)
{
    if (!aoqnet_valid(t) || g_ntexs >= MAX_TEXS) return;
    for (int i = 0; i < g_ntexs; i++) if (g_texs[i] == t) return;
    g_texs[g_ntexs++] = t;
}

/* the top-most ancestor of a renderer that is not protected: a house, a tree, a crate ... */
static void *topmost_free(void *go)
{
    void *top = go;
    for (int i = 0; i < 8; i++) {
        void *p = aoqnet_parent(top);
        if (!aoqnet_valid(p) || protected_go(p)) break;
        top = p;
    }
    return top;
}

static void scan_scene(float now)
{
    g_scanned = 1;
    g_nmats = g_ntexs = g_ntmpl = 0;
    int count = 0;
    void *arr = find_type("UnityEngine", "Renderer", &count);
    if (!arr) { LOGE("scan: no renderers"); return; }
    void **vec = (void **)((char *)arr + 0x10);
    for (int i = 0; i < count; i++) {
        void *r = vec[i];
        if (!aoqnet_valid(r)) continue;
        void *go = aoqnet_gameobject(r);
        if (protected_go(go) || is_piece(go)) continue;

        void *mat = an_rend_get_shared(r, NULL);
        add_mat(mat);
        if (aoqnet_valid(mat) && fn_get_maintex) add_tex(fn_get_maintex(mat, NULL));

        if (g_ntmpl >= MAX_TEMPL) continue;
        void *top = topmost_free(go);
        char name[64];
        obj_name(top, name, sizeof(name));
        if (!name[0] || strstr(name, "Clone")) continue;
        int dup = 0;
        for (int k = 0; k < g_ntmpl; k++) if (strcmp(g_tmpl[k].name, name) == 0) { dup = 1; break; }
        if (dup) continue;
        /* measure the original while it is still active: its geometry centre and size */
        Templ t = { 0 };
        snprintf(t.name, sizeof(t.name), "%s", name);
        t.size = (AoqVec3){ cfg_stud, cfg_stud, cfg_stud };
        void *rr = aoqnet_renderer(top);
        AoqVec3 pivot = { 0, 0, 0 };
        if (aoqnet_valid(rr) && fn_bounds && aoqnet_get_pos(top, &pivot)) {
            Bnd b = fn_bounds(rr, NULL);
            t.size = (AoqVec3){ b.e.x * 2, b.e.y * 2, b.e.z * 2 };
            t.off  = (AoqVec3){ pivot.x - b.c.x, pivot.y - b.c.y, pivot.z - b.c.z };
        }
        void *copy = aoqnet_instantiate_local(top);            /* a copy kept inactive: the original may be cleared later */
        if (!aoqnet_valid(copy)) continue;
        aoqnet_set_active(copy, 0);
        t.tmpl = copy;
        g_tmpl[g_ntmpl++] = t;
    }
    LOGI("scan: %d scene objects, %d materials, %d textures (%.1fs)", g_ntmpl, g_nmats, g_ntexs, now);
}

/* ── Choices ──────────────────────────────────────────────────────────────── */
static const char *PRIM_NAME[N_PRIM] = { "cube", "sphere", "cylinder", "capsule" };
static const int   PRIM[N_PRIM]      = { 3, 0, 2, 1 };
static int g_piece = 0;                          /* 0..N_PRIM-1 primitives, then scene objects */
static int g_mat_sel = 0, g_tex_sel = 0, g_shader_sel = 0;   /* 0 = keep the original */
static const char *SHADERS[N_SHADERS] = {
    "Standard", "Mobile/Diffuse", "Mobile/Unlit (Supports Lightmap)", "Custom/ColumnGlow",
    "RyanShader/Rz_SpotLit", "RyanShader/Rz_NoiseDissolve", "Unlit/Color", "Legacy Shaders/Diffuse"
};

static int n_piece_choices(void) { return N_PRIM + g_ntmpl; }

static void piece_label(int i, char *buf, int n)
{
    if (i < N_PRIM) snprintf(buf, n, "%s", PRIM_NAME[i]);
    else snprintf(buf, n, "%s", g_tmpl[i - N_PRIM].name);
}

/* ── Gun ──────────────────────────────────────────────────────────────────── */
typedef struct { void *gun, *barrel; float seen; } Flare;
static Flare g_flare_net, g_flare_loc;

static const Flare *flare(float now)
{
    if (aoqnet_valid(g_flare_net.barrel) && now - g_flare_net.seen < BARREL_FRESH) return &g_flare_net;
    if (aoqnet_valid(g_flare_loc.barrel) && now - g_flare_loc.seen < BARREL_FRESH) return &g_flare_loc;
    return NULL;
}

static int gun_tip(float now, AoqVec3 *tip)
{
    const Flare *f = flare(now);
    AoqVec3 b, g;
    if (!f || !aoqnet_get_pos(f->barrel, &b)) return 0;
    g = b;
    if (aoqnet_valid(f->gun) && an_comp_get_transform && an_tr_get_pos) {
        void *gt = an_comp_get_transform(f->gun, NULL);
        if (gt) g = an_tr_get_pos(gt, NULL);
    }
    float dx = b.x - g.x, dy = b.y - g.y, dz = b.z - g.z, l = sqrtf(dx*dx + dy*dy + dz*dz);
    AoqVec3 d = (l > 1e-4f) ? (AoqVec3){ dx / l, dy / l, dz / l } : (AoqVec3){ 0, 0, 1 };
    *tip = (AoqVec3){ b.x + d.x * 0.6f, b.y + d.y * 0.6f, b.z + d.z * 0.6f };
    return 1;
}

static AoqVec3 snap(AoqVec3 p, AoqVec3 size)
{
    float s = cfg_stud;
    if (s <= 0) return p;
    return (AoqVec3){ roundf(p.x / s) * s, roundf((p.y - size.y * 0.5f) / s) * s + size.y * 0.5f, roundf(p.z / s) * s };
}

/* ── Outline: a transparent box where the next piece goes ─────────────────── */
static void *g_outline = NULL;

static void outline_update(float now)
{
    if (!cfg_outline) { if (aoqnet_valid(g_outline)) aoqnet_set_active(g_outline, 0); return; }
    AoqVec3 tip;
    if (!gun_tip(now, &tip)) { if (aoqnet_valid(g_outline)) aoqnet_set_active(g_outline, 0); return; }
    if (!aoqnet_valid(g_outline)) {
        g_outline = aoqnet_create_primitive(3);
        if (!aoqnet_valid(g_outline)) return;
        aoqnet_set_name(g_outline, "MapOutline");
        aoqnet_collider_enabled(aoqnet_collider(g_outline), 0);
        void *m = aoqnet_material(aoqnet_renderer(g_outline));
        if (aoqnet_valid(m)) {
            aoqnet_material_shader(m, "Sprites/Default");
            aoqnet_material_color(m, (AoqnetColor){ 0.35f, 0.9f, 1.0f, 0.30f });
        }
    }
    aoqnet_set_active(g_outline, 1);
    AoqVec3 size = { cfg_stud, cfg_stud, cfg_stud };
    int p = g_piece;
    if (p >= N_PRIM && p - N_PRIM < g_ntmpl) size = g_tmpl[p - N_PRIM].size;
    AoqVec3 c = snap(tip, size);
    aoqnet_set_scale_local(g_outline, size);
    aoqnet_move(g_outline, c);
}

/* ── Status panel: what is selected, readable at a glance ─────────────────── */
static void *g_hud = NULL;
static char  g_panel[400] = "";
static int   g_panel_dirty = 1;
static char  g_note[160] = "";
static float g_note_until = 0.0f;

static void note(float now, const char *fmt, ...)
{
    va_list ap; va_start(ap, fmt); vsnprintf(g_note, sizeof(g_note), fmt, ap); va_end(ap);
    LOGI("%s", g_note);
    g_note_until = now + 3.0f;
    g_panel_dirty = 1;
}

static void panel_refresh(float now)
{
    char piece[96] = "", mat[96] = "original", tex[96] = "original", shd[96] = "original";
    piece_label(g_piece, piece, sizeof(piece));
    if (g_mat_sel > 0 && g_mat_sel <= g_nmats) snprintf(mat, sizeof(mat), "%.60s", "scene material");
    if (g_tex_sel > 0 && g_tex_sel <= g_ntexs) snprintf(tex, sizeof(tex), "%.60s", "scene texture");
    if (g_shader_sel > 0 && g_shader_sel <= N_SHADERS) snprintf(shd, sizeof(shd), "%.60s", SHADERS[g_shader_sel - 1]);
    int last = last_piece();
    char buf[400];
    snprintf(buf, sizeof(buf),
             "PIECE:    %s   (%d/%d)\n"
             "MATERIAL: %s   [left stick tap]\n"
             "TEXTURE:  %s   [left stick hold]\n"
             "SHADER:   %s   [right stick tap]\n"
             "Placed: %d   last piece: %s\n"
             "X place  |  X hold platform  |  Y undo  |  Y hold clear\n"
             "A next piece  |  A hold back  |  B copy  |  B hold delete\n"
             "Right stick hold: reset last piece%s%s",
             piece, g_piece + 1, n_piece_choices(), mat, tex, shd, g_npieces,
             last >= 0 ? "yes" : "none", g_note[0] ? "\n" : "", g_note);
    (void)now;
    if (strcmp(buf, g_panel) != 0) { memcpy(g_panel, buf, sizeof(buf)); g_panel_dirty = 1; }
}

static void hud_update(float now)
{
    panel_refresh(now);
    if (now > g_note_until && g_note[0]) { g_note[0] = 0; g_panel_dirty = 1; }
    if (!aoq_alive(g_hud)) {
        void *cam = aoq_camera_main();
        void *ctr = cam ? aoq_get_transform(cam) : NULL;
        if (!ctr) return;
        g_hud = aoq_tmp_create("MapBuilderPanel", ctr, 0.55f, -0.55f, 0.35f, 1.0f);
        if (!g_hud) return;
        aoq_tmp_set_alignment(g_hud, TMP_ALIGN_CENTER);
        aoq_tmp_set_color(g_hud, 1.0f, 1.0f, 1.0f, 1.0f);
        g_panel_dirty = 1;
    }
    if (g_panel_dirty) { aoq_tmp_set_text(g_hud, g_panel); g_panel_dirty = 0; }
}

/* ── Spawned copies ───────────────────────────────────────────────────────── */
/* A copy of a scene object can inherit the map's hidden state (the clear hid the originals), keep the original's
   parent (so it snaps back to where the original is), and carry a material whose shader is not the scene's.
   prepare_copy() fixes all three, then places the copy with its geometry on the spot. */
static int   g_copy_renderers = 0;

static void copy_renderers(void *go, int depth, int *budget)
{
    if (!aoqnet_valid(go) || *budget <= 0) return;
    (*budget)--;
    void *r = aoqnet_renderer(go);
    if (aoqnet_valid(r)) {
        aoqnet_renderer_enabled(r, 1);                                  /* visible again */
        void *orig = an_rend_get_shared(r, NULL);
        void *sh = (aoqnet_valid(orig) && fn_get_shader) ? fn_get_shader(orig, NULL) : NULL;
        if (aoqnet_valid(sh) && an_mat_set_shader) {
            void *mat = aoqnet_material(r);                             /* this copy's own material instance */
            if (aoqnet_valid(mat)) an_mat_set_shader(mat, sh, NULL);    /* same shader as the scene's material */
        }
        g_copy_renderers++;
    }
    void *c = aoqnet_collider(go);
    if (c) aoqnet_collider_enabled(c, 1);
    if (depth <= 0) return;
    int n = aoqnet_child_count(go);
    for (int i = 0; i < n && i < 32; i++) copy_renderers(aoqnet_child(go, i), depth - 1, budget);
}

static void prepare_copy(void *copy, AoqVec3 pos)
{
    aoqnet_set_parent(copy, NULL, 1);                                   /* detach from the original's parent, keep world position */
    aoqnet_set_active(copy, 1);
    g_copy_renderers = 0;
    int budget = 128;
    copy_renderers(copy, 6, &budget);
    aoqnet_move(copy, pos);
}

/* ── Actions ──────────────────────────────────────────────────────────────── */
static void *make_primitive(AoqVec3 pos, int prim, AoqVec3 size, void **rend_out)
{
    void *go = aoqnet_create_primitive(prim);
    if (!aoqnet_valid(go)) return NULL;
    aoqnet_set_name(go, "MapPiece");
    aoqnet_set_scale_local(go, size);
    aoqnet_move(go, pos);
    void *c = aoqnet_collider(go);
    if (c) aoqnet_collider_enabled(c, 1);
    if (rend_out) *rend_out = aoqnet_renderer(go);
    return go;
}

static void place_selected(float now)
{
    AoqVec3 tip;
    if (!gun_tip(now, &tip)) { note(now, "No flare gun found"); return; }
    void *go = NULL, *rend = NULL;
    if (g_piece < N_PRIM) {
        AoqVec3 size = { cfg_stud, cfg_stud, cfg_stud };
        go = make_primitive(snap(tip, size), PRIM[g_piece], size, &rend);
    } else {
        int t = g_piece - N_PRIM;
        if (t < 0 || t >= g_ntmpl || !aoqnet_valid(g_tmpl[t].tmpl)) { note(now, "That object is gone"); return; }
        go = aoqnet_instantiate_local(g_tmpl[t].tmpl);       /* a copy of the scene object, made active */
        if (!aoqnet_valid(go)) { note(now, "Could not copy that object"); return; }
        AoqVec3 c = snap(tip, g_tmpl[t].size), o = g_tmpl[t].off;
        prepare_copy(go, (AoqVec3){ c.x + o.x, c.y + o.y, c.z + o.z });
        LOGI("placed %s at (%.2f %.2f %.2f) offset (%.2f %.2f %.2f)", g_tmpl[t].name, c.x, c.y, c.z, o.x, o.y, o.z);
        rend = aoqnet_renderer(go);
    }
    if (!go) { note(now, "Could not place"); return; }
    int idx = piece_add(go, rend, tip);
    if (idx < 0) { aoqnet_destroy_local(go); note(now, "Piece limit reached"); return; }
    note(now, "Placed (%d pieces)", g_npieces);
}

static void place_platform(float now)
{
    AoqVec3 c = { cfg_px, cfg_py, cfg_pz }, s = { cfg_pw, cfg_ph, cfg_pd };
    void *rend = NULL;
    void *go = make_primitive(c, PRIM[0], s, &rend);
    if (!go || piece_add(go, rend, c) < 0) { note(now, "Platform: could not place"); return; }
    note(now, "Platform: %.0f x %.0f m", cfg_pw, cfg_pd);
}

static void undo_last(float now)
{
    int i = last_piece();
    if (i < 0) { note(now, "Nothing to undo"); return; }
    piece_remove(i);
    note(now, "Undo (%d left)", g_npieces);
}

static void delete_near_gun(float now)
{
    AoqVec3 tip;
    if (!gun_tip(now, &tip)) { note(now, "No flare gun found"); return; }
    int best = -1; float best_d2 = 1.5f * 1.5f;
    for (int i = 0; i < MAX_PIECES; i++) {
        if (!g_pieces[i].used) continue;
        float dx = g_pieces[i].pos.x - tip.x, dy = g_pieces[i].pos.y - tip.y, dz = g_pieces[i].pos.z - tip.z, d2 = dx*dx + dy*dy + dz*dz;
        if (d2 < best_d2) { best_d2 = d2; best = i; }
    }
    if (best < 0) { note(now, "No piece of yours near the gun"); return; }
    piece_remove(best);
    note(now, "Deleted (%d left)", g_npieces);
}

static void next_piece(float now, int step)
{
    int n = n_piece_choices();
    g_piece = ((g_piece + step) % n + n) % n;
    char nm[96]; piece_label(g_piece, nm, sizeof(nm));
    note(now, "Piece: %s", nm);
}

/* COPY MATERIAL: the scene renderer nearest the gun gives its material to the last piece */
static void copy_material(float now)
{
    AoqVec3 tip;
    if (!gun_tip(now, &tip)) { note(now, "No flare gun found"); return; }
    int last = last_piece();
    if (last < 0) { note(now, "Place a piece first"); return; }
    int count = 0;
    void *arr = find_type("UnityEngine", "Renderer", &count);
    if (!arr) return;
    void **vec = (void **)((char *)arr + 0x10);
    void *best = NULL; float best_d2 = 1e9f;
    for (int i = 0; i < count; i++) {
        void *r = vec[i];
        if (!aoqnet_valid(r) || protected_go(aoqnet_gameobject(r)) || is_piece(aoqnet_gameobject(r))) continue;
        void *gt = an_comp_get_transform ? an_comp_get_transform(r, NULL) : NULL;
        if (!gt || !an_tr_get_pos) continue;
        AoqVec3 p = an_tr_get_pos(gt, NULL);
        float dx = p.x - tip.x, dy = p.y - tip.y, dz = p.z - tip.z, d2 = dx*dx + dy*dy + dz*dz;
        if (d2 < best_d2) { best_d2 = d2; best = r; }
    }
    if (!best) { note(now, "No scene object near the gun"); return; }
    void *mat = an_rend_get_shared(best, NULL);
    if (!aoqnet_valid(mat) || !aoqnet_valid(g_pieces[last].rend)) { note(now, "Copy: no material"); return; }
    an_rend_set_material(g_pieces[last].rend, mat, NULL);
    note(now, "Copied a material onto the last piece");
}

/* MATERIAL: cycles through the scene's materials (0 = keep the piece's original) */
static void cycle_material(float now)
{
    int last = last_piece();
    if (last < 0) { note(now, "Place a piece first"); return; }
    g_mat_sel = (g_mat_sel + 1) % (g_nmats + 1);
    if (g_mat_sel == 0) { an_rend_set_material(g_pieces[last].rend, g_pieces[last].orig_mat, NULL); note(now, "Material: original"); return; }
    an_rend_set_material(g_pieces[last].rend, g_mats[g_mat_sel - 1], NULL);
    note(now, "Material %d/%d", g_mat_sel, g_nmats);
}

/* TEXTURE: cycles the main texture of the piece's own material instance (0 = keep the piece's texture) */
static void cycle_texture(float now)
{
    int last = last_piece();
    if (last < 0) { note(now, "Place a piece first"); return; }
    void *mat = aoqnet_material(g_pieces[last].rend);
    if (!aoqnet_valid(mat) || !fn_set_maintex) { note(now, "Texture: no material"); return; }
    g_tex_sel = (g_tex_sel + 1) % (g_ntexs + 1);
    if (g_tex_sel == 0) { an_rend_set_material(g_pieces[last].rend, g_pieces[last].orig_mat, NULL); note(now, "Texture: original"); return; }
    fn_set_maintex(mat, g_texs[g_tex_sel - 1], NULL);
    note(now, "Texture %d/%d", g_tex_sel, g_ntexs);
}

/* SHADER: cycles the shader of the piece's material instance (0 = keep the original shader) */
static void cycle_shader(float now)
{
    int last = last_piece();
    if (last < 0) { note(now, "Place a piece first"); return; }
    void *mat = aoqnet_material(g_pieces[last].rend);
    if (!aoqnet_valid(mat)) { note(now, "Shader: no material"); return; }
    g_shader_sel = (g_shader_sel + 1) % (N_SHADERS + 1);
    if (g_shader_sel == 0) { an_rend_set_material(g_pieces[last].rend, g_pieces[last].orig_mat, NULL); note(now, "Shader: original"); return; }
    if (!aoqnet_material_shader(mat, SHADERS[g_shader_sel - 1])) { note(now, "Not in build: %s", SHADERS[g_shader_sel - 1]); return; }
    note(now, "Shader: %s", SHADERS[g_shader_sel - 1]);
}

static void reset_last(float now)
{
    int last = last_piece();
    if (last < 0) { note(now, "Place a piece first"); return; }
    an_rend_set_material(g_pieces[last].rend, g_pieces[last].orig_mat, NULL);
    g_mat_sel = g_tex_sel = g_shader_sel = 0;
    note(now, "Reset to the original look");
}

/* CLEAR: hide the map, delete floors, and remove lights, particles and audio that belong to the map */
static void clear_map(float now)
{
    int count = 0, hidden = 0, floors = 0, fx = 0, kept = 0;
    void *arr = find_type("UnityEngine", "Renderer", &count);
    void **vec = arr ? (void **)((char *)arr + 0x10) : NULL;
    for (int i = 0; i < count && vec; i++) {
        void *r = vec[i];
        if (!aoqnet_valid(r)) continue;
        void *go = aoqnet_gameobject(r);
        if (protected_go(go) || is_piece(go)) { kept++; continue; }
        char n[128]; obj_name(go, n, sizeof(n));
        if (is_floor(n)) { aoqnet_destroy_local(go); floors++; continue; }
        aoqnet_renderer_enabled(r, 0);
        void *c = aoqnet_collider(go);
        if (c) aoqnet_collider_enabled(c, 0);
        hidden++;
    }
    /* lights, particle systems and audio that belong to the map */
    static const char *fx_types[3] = { "Light", "ParticleSystem", "AudioSource" };
    for (int t = 0; t < 3; t++) {
        int n = 0;
        void *a = find_type("UnityEngine", fx_types[t], &n);
        void **v = a ? (void **)((char *)a + 0x10) : NULL;
        for (int i = 0; i < n && v; i++) {
            void *obj = v[i];
            if (!aoqnet_valid(obj)) continue;
            void *go = aoqnet_gameobject(obj);
            if (protected_go(go) || is_piece(go)) continue;
            aoqnet_destroy_local(obj);
            fx++;
        }
    }
    note(now, "Cleared: %d hidden, %d floor, %d effects removed, %d kept", hidden, floors, fx, kept);
}

/* ── Spawn test: copy every scene object the probe found, in a grid, and record what happened ─────────── */
#define MAX_TEST 1024
static void *g_test_tops[MAX_TEST];
static char  g_test_names[MAX_TEST][64];
static int   g_test_kind[MAX_TEST];              /* 0 scene object, 1 prefab, 2 networked prefab (skipped) */
static int   g_test_n = 0, g_test_i = 0, g_test_state = 0;   /* 0 idle, 1 running, 2 done */
static FILE *g_test_f = NULL;
static int   g_test_ok = 0, g_test_norend = 0, g_test_zero = 0, g_test_fail = 0;

/* renderers on the copy or its children (up to 4 levels): returns the first renderer, or NULL */
static void *first_renderer(void *go, int depth)
{
    void *r = aoqnet_renderer(go);
    if (r) return r;
    if (depth <= 0) return NULL;
    int n = aoqnet_child_count(go);
    for (int i = 0; i < n && i < 16; i++) {
        void *x = first_renderer(aoqnet_child(go, i), depth - 1);
        if (x) return x;
    }
    return NULL;
}

static void test_collect_prefabs(void)
{
    if (!fn_loadall) return;
    void *gotype = aoqnet_type_object("UnityEngine", "GameObject");
    void *arr = gotype ? fn_loadall(aoqnet_str(""), gotype, NULL) : NULL;
    if (!arr) { LOGE("spawn test: Resources.LoadAll failed"); return; }
    int count = *(int *)((char *)arr + 0x0C);
    void **vec = (void **)((char *)arr + 0x10);
    for (int i = 0; i < count && g_test_n < MAX_TEST; i++) {
        void *go = vec[i];
        if (!aoqnet_valid(go)) continue;
        char name[64]; obj_name(go, name, sizeof(name));
        if (!name[0] || strstr(name, "Clone")) continue;
        int net = aoqnet_get_component(go, "Photon.Pun", "PhotonView") != NULL;
        g_test_kind[g_test_n] = net ? 2 : 1;
        snprintf(g_test_names[g_test_n], sizeof(g_test_names[0]), "prefab:%s", name);
        g_test_tops[g_test_n++] = go;
    }
}

static void test_collect(void)
{
    g_test_n = 0;
    int count = 0;
    void *arr = find_type("UnityEngine", "Renderer", &count);
    if (!arr) return;
    void **vec = (void **)((char *)arr + 0x10);
    for (int i = 0; i < count && g_test_n < MAX_TEST; i++) {
        void *r = vec[i];
        if (!aoqnet_valid(r)) continue;
        void *go = aoqnet_gameobject(r);
        if (protected_go(go) || is_piece(go)) continue;
        void *top = topmost_free(go);
        char name[64]; obj_name(top, name, sizeof(name));
        if (!name[0] || strstr(name, "Clone")) continue;
        int dup = 0;
        for (int k = 0; k < g_test_n; k++) if (strcmp(g_test_names[k], name) == 0) { dup = 1; break; }
        if (dup) continue;
        snprintf(g_test_names[g_test_n], sizeof(g_test_names[0]), "%s", name);
        g_test_kind[g_test_n] = 0;
        g_test_tops[g_test_n++] = top;
    }
    test_collect_prefabs();
}

static void test_start(float now)
{
    test_collect();
    mkdir("/sdcard/DCIM/AoQMods", 0777);
    mkdir("/sdcard/DCIM/AoQMods/probe", 0777);
    g_test_f = fopen("/sdcard/DCIM/AoQMods/probe/spawn_test.txt", "w");
    if (g_test_f) fprintf(g_test_f, "# spawn test: %d unique scene objects\n# index | name | status | bounds (m) | grid position\n", g_test_n);
    g_test_i = 0; g_test_ok = g_test_norend = g_test_zero = g_test_fail = 0;
    int np = 0; for (int k = 0; k < g_test_n; k++) if (g_test_kind[k] != 0) np++;
    if (g_test_f) fprintf(g_test_f, "# of which prefabs: %d (networked prefabs are skipped unless Test Networked Prefabs is on)\n", np);
    g_test_state = 1;
    note(now, "Spawn test: %d objects, writing spawn_test.txt", g_test_n);
    LOGI("spawn test started: %d objects", g_test_n);
}

static void test_step(float now)
{
    if (g_test_state != 1) return;
    for (int b = 0; b < cfg_tbatch && g_test_i < g_test_n; b++, g_test_i++) {
        int i = g_test_i;
        void *top = g_test_tops[i];
        int col = i % 20, row = i / 20;
        AoqVec3 pos = { cfg_tx + col * cfg_tspace, cfg_ty, cfg_tz + row * cfg_tspace };
        const char *status = "OK";
        AoqVec3 e = { 0, 0, 0 };
        if (g_test_kind[i] == 2 && !cfg_test_net) { status = "SKIPPED_NETWORKED"; g_test_fail++; }
        else if (!aoqnet_valid(top)) { status = "GONE"; g_test_fail++; }
        else {
            void *copy = aoqnet_instantiate_local(top);
            if (!aoqnet_valid(copy)) { status = "COPY_FAILED"; g_test_fail++; }
            else {
                prepare_copy(copy, pos);
                void *r = first_renderer(copy, 4);
                if (!r) { status = "NO_RENDERER"; g_test_norend++; }
                else {
                    Bnd bb = fn_bounds ? fn_bounds(r, NULL) : (Bnd){ {0,0,0}, {0,0,0} };
                    e = bb.e;
                    if (e.x + e.y + e.z <= 0.003f) { status = "ZERO_BOUNDS"; g_test_zero++; }
                    else g_test_ok++;
                }
            }
        }
        if (g_test_f) fprintf(g_test_f, "%d | %s | %s | %.2f x %.2f x %.2f | %.0f, %.0f, %.0f\n", i, g_test_names[i], status,
                              e.x * 2, e.y * 2, e.z * 2, pos.x, pos.y, pos.z);
    }
    if (g_test_i >= g_test_n) {
        g_test_state = 2;
        char summary[200];
        snprintf(summary, sizeof(summary), "# total %d: OK %d, NO_RENDERER %d, ZERO_BOUNDS %d, failed %d",
                 g_test_n, g_test_ok, g_test_norend, g_test_zero, g_test_fail);
        if (g_test_f) { fprintf(g_test_f, "\n%s\n", summary); fclose(g_test_f); g_test_f = NULL; }
        note(now, "Spawn test done: OK %d of %d", g_test_ok, g_test_n);
        LOGI("%s", summary);
    }
}

/* ── Scan timing, pieces ──────────────────────────────────────────────────── */
static void do_scan_when_ready(float now)
{
    if (g_scanned || g_scene_t < 0.0f) return;
    if (now - g_scene_t < SCAN_DELAY) return;
    if (!aoq_alive(g_pm)) return;
    scan_scene(now);
    note(now, "Scene scanned: %d objects, %d materials, %d textures", g_ntmpl, g_nmats, g_ntexs);
}

/* ── Buttons ──────────────────────────────────────────────────────────────── */
typedef struct { float down; int held, fired; } Btn;
static Btn g_lx, g_ly, g_ra, g_rb, g_ls, g_rs;

static void poll(Btn *b, int held, float now, int *tap, int *hold)
{
    *tap = *hold = 0;
    if (held && !b->held) { b->down = now; b->fired = 0; }
    if (held && !b->fired && now - b->down >= HOLD_SECS) { b->fired = 1; *hold = 1; }
    if (!held && b->held && !b->fired) *tap = 1;
    b->held = held;
}

/* ── Hooks ────────────────────────────────────────────────────────────────── */
MAKE_HOOK(PlayerManager_Awake, RVA_PlayerManager_Awake, void, void *self)
{
    PlayerManager_Awake(self);
    g_pm = self;
}

MAKE_HOOK(GameSetupController_Awake, RVA_GameSetupController_Awake, void, void *self)
{
    memset(g_pieces, 0, sizeof g_pieces);
    g_npieces = 0;
    g_scanned = 0;
    g_scene_t = now_s();
    g_outline = NULL;
    g_hud = NULL;
    g_mat_sel = g_tex_sel = g_shader_sel = 0;
    g_test_state = 0;
    GameSetupController_Awake(self);
}

MAKE_HOOK(NetworkFlareGun_Update, RVA_NetworkFlareGun_Update, void, void *self)
{
    NetworkFlareGun_Update(self);
    if (!aoq_alive(self) || !aoq_is_mine(self)) return;
    void *b = AOQ_FIELD(self, OFF_NetFlare_barrelEnd, void *);
    if (aoq_alive(b)) g_flare_net = (Flare){ self, b, now_s() };
}

MAKE_HOOK(FireFlare_Update, RVA_FireFlare_Update, void, void *self)
{
    FireFlare_Update(self);
    if (!aoq_alive(self) || !aoq_is_mine(self)) return;
    void *b = AOQ_FIELD(self, OFF_FireFlare_barrelEnd, void *);
    if (aoq_alive(b)) g_flare_loc = (Flare){ self, b, now_s() };
}

MAKE_HOOK(OVRManager_Update, RVA_OVRManager_Update, void, void *self)
{
    OVRManager_Update(self);
    if (!fn_Get) return;
    cfg_read();
    if (!cfg_on) return;
    float now = now_s();
    do_scan_when_ready(now);
    if (cfg_test && g_scanned && g_test_state == 0) test_start(now);
    test_step(now);

    int t, h;
    poll(&g_lx, fn_Get(BTN_ONE, CTRL_L, NULL), now, &t, &h);
    if (t) place_selected(now);
    if (h) place_platform(now);

    poll(&g_ly, fn_Get(BTN_TWO, CTRL_L, NULL), now, &t, &h);
    if (t) undo_last(now);
    if (h) clear_map(now);

    poll(&g_ra, fn_Get(BTN_ONE, CTRL_R, NULL), now, &t, &h);
    if (t) next_piece(now, 1);
    if (h) next_piece(now, -1);

    poll(&g_rb, fn_Get(BTN_TWO, CTRL_R, NULL), now, &t, &h);
    if (t) copy_material(now);
    if (h) delete_near_gun(now);

    poll(&g_ls, fn_Get(BTN_LSTK, CTRL_L, NULL), now, &t, &h);
    if (t) cycle_material(now);
    if (h) cycle_texture(now);

    poll(&g_rs, fn_Get(BTN_RSTK, CTRL_R, NULL), now, &t, &h);
    if (t) cycle_shader(now);
    if (h) reset_last(now);

    outline_update(now);
    hud_update(now);
}

__attribute__((constructor)) void lib_main(void)
{
    LOGI("map builder loading...");
    aoq_init();
    aoqmm_register(SO, "Map Builder", "3.0.0", "{x}Jester",
                   "Clears the map, places scene objects and primitives with outlines, cycles materials, textures and shaders. Local only.");
    aoqmm_ensure_config(SO, DEFAULT_CONFIG);
    aoqnet_init();

    fn_Get         = (Get_t)getRealOffset(RVA_OVRInput_Get);
    fn_time        = (Time_t)getRealOffset(RVA_Time_get_time);
    fn_find        = (Find_t)getRealOffset(RVA_FindObjectsOfType_Type);
    fn_get_maintex = (GetP_t)getRealOffset(RVA_Material_get_mainTexture);
    fn_set_maintex = (SetP_t)getRealOffset(RVA_Material_set_mainTexture);
    fn_bounds      = (Bounds_t)getRealOffset(RVA_Renderer_get_bounds);
    fn_loadall     = (LoadAll_t)getRealOffset(RVA_Resources_LoadAll);
    fn_get_shader  = (GetP_t)getRealOffset(RVA_Material_get_shader);

    cfg_read();
    INSTALL_HOOK(PlayerManager_Awake);
    INSTALL_HOOK(GameSetupController_Awake);
    INSTALL_HOOK(NetworkFlareGun_Update);
    INSTALL_HOOK(FireFlare_Update);
    INSTALL_HOOK(OVRManager_Update);
    LOGI("map builder hooks installed");
}