#pragma once

// ==============================================================================
// Dictionary data source (format v3: single index + per-meaning tier labels).
//
// Sources, in priority order: SD card override -> built-in LittleFS corpus ->
// embedded fallback (WordData.h, single meaning). One dict.idx holds ALL words
// with a per-word wordMinTier; each meaning carries a tierRange byte encoding
// minTier (bits 1:0) and maxTier (bits 3:2). The active tier drives a filtered
// view built in PSRAM — no re-read of dict.dat on tier switch.
//
// All index arrays live in PSRAM; meanings are streamed from dict.dat on demand.
// ==============================================================================

#include <Arduino.h>
#include <vector>

enum DictTier { TIER_SAFE = 0, TIER_MILD = 1, TIER_TEEN = 2, TIER_FULL = 3 };

// How a source contributes when a key is shared with lower-priority sources.
// MODE_ADDITIVE appends its meanings on top of lower sources; MODE_OVERRIDE
// contributes its own meanings then stops the merge for that key.
enum DictMode { MODE_ADDITIVE = 0, MODE_OVERRIDE = 1 };

// UI-facing snapshot of a loaded source, projected from the internal Source.
// Sources are presented in PRIORITY order (orderIdx 0 = highest priority).
struct DictSourceInfo
{
    String   name;       // display name (from .meta name=)
    bool     enabled;    // included in the merged view when true
    int      priority;   // 0 = highest priority
    DictMode mode;       // additive vs override
    DictTier floor;      // per-source minimum tier (effective min = max(floor, wmin))
    bool     onSD;       // true if backed by the SD card, false for flash
};

struct Meaning
{
    uint8_t minTier;   // minimum tier at which this meaning is shown (0=Safe)
    uint8_t maxTier;   // maximum tier at which this meaning is shown (3=Full)
    uint8_t posCode;   // 0 noun, 1 verb, 2 adjective, 3 adverb, 4 other
    String  def;
    String  example;   // "" if none
};

struct DictEntry
{
    String                term;
    std::vector<Meaning>  meanings;   // frequency-ordered
};

// Loads the dictionary for `tier` from the first available source. Returns true
// if a file-backed corpus loaded; false means the embedded fallback is active.
bool dictBegin(DictTier tier);

// Switches the active tier by rebuilding the filtered word view from the already-
// loaded full index. No re-read of dict.dat needed. Returns true on success.
// No-op-returns-false in embedded mode.
bool dictSetTier(DictTier tier);

// The currently active tier.
DictTier dictTier();

// Diagnostic string, e.g. "Flash (Everyone): 60123 words".
const char* dictStatus();

// Number of words in the active source.
int dictCount();

// Null-terminated lowercase term for index i. No I/O; valid for the session.
const char* dictTerm(int i);

// Fills a full entry (term + meanings) for index i. Returns false if out of range.
bool dictGet(int i, DictEntry& e);

// Exact (lowercase) term lookup -> index, or -1.
int dictFind(const char* term);

// First index whose term is >= key (lowercase). Bounds prefix scans.
int dictLowerBound(const char* key);

// Human-readable part of speech for a posCode ("noun", "verb", ...).
const char* posName(uint8_t posCode);

// ----------------------------------------------------------------------------
// Source-settings API. Sources are addressed by their position in PRIORITY
// order (orderIdx 0 = highest priority); mutating priority via dictMoveSource
// renumbers the rest. Each mutator persists to NVS and rebuilds the merged view.
// ----------------------------------------------------------------------------

// Number of loaded sources (0 in embedded fallback mode).
int  dictSourceCount();

// Fills `out` for the source at priority position `orderIdx`. Returns false if
// out of range.
bool dictGetSource(int orderIdx, DictSourceInfo& out);

// Enables/disables the source at `orderIdx` (rebuilds the view, persists).
void dictSetSourceEnabled(int orderIdx, bool enabled);

// Sets the additive/override mode for the source at `orderIdx`.
void dictSetSourceMode(int orderIdx, DictMode mode);

// Sets the per-source tier floor for the source at `orderIdx`.
void dictSetSourceFloor(int orderIdx, DictTier floor);

// Moves the source at `orderIdx` in priority: dir -1 raises priority (towards 0),
// dir +1 lowers it. Swaps with the neighbour, renumbers, re-sorts, persists,
// rebuilds the view. No-op at the ends.
void dictMoveSource(int orderIdx, int dir);

// Normalise a query or headword to a search key: lowercase, strip accents to ASCII,
// keep a-z 0-9 space '-', drop apostrophes, collapse runs of spaces, trim.
// MUST match the Python implementation in tools/normalize.py.
void dictNormalizeKey(const char* in, char* out, size_t cap);

// ----------------------------------------------------------------------------
// Runtime exclusion files (.excl). Each file under /dicts/exclude/ on LittleFS
// or SD reclassifies a set of keys: HIDE removes them from the merged view, GATE
// raises their effective min tier (so they only show at the gate tier and above).
// Files are addressed by load order. Each mutator persists to NVS and rebuilds
// the merged view.
// ----------------------------------------------------------------------------

// Number of loaded exclusion files (0 if none / embedded fallback).
int dictExclusionCount();

// Fills the file name and enabled flag for the exclusion at index `i`. Returns
// false if out of range.
bool dictGetExclusion(int i, String& fileOut, bool& enabledOut);

// Enables/disables the exclusion at index `i` (rebuilds the view, persists).
void dictSetExclusionEnabled(int i, bool enabled);
