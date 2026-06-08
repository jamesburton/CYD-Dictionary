#include "Dict.h"
#include "WordData.h"
#include "DisplayConfig.hpp"

#include <FS.h>
#include <SD_MMC.h>
#include <LittleFS.h>
#include <esp_heap_caps.h>
#include <string.h>

// ----------------------------------------------------------------------------
// State.
//
// Full index (ALL words from dict.idx) lives in s_idx (raw file bytes in PSRAM)
// with parallel arrays s_allTerm / s_allOff / s_allMin (also PSRAM). These are
// populated once in dictBegin() and stay until the next dictBegin().
//
// The filtered view (words whose wordMinTier <= s_tier) is kept in s_term /
// s_dataOff (PSRAM). Rebuilt quickly in rebuildTierView() on every tier switch,
// without re-reading dict.dat.
// ----------------------------------------------------------------------------
static bool          s_loaded   = false;
static fs::FS*       s_fs       = nullptr;   // source filesystem (SD_MMC or LittleFS)

// Full index arrays (all entries, never filtered)
static uint8_t*      s_idx      = nullptr;   // raw dict.idx bytes in PSRAM
static const char**  s_allTerm  = nullptr;   // term pointers into s_idx (PSRAM)
static uint32_t*     s_allOff   = nullptr;   // dict.dat offset per entry (PSRAM)
static uint8_t*      s_allMin   = nullptr;   // wordMinTier per entry (PSRAM)
static int           s_allCount = 0;

// Filtered view (entries where s_allMin[i] <= s_tier)
static const char**  s_term     = nullptr;   // PSRAM
static uint32_t*     s_dataOff  = nullptr;   // PSRAM
static int           s_count    = 0;

static File          s_dat;                  // shared dict.dat handle
static DictTier      s_tier     = TIER_FULL;
static char          s_status[64] = "Built-in set";

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

static void setStatus(const char* msg) { strncpy(s_status, msg, sizeof(s_status) - 1); s_status[sizeof(s_status) - 1] = 0; }

// Frees only the filtered-view arrays; leaves the full index intact.
static void freeFiltered()
{
    if (s_term)    { heap_caps_free(s_term);    s_term    = nullptr; }
    if (s_dataOff) { heap_caps_free(s_dataOff); s_dataOff = nullptr; }
    s_count = 0;
}

// Frees everything: full index + filtered view.
static void freeAll()
{
    freeFiltered();
    if (s_allTerm) { heap_caps_free(s_allTerm); s_allTerm = nullptr; }
    if (s_allOff)  { heap_caps_free(s_allOff);  s_allOff  = nullptr; }
    if (s_allMin)  { heap_caps_free(s_allMin);  s_allMin  = nullptr; }
    if (s_idx)     { heap_caps_free(s_idx);      s_idx     = nullptr; }
    s_allCount = 0;
}

static String readField(uint32_t len)
{
    static char buf[300];   // def<=240, example<=160 (see generator)
    if (len > sizeof(buf) - 1) len = sizeof(buf) - 1;
    int n = s_dat.read(reinterpret_cast<uint8_t*>(buf), len);
    if (n < 0) n = 0;
    buf[n] = 0;
    return String(buf);
}

// Builds (or rebuilds) the filtered view from the full index for the current
// s_tier. Called after loading the full index and on every tier switch.
static bool rebuildTierView()
{
    freeFiltered();
    if (s_allCount == 0) return true;   // empty corpus is valid

    // Count how many entries pass the filter.
    int filtered = 0;
    for (int i = 0; i < s_allCount; ++i) {
        if (s_allMin[i] <= (uint8_t)s_tier) ++filtered;
    }

    if (filtered > 0) {
        s_term    = static_cast<const char**>(heap_caps_malloc(sizeof(char*)    * filtered, MALLOC_CAP_SPIRAM));
        s_dataOff = static_cast<uint32_t*>  (heap_caps_malloc(sizeof(uint32_t) * filtered, MALLOC_CAP_SPIRAM));
        if (!s_term || !s_dataOff) {
            setStatus("PSRAM alloc failed (filter)");
            freeFiltered();
            return false;
        }
    }

    // Fill in sorted order (s_all* is already sorted, so the filtered subset stays sorted).
    int j = 0;
    for (int i = 0; i < s_allCount; ++i) {
        if (s_allMin[i] <= (uint8_t)s_tier) {
            s_term[j]    = s_allTerm[i];
            s_dataOff[j] = s_allOff[i];
            ++j;
        }
    }
    s_count = filtered;
    return true;
}

