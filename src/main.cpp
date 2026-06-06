// ==============================================================================
// CYD Dictionary - Freenove ESP32-S3 Display FNK0104 (2.8" 240x320 ILI9341,
// FT6336U capacitive touch). Landscape 320x240. Layout B: search-first with a
// persistent bottom tab bar.
//
// v1: child-friendly word set embedded in flash (WordData.h), no SD required.
// Iteration #2 streams the full Wordset corpus from SD via an on-disk index.
// ==============================================================================

#include <Arduino.h>
#include <Preferences.h>
#include <vector>
#include "LGFX_Setup.hpp"
#include "WordData.h"

static LGFX lcd;
static Preferences prefs;

// ----------------------------------------------------------------------------
// Palette
// ----------------------------------------------------------------------------
static uint16_t C_BG, C_HEADER, C_HEADERTX, C_TEXT, C_SUB, C_KEY, C_KEYTX,
    C_ACCENT, C_TABBAR, C_TABTX, C_TABON, C_ROW, C_ROWLINE, C_WOD, C_WODBORD, C_STAR,
    C_KEYDIM, C_KEYDIMTX;

static void initPalette()
{
    C_BG       = lcd.color565(238, 242, 245);
    C_HEADER   = lcd.color565(44, 122, 123);
    C_HEADERTX = lcd.color565(255, 255, 255);
    C_TEXT     = lcd.color565(31, 41, 51);
    C_SUB      = lcd.color565(110, 130, 140);
    C_KEY      = lcd.color565(255, 255, 255);
    C_KEYTX    = lcd.color565(34, 34, 34);
    C_ACCENT   = lcd.color565(44, 122, 123);
    C_TABBAR   = lcd.color565(31, 41, 51);
    C_TABTX    = lcd.color565(154, 165, 173);
    C_TABON    = lcd.color565(87, 196, 196);
    C_ROW      = lcd.color565(255, 255, 255);
    C_ROWLINE  = lcd.color565(226, 232, 236);
    C_WOD      = lcd.color565(255, 251, 230);
    C_WODBORD  = lcd.color565(240, 216, 105);
    C_STAR     = lcd.color565(245, 190, 60);
    C_KEYDIM   = lcd.color565(214, 220, 224);
    C_KEYDIMTX = lcd.color565(176, 184, 190);
}

// ----------------------------------------------------------------------------
// Screen / layout constants
// ----------------------------------------------------------------------------
enum Screen { SCR_SEARCH, SCR_BROWSE, SCR_SAVED, SCR_MORE, SCR_DEFINITION, SCR_HISTORY };

static const int TABBAR_Y = 210;
static const int TABBAR_H = SCREEN_H - TABBAR_Y;   // 30
static const int HEADER_H = 28;
static const int ROW_H    = 30;
static const int SUG_TOP  = HEADER_H;           // suggestion strip top (28)
static const int SUG_H    = 30;                 // suggestion strip height
static const int PRV_TOP  = SUG_TOP + SUG_H;    // preview area top (58)

static Screen g_screen = SCR_SEARCH;
static Screen g_prevScreen = SCR_SEARCH;    // where DEFINITION/HISTORY returns to

// ----------------------------------------------------------------------------
// State
// ----------------------------------------------------------------------------
static String g_query = "";
static std::vector<int> g_results;          // word indices matching query
static int g_browseOffset = 0;
static int g_savedOffset = 0;
static int g_historyOffset = 0;
static int g_currentWord = -1;              // index shown in DEFINITION
static int g_wotd = 0;

static std::vector<String> g_favs;
static std::vector<String> g_history;
static bool g_valid[26];                    // which next letters can still form a word

// ----------------------------------------------------------------------------
// Touch helpers
// ----------------------------------------------------------------------------
static bool g_wasTouched = false;

struct Tap { bool hit; int x; int y; };

// Returns a tap on the rising edge of a touch (one event per press).
static Tap pollTap()
{
    int32_t x, y;
    bool now = lcd.getTouch(&x, &y);
    Tap t{false, 0, 0};
    if (now && !g_wasTouched) {
        t.hit = true;
        t.x = x;
        t.y = y;
    }
    g_wasTouched = now;
    return t;
}

static bool inRect(const Tap& t, int x, int y, int w, int h)
{
    return t.x >= x && t.x < x + w && t.y >= y && t.y < y + h;
}

