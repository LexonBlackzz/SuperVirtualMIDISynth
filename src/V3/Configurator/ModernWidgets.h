#ifndef SVMS_CONFIGURATOR_MODERNWIDGETS_H
#define SVMS_CONFIGURATOR_MODERNWIDGETS_H

// The 0.11 control set (CONFIGURATOR_REDESIGN/VISION.md §6). Every control
// draws the Modern look, or the Rack look via the rack primitives in
// Widgets.cpp when the Rack theme is active.

#include "imgui.h"

#include <cstdint>

namespace svms::cfg::ui {

// Design tokens for the active theme (Modern values from the prototype;
// Rack derives them from the rack palette). Refreshed by BeginFrame().
struct Tokens {
    ImU32 bg, side, card, card2, card3, line, line2, text, muted;
    ImU32 ok, bad, cpu, exp, acc, accSoft, accLine, onAcc;
    ImVec4 accV;
    float rCard, rCtl, rThumb;
    bool rack;
};
const Tokens& Tok();

// Call once per frame before any control.
void BeginFrame(float uiScale);
float Scale();

// Fonts. Weight: 0 regular, 1 medium, 2 semibold, 3 bold. Rack uses the mono
// face for everything. Sizes are in unscaled px.
void PushText(int weight, float px);
void PushNum(float px, bool medium = false);   // Geist Mono, tabular readouts
void PopFont();
// Rack shows labels in caps; returns `text` unchanged in Modern. The pointer
// is valid until the next call.
const char* Caps(const char* text);

// --- Drawing helpers ---------------------------------------------------------
ImU32 WithAlpha(ImU32 col, float alpha);
ImU32 MixU32(ImU32 a, ImU32 b, float t);
// Concentric low-alpha rounded rects: a cheap soft shadow / glow.
void SoftShadow(ImDrawList* dl, ImVec2 min, ImVec2 max, float rounding,
                ImU32 color, float spread, int layers = 8);
void Glow(ImDrawList* dl, ImVec2 center, float radius, ImU32 color,
          float spread, int layers = 8);
// Render-load colour for a percent of the callback budget (one lap per 100%):
// accent to red to orange to cyan, and the glow radius that goes with it.
ImU32 LoadColor(float percent, ImU32 accent);
float LoadGlow(float percent);
// Dashed outline of a rounded rect.
void DashedRect(ImDrawList* dl, ImVec2 min, ImVec2 max, float rounding,
                ImU32 color, float dash = 5.0f, float gap = 4.0f);
// Stroke a 24x24 SVG-style icon path ("M.. L.. H.. V.. A.. Z", absolute and
// relative commands incl. arcs) scaled into a square at `pos` of `size` px.
void StrokeIcon(ImDrawList* dl, const char* path, ImVec2 pos, float size,
                ImU32 color, float thickness, float rotateRad = 0.0f,
                float scale = 1.0f);

// --- Page entry / reveal -----------------------------------------------------
// Starts the page-entry stagger: each BeginEntry() afterwards (cards, rack
// panels) rises 12 px and fades in, 40 ms apart. Calm/Off skip it.
void PageEntered();
void BeginEntry();
void EndEntry();

// --- Search flash anchors ----------------------------------------------------
// Set by the search jump. The item registering this anchor scrolls itself to
// the centre once and flashes (accent outline + glow fading over 1.8 s).
void FlashAnchor(const char* anchor, const char* legacyCaption);
const char* FlashTarget();
// The page's scrolling child calls this so jumps centre rows inside it.
void SetScrollHost();
// Rows and cards call this with their anchor id and rect.
void Anchor(const char* anchor, ImVec2 min, ImVec2 max);
// Temporary (Phase 2): the old pages' panels and captions register here by
// caption so search jumps still land on them.
void LegacyAnchor(const char* caption, ImVec2 min, ImVec2 max);

// --- Containers --------------------------------------------------------------
// Card: 14 px radius, padding 22x24. `anchor` (optional) makes the card a
// search target. With rightSlotW > 0 the cursor is left at the header's
// right slot; call CardHeaderEnd() after drawing it.
void BeginCard(const char* id, const char* title = nullptr,
               const char* subtitle = nullptr, float rightSlotW = 0.0f,
               const char* anchor = nullptr);
void CardHeaderEnd();
// The next BeginCard is at least this tall (px, unscaled): tiles that sit in a
// row stay equal without measuring each other.
void SetNextCardMinHeight(float px);
// Cards drawn between these (e.g. one table row) end up equally tall. The
// height is measured on the previous frame, as the tile grid does.
void BeginCardRow(const char* id);
void EndCardRow();
void EndCard();

enum class ChipKind { Cpu, Bad, Ok, Exp, Acc };
struct ChipDef {
    const char* text;
    ChipKind kind;
};
void Chip(const char* text, ChipKind kind);
ImVec2 ChipSize(const char* text);
// ↻ "Applies after restart" with a tooltip.
void RestartMark(bool withText = true);
float RestartMarkWidth(bool withText = true);

// Setting row: title (+ chips, + ↻ when the registry says restart), a muted
// one-line description, and a right-aligned control of `controlSize`.
// Draw the control between BeginRow and EndRow. `anchor` is the registry id.
void BeginRow(const char* anchor, const char* title, const char* desc,
              ImVec2 controlSize, const ChipDef* chips = nullptr,
              int chipCount = 0);
void EndRow();

// Disclosure row (dashed border, chevron rotates 90°). Returns the open
// state; wrap the revealed content in BeginReveal/EndReveal.
bool Disclosure(const char* key, const char* title, const char* hint);
bool& DisclosureState(const char* key);
void BeginReveal(const char* key);
void EndReveal();

// --- Controls ----------------------------------------------------------------
// 46x28 toggle. `delay` staggers the visual flip (profile apply, 40 ms steps).
bool Toggle(const char* id, bool* value, float delay = 0.0f);

// Equal-width cells; width 0 = natural, < 0 = fill the available width.
bool Segmented(const char* id, int* current, const char* const* labels,
               int count, float width = 0.0f);
ImVec2 SegmentedSize(const char* const* labels, int count, float width = 0.0f);

struct KnobSpec {
    const char* label;
    float min, max, def;
    float step = 0.0f;               // 0 = continuous
    bool log = false;
    const char* format = "%.2f";     // printf for the readout
    float (*display)(float) = nullptr;   // stored -> shown value
    const char* zeroText = nullptr;  // shown when value <= min (e.g. "Off")
};
// 64 px knob: vertical drag (180 px = full range, Shift = 700 px), double
// click resets, arrows/Home/End when hovered. `committed` is set on release
// (and on discrete key/reset edits) for send-on-release settings.
bool Knob(const char* id, float* value, const KnobSpec& spec,
          bool* committed = nullptr);

enum class ButtonKind { Normal, Primary, Danger };
bool Button(const char* label, ButtonKind kind = ButtonKind::Normal,
            ImVec2 size = ImVec2(0, 0), bool enabled = true);
ImVec2 ButtonSize(const char* label);
// Two clicks: the first arms it ("Click again…", shake, disarms after 3 s).
bool DangerButton(const char* id, const char* label, const char* armedLabel);

// Dropdown styled like a field. Returns true when the selection changed.
bool Select(const char* id, int* current, const char* const* items, int count,
            float width);
bool Field(const char* id, char* buf, size_t bufSize, float width,
           const char* hint = nullptr);
bool NumberField(const char* id, int* value, int minValue, int maxValue,
                 float width = 150.0f);
// Filled-track slider. Returns true while the value changes.
bool RangeSlider(const char* id, float* value, float minValue, float maxValue,
                 float width, bool* committed = nullptr);

// Shortcut tile for the Performance grid: title + toggle, description, tags
// pinned to the bottom. Returns *on; nested content starts with TileNested()
// (the dashed divider). Always close with EndTile().
bool BeginTile(const char* anchor, const char* title, const char* desc,
               bool* on, bool* changed, const ChipDef* tags, int tagCount,
               float toggleDelay = 0.0f);
void TileNested();
void EndTile();
// Two-column grid of tiles with equal row heights.
void BeginTileGrid(const char* id);
void EndTileGrid();

} // namespace svms::cfg::ui

#endif
