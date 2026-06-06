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
#include <functional>
#include "LGFX_Setup.hpp"
#include "WordData.h"
#include "Dict.h"

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
    return dictFind(lower(term).c_str());
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
    g_wotd = (dictCount() > 0) ? (int)(boot % dictCount()) : 0;
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

static String getPin() { return prefs.getString("pin", ""); }
static void   setPin(const String& p) { prefs.putString("pin", p); }

// Blocking numeric keypad. Returns the entered 4-digit string, or "" if cancelled.
// `title` is shown at the top.
static String promptPin(const char* title)
{
    String entry = "";
    // 3x4 grid: 1..9, Cancel, 0, OK
    const char* labels[12] = {"1","2","3","4","5","6","7","8","9","Cancel","0","OK"};
    int gw = 80, gh = 38, gap = 6;
    int gridW = gw * 3 + gap * 2;
    int x0 = (SCREEN_W - gridW) / 2;
    int y0 = 56;   // grid bottom = 56 + 4*38 + 3*6 = 226, fits the 240px screen

    auto redraw = [&]() {
        lcd.fillScreen(C_BG);
        lcd.setFont(&fonts::Font2);
        lcd.setTextColor(C_TEXT, C_BG);
        lcd.setTextDatum(textdatum_t::middle_center);
        lcd.drawString(title, SCREEN_W / 2, 18);
        // masked entry: entered digits shown as '*', remaining slots as '_'
        String shown = "";
        for (size_t i = 0; i < 4; ++i) shown += (i < entry.length() ? '*' : '_');
        lcd.drawString(shown, SCREEN_W / 2, 40);
        for (int i = 0; i < 12; ++i) {
            int r = i / 3, c = i % 3;
            int bx = x0 + c * (gw + gap), by = y0 + r * (gh + gap);
            uint16_t col = (i == 9) ? C_SUB : (i == 11 ? C_ACCENT : C_KEY);
            uint16_t tx = (i == 9 || i == 11) ? C_HEADERTX : C_KEYTX;
            lcd.fillRoundRect(bx, by, gw, gh, 6, col);
            lcd.setTextColor(tx, col);
            lcd.drawString(labels[i], bx + gw / 2, by + gh / 2);
        }
        lcd.setTextDatum(textdatum_t::top_left);
    };
    redraw();

    g_wasTouched = true;   // ignore the press that opened this modal
    for (;;) {
        Tap t = pollTap();
        if (!t.hit) { delay(10); continue; }
        for (int i = 0; i < 12; ++i) {
            int r = i / 3, c = i % 3;
            int bx = x0 + c * (gw + gap), by = y0 + r * (gh + gap);
            if (inRect(t, bx, by, gw, gh)) {
                if (i == 9) return "";                       // Cancel
                if (i == 11) return entry;                   // OK
                char d = labels[i][0];
                if (entry.length() < 4) { entry += d; redraw(); }
            }
        }
    }
}

// ----------------------------------------------------------------------------
// On-screen keyboard model
// ----------------------------------------------------------------------------
enum KeyType { K_CHAR, K_BACK };
struct Key { int x, y, w, h; char c; KeyType type; };
static std::vector<Key> g_keys;

// 3-row keyboard (single-word search needs no space/clear keys). Taller keys with
// padding between rows and a gap above the tab bar for easier targeting.
static const int KB_TOP = 116;
static const int KEY_H  = 24;
static const int ROW_PITCH = 30;

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
    for (int c = 0; c < 26; ++c) g_valid[c] = false;
    g_results.clear();

    int n = dictCount();

    // Empty query: a letter is "live" if any word starts with it.
    if (q.length() == 0) {
        for (int c = 0; c < 26; ++c) {
            char p[2] = {(char)('a' + c), 0};
            int lb = dictLowerBound(p);
            g_valid[c] = (lb < n && dictTerm(lb)[0] == ('a' + c));
        }
        return;
    }

    // Scan only the contiguous block of terms sharing the query prefix. One pass
    // fills both the suggestion list (capped) and the dead-key map.
    int qlen = q.length();
    for (int i = dictLowerBound(q.c_str()); i < n; ++i) {
        const char* t = dictTerm(i);
        if (strncmp(t, q.c_str(), qlen) != 0) break;
        char nx = t[qlen];
        if (nx >= 'a' && nx <= 'z') g_valid[nx - 'a'] = true;
        if ((int)g_results.size() < 20) g_results.push_back(i);
    }
}