// ----------------------------------------------------------------------------
// String helpers
// ----------------------------------------------------------------------------
static String lower(const String& s)
{
    String r = s;
    r.toLowerCase();
    return r;
}

static int findWord(const String& term)
{
    String t = lower(term);
    for (int i = 0; i < WORD_COUNT; ++i) {
        if (t == WORDS[i].term) return i;
    }
    return -1;
}

// ----------------------------------------------------------------------------
// Persistence (NVS)
// ----------------------------------------------------------------------------
static void splitInto(const String& joined, std::vector<String>& out)
{
    out.clear();
    int start = 0;
    while (start < (int)joined.length()) {
        int nl = joined.indexOf('\n', start);
        if (nl < 0) nl = joined.length();
        String item = joined.substring(start, nl);
        if (item.length()) out.push_back(item);
        start = nl + 1;
    }
}

static String joinVec(const std::vector<String>& v)
{
    String s;
    for (auto& it : v) { s += it; s += '\n'; }
    return s;
}

static void loadState()
{
    splitInto(prefs.getString("favs", ""), g_favs);
    splitInto(prefs.getString("hist", ""), g_history);
    uint32_t boot = prefs.getUInt("boot", 0) + 1;
    prefs.putUInt("boot", boot);
    g_wotd = (WORD_COUNT > 0) ? (int)(boot % WORD_COUNT) : 0;
}

static void saveFavs()    { prefs.putString("favs", joinVec(g_favs)); }
static void saveHistory() { prefs.putString("hist", joinVec(g_history)); }

static bool isFav(const String& term)
{
    for (auto& f : g_favs) if (f == term) return true;
    return false;
}

static void toggleFav(const String& term)
{
    for (size_t i = 0; i < g_favs.size(); ++i) {
        if (g_favs[i] == term) { g_favs.erase(g_favs.begin() + i); saveFavs(); return; }
    }
    g_favs.push_back(term);
    saveFavs();
}

static void pushHistory(const String& term)
{
    for (size_t i = 0; i < g_history.size(); ++i) {
        if (g_history[i] == term) { g_history.erase(g_history.begin() + i); break; }
    }
    g_history.insert(g_history.begin(), term);
    while (g_history.size() > 12) g_history.pop_back();
    saveHistory();
}

// ----------------------------------------------------------------------------
// On-screen keyboard model
// ----------------------------------------------------------------------------
enum KeyType { K_CHAR, K_BACK, K_SPACE, K_CLEAR };
struct Key { int x, y, w, h; char c; KeyType type; };
static std::vector<Key> g_keys;

static const int KB_TOP = 122;
static const int KEY_H  = 20;
static const int ROW_PITCH = 22;

static void addRow(const char* letters, int y)
{
    int n = strlen(letters);
    int kw = 30, gap = 2;
    int total = n * kw + (n - 1) * gap;
    int x = (SCREEN_W - total) / 2;
    for (int i = 0; i < n; ++i) {
        g_keys.push_back({x, y, kw, KEY_H, letters[i], K_CHAR});
        x += kw + gap;
    }
}

static void buildKeyboard()
{
    g_keys.clear();
    addRow("qwertyuiop", KB_TOP);
    addRow("asdfghjkl", KB_TOP + ROW_PITCH);

    // Row 3: z x c v b n m + backspace
    {
        const char* letters = "zxcvbnm";
        int n = strlen(letters), kw = 30, gap = 2, bsw = 60;
        int total = n * kw + (n - 1) * gap + gap + bsw;
        int x = (SCREEN_W - total) / 2;
        int y = KB_TOP + ROW_PITCH * 2;
        for (int i = 0; i < n; ++i) {
            g_keys.push_back({x, y, kw, KEY_H, letters[i], K_CHAR});
            x += kw + gap;
        }
        g_keys.push_back({x + gap, y, bsw, KEY_H, 0, K_BACK});
    }

    // Row 4: space + clear
    {
        int y = KB_TOP + ROW_PITCH * 3;
        int spw = 200, clw = 80, gap = 8;
        int total = spw + gap + clw;
        int x = (SCREEN_W - total) / 2;
        g_keys.push_back({x, y, spw, KEY_H, ' ', K_SPACE});
        g_keys.push_back({x + spw + gap, y, clw, KEY_H, 0, K_CLEAR});
    }
}

