#include "Dict.h"
#include "WordData.h"
#include "DisplayConfig.hpp"

#include <FS.h>
#include <SD_MMC.h>
#include <LittleFS.h>
#include <esp_heap_caps.h>
#include <string.h>

// ----------------------------------------------------------------------------
// State. Index buffers live in PSRAM and are freed/reloaded on tier change;
// the shared dict.dat handle and source FS stay open across tier switches.
// ----------------------------------------------------------------------------
static bool          s_loaded   = false;
static fs::FS*       s_fs       = nullptr;   // source filesystem (SD_MMC or LittleFS)
static uint8_t*      s_idx      = nullptr;   // current index file (PSRAM)
static const char**  s_term     = nullptr;   // term pointers into s_idx (PSRAM)
static uint32_t*     s_dataOff  = nullptr;   // dict.dat offset per entry (PSRAM)
static int           s_count    = 0;
static File          s_dat;                  // shared dict.dat handle
static DictTier      s_tier     = TIER_EVERYONE;
static char          s_status[64] = "Built-in set";

static const char* tierIdxPath(DictTier t) { return t == TIER_KIDS ? "/dict_kids.idx" : "/dict.idx"; }
static const char* tierName(DictTier t)    { return t == TIER_KIDS ? "Kids" : "Everyone"; }

static void setStatus(const char* msg) { strncpy(s_status, msg, sizeof(s_status) - 1); s_status[sizeof(s_status) - 1] = 0; }

static void freeIndex()
{
    if (s_idx)     { heap_caps_free(s_idx);     s_idx = nullptr; }
    if (s_term)    { heap_caps_free(s_term);    s_term = nullptr; }
    if (s_dataOff) { heap_caps_free(s_dataOff); s_dataOff = nullptr; }
    s_count = 0;
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

// Loads an index file from s_fs into PSRAM, replacing any current index.
static bool loadIndex(const char* idxPath, const char* tag)
{
    char st[64];
    File f = s_fs->open(idxPath, FILE_READ);
    if (!f) { snprintf(st, sizeof(st), "%s: %s missing", tag, idxPath); setStatus(st); return false; }

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
    if (version != 2) { snprintf(st, sizeof(st), "%s: idx version %u", tag, (unsigned)version); setStatus(st); heap_caps_free(idx); return false; }

    const char** term    = static_cast<const char**>(heap_caps_malloc(sizeof(char*) * count, MALLOC_CAP_SPIRAM));
    uint32_t*    dataOff = static_cast<uint32_t*>(heap_caps_malloc(sizeof(uint32_t) * count, MALLOC_CAP_SPIRAM));
    if (!term || !dataOff) {
        snprintf(st, sizeof(st), "%s: PSRAM alloc failed", tag); setStatus(st);
        heap_caps_free(idx); heap_caps_free(term); heap_caps_free(dataOff);
        return false;
    }

    size_t p = 12;
    for (uint32_t i = 0; i < count; ++i) {
        uint32_t off; memcpy(&off, idx + p, 4); p += 4;
        dataOff[i] = off;
        term[i] = reinterpret_cast<const char*>(idx + p);
        while (idx[p] != 0) ++p;
        ++p;
    }

    freeIndex();
    s_idx = idx; s_term = term; s_dataOff = dataOff; s_count = static_cast<int>(count);
    snprintf(st, sizeof(st), "%s: %d words", tag, s_count);
    setStatus(st);
    return true;
}

// Opens the shared dict.dat from SD (if a card has it) else LittleFS. Sets s_fs.
static bool openSource()
{
    SD_MMC.setPins(SD_CLK, SD_CMD, SD_D0, SD_D1, SD_D2, SD_D3);
    if (SD_MMC.begin("/sdcard", false, false, SDMMC_FREQ_DEFAULT, 5) &&
        SD_MMC.cardType() != CARD_NONE) {
        File d = SD_MMC.open("/dict.dat", FILE_READ);
        if (d) { s_dat = d; s_fs = &SD_MMC; return true; }
        SD_MMC.end();
    }
    if (LittleFS.begin(false)) {
        File d = LittleFS.open("/dict.dat", FILE_READ);
        if (d) { s_dat = d; s_fs = &LittleFS; return true; }
        setStatus("Flash: dict.dat missing");
    } else {
        setStatus("Flash FS mount failed");
    }
    return false;
}

bool dictBegin(DictTier tier)
{
    s_tier = tier;
    if (!openSource()) { s_loaded = false; setStatus("Built-in set"); return false; }
    char tag[24]; snprintf(tag, sizeof(tag), "%s (%s)", (s_fs == &SD_MMC ? "SD" : "Flash"), tierName(tier));
    if (!loadIndex(tierIdxPath(tier), tag)) { s_loaded = false; return false; }
    s_loaded = true;
    return true;
}

bool dictSetTier(DictTier tier)
{
    if (!s_loaded || !s_fs) return false;
    char tag[24]; snprintf(tag, sizeof(tag), "%s (%s)", (s_fs == &SD_MMC ? "SD" : "Flash"), tierName(tier));
    if (!loadIndex(tierIdxPath(tier), tag)) return false;
    s_tier = tier;
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
        Meaning m; m.posCode = posCodeFromName(WORDS[i].pos); m.def = WORDS[i].def; m.example = WORDS[i].example;
        e.meanings.push_back(m);
        return true;
    }

    e.term = s_term[i];
    if (!s_dat) return false;
    s_dat.seek(s_dataOff[i]);
    uint8_t n = 0;
    s_dat.read(&n, 1);
    for (uint8_t k = 0; k < n; ++k) {
        Meaning m;
        uint8_t pc = 0; s_dat.read(&pc, 1); m.posCode = pc;
        uint16_t dl = 0; s_dat.read(reinterpret_cast<uint8_t*>(&dl), 2); m.def = readField(dl);
        uint16_t el = 0; s_dat.read(reinterpret_cast<uint8_t*>(&el), 2); m.example = readField(el);
        e.meanings.push_back(m);
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
