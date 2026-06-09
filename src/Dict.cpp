#include "Dict.h"
#include "WordData.h"
#include "DisplayConfig.hpp"

#include <FS.h>
#include <SD_MMC.h>
#include <LittleFS.h>
#include <Preferences.h>
#include <esp_heap_caps.h>
#include <string.h>

// ==============================================================================
// Multi-source dictionary engine (format v4).
//
// Each loaded source keeps its whole .idx in PSRAM (idxBuf) with parallel
// arrays key[]/dataOff[]/wmin[] and an open .dat File. Sources are discovered by
// scanning /dicts/*.meta on LittleFS and SD; an SD source shadows a same-named
// (same id = filename stem) flash source. Per-source settings (enabled, mode,
// floor, priority) are loaded from NVS (defaulting from .meta) and sources are
// sorted by priority (0 = highest).
//
// A merged, tier-filtered, deduped view is built across the *enabled* sources in
// rebuildView(). It is a CSR-style structure:
//   s_viewKey[N]       key string (points into the highest-priority contributor's
//                      idxBuf) for each distinct key, sorted ascending
//   s_contribBase[N]   start index into the flat contributor arrays
//   s_contribCount[N]  number of contributors for this key
//   s_cSrc[M] / s_cOff[M]   per-contributor source-order index + .dat offset,
//                      stored in priority order; M = total contributors
// dictCount/dictTerm/dictFind/dictLowerBound operate on this view; dictGet walks
// a key's contributors in priority order (additive appends, override stops).
//
// If no source loads, the engine falls back to the embedded WORDS table.
// ==============================================================================

static const int MAX_SOURCES = 16;

struct Source
{
    char       id[16];          // filename stem (e.g. "base"), <=13 chars for NVS key
    String     name;            // display name from .meta (name=)
    fs::FS*    fs;              // backing filesystem (&LittleFS or &SD_MMC)
    bool       onSD;            // true if backed by SD

    uint8_t*   idxBuf;          // whole .idx in PSRAM
    const char** key;           // term pointers into idxBuf (PSRAM)
    uint32_t*  dataOff;         // .dat offset per entry (PSRAM)
    uint8_t*   wmin;            // per-entry wordMinTier (PSRAM)
    int        count;
    File       dat;             // open .dat handle

    // Settings (defaults from .meta, overridden by NVS).
    bool       enabled;
    int        priority;        // 0 = highest
    DictMode   mode;
    DictTier   floor;
};

static Source  s_src[MAX_SOURCES];
static int     s_srcCount = 0;

// ----------------------------------------------------------------------------
// Runtime exclusions (.excl files under /dicts/exclude).
//
// Each file declares an action (HIDE removes matched keys; GATE raises their
// effective min tier) and a scope (all sources, or a set of source ids/stems).
// Keys are normalised on load. An exclusion only affects a (key, source) pair
// when the exclusion is enabled, the source's id is in scope, and the key is in
// the exclusion's key set.
// ----------------------------------------------------------------------------
static const int MAX_EXCLUSIONS = 16;
static const int MAX_EXCL_SCOPE = 8;
static const int MAX_EXCL_KEYS  = 256;

enum ExclAction { EXCL_HIDE = 0, EXCL_GATE = 1 };

struct Exclusion
{
    char       stem[16];                       // filename stem (e.g. "extra-rude"), <=13 for NVS
    String     file;                           // display file name (e.g. "extra-rude.excl")
    bool       enabled;
    ExclAction action;
    DictTier   gateTier;                       // gate target tier (EXCL_GATE only)
    bool       scopeAll;                        // true => applies to every source
    char       scope[MAX_EXCL_SCOPE][16];      // in-scope source ids (when !scopeAll)
    int        scopeCount;
    char**     keys;                           // sorted, normalised key strings (PSRAM)
    int        keyCount;
};

static Exclusion s_excl[MAX_EXCLUSIONS];
static int       s_exclCount = 0;
static int       s_exclEnabledCount = 0;       // cached: number of enabled exclusions (fast path)

// Merged view (CSR).
static const char** s_viewKey      = nullptr;   // PSRAM, N
static uint32_t*    s_contribBase  = nullptr;   // PSRAM, N
static uint8_t*     s_contribCount = nullptr;   // PSRAM, N
static uint16_t*    s_cSrc         = nullptr;   // PSRAM, M (source-order index)
static uint32_t*    s_cOff         = nullptr;   // PSRAM, M (.dat offset)
static int          s_count        = 0;         // distinct keys (N)

static bool      s_loaded   = false;            // true once >=1 source loaded
static DictTier  s_tier     = TIER_FULL;
static char      s_status[64] = "Built-in set";