static void drawKey(const Key& k)
{
    bool dim = (k.type == K_CHAR) && !g_valid[k.c - 'a'];
    uint16_t bg = dim ? C_KEYDIM : C_KEY;
    uint16_t tx = dim ? C_KEYDIMTX : C_KEYTX;
    lcd.fillRoundRect(k.x, k.y, k.w, k.h, 4, bg);
    lcd.setFont(&fonts::Font2);
    lcd.setTextColor(tx, bg);
    lcd.setTextDatum(textdatum_t::middle_center);
    const char* label = nullptr;
    char buf[2] = {0, 0};
    switch (k.type) {
        case K_CHAR:  buf[0] = k.c; label = buf; break;
        case K_BACK:  label = "<-"; break;
        case K_SPACE: label = "space"; break;
        case K_CLEAR: label = "clear"; break;
    }
    lcd.drawString(label, k.x + k.w / 2, k.y + k.h / 2);
    lcd.setTextDatum(textdatum_t::top_left);
}

static void drawKeyboard()
{
    for (auto& k : g_keys) drawKey(k);
}

// Enlarged bubble above a pressed key, for visual feedback.
static void drawKeyPopup(const Key& k)
{
    if (k.type != K_CHAR) return;
    int bw = 38, bh = 38;
    int bx = k.x + k.w / 2 - bw / 2;
    if (bx < 2) bx = 2;
    if (bx + bw > SCREEN_W - 2) bx = SCREEN_W - 2 - bw;
    int by = k.y - bh - 2;
    if (by < SUG_TOP) by = k.y + 2;
    lcd.fillRoundRect(bx, by, bw, bh, 6, C_ACCENT);
    lcd.setFont(&fonts::Font4);
    lcd.setTextColor(C_HEADERTX, C_ACCENT);
    lcd.setTextDatum(textdatum_t::middle_center);
    char b[2] = {k.c, 0};
    lcd.drawString(b, bx + bw / 2, by + bh / 2);
    lcd.setTextDatum(textdatum_t::top_left);
}

// ----------------------------------------------------------------------------
// Shared chrome
// ----------------------------------------------------------------------------
static void drawHeader(const char* title)
{
    lcd.fillRect(0, 0, SCREEN_W, HEADER_H, C_HEADER);
    lcd.setFont(&fonts::Font2);
    lcd.setTextColor(C_HEADERTX, C_HEADER);
    lcd.setTextDatum(textdatum_t::middle_left);
    lcd.drawString(title, 8, HEADER_H / 2);
    lcd.setTextDatum(textdatum_t::top_left);
}

static void drawTabBar(Screen active)
{
    const char* labels[4] = {"Search", "Browse", "Saved", "More"};
    Screen scr[4] = {SCR_SEARCH, SCR_BROWSE, SCR_SAVED, SCR_MORE};
    int tw = SCREEN_W / 4;
    lcd.fillRect(0, TABBAR_Y, SCREEN_W, TABBAR_H, C_TABBAR);
    lcd.setFont(&fonts::Font2);
    lcd.setTextDatum(textdatum_t::middle_center);
    for (int i = 0; i < 4; ++i) {
        bool on = (scr[i] == active);
        if (on) lcd.fillRect(i * tw, TABBAR_Y, tw, 3, C_TABON);
        lcd.setTextColor(on ? C_HEADERTX : C_TABTX, C_TABBAR);
        lcd.drawString(labels[i], i * tw + tw / 2, TABBAR_Y + TABBAR_H / 2 + 1);
    }
    lcd.setTextDatum(textdatum_t::top_left);
}

// Tap handling for the tab bar; returns true if a tab was tapped.
static bool handleTabBar(const Tap& t);

// ----------------------------------------------------------------------------
// Word-wrapped text
// ----------------------------------------------------------------------------
static int drawWrapped(const String& text, int x, int y, int maxw, int lineH)
{
    lcd.setFont(&fonts::Font2);
    lcd.setTextDatum(textdatum_t::top_left);
    String line = "";
    int cy = y;
    int start = 0;
    while (start <= (int)text.length()) {
        int sp = text.indexOf(' ', start);
        String word = (sp < 0) ? text.substring(start) : text.substring(start, sp);
        String trial = line.length() ? line + " " + word : word;
        if (lcd.textWidth(trial) > maxw && line.length()) {
            lcd.drawString(line, x, cy);
            cy += lineH;
            line = word;
        } else {
            line = trial;
        }
        if (sp < 0) break;
        start = sp + 1;
    }
    if (line.length()) { lcd.drawString(line, x, cy); cy += lineH; }
    return cy;
}

