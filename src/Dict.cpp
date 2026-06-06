#include "Dict.h"
#include "WordData.h"
#include "DisplayConfig.hpp"

#include <SD_MMC.h>
#include <esp_heap_caps.h>
#include <string.h>

// ----------------------------------------------------------------------------
// SD-backed state. All large buffers live in PSRAM and are never freed (valid
// for the whole session).
// ----------------------------------------------------------------------------
static bool          s_sd       = false;
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

// Loads dict.idx fully into PSRAM and builds the term/offset arrays.
static bool loadIndex()
{
    File f = SD_MMC.open("/dict.idx", FILE_READ);
    if (!f) { setStatus("SD ok, dict.idx missing"); return false; }

    size_t sz = f.size();
    s_idx = static_cast<uint8_t*>(heap_caps_malloc(sz, MALLOC_CAP_SPIRAM));
    if (!s_idx) { setStatus("SD ok, PSRAM alloc failed"); f.close(); return false; }

    size_t got = f.read(s_idx, sz);   // one big read; per-record reads would be slow
    f.close();
    if (got != sz) { setStatus("SD ok, dict.idx short read"); return false; }

    if (memcmp(s_idx, "DIDX", 4) != 0) { setStatus("SD ok, dict.idx bad magic"); return false; }
    uint32_t version = 0, count = 0;
    memcpy(&version, s_idx + 4, 4);
    memcpy(&count,   s_idx + 8, 4);

    s_term    = static_cast<const char**>(heap_caps_malloc(sizeof(char*) * count, MALLOC_CAP_SPIRAM));
    s_dataOff = static_cast<uint32_t*>(heap_caps_malloc(sizeof(uint32_t) * count, MALLOC_CAP_SPIRAM));
    if (!s_term || !s_dataOff) { setStatus("SD ok, PSRAM alloc failed"); return false; }

    // Each record: [u32 dataOffset][term bytes][0x00]. Term pointers index into
    // s_idx, so the C-strings come for free.
    size_t p = 12;
    for (uint32_t i = 0; i < count; ++i) {
        uint32_t off;
        memcpy(&off, s_idx + p, 4);
        p += 4;
        s_dataOff[i] = off;
        s_term[i] = reinterpret_cast<const char*>(s_idx + p);
        while (s_idx[p] != 0) ++p;   // skip the term
        ++p;                          // skip the null terminator
    }
    s_count = static_cast<int>(count);
    return true;
}

bool dictBegin()
{
    SD_MMC.setPins(SD_CLK, SD_CMD, SD_D0, SD_D1, SD_D2, SD_D3);
    if (!SD_MMC.begin("/sdcard", false, false, SDMMC_FREQ_DEFAULT, 5)) {
        setStatus("SD mount failed");
        return false;
    }
    if (SD_MMC.cardType() == CARD_NONE) {
        setStatus("No SD card");
        return false;
    }

    if (!loadIndex()) {
        return false;   // loadIndex() already set a specific status
    }
    s_dat = SD_MMC.open("/dict.dat", FILE_READ);
    if (!s_dat) {
        setStatus("SD ok, dict.dat missing");
        return false;
    }

    s_sd = true;
    char msg[48];
    snprintf(msg, sizeof(msg), "SD: %d words", s_count);
    setStatus(msg);
    return true;
}

bool dictUsingSD() { return s_sd; }

const char* dictStatus() { return s_status; }

int dictCount() { return s_sd ? s_count : WORD_COUNT; }

const char* dictTerm(int i)
{
    if (i < 0 || i >= dictCount()) return "";
    return s_sd ? s_term[i] : WORDS[i].term;
}

bool dictGet(int i, DictEntry& e)
{
    if (i < 0 || i >= dictCount()) return false;

    if (!s_sd) {
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