// Normalise a query/headword to a search key: lowercase, strip accents to ASCII,
// keep a-z 0-9 space '-', drop apostrophes, collapse spaces. MUST match
// tools/normalize.py. ASCII-only accent fold (covers Latin-1 supplement).
void dictNormalizeKey(const char* in, char* out, size_t cap)
{
    size_t o = 0; bool lastSpace = true;
    for (const unsigned char* p = (const unsigned char*)in; *p && o + 1 < cap; ++p) {
        unsigned char c = *p;
        char m = 0;
        if (c >= 'A' && c <= 'Z') m = c - 'A' + 'a';
        else if ((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9')) m = c;
        else if (c == '-') m = '-';
        else if (c == ' ' || (c >= 9 && c <= 13)) m = ' ';
        else if (c == '\'' || c == 0x92) continue;       // drop apostrophes
        else m = ' ';                                     // other punct/UTF-8 -> space
        if (m == ' ') { if (lastSpace) continue; lastSpace = true; }
        else lastSpace = false;
        out[o++] = m;
    }
    while (o > 0 && out[o-1] == ' ') --o;                  // trim trailing
    out[o] = 0;
}

static const char* tierName(DictTier t)
{
    switch (t) {
        case TIER_SAFE: return "Safe";
        case TIER_MILD: return "Mild";
        case TIER_TEEN: return "Teen";
        default:        return "Full";
    }
}

static DictTier tierFromName(const char* s)
{
    if (!s) return TIER_SAFE;
    if (!strcasecmp(s, "mild")) return TIER_MILD;
    if (!strcasecmp(s, "teen")) return TIER_TEEN;
    if (!strcasecmp(s, "full")) return TIER_FULL;
    return TIER_SAFE;
}

static DictMode modeFromName(const char* s)
{
    if (s && !strcasecmp(s, "override")) return MODE_OVERRIDE;
    return MODE_ADDITIVE;
}

static void setStatus(const char* msg) { strncpy(s_status, msg, sizeof(s_status) - 1); s_status[sizeof(s_status) - 1] = 0; }

// Reads `len` bytes from `f` into a String, capped to the field buffer.
static String readField(File& f, uint32_t len)
{
    static char buf[700];   // def<=600, example<=400 (supp builder caps)
    if (len > sizeof(buf) - 1) len = sizeof(buf) - 1;
    int n = f.read(reinterpret_cast<uint8_t*>(buf), len);
    if (n < 0) n = 0;
    buf[n] = 0;
    return String(buf);
}

// ----------------------------------------------------------------------------
// View (de)allocation.
// ----------------------------------------------------------------------------
static void freeView()
{
    if (s_viewKey)      { heap_caps_free(s_viewKey);      s_viewKey      = nullptr; }
    if (s_contribBase)  { heap_caps_free(s_contribBase);  s_contribBase  = nullptr; }
    if (s_contribCount) { heap_caps_free(s_contribCount); s_contribCount = nullptr; }
    if (s_cSrc)         { heap_caps_free(s_cSrc);         s_cSrc         = nullptr; }
    if (s_cOff)         { heap_caps_free(s_cOff);         s_cOff         = nullptr; }
    s_count = 0;
}

static void freeSource(Source& s)
{
    if (s.idxBuf)  { heap_caps_free(s.idxBuf);  s.idxBuf  = nullptr; }
    if (s.key)     { heap_caps_free(s.key);     s.key     = nullptr; }
    if (s.dataOff) { heap_caps_free(s.dataOff); s.dataOff = nullptr; }
    if (s.wmin)    { heap_caps_free(s.wmin);    s.wmin    = nullptr; }
    if (s.dat)     { s.dat.close(); }
    s.count = 0;
}

static void freeAllSources()
{
    freeView();
    for (int i = 0; i < s_srcCount; ++i) freeSource(s_src[i]);
    s_srcCount = 0;
}

static void freeExclusions()
{
    for (int i = 0; i < s_exclCount; ++i) {
        Exclusion& x = s_excl[i];
        if (x.keys) {
            for (int k = 0; k < x.keyCount; ++k) heap_caps_free(x.keys[k]);
            heap_caps_free(x.keys);
            x.keys = nullptr;
        }
        x.keyCount = 0;
    }
    s_exclCount = 0;
    s_exclEnabledCount = 0;
}

// Recomputes the cached count of enabled exclusions (fast-path gate).
static void recountEnabledExclusions()
{
    int n = 0;
    for (int i = 0; i < s_exclCount; ++i) if (s_excl[i].enabled) ++n;
    s_exclEnabledCount = n;
}

// ----------------------------------------------------------------------------
// Per-source NVS settings.
//
// Packed uint32: bit0 enabled | bit1 mode | bits3:2 floor | bits11:4 priority.
// Namespace "dict", key "s_<id>". Defaults come from .meta (caller pre-fills).
// ----------------------------------------------------------------------------
static uint32_t packSettings(const Source& s)
{
    uint32_t v = 0;
    v |= (s.enabled ? 1u : 0u);
    v |= ((uint32_t)(s.mode & 1) << 1);
    v |= ((uint32_t)(s.floor & 3) << 2);
    v |= ((uint32_t)(s.priority & 0xFF) << 4);
    return v;
}

static void unpackSettings(Source& s, uint32_t v)
{
    s.enabled  = (v & 1) != 0;
    s.mode     = (DictMode)((v >> 1) & 1);
    s.floor    = (DictTier)((v >> 2) & 3);
    s.priority = (int)((v >> 4) & 0xFF);
}

static void nvsKey(const Source& s, char* out, size_t cap)
{
    snprintf(out, cap, "s_%s", s.id);   // id capped to keep within NVS 15-char key limit
}

static void persistSource(const Source& s)
{
    Preferences p;
    if (!p.begin("dict", false)) return;
    char k[16]; nvsKey(s, k, sizeof(k));
    p.putUInt(k, packSettings(s));
    p.end();
}

// Loads NVS overrides for every source (meta-derived defaults already in place).
static void loadSourceSettings()
{
    Preferences p;
    if (!p.begin("dict", true)) return;
    for (int i = 0; i < s_srcCount; ++i) {
        char k[16]; nvsKey(s_src[i], k, sizeof(k));
        if (p.isKey(k)) unpackSettings(s_src[i], p.getUInt(k, packSettings(s_src[i])));
    }
    p.end();
}

// ----------------------------------------------------------------------------
// Source discovery + per-source index load.
// ----------------------------------------------------------------------------

// Reads name/mode/floor from a .meta file; returns false on open failure.
static bool readMeta(fs::FS* fs, const char* path, String& outName, DictMode& outMode, DictTier& outFloor)
{
    File f = fs->open(path, FILE_READ);
    if (!f) return false;
    outName = ""; outMode = MODE_ADDITIVE; outFloor = TIER_SAFE;
    while (f.available()) {
        String line = f.readStringUntil('\n');
        line.trim();
        int eq = line.indexOf('=');
        if (eq < 0) continue;
        String k = line.substring(0, eq); k.trim();
        String v = line.substring(eq + 1); v.trim();
        if (k == "name")  outName = v;
        else if (k == "mode")  outMode  = modeFromName(v.c_str());
        else if (k == "floor") outFloor = tierFromName(v.c_str());
    }
    f.close();
    return true;
}

// Parses a v4 .idx already resident in `idxBuf` (size `sz`) into key/dataOff/wmin
// arrays (allocated in PSRAM). Returns the entry count, or -1 on any error.
static int parseIdx(uint8_t* idxBuf, size_t sz, const char*** outKey, uint32_t** outOff, uint8_t** outMin)
{
    if (sz < 12 || memcmp(idxBuf, "DIDX", 4) != 0) return -1;
    uint32_t version = 0, count = 0;
    memcpy(&version, idxBuf + 4, 4);
    memcpy(&count,   idxBuf + 8, 4);
    if (version != 4) return -1;

    const char** key = static_cast<const char**>(heap_caps_malloc(sizeof(char*)   * count, MALLOC_CAP_SPIRAM));
    uint32_t*    off = static_cast<uint32_t*>  (heap_caps_malloc(sizeof(uint32_t) * count, MALLOC_CAP_SPIRAM));
    uint8_t*     wmn = static_cast<uint8_t*>   (heap_caps_malloc(sizeof(uint8_t)  * count, MALLOC_CAP_SPIRAM));
    if (!key || !off || !wmn) {
        heap_caps_free(key); heap_caps_free(off); heap_caps_free(wmn);
        return -1;
    }

    // Per entry: [u32 dataOffset][u8 wordMinTier][searchkey bytes][0x00].
    size_t p = 12;
    for (uint32_t i = 0; i < count; ++i) {
        if (p + 5 > sz) { heap_caps_free(key); heap_caps_free(off); heap_caps_free(wmn); return -1; }
        uint32_t o; memcpy(&o, idxBuf + p, 4); p += 4;
        off[i] = o;
        wmn[i] = idxBuf[p]; ++p;
        key[i] = reinterpret_cast<const char*>(idxBuf + p);
        while (p < sz && idxBuf[p] != 0) ++p;
        if (p >= sz) { heap_caps_free(key); heap_caps_free(off); heap_caps_free(wmn); return -1; }
        ++p;   // skip NUL
    }

    *outKey = key; *outOff = off; *outMin = wmn;
    return (int)count;
}

// Derives the filename stem (without directory or ".meta") into `out`.
static void stemFromMeta(const char* metaPath, char* out, size_t cap)
{
    const char* base = strrchr(metaPath, '/');
    base = base ? base + 1 : metaPath;
    size_t n = 0;
    for (; base[n] && n + 1 < cap; ++n) {
        if (base[n] == '.') break;   // stop at extension
        out[n] = base[n];
    }
    out[n] = 0;
}

// Loads one source given its .meta path. `idxPath`/`datPath` are derived. Returns
// true on success and appends/updates s_src; SD shadows a same-id flash source.
static bool loadOneSource(fs::FS* fs, bool onSD, const char* metaPath)
{
    char stem[16]; stemFromMeta(metaPath, stem, sizeof(stem));
    if (stem[0] == 0) return false;
    if (strlen(stem) > 13) return false;   // keep NVS key "s_<id>" within 15 chars

    char idxPath[64], datPath[64];
    snprintf(idxPath, sizeof(idxPath), "/dicts/%s.idx", stem);
    snprintf(datPath, sizeof(datPath), "/dicts/%s.dat", stem);

    File idxF = fs->open(idxPath, FILE_READ);
    if (!idxF) return false;
    File datF = fs->open(datPath, FILE_READ);
    if (!datF) { idxF.close(); return false; }

    String name; DictMode mode; DictTier floor;
    if (!readMeta(fs, metaPath, name, mode, floor)) { idxF.close(); datF.close(); return false; }
    if (name.length() == 0) name = stem;

    size_t sz = idxF.size();
    uint8_t* idxBuf = static_cast<uint8_t*>(heap_caps_malloc(sz, MALLOC_CAP_SPIRAM));
    if (!idxBuf) { idxF.close(); datF.close(); return false; }
    size_t got = idxF.read(idxBuf, sz);
    idxF.close();
    if (got != sz) { heap_caps_free(idxBuf); datF.close(); return false; }

    const char** key = nullptr; uint32_t* off = nullptr; uint8_t* wmn = nullptr;
    int count = parseIdx(idxBuf, sz, &key, &off, &wmn);
    if (count < 0) { heap_caps_free(idxBuf); datF.close(); return false; }

    // SD shadows a same-id flash source: replace its backing fs/files/index.
    int existing = -1;
    for (int i = 0; i < s_srcCount; ++i) {
        if (strcmp(s_src[i].id, stem) == 0) { existing = i; break; }
    }

    if (existing >= 0 && onSD) {
        Source& s = s_src[existing];
        // Free old index/file, swap in SD-backed data, keep settings (re-derive name/mode/floor from SD meta).
        if (s.idxBuf)  heap_caps_free(s.idxBuf);
        if (s.key)     heap_caps_free(s.key);
        if (s.dataOff) heap_caps_free(s.dataOff);
        if (s.wmin)    heap_caps_free(s.wmin);
        if (s.dat)     s.dat.close();
        s.fs = fs; s.onSD = true;
        s.idxBuf = idxBuf; s.key = key; s.dataOff = off; s.wmin = wmn; s.count = count;
        s.dat = datF;
        s.name = name; s.mode = mode; s.floor = floor;
        return true;
    }
    if (existing >= 0) {
        // Same id already loaded (e.g. duplicate) and this is not an SD shadow: skip.
        heap_caps_free(idxBuf); heap_caps_free(key); heap_caps_free(off); heap_caps_free(wmn);
        datF.close();
        return false;
    }
    if (s_srcCount >= MAX_SOURCES) {
        heap_caps_free(idxBuf); heap_caps_free(key); heap_caps_free(off); heap_caps_free(wmn);
        datF.close();
        return false;
    }

    Source& s = s_src[s_srcCount];
    strncpy(s.id, stem, sizeof(s.id) - 1); s.id[sizeof(s.id) - 1] = 0;
    s.name = name; s.fs = fs; s.onSD = onSD;
    s.idxBuf = idxBuf; s.key = key; s.dataOff = off; s.wmin = wmn; s.count = count;
    s.dat = datF;
    s.enabled = true;
    s.priority = s_srcCount;   // provisional discovery order; normalised in dictBegin
    s.mode = mode; s.floor = floor;
    ++s_srcCount;
    return true;
}

// Scans /dicts on `fs` for *.meta files and loads each as a source.
static void scanDir(fs::FS* fs, bool onSD)
{
    File dir = fs->open("/dicts");
    if (!dir || !dir.isDirectory()) { if (dir) dir.close(); return; }
    for (File entry = dir.openNextFile(); entry; entry = dir.openNextFile()) {
        if (!entry.isDirectory()) {
            const char* nm = entry.name();   // may be bare name or full path depending on FS
            // Build a /dicts/-relative path and check the .meta extension.
            const char* slash = strrchr(nm, '/');
            const char* base = slash ? slash + 1 : nm;
            size_t len = strlen(base);
            if (len > 5 && strcasecmp(base + len - 5, ".meta") == 0) {
                char metaPath[64];
                snprintf(metaPath, sizeof(metaPath), "/dicts/%s", base);
                loadOneSource(fs, onSD, metaPath);
            }
        }
        entry.close();
    }
    dir.close();
}

// Assigns default priorities (used before NVS overrides): supplementary sources
// rank above the base corpus (plan: "Base is lowest priority by default"), so an
// override supplementary can suppress base. Supplementaries keep their discovery
// order amongst themselves; "base" is forced to the lowest priority (highest #).
static void assignDefaultPriorities()
{
    int p = 0;
    for (int i = 0; i < s_srcCount; ++i) {
        if (strcmp(s_src[i].id, "base") != 0) s_src[i].priority = p++;
    }
    for (int i = 0; i < s_srcCount; ++i) {
        if (strcmp(s_src[i].id, "base") == 0) s_src[i].priority = p++;
    }
}

// ----------------------------------------------------------------------------
// Priority sort.
// ----------------------------------------------------------------------------
static void sortByPriority()
{
    // Small array; insertion sort keeps it stable and simple.
    for (int i = 1; i < s_srcCount; ++i) {
        Source tmp = s_src[i];
        int j = i - 1;
        while (j >= 0 && s_src[j].priority > tmp.priority) {
            s_src[j + 1] = s_src[j];
            --j;
        }
        s_src[j + 1] = tmp;
    }
}

// ----------------------------------------------------------------------------
// Merged tier-filtered view.
//
// k-way merge of every enabled source's sorted key array, skipping entries that
// fail the active tier (effective min = max(source.floor, wmin) > activeTier),
// deduping by key. Contributors per distinct key are collected in priority order
// (sources are already sorted by priority, so iterating sources in order yields
// priority order). Two passes: count, then fill.
// ----------------------------------------------------------------------------

// Marker returned by effectiveMinTier when a (key, source) pair is HIDE-excluded
// (so it can never pass any tier).
static const uint8_t TIER_HIDDEN = 0xFF;

// True if exclusion `xi` is in scope for source order index `si`.
static inline bool exclInScope(int xi, int si)
{
    const Exclusion& x = s_excl[xi];
    if (x.scopeAll) return true;
    const char* id = s_src[si].id;
    for (int k = 0; k < x.scopeCount; ++k) {
        if (strcmp(x.scope[k], id) == 0) return true;
    }
    return false;
}

// Binary-searches an exclusion's sorted key set for `key`. Returns true if found.
static bool exclHasKey(const Exclusion& x, const char* key)
{
    int lo = 0, hi = x.keyCount - 1;
    while (lo <= hi) {
        int mid = (lo + hi) / 2;
        int c = strcmp(x.keys[mid], key);
        if (c == 0) return true;
        if (c < 0) lo = mid + 1; else hi = mid - 1;
    }
    return false;
}

// Resolves all enabled, in-scope exclusions matching (`key`, source `si`) into a
// single classification: HIDE dominates; otherwise the gate tier is the max of
// matching gates. `outGate` receives the highest gate tier when GATE applies.
// Returns: 0 = no exclusion, 1 = GATE (outGate set), 2 = HIDE.
static int exclClassify(int si, const char* key, uint8_t& outGate)
{
    if (s_exclEnabledCount == 0) return 0;   // fast path: no enabled exclusions
    int result = 0;
    uint8_t gate = 0;
    for (int xi = 0; xi < s_exclCount; ++xi) {
        const Exclusion& x = s_excl[xi];
        if (!x.enabled) continue;
        if (!exclInScope(xi, si)) continue;
        if (!exclHasKey(x, key)) continue;
        if (x.action == EXCL_HIDE) return 2;   // HIDE dominates
        if ((uint8_t)x.gateTier > gate) gate = (uint8_t)x.gateTier;
        result = 1;
    }
    outGate = gate;
    return result;
}

// Effective min tier for source `si`'s entry `ei`: max(wmin, source floor) plus
// any GATE raise; TIER_HIDDEN if HIDE-excluded. This is the SINGLE source of the
// per-(key,source) inclusion decision — called identically from both rebuildView
// passes and from dictGet, so the count/fill passes can never disagree.
static inline uint8_t effectiveMinTier(int si, int ei)
{
    uint8_t eff = s_src[si].wmin[ei];
    if ((uint8_t)s_src[si].floor > eff) eff = (uint8_t)s_src[si].floor;
    if (s_exclEnabledCount > 0) {
        uint8_t gate = 0;
        int cls = exclClassify(si, s_src[si].key[ei], gate);
        if (cls == 2) return TIER_HIDDEN;      // HIDE
        if (cls == 1 && gate > eff) eff = gate; // GATE raises effective min
    }
    return eff;
}

// True if source `si`'s entry `ei` is included at the active tier (passes the tier
// floor and is not HIDE-excluded).
static inline bool entryPassesTier(int si, int ei)
{
    uint8_t eff = effectiveMinTier(si, ei);
    return eff != TIER_HIDDEN && eff <= (uint8_t)s_tier;
}

static bool rebuildView()
{
    freeView();
    if (s_srcCount == 0) return true;

    // Cursor per source over its sorted key array.
    int cur[MAX_SOURCES];
    for (int i = 0; i < s_srcCount; ++i) cur[i] = s_src[i].enabled ? 0 : s_src[i].count;

    // ---- Pass 1: count distinct keys (N) and total contributors (M). ----
    int N = 0, M = 0;
    {
        int c[MAX_SOURCES];
        memcpy(c, cur, sizeof(int) * s_srcCount);
        for (;;) {
            // Find the smallest current key among all sources.
            const char* best = nullptr;
            for (int i = 0; i < s_srcCount; ++i) {
                if (c[i] >= s_src[i].count) continue;
                const char* k = s_src[i].key[c[i]];
                if (!best || strcmp(k, best) < 0) best = k;
            }
            if (!best) break;
            // Advance every source matching `best`, counting passing contributors.
            int contrib = 0;
            for (int i = 0; i < s_srcCount; ++i) {
                while (c[i] < s_src[i].count && strcmp(s_src[i].key[c[i]], best) == 0) {
                    if (entryPassesTier(i, c[i])) ++contrib;
                    ++c[i];
                }
            }
            if (contrib > 0) { ++N; M += contrib; }
        }
    }

    if (N == 0) { s_count = 0; return true; }

    s_viewKey      = static_cast<const char**>(heap_caps_malloc(sizeof(char*)   * N, MALLOC_CAP_SPIRAM));
    s_contribBase  = static_cast<uint32_t*>   (heap_caps_malloc(sizeof(uint32_t)* N, MALLOC_CAP_SPIRAM));
    s_contribCount = static_cast<uint8_t*>    (heap_caps_malloc(sizeof(uint8_t) * N, MALLOC_CAP_SPIRAM));
    s_cSrc         = static_cast<uint16_t*>   (heap_caps_malloc(sizeof(uint16_t)* M, MALLOC_CAP_SPIRAM));
    s_cOff         = static_cast<uint32_t*>   (heap_caps_malloc(sizeof(uint32_t)* M, MALLOC_CAP_SPIRAM));
    if (!s_viewKey || !s_contribBase || !s_contribCount || !s_cSrc || !s_cOff) {
        setStatus("PSRAM alloc failed (view)");
        freeView();
        return false;
    }

    // ---- Pass 2: fill. ----
    int ni = 0, mi = 0;
    for (;;) {
        const char* best = nullptr;
        for (int i = 0; i < s_srcCount; ++i) {
            if (cur[i] >= s_src[i].count) continue;
            const char* k = s_src[i].key[cur[i]];
            if (!best || strcmp(k, best) < 0) best = k;
        }
        if (!best) break;

        uint32_t base = (uint32_t)mi;
        int contrib = 0;
        const char* keyPtr = nullptr;
        // Sources are in priority order, so this naturally records contributors
        // high->low priority.
        for (int i = 0; i < s_srcCount; ++i) {
            while (cur[i] < s_src[i].count && strcmp(s_src[i].key[cur[i]], best) == 0) {
                if (entryPassesTier(i, cur[i])) {
                    if (!keyPtr) keyPtr = s_src[i].key[cur[i]];   // first (highest-priority) contributor
                    s_cSrc[mi] = (uint16_t)i;
                    s_cOff[mi] = s_src[i].dataOff[cur[i]];
                    ++mi; ++contrib;
                }
                ++cur[i];
            }
        }
        if (contrib > 0) {
            s_viewKey[ni]      = keyPtr;
            s_contribBase[ni]  = base;
            s_contribCount[ni] = (uint8_t)contrib;
            ++ni;
        }
    }

    s_count = ni;
    return true;
}

// ----------------------------------------------------------------------------
// Exclusion files: discovery, parse, NVS toggle persistence.
//
// .excl format: leading `#`-prefixed directive lines `# action: hide` or
// `# action: gate:<tier>` and `# scope: all` or `# scope: id1,id2`; remaining
// non-`#` lines are keys (normalised on load). NVS toggle key "x_<stem>".
// ----------------------------------------------------------------------------

// Persists one exclusion's enabled flag to NVS (namespace "dict", key "x_<stem>").
static void persistExclusion(const Exclusion& x)
{
    Preferences p;
    if (!p.begin("dict", false)) return;
    char k[16]; snprintf(k, sizeof(k), "x_%s", x.stem);
    p.putBool(k, x.enabled);
    p.end();
}

// Loads NVS enabled-flag overrides for every exclusion (defaults already in place).
static void loadExclusionSettings()
{
    Preferences p;
    if (!p.begin("dict", true)) return;
    for (int i = 0; i < s_exclCount; ++i) {
        char k[16]; snprintf(k, sizeof(k), "x_%s", s_excl[i].stem);
        if (p.isKey(k)) s_excl[i].enabled = p.getBool(k, s_excl[i].enabled);
    }
    p.end();
}

// Parses one directive value into an exclusion. `key` is the directive name
// ("action"/"scope"); `val` its value.
static void applyExclDirective(Exclusion& x, const String& key, const String& val)
{
    if (key == "action") {
        if (val.startsWith("gate")) {
            x.action = EXCL_GATE;
            int colon = val.indexOf(':');
            x.gateTier = (colon >= 0) ? tierFromName(val.substring(colon + 1).c_str()) : TIER_FULL;
        } else {
            x.action = EXCL_HIDE;   // "hide" (default)
        }
    } else if (key == "scope") {
        if (val == "all") {
            x.scopeAll = true;
        } else {
            x.scopeAll = false;
            x.scopeCount = 0;
            // Comma-separated source ids.
            int start = 0;
            while (start < (int)val.length() && x.scopeCount < MAX_EXCL_SCOPE) {
                int comma = val.indexOf(',', start);
                String id = (comma < 0) ? val.substring(start) : val.substring(start, comma);
                id.trim();
                if (id.length() > 0 && id.length() < 16) {
                    strncpy(x.scope[x.scopeCount], id.c_str(), 15);
                    x.scope[x.scopeCount][15] = 0;
                    ++x.scopeCount;
                }
                if (comma < 0) break;
                start = comma + 1;
            }
        }
    }
}

// qsort comparator over char* key strings.
static int cmpKeyPtr(const void* a, const void* b)
{
    return strcmp(*(const char* const*)a, *(const char* const*)b);
}

// Loads one .excl file into s_excl. Returns true on success.
static bool loadOneExclusion(fs::FS* fs, const char* path, const char* baseName)
{
    if (s_exclCount >= MAX_EXCLUSIONS) return false;

    // Derive the stem (without ".excl") for the NVS key; guard the 15-char limit.
    char stem[16];
    size_t bn = strlen(baseName);
    size_t copy = (bn >= sizeof(stem)) ? sizeof(stem) - 1 : bn;
    memcpy(stem, baseName, copy); stem[copy] = 0;
    char* dot = strrchr(stem, '.');
    if (dot) *dot = 0;
    if (stem[0] == 0 || strlen(stem) > 13) return false;   // "x_<stem>" within 15 chars

    File f = fs->open(path, FILE_READ);
    if (!f) return false;

    Exclusion& x = s_excl[s_exclCount];
    strncpy(x.stem, stem, sizeof(x.stem) - 1); x.stem[sizeof(x.stem) - 1] = 0;
    x.file = baseName;
    x.enabled = true;
    x.action = EXCL_HIDE;
    x.gateTier = TIER_FULL;
    x.scopeAll = true;
    x.scopeCount = 0;
    x.keys = nullptr;
    x.keyCount = 0;

    // Key-pointer buffer in PSRAM, sized for the per-file cap. Only the first
    // keyCount slots are ever read (freeExclusions frees all of them + the array).
    char** keys = static_cast<char**>(heap_caps_malloc(sizeof(char*) * MAX_EXCL_KEYS, MALLOC_CAP_SPIRAM));
    if (!keys) { f.close(); return false; }
    int kc = 0;

    while (f.available()) {
        String line = f.readStringUntil('\n');
        line.trim();
        if (line.length() == 0) continue;
        if (line.startsWith("#")) {
            // Directive: "# action: ..." / "# scope: ...".
            String body = line.substring(1); body.trim();
            int colon = body.indexOf(':');
            if (colon < 0) continue;
            String dk = body.substring(0, colon); dk.trim(); dk.toLowerCase();
            String dv = body.substring(colon + 1); dv.trim(); dv.toLowerCase();
            applyExclDirective(x, dk, dv);
            continue;
        }
        // A key line: normalise and store.
        if (kc >= MAX_EXCL_KEYS) continue;
        char norm[96];
        dictNormalizeKey(line.c_str(), norm, sizeof(norm));
        if (norm[0] == 0) continue;
        size_t len = strlen(norm);
        char* kp = static_cast<char*>(heap_caps_malloc(len + 1, MALLOC_CAP_SPIRAM));
        if (!kp) continue;
        memcpy(kp, norm, len + 1);
        keys[kc++] = kp;
    }
    f.close();

    if (kc == 0) { heap_caps_free(keys); return false; }   // nothing to exclude

    // Sort for binary search (exclHasKey).
    qsort(keys, kc, sizeof(char*), cmpKeyPtr);
    x.keys = keys;
    x.keyCount = kc;
    ++s_exclCount;
    return true;
}

// Scans /dicts/exclude on `fs` for *.excl files and loads each.
static void scanExclusions(fs::FS* fs)
{
    File dir = fs->open("/dicts/exclude");
    if (!dir || !dir.isDirectory()) { if (dir) dir.close(); return; }
    for (File entry = dir.openNextFile(); entry; entry = dir.openNextFile()) {
        if (!entry.isDirectory()) {
            const char* nm = entry.name();
            const char* slash = strrchr(nm, '/');
            const char* base = slash ? slash + 1 : nm;
            size_t len = strlen(base);
            if (len > 5 && strcasecmp(base + len - 5, ".excl") == 0) {
                // Skip a same-named exclusion already loaded (e.g. flash then SD).
                bool dup = false;
                for (int i = 0; i < s_exclCount; ++i) {
                    if (strcasecmp(s_excl[i].file.c_str(), base) == 0) { dup = true; break; }
                }
                if (!dup) {
                    char path[80];
                    snprintf(path, sizeof(path), "/dicts/exclude/%s", base);
                    loadOneExclusion(fs, path, base);
                }
            }
        }
        entry.close();
    }
    dir.close();
}

// ----------------------------------------------------------------------------
// Lifecycle.
// ----------------------------------------------------------------------------
static bool mountSD()
{
    SD_MMC.setPins(SD_CLK, SD_CMD, SD_D0, SD_D1, SD_D2, SD_D3);
    return SD_MMC.begin("/sdcard", false, false, SDMMC_FREQ_DEFAULT, 5) &&
           SD_MMC.cardType() != CARD_NONE;
}

bool dictBegin(DictTier tier)
{
    s_tier = tier;
    freeAllSources();
    freeExclusions();

    bool flashOk = LittleFS.begin(false);
    bool sdOk    = mountSD();

    // Flash first (so flash sources own the canonical id), then SD (shadows).
    if (flashOk) {
        scanDir(&LittleFS, false);
    } else {
        setStatus("Flash FS mount failed");
    }
    if (sdOk) {
        scanDir(&SD_MMC, true);
    }

    // Exclusion files (flash first, SD adds any not already present).
    if (flashOk) scanExclusions(&LittleFS);
    if (sdOk)    scanExclusions(&SD_MMC);

    if (s_srcCount == 0) {
        s_loaded = false;
        setStatus("Built-in set");
        return false;
    }

    // Default priorities (base lowest), then NVS overrides, then sort.
    assignDefaultPriorities();
    loadSourceSettings();
    sortByPriority();

    // Exclusion enabled-flag overrides (defaults: enabled), then cache the count.
    loadExclusionSettings();
    recountEnabledExclusions();

    if (!rebuildView()) { s_loaded = false; return false; }

    s_loaded = true;
    char st[64];
    snprintf(st, sizeof(st), "%d dict%s: %d words (%s)",
             s_srcCount, s_srcCount == 1 ? "" : "s", s_count, tierName(tier));
    setStatus(st);
    return true;
}

bool dictSetTier(DictTier tier)
{
    if (!s_loaded) return false;
    s_tier = tier;
    if (!rebuildView()) return false;
    char st[64];
    snprintf(st, sizeof(st), "%d dict%s: %d words (%s)",
             s_srcCount, s_srcCount == 1 ? "" : "s", s_count, tierName(tier));
    setStatus(st);
    return true;
}

DictTier dictTier() { return s_tier; }
const char* dictStatus() { return s_status; }
int dictCount() { return s_loaded ? s_count : WORD_COUNT; }

const char* dictTerm(int i)
{
    if (i < 0 || i >= dictCount()) return "";
    return s_loaded ? s_viewKey[i] : WORDS[i].term;
}

static uint8_t posCodeFromName(const char* pos)
{
    if (!pos) return 4;
    if (!strcmp(pos, "noun")) return 0;
    if (!strcmp(pos, "verb")) return 1;
    if (!strcmp(pos, "adjective")) return 2;
    if (!strcmp(pos, "adverb")) return 3;
    return 4;
}

bool dictGet(int i, DictEntry& e)
{
    if (i < 0 || i >= dictCount()) return false;
    e.meanings.clear();

    if (!s_loaded) {
        e.term = WORDS[i].term;
        Meaning m;
        m.minTier = 0; m.maxTier = 3;   // embedded words are always visible
        m.posCode = posCodeFromName(WORDS[i].pos);
        m.def = WORDS[i].def;
        m.example = WORDS[i].example;
        e.meanings.push_back(m);
        return true;
    }

    String display = "";
    uint32_t base = s_contribBase[i];
    uint8_t  cnt  = s_contribCount[i];
    const char* key = s_viewKey[i];

    for (uint8_t c = 0; c < cnt; ++c) {
        int      si  = s_cSrc[base + c];
        uint32_t off = s_cOff[base + c];
        Source&  s   = s_src[si];
        if (!s.dat) continue;

        // Per-(key,source) GATE raise (HIDE contributors were dropped from the
        // view in rebuildView, so only GATE can apply here).
        uint8_t gate = 0;
        if (s_exclEnabledCount > 0) exclClassify(si, key, gate);

        s.dat.seek(off);

        // v4 .dat entry: [u8 dispLen][display] then [u8 nMeanings] x meanings.
        uint8_t dispLen = 0; s.dat.read(&dispLen, 1);
        String disp = readField(s.dat, dispLen);
        if (display.length() == 0) display = disp;   // first (highest-priority) contributor wins display

        uint8_t n = 0; s.dat.read(&n, 1);
        for (uint8_t k = 0; k < n; ++k) {
            uint8_t  tierRange = 0; s.dat.read(&tierRange, 1);
            uint8_t  pc = 0;        s.dat.read(&pc, 1);
            uint16_t dl = 0;        s.dat.read(reinterpret_cast<uint8_t*>(&dl), 2);
            String def = readField(s.dat, dl);
            uint16_t el = 0;        s.dat.read(reinterpret_cast<uint8_t*>(&el), 2);
            String ex = readField(s.dat, el);

            uint8_t minT = tierRange & 3;
            uint8_t maxT = (tierRange >> 2) & 3;

            // Effective min = max(source floor, meaning minTier, GATE tier).
            uint8_t eff = minT;
            if ((uint8_t)s.floor > eff) eff = (uint8_t)s.floor;
            if (gate > eff) eff = gate;
            if (eff <= (uint8_t)s_tier && (uint8_t)s_tier <= maxT) {
                Meaning m;
                m.minTier = minT; m.maxTier = maxT;
                m.posCode = pc;
                m.def = def;
                m.example = ex;
                e.meanings.push_back(m);
            }
        }

        if (s.mode == MODE_OVERRIDE) break;   // stop lower-priority sources for this key
    }

    e.term = display;
    return !e.meanings.empty();
}

int dictFind(const char* term)
{
    int lo = 0, hi = dictCount() - 1;
    while (lo <= hi) {
        int mid = (lo + hi) / 2;
        int c = strcmp(dictTerm(mid), term);
        if (c == 0) return mid;
        if (c < 0) lo = mid + 1; else hi = mid - 1;
    }
    return -1;
}

int dictLowerBound(const char* key)
{
    int lo = 0, hi = dictCount();
    while (lo < hi) {
        int mid = (lo + hi) / 2;
        if (strcmp(dictTerm(mid), key) < 0) lo = mid + 1; else hi = mid;
    }
    return lo;
}

const char* posName(uint8_t posCode)
{
    switch (posCode) {
        case 0: return "noun";
        case 1: return "verb";
        case 2: return "adjective";
        case 3: return "adverb";
        default: return "";
    }
}

// ----------------------------------------------------------------------------
// Source-settings API (priority order = array order after sortByPriority).
// ----------------------------------------------------------------------------
int dictSourceCount() { return s_loaded ? s_srcCount : 0; }

bool dictGetSource(int orderIdx, DictSourceInfo& out)
{
    if (orderIdx < 0 || orderIdx >= s_srcCount) return false;
    const Source& s = s_src[orderIdx];
    out.name     = s.name;
    out.enabled  = s.enabled;
    out.priority = s.priority;
    out.mode     = s.mode;
    out.floor    = s.floor;
    out.onSD     = s.onSD;
    return true;
}

void dictSetSourceEnabled(int orderIdx, bool enabled)
{
    if (orderIdx < 0 || orderIdx >= s_srcCount) return;
    s_src[orderIdx].enabled = enabled;
    persistSource(s_src[orderIdx]);
    rebuildView();
}

void dictSetSourceMode(int orderIdx, DictMode mode)
{
    if (orderIdx < 0 || orderIdx >= s_srcCount) return;
    s_src[orderIdx].mode = mode;
    persistSource(s_src[orderIdx]);
    rebuildView();   // mode affects merge output for shared keys
}

void dictSetSourceFloor(int orderIdx, DictTier floor)
{
    if (orderIdx < 0 || orderIdx >= s_srcCount) return;
    s_src[orderIdx].floor = floor;
    persistSource(s_src[orderIdx]);
    rebuildView();
}

void dictMoveSource(int orderIdx, int dir)
{
    if (orderIdx < 0 || orderIdx >= s_srcCount) return;
    int other = orderIdx + (dir < 0 ? -1 : 1);
    if (other < 0 || other >= s_srcCount) return;   // at an end

    // Swap, then renumber priorities to match array order and persist both.
    Source tmp = s_src[orderIdx];
    s_src[orderIdx] = s_src[other];
    s_src[other] = tmp;
    for (int i = 0; i < s_srcCount; ++i) s_src[i].priority = i;
    persistSource(s_src[orderIdx]);
    persistSource(s_src[other]);
    rebuildView();
}

// ----------------------------------------------------------------------------
// Exclusion API.
// ----------------------------------------------------------------------------
int dictExclusionCount() { return s_loaded ? s_exclCount : 0; }

bool dictGetExclusion(int i, String& fileOut, bool& enabledOut)
{
    if (i < 0 || i >= s_exclCount) return false;
    fileOut    = s_excl[i].file;
    enabledOut = s_excl[i].enabled;
    return true;
}

void dictSetExclusionEnabled(int i, bool enabled)
{
    if (i < 0 || i >= s_exclCount) return;
    s_excl[i].enabled = enabled;
    persistExclusion(s_excl[i]);
    recountEnabledExclusions();
    rebuildView();
}