// ----------------------------------------------------------------------------
// SEARCH screen
// ----------------------------------------------------------------------------
static void buildResults()
{
    String q = lower(g_query);

    // Which next letters can still lead to a word (for dimming dead keys).
    for (int c = 0; c < 26; ++c) {
        String pre = q + (char)('a' + c);
        bool ok = false;
        for (int i = 0; i < WORD_COUNT; ++i) {
            if (strncmp(WORDS[i].term, pre.c_str(), pre.length()) == 0) { ok = true; break; }
        }
        g_valid[c] = ok;
    }

    g_results.clear();
    if (q.length() == 0) return;
    for (int i = 0; i < WORD_COUNT && (int)g_results.size() < 20; ++i) {
        if (strncmp(WORDS[i].term, q.c_str(), q.length()) == 0) g_results.push_back(i);
    }
}

static void drawSearchField()
{
    lcd.fillRect(0, 0, SCREEN_W, HEADER_H, C_HEADER);
    lcd.fillRoundRect(6, 4, SCREEN_W - 12, HEADER_H - 8, 8, C_KEY);
    lcd.setFont(&fonts::Font2);
    lcd.setTextDatum(textdatum_t::middle_left);
    if (g_query.length()) {
        lcd.setTextColor(C_TEXT, C_KEY);
        lcd.drawString(g_query + "_", 14, HEADER_H / 2);
    } else {
        lcd.setTextColor(C_SUB, C_KEY);
        lcd.drawString("Type a word...", 14, HEADER_H / 2);
    }
    lcd.setTextDatum(textdatum_t::top_left);
}

struct SugChip { int x, y, w, h, idx; };
static std::vector<SugChip> g_sugChips;

// Horizontal strip of tappable word suggestions just below the search field.
static void drawSuggestionStrip()
{
    g_sugChips.clear();
    lcd.fillRect(0, SUG_TOP, SCREEN_W, SUG_H, C_BG);
    lcd.setFont(&fonts::Font2);
    lcd.setTextDatum(textdatum_t::middle_center);
    int x = 6, y = SUG_TOP + 3, h = SUG_H - 6;
    for (size_t i = 0; i < g_results.size() && i < 6; ++i) {
        const char* term = WORDS[g_results[i]].term;
        int w = lcd.textWidth(term) + 16;
        if (x + w > SCREEN_W - 6) break;
        lcd.fillRoundRect(x, y, w, h, h / 2, C_ACCENT);
        lcd.setTextColor(C_HEADERTX, C_ACCENT);
        lcd.drawString(term, x + w / 2, y + h / 2);
        g_sugChips.push_back({x, y, w, h, g_results[i]});
        x += w + 6;
    }
    lcd.setTextDatum(textdatum_t::top_left);
}

// Live preview of the top match (word + part of speech + definition).
static void drawPreview()
{
    int top = PRV_TOP;
    int h = KB_TOP - PRV_TOP - 2;
    lcd.fillRect(0, top, SCREEN_W, h, C_BG);
    lcd.setTextDatum(textdatum_t::top_left);

    if (g_query.length() == 0) {
        lcd.setFont(&fonts::Font2);
        lcd.setTextColor(C_SUB, C_BG);
        lcd.drawString("Type to find a word", 10, top + 6);
        return;
    }
    if (g_results.empty()) {
        lcd.setFont(&fonts::Font2);
        lcd.setTextColor(C_SUB, C_BG);
        lcd.drawString("No words found", 10, top + 6);
        return;
    }
    const Word& w = WORDS[g_results[0]];
    lcd.setFont(&fonts::Font4);
    int tw = lcd.textWidth(w.term);
    lcd.setTextColor(C_TEXT, C_BG);
    lcd.drawString(w.term, 10, top);
    lcd.setFont(&fonts::Font2);
    lcd.setTextColor(C_ACCENT, C_BG);
    lcd.drawString(w.pos, 10 + tw + 8, top + 10);
    lcd.setTextColor(C_TEXT, C_BG);
    drawWrapped(w.def, 10, top + 28, SCREEN_W - 20, 18);
}