// Reads dict.idx from s_fs into PSRAM and builds the full index arrays.
// Version must be 3. Sets a status string and returns false on any error.
static bool loadFullIndex(const char* tag)
{
    char st[64];
    File f = s_fs->open("/dicts/base.idx", FILE_READ);
    if (!f) { snprintf(st, sizeof(st), "%s: base.idx missing", tag); setStatus(st); return false; }

    size_t sz = f.size();
    uint8_t* idx = static_cast<uint8_t*>(heap_caps_malloc(sz, MALLOC_CAP_SPIRAM));
    if (!idx) { snprintf(st, sizeof(st), "%s: PSRAM alloc failed", tag); setStatus(st); f.close(); return false; }
    size_t got = f.read(idx, sz);
    f.close();
    if (got != sz) { snprintf(st, sizeof(st), "%s: idx short read", tag); setStatus(st); heap_caps_free(idx); return false; }
    if (memcmp(idx, "DIDX", 4) != 0) { snprintf(st, sizeof(st), "%s: bad magic", tag); setStatus(st); heap_caps_free(idx); return false; }

    uint32_t version = 0, count = 0;
    memcpy(&version, idx + 4, 4);
    memcpy(&count,   idx + 8, 4);
    if (version != 4) {
        snprintf(st, sizeof(st), "%s: idx version %u", tag, (unsigned)version);
        setStatus(st);
        heap_caps_free(idx);
        return false;
    }

    const char** allTerm = static_cast<const char**>(heap_caps_malloc(sizeof(char*)    * count, MALLOC_CAP_SPIRAM));
    uint32_t*    allOff  = static_cast<uint32_t*>  (heap_caps_malloc(sizeof(uint32_t) * count, MALLOC_CAP_SPIRAM));
    uint8_t*     allMin  = static_cast<uint8_t*>   (heap_caps_malloc(sizeof(uint8_t)  * count, MALLOC_CAP_SPIRAM));
    if (!allTerm || !allOff || !allMin) {
        snprintf(st, sizeof(st), "%s: PSRAM alloc failed", tag); setStatus(st);
        heap_caps_free(idx); heap_caps_free(allTerm); heap_caps_free(allOff); heap_caps_free(allMin);
        return false;
    }

    // Parse entries: [u32 dataOffset][u8 wordMinTier][term\0]
    size_t p = 12;
    for (uint32_t i = 0; i < count; ++i) {
        uint32_t off; memcpy(&off, idx + p, 4); p += 4;
        allOff[i]  = off;
        allMin[i]  = idx[p]; ++p;                                   // wordMinTier byte
        allTerm[i] = reinterpret_cast<const char*>(idx + p);
        while (idx[p] != 0) ++p;
        ++p;   // skip NUL terminator
    }

    freeAll();
    s_idx = idx; s_allTerm = allTerm; s_allOff = allOff; s_allMin = allMin;
    s_allCount = static_cast<int>(count);
    return true;
}

// Opens the shared dict.dat from SD (if a card has it) else LittleFS. Sets s_fs.
static bool openSource()
{
    SD_MMC.setPins(SD_CLK, SD_CMD, SD_D0, SD_D1, SD_D2, SD_D3);
    if (SD_MMC.begin("/sdcard", false, false, SDMMC_FREQ_DEFAULT, 5) &&
        SD_MMC.cardType() != CARD_NONE) {
        File d = SD_MMC.open("/dicts/base.dat", FILE_READ);
        if (d) { s_dat = d; s_fs = &SD_MMC; return true; }
        SD_MMC.end();
    }
    if (LittleFS.begin(false)) {
        File d = LittleFS.open("/dicts/base.dat", FILE_READ);
        if (d) { s_dat = d; s_fs = &LittleFS; return true; }
        setStatus("Flash: base.dat missing");
    } else {
        setStatus("Flash FS mount failed");
    }
    return false;
}

bool dictBegin(DictTier tier)
{
    s_tier = tier;
    if (!openSource()) { s_loaded = false; setStatus("Built-in set"); return false; }

    const char* srcTag = (s_fs == &SD_MMC) ? "SD" : "Flash";
    if (!loadFullIndex(srcTag)) { s_loaded = false; return false; }

    if (!rebuildTierView()) { s_loaded = false; return false; }

    s_loaded = true;
    char st[64];
    snprintf(st, sizeof(st), "%s: %d words (%s)", srcTag, s_count, tierName(tier));
    setStatus(st);
    return true;
}

bool dictSetTier(DictTier tier)
{
    if (!s_loaded || !s_fs) return false;
    s_tier = tier;
    if (!rebuildTierView()) return false;
    char st[64];
    const char* srcTag = (s_fs == &SD_MMC) ? "SD" : "Flash";
    snprintf(st, sizeof(st), "%s: %d words (%s)", srcTag, s_count, tierName(tier));
    setStatus(st);
    return true;
}

DictTier dictTier() { return s_tier; }
const char* dictStatus() { return s_status; }
int dictCount() { return s_loaded ? s_count : WORD_COUNT; }

const char* dictTerm(int i)
{
    if (i < 0 || i >= dictCount()) return "";
    return s_loaded ? s_term[i] : WORDS[i].term;
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

    if (!s_dat) return false;
    s_dat.seek(s_dataOff[i]);

    // v4: read display headword before nMeanings.
    uint8_t dispLen = 0; s_dat.read(&dispLen, 1);
    e.term = readField(dispLen);

    uint8_t n = 0;
    s_dat.read(&n, 1);

    for (uint8_t k = 0; k < n; ++k) {
        // Read ALL fields first (keeps the file pointer in sync even for filtered meanings).
        uint8_t tierRange = 0; s_dat.read(&tierRange, 1);
        uint8_t pc = 0;        s_dat.read(&pc, 1);
        uint16_t dl = 0;       s_dat.read(reinterpret_cast<uint8_t*>(&dl), 2);
        String def = readField(dl);
        uint16_t el = 0;       s_dat.read(reinterpret_cast<uint8_t*>(&el), 2);
        String ex = readField(el);

        uint8_t minT = tierRange & 3;
        uint8_t maxT = (tierRange >> 2) & 3;

        // Only include this meaning if the active tier falls within its range.
        if (minT <= (uint8_t)s_tier && (uint8_t)s_tier <= maxT) {
            Meaning m;
            m.minTier = minT; m.maxTier = maxT;
            m.posCode = pc;
            m.def = def;
            m.example = ex;
            e.meanings.push_back(m);
        }
    }
    return true;
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
