#include "Dict.h"
#include "WordData.h"
#include "DisplayConfig.hpp"

#include <FS.h>
#include <SD_MMC.h>
#include <LittleFS.h>
#include <esp_heap_caps.h>
#include <string.h>

// ----------------------------------------------------------------------------
// Source state. The dictionary is loaded from the first available of:
//   1. SD card    (optional override - swap dictionaries without reflashing)
//   2. LittleFS   (built-in full corpus, flashed with `pio run -t uploadfs`)
//   3. embedded   (tiny child-friendly set in WordData.h, last resort)
//
// The index lives in PSRAM (term blob + offset arrays); definitions are streamed
// from the source filesystem on demand. dict.idx/dict.dat have the same on-disk
// format on SD and LittleFS, so one loader serves both.
// ----------------------------------------------------------------------------
static bool          s_loaded   = false;
static uint8_t*      s_idx      = nullptr;   // entire dict.idx file (PSRAM)
static const char**  s_term     = nullptr;   // term pointers into s_idx (PSRAM)
static uint32_t*     s_dataOff  = nullptr;   // dict.dat offset per entry (PSRAM)
static int           s_count    = 0;
static File          s_dat;                  // open handle to dict.dat
static char          s_status[64] = "Built-in set";

static void setStatus(const char* msg) { strncpy(s_status, msg, sizeof(s_status) - 1); s_status[sizeof(s_status) - 1] = 0; }

// Reads `len` bytes from dict.dat (at the current position) into a String,
// clamping to the scratch buffer.
static String readField(uint32_t len)
{
    static char buf[680];   // def<=600, example<=400, pos<=255 (see generator)
    if (len > sizeof(buf) - 1) len = sizeof(buf) - 1;
    int n = s_dat.read(reinterpret_cast<uint8_t*>(buf), len);
    if (n < 0) n = 0;
    buf[n] = 0;
    return String(buf);
}

// Loads dict.idx + dict.dat from a mounted filesystem. `tag` ("SD"/"Flash")
// prefixes any status message. Returns true on success; on failure sets a
// specific status and leaves s_loaded false so the caller can try the next source.
static bool loadFromFS(fs::FS& fs, const char* tag)
{
    char st[64];

    File f = fs.open("/dict.idx", FILE_READ);
    if (!f) { snprintf(st, sizeof(st), "%s ok, dict.idx missing", tag); setStatus(st); return false; }

    size_t sz = f.size();
    uint8_t* idx = static_cast<uint8_t*>(heap_caps_malloc(sz, MALLOC_CAP_SPIRAM));
    if (!idx) { snprintf(st, sizeof(st), "%s ok, PSRAM alloc failed", tag); setStatus(st); f.close(); return false; }

    size_t got = f.read(idx, sz);   // one big read; per-record reads would be slow
    f.close();
    if (got != sz) { snprintf(st, sizeof(st), "%s ok, dict.idx short read", tag); setStatus(st); heap_caps_free(idx); return false; }
    if (memcmp(idx, "DIDX", 4) != 0) { snprintf(st, sizeof(st), "%s ok, dict.idx bad magic", tag); setStatus(st); heap_caps_free(idx); return false; }

    uint32_t version = 0, count = 0;
    memcpy(&version, idx + 4, 4);
    memcpy(&count,   idx + 8, 4);

    const char** term    = static_cast<const char**>(heap_caps_malloc(sizeof(char*) * count, MALLOC_CAP_SPIRAM));
    uint32_t*    dataOff = static_cast<uint32_t*>(heap_caps_malloc(sizeof(uint32_t) * count, MALLOC_CAP_SPIRAM));
    if (!term || !dataOff) {
        snprintf(st, sizeof(st), "%s ok, PSRAM alloc failed", tag); setStatus(st);
        heap_caps_free(idx); heap_caps_free(term); heap_caps_free(dataOff);
        return false;
    }

    File dat = fs.open("/dict.dat", FILE_READ);
    if (!dat) {
        snprintf(st, sizeof(st), "%s ok, dict.dat missing", tag); setStatus(st);
        heap_caps_free(idx); heap_caps_free(term); heap_caps_free(dataOff);
        return false;
    }

    // Each record: [u32 dataOffset][term bytes][0x00]. Term pointers index into
    // idx, so the C-strings come for free.
    size_t p = 12;
    for (uint32_t i = 0; i < count; ++i) {
        uint32_t off;
        memcpy(&off, idx + p, 4);
        p += 4;
        dataOff[i] = off;
        term[i] = reinterpret_cast<const char*>(idx + p);
        while (idx[p] != 0) ++p;   // skip the term
        ++p;                        // skip the null terminator
    }

    s_idx     = idx;
    s_term    = term;
    s_dataOff = dataOff;
    s_count   = static_cast<int>(count);
    s_dat     = dat;
    s_loaded  = true;

    snprintf(st, sizeof(st), "%s: %d words", tag, s_count);
    setStatus(st);
    return true;
}

bool dictBegin()
{
    // 1. SD card override, if a card with a dictionary is present.
    SD_MMC.setPins(SD_CLK, SD_CMD, SD_D0, SD_D1, SD_D2, SD_D3);
    if (SD_MMC.begin("/sdcard", false, false, SDMMC_FREQ_DEFAULT, 5) &&
        SD_MMC.cardType() != CARD_NONE) {
        if (loadFromFS(SD_MMC, "SD")) return true;
        SD_MMC.end();   // card present but no usable dictionary; fall through
    }

    // 2. Built-in corpus in flash (LittleFS).
    if (LittleFS.begin(false)) {
        if (loadFromFS(LittleFS, "Flash")) return true;
    } else {
        setStatus("Flash FS mount failed");
    }

    // 3. Embedded fallback set.
    setStatus("Built-in set");
    return false;
}

const char* dictStatus() { return s_status; }

int dictCount() { return s_loaded ? s_count : WORD_COUNT; }

const char* dictTerm(int i)
{
    if (i < 0 || i >= dictCount()) return "";
    return s_loaded ? s_term[i] : WORDS[i].term;
}

bool dictGet(int i, DictEntry& e)
{
    if (i < 0 || i >= dictCount()) return false;

    if (!s_loaded) {
        e.term    = WORDS[i].term;
        e.pos     = WORDS[i].pos;
        e.def     = WORDS[i].def;
        e.example = WORDS[i].example;
        return true;
    }

    e.term = s_term[i];
    if (!s_dat) return false;
    s_dat.seek(s_dataOff[i]);

    uint8_t posLen = 0;
    s_dat.read(&posLen, 1);
    e.pos = readField(posLen);

    uint16_t defLen = 0;
    s_dat.read(reinterpret_cast<uint8_t*>(&defLen), 2);   // little-endian on ESP32
    e.def = readField(defLen);

    uint16_t exLen = 0;
    s_dat.read(reinterpret_cast<uint8_t*>(&exLen), 2);
    e.example = readField(exLen);
    return true;
}

int dictFind(const char* term)
{
    int lo = 0, hi = dictCount() - 1;
    while (lo <= hi) {
        int mid = (lo + hi) / 2;
        int c = strcmp(dictTerm(mid), term);
        if (c == 0) return mid;
        if (c < 0) lo = mid + 1;
        else       hi = mid - 1;
    }
    return -1;
}

int dictLowerBound(const char* key)
{
    int lo = 0, hi = dictCount();
    while (lo < hi) {
        int mid = (lo + hi) / 2;
        if (strcmp(dictTerm(mid), key) < 0) lo = mid + 1;
        else                                hi = mid;
    }
    return lo;
}