static void drawSearchScreen()
{
    lcd.fillScreen(C_BG);
    drawSearchField();
    drawSuggestionStrip();
    drawPreview();
    drawKeyboard();
    drawTabBar(SCR_SEARCH);
}

static void openDefinition(int idx);

static void handleSearch(const Tap& t)
{
    if (handleTabBar(t)) return;

    // Suggestion chips
    for (auto& c : g_sugChips) {
        if (inRect(t, c.x, c.y, c.w, c.h)) { openDefinition(c.idx); return; }
    }

    // Preview area -> open the top match
    if (t.y >= PRV_TOP && t.y < KB_TOP && !g_results.empty()) {
        openDefinition(g_results[0]);
        return;
    }

    // Keyboard
    for (auto& k : g_keys) {
        if (inRect(t, k.x, k.y, k.w, k.h)) {
            if (k.type == K_CHAR && !g_valid[k.c - 'a']) return;   // dead key: ignore
            if (k.type == K_CHAR) drawKeyPopup(k);                 // tactile feedback
            switch (k.type) {
                case K_CHAR:  if (g_query.length() < 24) g_query += k.c; break;
                case K_SPACE: if (g_query.length() < 24) g_query += ' '; break;
                case K_BACK:  if (g_query.length()) g_query.remove(g_query.length() - 1); break;
                case K_CLEAR: g_query = ""; break;
            }
            if (k.type == K_CHAR) delay(110);                      // let the popup show
            buildResults();
            drawSearchField();
            drawSuggestionStrip();
            drawPreview();
            drawKeyboard();
            return;
        }
    }
}

// ----------------------------------------------------------------------------
// Generic scrolling list (Browse / Saved / History)
// ----------------------------------------------------------------------------
static const int LIST_TOP = HEADER_H;
static const int LIST_BTN_W = 36;
static int listVisibleRows()
{
    return (TABBAR_Y - LIST_TOP) / ROW_H;
}

// Draws up to N word rows from an index list, plus up/down buttons. Returns rows shown.
static void drawWordList(const std::vector<int>& idxs, int offset)
{
    int listW = SCREEN_W - LIST_BTN_W;
    int areaH = TABBAR_Y - LIST_TOP;
    lcd.fillRect(0, LIST_TOP, SCREEN_W, areaH, C_BG);

    int rows = listVisibleRows();
    lcd.setFont(&fonts::Font2);
    lcd.setTextDatum(textdatum_t::middle_left);
    if (idxs.empty()) {
        lcd.setTextColor(C_SUB, C_BG);
        lcd.drawString("Nothing here yet", 10, LIST_TOP + 16);
    }
    for (int i = 0; i < rows; ++i) {
        int di = offset + i;
        if (di >= (int)idxs.size()) break;
        int y = LIST_TOP + i * ROW_H;
        const Word& w = WORDS[idxs[di]];
        lcd.fillRect(0, y, listW, ROW_H - 1, C_ROW);
        lcd.setTextColor(C_TEXT, C_ROW);
        lcd.drawString(w.term, 10, y + ROW_H / 2);
        lcd.setTextColor(C_SUB, C_ROW);
        lcd.drawString(w.pos, listW - 70, y + ROW_H / 2);
    }

    // Up / Down buttons on the right
    int bx = SCREEN_W - LIST_BTN_W;
    int halfH = areaH / 2;
    lcd.fillRect(bx, LIST_TOP, LIST_BTN_W, areaH, C_BG);
    lcd.fillRoundRect(bx + 2, LIST_TOP + 2, LIST_BTN_W - 4, halfH - 4, 4, C_ACCENT);
    lcd.fillRoundRect(bx + 2, LIST_TOP + halfH + 2, LIST_BTN_W - 4, halfH - 4, 4, C_ACCENT);
    lcd.setTextColor(C_HEADERTX);
    lcd.setTextDatum(textdatum_t::middle_center);
    lcd.drawString("UP", bx + LIST_BTN_W / 2, LIST_TOP + halfH / 2);
    lcd.drawString("DN", bx + LIST_BTN_W / 2, LIST_TOP + halfH + halfH / 2);
    lcd.setTextDatum(textdatum_t::top_left);
}

