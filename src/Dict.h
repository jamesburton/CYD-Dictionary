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
