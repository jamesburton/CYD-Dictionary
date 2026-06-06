#pragma once

// ==============================================================================
// Dictionary data source (format v2: multiple meanings per word).
//
// Sources, in priority order: SD card override -> built-in LittleFS corpus ->
// embedded fallback (WordData.h, single meaning). The active tier (Everyone or
// Kids) selects which index file is used over the shared dict.dat.
//
// The index lives in PSRAM; meanings are streamed from dict.dat on demand.
// ==============================================================================

#include <Arduino.h>
#include <vector>

enum DictTier { TIER_EVERYONE = 0, TIER_KIDS = 1 };

struct Meaning
{
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

// Switches the active tier by reloading its index over the already-open dict.dat.
// Returns true on success. No-op-returns-false in embedded mode.
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