// Handles taps on a word list. Writes the chosen word index to *chosen (or -1),
// and updates *offset for scrolling. Returns true if the view needs a redraw.
static bool handleWordList(const Tap& t, const std::vector<int>& idxs, int* offset, int* chosen)
{
    *chosen = -1;
    int areaH = TABBAR_Y - LIST_TOP;
    int bx = SCREEN_W - LIST_BTN_W;
    int halfH = areaH / 2;
    int rows = listVisibleRows();

    if (t.x >= bx) {
        if (t.y < LIST_TOP + halfH) {            // UP
            *offset = max(0, *offset - rows);
        } else {                                 // DOWN
            if (*offset + rows < (int)idxs.size()) *offset += rows;
        }
        return true;
    }
    if (t.y >= LIST_TOP && t.y < TABBAR_Y) {
        int i = (t.y - LIST_TOP) / ROW_H;
        int di = *offset + i;
        if (di < (int)idxs.size()) { *chosen = idxs[di]; }
    }
    return false;
}

// ----------------------------------------------------------------------------
// BROWSE
// ----------------------------------------------------------------------------
static void drawBrowse()
{
    lcd.fillScreen(C_BG);
    drawHeader("Browse A-Z");
    std::vector<int> all;
    for (int i = 0; i < WORD_COUNT; ++i) all.push_back(i);
    drawWordList(all, g_browseOffset);
    drawTabBar(SCR_BROWSE);
}

static void handleBrowse(const Tap& t)
{
    if (handleTabBar(t)) return;
    std::vector<int> all;
    for (int i = 0; i < WORD_COUNT; ++i) all.push_back(i);
    int chosen;
    if (handleWordList(t, all, &g_browseOffset, &chosen)) { drawBrowse(); return; }
    if (chosen >= 0) openDefinition(chosen);
}

// ----------------------------------------------------------------------------
// SAVED
// ----------------------------------------------------------------------------
static std::vector<int> favIndices()
{
    std::vector<int> v;
    for (auto& term : g_favs) { int i = findWord(term); if (i >= 0) v.push_back(i); }
    return v;
}

static void drawSaved()
{
    lcd.fillScreen(C_BG);
    drawHeader("Saved Words");
    drawWordList(favIndices(), g_savedOffset);
    drawTabBar(SCR_SAVED);
}

static void handleSaved(const Tap& t)
{
    if (handleTabBar(t)) return;
    auto v = favIndices();
    int chosen;
    if (handleWordList(t, v, &g_savedOffset, &chosen)) { drawSaved(); return; }
    if (chosen >= 0) openDefinition(chosen);
}

// ----------------------------------------------------------------------------
// HISTORY
// ----------------------------------------------------------------------------
static std::vector<int> historyIndices()
{
    std::vector<int> v;
    for (auto& term : g_history) { int i = findWord(term); if (i >= 0) v.push_back(i); }
    return v;
}

static void drawHistory()
{
    lcd.fillScreen(C_BG);
    drawHeader("Recent Words  (<- More)");
    drawWordList(historyIndices(), g_historyOffset);
    drawTabBar(SCR_MORE);
}

static void handleHistory(const Tap& t)
{
    if (handleTabBar(t)) return;   // tapping a tab leaves history
    auto v = historyIndices();
    int chosen;
    if (handleWordList(t, v, &g_historyOffset, &chosen)) { drawHistory(); return; }
    if (chosen >= 0) openDefinition(chosen);
}

// ----------------------------------------------------------------------------
// MORE
// ----------------------------------------------------------------------------
struct MoreBtn { int x, y, w, h; const char* label; };
static MoreBtn g_moreBtns[3];