// Clear (X) button inside the search field, shown only when there's a query.
static const int SF_X_W = 24;
static const int SF_X_X = SCREEN_W - 6 - SF_X_W - 2;   // left edge of the X button
static const int SF_X_Y = 7;
static const int SF_X_H = HEADER_H - 14;

static void drawSearchField()
{
    lcd.fillRect(0, 0, SCREEN_W, HEADER_H, C_HEADER);
    lcd.fillRoundRect(6, 4, SCREEN_W - 12, HEADER_H - 8, 8, C_KEY);
    lcd.setFont(&fonts::Font2);
    lcd.setTextDatum(textdatum_t::middle_left);
    if (g_query.length()) {
        lcd.setTextColor(C_TEXT, C_KEY);
        lcd.drawString(g_query + "_", 14, HEADER_H / 2);
        // X clear button
        lcd.fillRoundRect(SF_X_X, SF_X_Y, SF_X_W, SF_X_H, 4, C_ROWLINE);
        lcd.setTextColor(C_TEXT, C_ROWLINE);
        lcd.setTextDatum(textdatum_t::middle_center);
        lcd.drawString("x", SF_X_X + SF_X_W / 2, SF_X_Y + SF_X_H / 2);
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
        const char* term = dictTerm(g_results[i]);
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
    DictEntry w;
    dictGet(g_results[0], w);
    if (w.meanings.empty()) return;
    const Meaning& m0 = w.meanings[0];
    lcd.setFont(&fonts::Font4);
    int tw = lcd.textWidth(w.term);
    lcd.setTextColor(C_TEXT, C_BG);
    lcd.drawString(w.term, 10, top);
    lcd.setFont(&fonts::Font2);
    lcd.setTextColor(C_ACCENT, C_BG);
    lcd.drawString(posName(m0.posCode), 10 + tw + 8, top + 10);
    lcd.setTextColor(C_TEXT, C_BG);
    drawWrapped(m0.def, 10, top + 28, SCREEN_W - 20, 18);
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

    // Clear (X) button in the search field
    if (g_query.length() && inRect(t, SF_X_X, SF_X_Y, SF_X_W, SF_X_H)) {
        g_query = "";
        buildResults();
        drawSearchScreen();
        return;
    }

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
                case K_BACK:  if (g_query.length()) g_query.remove(g_query.length() - 1); break;
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

// Maps a list position to a dictionary index. Browse maps 1:1; Saved/History map
// through their term->index vectors.
using IndexAt = std::function<int(int)>;

// Draws up to N word rows from a list of `total` items, plus up/down buttons.
static void drawWordList(int total, const IndexAt& at, int offset)
{
    int listW = SCREEN_W - LIST_BTN_W;
    int areaH = TABBAR_Y - LIST_TOP;
    lcd.fillRect(0, LIST_TOP, SCREEN_W, areaH, C_BG);

    int rows = listVisibleRows();
    lcd.setFont(&fonts::Font2);
    lcd.setTextDatum(textdatum_t::middle_left);
    if (total == 0) {
        lcd.setTextColor(C_SUB, C_BG);
        lcd.drawString("Nothing here yet", 10, LIST_TOP + 16);
    }
    for (int i = 0; i < rows; ++i) {
        int di = offset + i;
        if (di >= total) break;
        int y = LIST_TOP + i * ROW_H;
        DictEntry w;
        dictGet(at(di), w);
        lcd.fillRect(0, y, listW, ROW_H - 1, C_ROW);
        lcd.setTextColor(C_TEXT, C_ROW);
        lcd.drawString(w.term, 10, y + ROW_H / 2);
        lcd.setTextColor(C_SUB, C_ROW);
        lcd.drawString(w.meanings.empty() ? "" : posName(w.meanings[0].posCode), listW - 70, y + ROW_H / 2);
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
static bool handleWordList(const Tap& t, int total, const IndexAt& at, int* offset, int* chosen)
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
            if (*offset + rows < total) *offset += rows;
        }
        return true;
    }
    if (t.y >= LIST_TOP && t.y < TABBAR_Y) {
        int i = (t.y - LIST_TOP) / ROW_H;
        int di = *offset + i;
        if (di < total) { *chosen = at(di); }
    }
    return false;
}

// ----------------------------------------------------------------------------
// BROWSE
// ----------------------------------------------------------------------------
// Browse lists every word in order, so position == dictionary index.
static int browseAt(int pos) { return pos; }

static void drawBrowse()
{
    lcd.fillScreen(C_BG);
    drawHeader("Browse A-Z");
    drawWordList(dictCount(), browseAt, g_browseOffset);
    drawTabBar(SCR_BROWSE);
}

static void handleBrowse(const Tap& t)
{
    if (handleTabBar(t)) return;
    int chosen;
    if (handleWordList(t, dictCount(), browseAt, &g_browseOffset, &chosen)) { drawBrowse(); return; }
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
    auto v = favIndices();
    drawWordList(v.size(), [&v](int i) { return v[i]; }, g_savedOffset);
    drawTabBar(SCR_SAVED);
}

static void handleSaved(const Tap& t)
{
    if (handleTabBar(t)) return;
    auto v = favIndices();
    int chosen;
    if (handleWordList(t, v.size(), [&v](int i) { return v[i]; }, &g_savedOffset, &chosen)) {
        drawSaved();
        return;
    }
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
    auto v = historyIndices();
    drawWordList(v.size(), [&v](int i) { return v[i]; }, g_historyOffset);
    drawTabBar(SCR_MORE);
}

static void handleHistory(const Tap& t)
{
    if (handleTabBar(t)) return;   // tapping a tab leaves history
    auto v = historyIndices();
    int chosen;
    if (handleWordList(t, v.size(), [&v](int i) { return v[i]; }, &g_historyOffset, &chosen)) {
        drawHistory();
        return;
    }
    if (chosen >= 0) openDefinition(chosen);
}

// ----------------------------------------------------------------------------
// MORE
// ----------------------------------------------------------------------------
struct MoreBtn { int x, y, w, h; const char* label; };
static MoreBtn g_moreBtns[4];     // Random, Recent, Recalibrate, Set/Change PIN
static MoreBtn g_tierPills[4];    // Safe / Mild / Teen / Full selector pills

static void drawMore()
{
    lcd.fillScreen(C_BG);
    drawHeader("More");

    int cardX = 8, cardY = HEADER_H + 4, cardW = SCREEN_W - 16, cardH = 40;
    lcd.fillRoundRect(cardX, cardY, cardW, cardH, 8, C_WOD);
    lcd.drawRoundRect(cardX, cardY, cardW, cardH, 8, C_WODBORD);
    lcd.setFont(&fonts::Font2);
    lcd.setTextDatum(textdatum_t::top_left);
    lcd.setTextColor(C_SUB, C_WOD);
    lcd.drawString("WORD OF THE DAY", cardX + 8, cardY + 3);
    lcd.setFont(&fonts::Font4);
    lcd.setTextColor(C_TEXT, C_WOD);
    lcd.drawString(dictTerm(g_wotd), cardX + 8, cardY + 18);

    // Tier selector
    int ty = cardY + cardH + 4;
    lcd.setFont(&fonts::Font2);
    lcd.setTextDatum(textdatum_t::top_left);
    lcd.setTextColor(C_TEXT, C_BG);
    lcd.drawString(String("Tier:") + (getPin().length() ? "   (locked)" : ""), 12, ty);
    int py = ty + 16;
    const char* tlabels[4] = {"Safe", "Mild", "Teen", "Full"};
    int gap = 6, pw = (SCREEN_W - 16 - gap * 3) / 4, ph = 26;
    lcd.setTextDatum(textdatum_t::middle_center);
    for (int i = 0; i < 4; ++i) {
        int px = 8 + i * (pw + gap);
        g_tierPills[i] = {px, py, pw, ph, tlabels[i]};
        bool on = ((int)dictTier() == i);
        lcd.fillRoundRect(px, py, pw, ph, 6, on ? C_ACCENT : C_ROWLINE);
        lcd.setTextColor(on ? C_HEADERTX : C_SUB, on ? C_ACCENT : C_ROWLINE);
        lcd.drawString(tlabels[i], px + pw / 2, py + ph / 2);
    }

    // 2x2 buttons
    const char* labels[4] = {"Random word", "Recent words", "Recalibrate", "Set / Change PIN"};
    int by = py + ph + 6;
    int bw = (SCREEN_W - 16 - 8) / 2, bh = 28, bgap = 6;
    for (int i = 0; i < 4; ++i) {
        int r = i / 2, c = i % 2;
        int bx = 8 + c * (bw + 8);
        int yy = by + r * (bh + bgap);
        g_moreBtns[i] = {bx, yy, bw, bh, labels[i]};
        lcd.fillRoundRect(bx, yy, bw, bh, 6, C_ACCENT);
        lcd.setFont(&fonts::Font2);
        lcd.setTextColor(C_HEADERTX, C_ACCENT);
        lcd.setTextDatum(textdatum_t::middle_center);
        lcd.drawString(labels[i], bx + bw / 2, yy + bh / 2);
    }

    // status
    lcd.setTextDatum(textdatum_t::top_left);
    lcd.setFont(&fonts::Font2);
    lcd.setTextColor(C_SUB, C_BG);
    lcd.drawString(dictStatus(), 10, by + 2 * bh + bgap + 4);

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

static void switchTier(DictTier target)
{
    if (target == dictTier()) return;
    if ((int)target > (int)dictTier()) {       // becoming more permissive
        String pin = getPin();
        if (pin.length()) {
            String got = promptPin("Enter PIN to unlock");
            if (got != pin) { drawMore(); return; }
        }
    }
    if (dictSetTier(target)) {
        prefs.putUChar("tier", (uint8_t)target);
        g_browseOffset = g_savedOffset = g_historyOffset = 0;
        g_query = ""; buildResults();
    }
    drawMore();
}

static void handleMore(const Tap& t)
{
    if (handleTabBar(t)) return;

    int cardY = HEADER_H + 4;
    if (t.y >= cardY && t.y < cardY + 40) { openDefinition(g_wotd); return; }

    for (int i = 0; i < 4; ++i) {
        MoreBtn& p = g_tierPills[i];
        if (inRect(t, p.x, p.y, p.w, p.h)) { switchTier((DictTier)i); return; }
    }
    for (int i = 0; i < 4; ++i) {
        MoreBtn& b = g_moreBtns[i];
        if (inRect(t, b.x, b.y, b.w, b.h)) {
            if (i == 0) openDefinition((int)(esp_random() % dictCount()));
            else if (i == 1) { g_screen = SCR_HISTORY; g_historyOffset = 0; drawHistory(); }
            else if (i == 2) { recalibrateTouch(); g_wasTouched = false; drawMore(); }
            else { String np = promptPin("New PIN (Cancel = clear)"); setPin(np); drawMore(); }
            return;
        }
    }
}

// ----------------------------------------------------------------------------
// DEFINITION
// ----------------------------------------------------------------------------
static const int DEF_BACK_W = 70;
static const int DEF_STAR_W = 50;
static const int DEF_BODY_TOP = HEADER_H;                 // body starts under the fixed header
static const int DEF_VIEW_H   = SCREEN_H - DEF_BODY_TOP;  // viewport height (212)
static const int DEF_SB_W     = 5;                        // scrollbar width
static int g_defScroll = 0;                               // current scroll offset (px)
static int g_defContentH = 0;                             // total content height (px), set by layout

// Off-screen content sprite (PSRAM) so scrolling is a single fast blit, not a
// per-frame re-render. Rebuilt once when a definition opens; freed on leaving.
static lgfx::LGFX_Sprite g_defSprite(&lcd);
static bool g_defSpriteOk = false;
static void freeDefSprite() { if (g_defSpriteOk) { g_defSprite.deleteSprite(); g_defSpriteOk = false; } }

// Drag-to-scroll state (declarations here so handleDefinition can reference them)
static bool s_defDown = false;
static int  s_defY0 = 0, s_defScroll0 = 0, s_defDownX = 0, s_defDownY = 0;
static bool s_defDragged = false;

// Draws the fixed top bar (Back + headword + favourite toggle).
static void drawDefinitionHeader(const DictEntry& w)
{
    lcd.fillRect(0, 0, SCREEN_W, HEADER_H, C_HEADER);
    lcd.fillRoundRect(6, 4, DEF_BACK_W, HEADER_H - 8, 6, C_ACCENT);
    lcd.setFont(&fonts::Font2);
    lcd.setTextDatum(textdatum_t::middle_center);
    lcd.setTextColor(C_HEADERTX, C_ACCENT);
    lcd.drawString("< Back", 6 + DEF_BACK_W / 2, HEADER_H / 2);

    bool fav = isFav(w.term);
    lcd.fillRoundRect(SCREEN_W - DEF_STAR_W - 6, 4, DEF_STAR_W, HEADER_H - 8, 6, fav ? C_STAR : C_ACCENT);
    lcd.setTextColor(C_HEADERTX);
    lcd.drawString(fav ? "* in" : "+ fav", SCREEN_W - DEF_STAR_W / 2 - 6, HEADER_H / 2);

    // headword centred between the two buttons
    lcd.setFont(&fonts::Font4);
    lcd.setTextColor(C_HEADERTX, C_HEADER);
    lcd.drawString(w.term, SCREEN_W / 2, HEADER_H / 2);
    lcd.setTextDatum(textdatum_t::top_left);
}

// Lays out the grouped meanings at content coords (origin 0,0). If g != nullptr,
// draws into it; otherwise measures only. Returns total content height. Grouped by
// POS in first-seen order. Drawing into a tall sprite lets scrolling be one blit.
static int layoutDefinitionBody(const DictEntry& w, lgfx::LovyanGFX* g)
{
    lgfx::LovyanGFX* M = g ? g : static_cast<lgfx::LovyanGFX*>(&lcd);   // width metrics / draw target
    const int x = 10;
    const int maxw = SCREEN_W - DEF_SB_W - x - 6;
    const int lineH = 20;
    int vy = 0;   // content-space y

    // unique POS codes in first-seen order
    uint8_t order[8]; int nOrder = 0;
    for (const auto& m : w.meanings) {
        bool seen = false;
        for (int k = 0; k < nOrder; ++k) if (order[k] == m.posCode) { seen = true; break; }
        if (!seen && nOrder < 8) order[nOrder++] = m.posCode;
    }

    auto drawLineWrapped = [&](const String& text, uint16_t color, int indent) {
        M->setFont(&fonts::Font2);
        String line = "";
        int start = 0;
        int avail = maxw - indent;
        while (start <= (int)text.length()) {
            int sp = text.indexOf(' ', start);
            String word = (sp < 0) ? text.substring(start) : text.substring(start, sp);
            String trial = line.length() ? line + " " + word : word;
            if (M->textWidth(trial) > avail && line.length()) {
                if (g) { g->setTextColor(color, C_BG); g->drawString(line, x + indent, vy); }
                vy += lineH;
                line = word;
            } else {
                line = trial;
            }
            if (sp < 0) break;
            start = sp + 1;
        }
        if (line.length()) {
            if (g) { g->setTextColor(color, C_BG); g->drawString(line, x + indent, vy); }
            vy += lineH;
        }
    };

    vy += 6;
    for (int gi = 0; gi < nOrder; ++gi) {
        uint8_t pc = order[gi];
        if (g) {
            g->setFont(&fonts::Font2);
            g->setTextColor(C_ACCENT, C_BG);
            String h = String(posName(pc)); h.toUpperCase();
            g->drawString(h, x, vy);
        }
        vy += 22;
        int n = 1;
        for (const auto& m : w.meanings) {
            if (m.posCode != pc) continue;
            drawLineWrapped(String(n) + ".  " + m.def, C_TEXT, 0);
            if (m.example.length()) drawLineWrapped(String("\"") + m.example + "\"", C_SUB, 14);
            vy += 4;
            ++n;
        }
        vy += 6;
    }
    return vy;
}

static int defMaxScroll()
{
    int m = g_defContentH - DEF_VIEW_H;
    return m > 0 ? m : 0;
}

// Redraws just the scrolling body + scrollbar (header stays put). The body is one
// blit of the pre-rendered content sprite, clipped to the viewport.
static void drawDefinitionBody()
{
    int trackX = SCREEN_W - DEF_SB_W;
    if (g_defSpriteOk) {
        lcd.setClipRect(0, DEF_BODY_TOP, SCREEN_W - DEF_SB_W, DEF_VIEW_H);
        g_defSprite.pushSprite(0, DEF_BODY_TOP - g_defScroll);
        lcd.clearClipRect();
    } else {
        lcd.fillRect(0, DEF_BODY_TOP, SCREEN_W - DEF_SB_W, DEF_VIEW_H, C_BG);
    }

    // scrollbar
    int maxs = defMaxScroll();
    if (maxs > 0) {
        lcd.fillRect(trackX, DEF_BODY_TOP, DEF_SB_W, DEF_VIEW_H, C_ROWLINE);
        int thumbH = DEF_VIEW_H * DEF_VIEW_H / g_defContentH;
        if (thumbH < 18) thumbH = 18;
        int thumbY = DEF_BODY_TOP + (DEF_VIEW_H - thumbH) * g_defScroll / maxs;
        lcd.fillRoundRect(trackX, thumbY, DEF_SB_W, thumbH, 2, C_ACCENT);
    } else {
        lcd.fillRect(trackX, DEF_BODY_TOP, DEF_SB_W, DEF_VIEW_H, C_BG);
    }
}

static void drawDefinition()
{
    DictEntry w;
    dictGet(g_currentWord, w);
    lcd.fillScreen(C_BG);
    g_defContentH = layoutDefinitionBody(w, nullptr);   // measure

    // Render the whole body once into a PSRAM sprite for flicker-free scrolling.
    freeDefSprite();
    int sw = SCREEN_W - DEF_SB_W;
    int sh = g_defContentH < DEF_VIEW_H ? DEF_VIEW_H : g_defContentH;
    if (sh > 4000) sh = 4000;   // safety cap
    g_defSprite.setColorDepth(16);
    g_defSprite.setPsram(true);
    g_defSpriteOk = (g_defSprite.createSprite(sw, sh) != nullptr);
    if (g_defSpriteOk) {
        g_defSprite.fillScreen(C_BG);
        g_defSprite.setTextDatum(textdatum_t::top_left);
        layoutDefinitionBody(w, &g_defSprite);
    }

    drawDefinitionHeader(w);
    drawDefinitionBody();
}

static void openDefinition(int idx)
{
    if (idx < 0 || idx >= dictCount()) return;
    if (g_screen != SCR_DEFINITION && g_screen != SCR_HISTORY) g_prevScreen = g_screen;
    g_currentWord = idx;
    g_defScroll = 0;
    g_screen = SCR_DEFINITION;
    pushHistory(dictTerm(idx));
    drawDefinition();
}

static void redrawCurrent();

static void handleDefinition(const Tap& t)
{
    // Back
    if (inRect(t, 6, 4, DEF_BACK_W, HEADER_H - 8)) {
        freeDefSprite();                          // reclaim PSRAM
        g_screen = g_prevScreen;
        s_defDown = false; g_wasTouched = true;   // swallow this release on the next screen
        redrawCurrent();
        return;
    }
    // Star toggle
    if (inRect(t, SCREEN_W - DEF_STAR_W - 6, 4, DEF_STAR_W, HEADER_H - 8)) {
        toggleFav(dictTerm(g_currentWord));
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

    // Load the dictionary for the saved tier (SD corpus if present, else flash, else embedded).
    DictTier startTier = (DictTier)prefs.getUChar("tier", TIER_FULL);
    dictBegin(startTier);

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
    // esp_rom_printf reaches the USB-Serial/JTAG console (Serial is on UART0 here).
    esp_rom_printf("[dict] ready: %s\n", dictStatus());
}

// Continuous touch handler for the definition screen: drag to scroll, tap (no
// movement) falls through to handleDefinition() for Back / favourite.
static void handleDefinitionTouch()
{
    int32_t x, y;
    bool now = lcd.getTouch(&x, &y);
    if (now && !s_defDown) {
        s_defDown = true; s_defDragged = false;
        s_defY0 = y; s_defScroll0 = g_defScroll; s_defDownX = x; s_defDownY = y;
    } else if (now && s_defDown) {
        int dy = s_defY0 - y;
        if (abs(dy) > 6) s_defDragged = true;
        if (s_defDragged) {
            int ns = s_defScroll0 + dy;
            if (ns < 0) ns = 0;
            if (ns > defMaxScroll()) ns = defMaxScroll();
            if (ns != g_defScroll) { g_defScroll = ns; drawDefinitionBody(); }
        }
    } else if (!now && s_defDown) {
        s_defDown = false;
        if (!s_defDragged) {
            Tap t{true, s_defDownX, s_defDownY};
            handleDefinition(t);   // Back / favourite hit-testing on the fixed header
        }
    }
}

void loop()
{
    if (g_screen == SCR_DEFINITION) {
        handleDefinitionTouch();
        delay(10);
        return;
    }
    Tap t = pollTap();
    if (t.hit) {
        switch (g_screen) {
            case SCR_SEARCH:     handleSearch(t); break;
            case SCR_BROWSE:     handleBrowse(t); break;
            case SCR_SAVED:      handleSaved(t); break;
            case SCR_MORE:       handleMore(t); break;
            case SCR_DEFINITION: break;   // handled above
            case SCR_HISTORY:    handleHistory(t); break;
        }
    }
    delay(10);
}
