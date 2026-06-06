#pragma once

// ==============================================================================
// Dictionary data source.
//
// Provides a uniform word interface backed by either:
//   - the full Wordset corpus on the SD card (dict.idx + dict.dat), or
//   - the embedded child-friendly starter set (WordData.h) as a fallback when no
//     SD card / dictionary files are present.
//
// Terms live resident in PSRAM (loaded once from dict.idx), so prefix search and
// browsing touch no I/O. Definitions are streamed from dict.dat on demand.
// ==============================================================================

#include <Arduino.h>

struct DictEntry
{
    String term;
    String pos;       // part of speech
    String def;       // definition
    String example;   // example sentence ("" if none)
};

// Mounts the SD card and loads the on-disk index into PSRAM. Returns true if the
// SD dictionary is available; false means the embedded set is used instead.
bool dictBegin();

// True when the SD-backed corpus is active (false = embedded fallback).
bool dictUsingSD();

// Human-readable source/diagnostic string, e.g. "SD: 64645 words",
// "No SD card", or "SD ok, dict.idx missing". Shown on the More screen.
const char* dictStatus();

// Number of words in the active source.
int dictCount();

// Null-terminated lowercase term for index i. No I/O; pointer valid for the
// whole session. Returns "" if i is out of range.
const char* dictTerm(int i);

// Fills a full entry (term/pos/def/example) for index i, reading dict.dat in SD
// mode. Returns false if i is out of range.
bool dictGet(int i, DictEntry& e);

// Binary search for an exact (lowercase) term. Returns its index, or -1.
int dictFind(const char* term);

// Index of the first term that is >= key (lowercase). Used to bound prefix scans.
int dictLowerBound(const char* key);