static void drawMore()
{
    lcd.fillScreen(C_BG);
    drawHeader("More");

    // Word of the Day card
    int cardX = 8, cardY = HEADER_H + 8, cardW = SCREEN_W - 16, cardH = 60;
    lcd.fillRoundRect(cardX, cardY, cardW, cardH, 8, C_WOD);
    lcd.drawRoundRect(cardX, cardY, cardW, cardH, 8, C_WODBORD);
    lcd.setFont(&fonts::Font2);
    lcd.setTextDatum(textdatum_t::top_left);
    lcd.setTextColor(C_SUB, C_WOD);
    lcd.drawString("WORD OF THE DAY", cardX + 10, cardY + 6);
    lcd.setFont(&fonts::Font4);
    lcd.setTextColor(C_TEXT, C_WOD);
    lcd.drawString(WORDS[g_wotd].term, cardX + 10, cardY + 24);

    // Buttons
    const char* labels[3] = {"Random word", "Recent words", "Recalibrate touch"};
    int bx = 8, bw = SCREEN_W - 16, bh = 30, gap = 8;
    int by = cardY + cardH + 10;
    for (int i = 0; i < 3; ++i) {
        g_moreBtns[i] = {bx, by, bw, bh, labels[i]};
        lcd.fillRoundRect(bx, by, bw, bh, 6, C_ACCENT);
        lcd.setFont(&fonts::Font2);
        lcd.setTextColor(C_HEADERTX, C_ACCENT);
        lcd.setTextDatum(textdatum_t::middle_center);
        lcd.drawString(labels[i], bx + bw / 2, by + bh / 2);
        by += bh + gap;
    }
    lcd.setTextDatum(textdatum_t::top_left);
    drawTabBar(SCR_MORE);
}

// Runs LovyanGFX corner-tap calibration and stores the result to NVS. Works for
// capacitive panels too: it derives the raw->screen transform (rotation/mirror/
// scale) from the taps, so we don't have to guess offset_rotation.
static void recalibrateTouch()
{
    uint16_t calData[8];
    lcd.fillScreen(C_BG);
    lcd.setFont(&fonts::Font2);
    lcd.setTextColor(C_TEXT, C_BG);
    lcd.setTextDatum(textdatum_t::middle_center);
    lcd.drawString("Touch calibration", SCREEN_W / 2, SCREEN_H / 2 - 24);
    lcd.drawString("Tap each corner marker as it appears", SCREEN_W / 2, SCREEN_H / 2);
    lcd.setTextDatum(textdatum_t::top_left);
    delay(1400);
    lcd.calibrateTouch(calData, C_ACCENT, C_BG, 18);
    prefs.putBytes("touchcal", calData, sizeof(calData));
}

static void handleMore(const Tap& t)
{
    if (handleTabBar(t)) return;

    // WotD card
    int cardY = HEADER_H + 8;
    if (t.y >= cardY && t.y < cardY + 60) { openDefinition(g_wotd); return; }

    for (int i = 0; i < 3; ++i) {
        MoreBtn& b = g_moreBtns[i];
        if (inRect(t, b.x, b.y, b.w, b.h)) {
            if (i == 0) {                                   // Random
                openDefinition((int)(esp_random() % WORD_COUNT));
            } else if (i == 1) {                            // Recent
                g_screen = SCR_HISTORY; g_historyOffset = 0; drawHistory();
            } else {                                        // Recalibrate
                recalibrateTouch();
                g_wasTouched = false;
                drawMore();
            }
            return;
        }
    }
}

// ----------------------------------------------------------------------------
// DEFINITION
// ----------------------------------------------------------------------------
static const int DEF_BACK_W = 70;
static const int DEF_STAR_W = 50;

static void drawDefinition()
{
    const Word& w = WORDS[g_currentWord];
    lcd.fillScreen(C_BG);

    // Top bar: Back + Star
    lcd.fillRect(0, 0, SCREEN_W, HEADER_H, C_HEADER);
    lcd.fillRoundRect(6, 4, DEF_BACK_W, HEADER_H - 8, 6, C_ACCENT);
    lcd.setFont(&fonts::Font2);
    lcd.setTextDatum(textdatum_t::middle_center);
    lcd.setTextColor(C_HEADERTX, C_ACCENT);
    lcd.drawString("< Back", 6 + DEF_BACK_W / 2, HEADER_H / 2);

    bool fav = isFav(w.term);
    lcd.fillRoundRect(SCREEN_W - DEF_STAR_W - 6, 4, DEF_STAR_W, HEADER_H - 8, 6,
                      fav ? C_STAR : C_ACCENT);
    lcd.setTextColor(C_HEADERTX);
    lcd.drawString(fav ? "* in" : "+ fav", SCREEN_W - DEF_STAR_W / 2 - 6, HEADER_H / 2);
    lcd.setTextDatum(textdatum_t::top_left);

    // Headword + part of speech
    int y = HEADER_H + 8;
    lcd.setFont(&fonts::Font4);
    lcd.setTextColor(C_TEXT, C_BG);
    lcd.drawString(w.term, 10, y);
    y += 30;
    lcd.setFont(&fonts::Font2);
    lcd.setTextColor(C_ACCENT, C_BG);
    lcd.drawString(w.pos, 10, y);
    y += 22;

    // Definition
    lcd.setTextColor(C_TEXT, C_BG);
    y = drawWrapped(w.def, 10, y, SCREEN_W - 20, 20) + 6;

    // Example
    if (w.example && strlen(w.example)) {
        lcd.setTextColor(C_SUB, C_BG);
        drawWrapped(String("\"") + w.example + "\"", 10, y, SCREEN_W - 20, 20);
    }
}

static void openDefinition(int idx)
{
    if (idx < 0 || idx >= WORD_COUNT) return;
    if (g_screen != SCR_DEFINITION && g_screen != SCR_HISTORY) g_prevScreen = g_screen;
    g_currentWord = idx;
    g_screen = SCR_DEFINITION;
    pushHistory(WORDS[idx].term);
    drawDefinition();
}

static void redrawCurrent();

static void handleDefinition(const Tap& t)
{
    // Back
    if (inRect(t, 6, 4, DEF_BACK_W, HEADER_H - 8)) {
        g_screen = g_prevScreen;
        redrawCurrent();
        return;
    }
    // Star toggle
    if (inRect(t, SCREEN_W - DEF_STAR_W - 6, 4, DEF_STAR_W, HEADER_H - 8)) {
        toggleFav(WORDS[g_currentWord].term);
        drawDefinition();
        return;
    }
}

// ----------------------------------------------------------------------------
// Screen routing
// ----------------------------------------------------------------------------
static bool handleTabBar(const Tap& t)
{
    if (t.y < TABBAR_Y) return false;
    int tw = SCREEN_W / 4;
    int i = t.x / tw;
    Screen scr[4] = {SCR_SEARCH, SCR_BROWSE, SCR_SAVED, SCR_MORE};
    g_screen = scr[i];
    redrawCurrent();
    return true;
}

static void redrawCurrent()
{
    switch (g_screen) {
        case SCR_SEARCH:     drawSearchScreen(); break;
        case SCR_BROWSE:     drawBrowse(); break;
        case SCR_SAVED:      drawSaved(); break;
        case SCR_MORE:       drawMore(); break;
        case SCR_DEFINITION: drawDefinition(); break;
        case SCR_HISTORY:    drawHistory(); break;
    }
}

// ----------------------------------------------------------------------------
// Arduino entry points
// ----------------------------------------------------------------------------
void setup()
{
    Serial.begin(115200);

    // Manual reset pulse for the FT6336U touch controller.
    pinMode(PIN_TOUCH_RST, OUTPUT);
    digitalWrite(PIN_TOUCH_RST, LOW);
    delay(10);
    digitalWrite(PIN_TOUCH_RST, HIGH);
    delay(50);

    lcd.init();
    lcd.setRotation(LCD_ROTATION);
    lcd.setBrightness(255);

    initPalette();
    buildKeyboard();

    prefs.begin("dict", false);
    loadState();

    // Touch calibration: apply stored transform, or run a one-time calibration.
    uint16_t calData[8];
    if (prefs.getBytesLength("touchcal") == sizeof(calData)) {
        prefs.getBytes("touchcal", calData, sizeof(calData));
        lcd.setTouchCalibrate(calData);
    } else {
        recalibrateTouch();
    }
    g_wasTouched = false;

    buildResults();
    drawSearchScreen();
    Serial.printf("CYD Dictionary ready: %d words\n", WORD_COUNT);
}

void loop()
{
    Tap t = pollTap();
    if (t.hit) {
        switch (g_screen) {
            case SCR_SEARCH:     handleSearch(t); break;
            case SCR_BROWSE:     handleBrowse(t); break;
            case SCR_SAVED:      handleSaved(t); break;
            case SCR_MORE:       handleMore(t); break;
            case SCR_DEFINITION: handleDefinition(t); break;
            case SCR_HISTORY:    handleHistory(t); break;
        }
    }
    delay(10);
}
