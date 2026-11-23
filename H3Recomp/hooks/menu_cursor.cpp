// Clickable menu cursor (private/re/menu_cursor.md).
//
// The game's menu widgets (retail, matched to MCC's c_gui_* code):
//   widget +8 type: 0 text, 1 bitmap, 2 model, 3 group, 4 button_key,
//     5 list_item, 6 slider, 7 list, 8 screen; +16 parent, +28 first
//     child, +40 next sibling: handles, resolved by sub_82482A48.
//   sub_8248B648(widget) = its screen.
//   screen +216 focused item, +220 byte: nothing focused; +64 its id.
//   list item vtable +20: its datasource element (-1: no data).
//   text widget +0xF4: its bounds (x0, x1, y0, y1; floats) in the UI's
//     1152x640 space, origin at the centre, y down; the whole frame. The
//     text is drawn at the top of its box, which can be taller than a row.
//   sub_824E1CB8(list, event) = the list's controller input: the D-pad
//     moves focus with sub_824E2118, which ends with
//     sub_824E2D20(list, element, 0) (scroll it into view) and
//     sub_82486700(screen, list, element, 1, 1) (focus it, with the
//     focus animations).
//   sub_824E1B00(list) (list vtable +0x5C): the list's per-frame update.
//   sub_824E5910(text, data) (text widget vtable +104): fills the text's
//     render data (base widget part by sub_8248B420 first); data +8..+36
//     the drawn quad's corners (x, y pairs). Its space is per screen (the
//     popups' is the bounds + (576, 320); the start menu's isn't), but a
//     list drawn away from its bounds shows as an offset from the rest of
//     its screen's text: Select Network Mode's side list (HOST GAME / FIND
//     GAME...) is drawn one row (26) below its bounds, next to the focused
//     row. The hit test moves such text by that difference.
//   sub_8247F008(owner, event kind, component) plays a menu event's sound
//     through sub_82483698(sound, owner); navigation plays sound 1 or 2,
//     owner -1 = the global one (the window manager's own calls).
//   The window manager (global 0x83142940, sub_8208B958 updates it) holds
//     the screens in 5 layers, +8 + layer * 40 (10 pointers), counts at
//     +208 + layer * 4, later on top. The menus are in layer 4 (the one for
//     "any controller", sub_8208B370(255)). sub_8208B288(controller) = the
//     screen its input goes to: the last in its layer, skipping the
//     screens with ids 0x100BB and 0x100BC (overlays).
//
// Each list's update hit-tests the cursor against its items (the bounds
// of their text) and, when the cursor has moved onto an item, focuses it
// the way the D-pad does. Only lists on the screen that has the input
// take part (not the lobby behind a popup or the start menu).
// input/input.cpp releases the mouse while a menu is open, turns a left
// click into "focus the item under the cursor, then A", a right click
// into B and the wheel into the D-pad.

#include "hooks/menu_cursor.h"

#include "config/env.h"
#include "memory.h"

#include <ppc_recomp_shared.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cmath>
#include <cstring>
#include <map>
#include <mutex>
#include <unordered_map>

namespace
{
    constexpr uint32_t kTypeText = 0, kTypeListItem = 5, kTypeSlider = 6;
    constexpr uint32_t kTextBounds = 0xF4;
    constexpr uint32_t kScreenFocusedItem = 216, kScreenNothingFocused = 220, kScreenId = 64;
    constexpr uint32_t kWindowManager = 0x83142940, kMenuLayer = 4;
    constexpr float kUiWidth = 1152.0f, kUiHeight = 640.0f;

    // The cursor: x and y (frame fractions, 16 bits each) and bit 63
    // "inside"; and a count of its moves.
    std::atomic<uint64_t> g_cursor{ 0 };
    std::atomic<uint32_t> g_cursorMoves{ 0 };
    std::atomic<int64_t> g_lastListNs{ 0 };
    std::atomic<bool> g_clickRequested{ false }, g_clickReady{ false };
    std::atomic<int64_t> g_overSliderNs{ 0 };  // the cursor was last over an item with a slider

    // Where each text widget was last drawn, relative to its bounds: the
    // offset (in the UI's units) from its bounds' top left to its drawn
    // quad's, from its render data (the sub_824E5910 hook below). Render
    // data isn't in one screen space (the start menu's is shifted from the
    // popups'), but on a screen most text shares one offset; a list drawn
    // elsewhere than its bounds say (Select Network Mode's side list, one
    // row below them, next to the focused row) has its own.
    struct Drawn
    {
        float dx = 0, dy = 0;
        int64_t ns = 0;
    };
    std::mutex g_drawnLock;
    std::unordered_map<uint32_t, Drawn> g_drawn;  // text widget -> offset
    constexpr int64_t kDrawnFreshNs = 250'000'000;

    bool DrawnOffset(uint32_t widget, int64_t now, float& dx, float& dy)
    {
        std::lock_guard<std::mutex> lock(g_drawnLock);
        auto found = g_drawn.find(widget);
        if (found == g_drawn.end() || now - found->second.ns > kDrawnFreshNs)
            return false;
        dx = found->second.dx;
        dy = found->second.dy;
        return true;
    }

    int64_t NowNs()
    {
        return std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now().time_since_epoch()).count();
    }

    // Calls a recompiled function with up to five integer arguments from
    // inside a hook (same thread, same stack: the callee allocates below
    // r1). Returns r3. The callee's floating-point state comes back to the
    // caller: the generated code keeps the host's flush-to-zero mode cached
    // in ctx.fpscr and only switches the host when the cache says so, so a
    // callee that switched it on a copy left the caller's cache wrong (its
    // later float math then ran in the other mode: Storm's film diverged).
    uint32_t Call(PPCContext& ctx, uint8_t* base, PPCFunc* function, uint32_t a0, uint32_t a1 = 0, uint32_t a2 = 0,
        uint32_t a3 = 0, uint32_t a4 = 0)
    {
        PPCContext call = ctx;
        call.r3.u64 = a0;
        call.r4.u64 = a1;
        call.r5.u64 = a2;
        call.r6.u64 = a3;
        call.r7.u64 = a4;
        function(call, base);
        ctx.fpscr = call.fpscr;
        return call.r3.u32;
    }

    uint32_t Virtual(PPCContext& ctx, uint8_t* base, uint32_t object, uint32_t offset)
    {
        uint32_t vtable = h3::GuestRef<uint32_t>(object);
        uint32_t function = h3::GuestRef<uint32_t>(vtable + offset);
        return Call(ctx, base, PPC_LOOKUP_FUNC(base, function), object);
    }

    uint32_t Resolve(PPCContext& ctx, uint8_t* base, uint32_t handle)
    {
        return Call(ctx, base, sub_82482A48, handle);
    }

    bool Plausible(uint32_t address)
    {
        return address >= 0x40000000 && address < 0xC0000000;
    }

    struct Rect
    {
        float x0 = 1e9f, x1 = -1e9f, y0 = 1e9f, y1 = -1e9f;
        bool Empty() const { return x0 >= x1 || y0 >= y1; }
        bool Contains(float x, float y) const { return x >= x0 && x < x1 && y >= y0 && y < y1; }
    };

    // The union of the bounds of the text under a widget (depth-limited);
    // slider: one of the widgets under it is a slider.
    // The offset most of a screen's text is drawn at (see Drawn).
    struct Reference
    {
        bool valid = false;
        float dx = 0, dy = 0;
    };

    void TextBounds(PPCContext& ctx, uint8_t* base, uint32_t widget, Rect& rect, int depth, bool* slider = nullptr,
        const Reference* reference = nullptr, int64_t now = 0)
    {
        int guard = 0;
        for (uint32_t child = Resolve(ctx, base, widget + 28); child && Plausible(child) && guard < 64;
            child = Resolve(ctx, base, child + 40), guard++)
        {
            if (slider && h3::GuestRef<uint32_t>(child + 8) == kTypeSlider)
                *slider = true;
            if (h3::GuestRef<uint32_t>(child + 8) == kTypeText)
            {
                float x0 = h3::GuestRef<float>(child + kTextBounds), x1 = h3::GuestRef<float>(child + kTextBounds + 4);
                float y0 = h3::GuestRef<float>(child + kTextBounds + 8), y1 = h3::GuestRef<float>(child + kTextBounds + 12);
                float dx, dy;
                if (reference && reference->valid && DrawnOffset(child, now, dx, dy))
                {
                    // Drawn away from its bounds (relative to the screen's
                    // other text): hit-test where it is.
                    x0 += dx - reference->dx, x1 += dx - reference->dx;
                    y0 += dy - reference->dy, y1 += dy - reference->dy;
                }
                if (x0 < x1 && y0 < y1 && x1 - x0 < 4 * kUiWidth && y1 - y0 < 4 * kUiHeight)
                {
                    rect.x0 = std::min(rect.x0, x0);
                    rect.x1 = std::max(rect.x1, x1);
                    rect.y0 = std::min(rect.y0, y0);
                    rect.y1 = std::max(rect.y1, y1);
                }
            }
            if (depth > 0)
                TextBounds(ctx, base, child, rect, depth - 1, slider, reference, now);
        }
    }

    // Counts the recently drawn offsets of the text under a widget: outside
    // list items into outside, all into all (rounded to whole units).
    void CountOffsets(PPCContext& ctx, uint8_t* base, uint32_t widget, int depth, bool inItem, int64_t now,
        std::map<std::pair<int, int>, int>& outside, std::map<std::pair<int, int>, int>& all, int& budget)
    {
        for (uint32_t child = Resolve(ctx, base, widget + 28); child && Plausible(child) && budget > 0;
            child = Resolve(ctx, base, child + 40))
        {
            budget--;
            uint32_t type = h3::GuestRef<uint32_t>(child + 8);
            float dx, dy;
            if (type == kTypeText && DrawnOffset(child, now, dx, dy))
            {
                std::pair<int, int> key{ int(std::lround(dx)), int(std::lround(dy)) };
                all[key]++;
                if (!inItem)
                    outside[key]++;
            }
            if (depth > 0)
                CountOffsets(ctx, base, child, depth - 1, inItem || type == kTypeListItem, now, outside, all, budget);
        }
    }

    // The screen's reference offset (cached for 100 ms per screen).
    Reference ScreenReference(PPCContext& ctx, uint8_t* base, uint32_t screen, int64_t now)
    {
        static uint32_t cachedScreen = 0;
        static int64_t cachedNs = 0;
        static Reference cached;
        if (screen == cachedScreen && now - cachedNs < 100'000'000)
            return cached;
        std::map<std::pair<int, int>, int> outside, all;
        int budget = 1024;
        CountOffsets(ctx, base, screen, 8, false, now, outside, all, budget);
        const auto& counts = outside.empty() ? all : outside;
        Reference reference;
        int best = 0;
        for (const auto& [key, count] : counts)
            if (count > best)
            {
                best = count;
                reference = { true, float(key.first), float(key.second) };
            }
        cachedScreen = screen;
        cachedNs = now;
        cached = reference;
        return reference;
    }

    // The screen the game sends menu input to (sub_8208B288's choice).
    uint32_t TopScreen()
    {
        uint32_t count = std::min<uint32_t>(h3::GuestRef<uint32_t>(kWindowManager + 208 + kMenuLayer * 4), 10);
        for (uint32_t i = count; i-- > 0;)
        {
            uint32_t screen = h3::GuestRef<uint32_t>(kWindowManager + 8 + kMenuLayer * 40 + i * 4);
            if (!Plausible(screen))
                continue;
            uint32_t id = h3::GuestRef<uint32_t>(screen + kScreenId);
            if (id != 0x100BB && id != 0x100BC)
                return screen;
        }
        return 0;
    }

    // The screen whose lists the cursor works on: the one holding the top
    // screen's focused item. The start menu's top screen is its pane, which
    // holds the sidebar list; a popup's focus is in its own list; a lobby's
    // in its list, with the roster on the same screen.
    uint32_t InputScreen(PPCContext& ctx, uint8_t* base)
    {
        uint32_t top = TopScreen();
        if (!top || h3::GuestRef<uint8_t>(top + kScreenNothingFocused))
            return top;
        uint32_t focused = h3::GuestRef<uint32_t>(top + kScreenFocusedItem);
        if (!Plausible(focused))
            return top;
        uint32_t screen = Call(ctx, base, sub_8248B648, focused);
        return Plausible(screen) ? screen : top;
    }

    // A list's update: hit-test, and focus the item under a moved cursor
    // (or a clicked one).
    void UpdateList(PPCContext& ctx, uint8_t* base, uint32_t list)
    {
        static const bool log = h3::EnvFlag("H3_LOG_MENU");
        g_lastListNs.store(NowNs());

        // Nothing to do (and no game code to call) until the pointer has
        // been in the window (the menu cursor off, a film or scripted run).
        uint64_t cursor = g_cursor.load();
        if (g_cursorMoves.load() == 0 && !(cursor >> 63) && !g_clickRequested.load())
            return;

        uint32_t screen = Call(ctx, base, sub_8248B648, list);
        uint32_t inputScreen = InputScreen(ctx, base);
        if (!Plausible(screen) || screen != inputScreen)
            return;

        // The cursor's moves count from when this screen got the input (a
        // menu opening under a resting cursor keeps the game's focus).
        static uint32_t lastInputScreen = 0, handledMoves = 0;
        uint32_t moves = g_cursorMoves.load();
        if (inputScreen != lastInputScreen)
        {
            if (log)
                fprintf(stderr, "[menu] input screen %08X\n", inputScreen);
            lastInputScreen = inputScreen;
            handledMoves = moves;
        }

        // H3_LOG_MENU: a list's item boxes, a second after its screen got
        // the input (once its opening animation is over).
        static std::map<uint64_t, uint32_t> boxesUpdates;  // (screen, list) -> updates seen
        if (log && ++boxesUpdates[(uint64_t(inputScreen) << 32) | list] == 60)
        {
            // H3_LOG_MENU_FIELDS: per item, its two rectangles (+0xD8 and
            // +0xE8) and its first text widget's (+0xE4 and +0xF4, the
            // bounds the hit test uses), to find render offsets the text
            // bounds don't include. Known widget fields only.
            static const bool fields = h3::EnvFlag("H3_LOG_MENU_FIELDS");
            if (fields)
            {
                // The list widget's own words (+0x20..+0x200; plain reads):
                // as floats where plausible, else as small integers.
                fprintf(stderr, "[menu] fields list %08X words:", list);
                for (uint32_t offset = 0x20; offset < 0x200; offset += 4)
                {
                    uint32_t word = h3::GuestRef<uint32_t>(list + offset);
                    float v;
                    memcpy(&v, &word, 4);
                    if (word == 0)
                        continue;
                    if (std::fabs(v) >= 1e-3f && std::fabs(v) <= 4000.0f)
                        fprintf(stderr, " +%X=%gf", offset, v);
                    else if (word < 0x10000 || word > 0xFFFF0000u)
                        fprintf(stderr, " +%X=%d", offset, int32_t(word));
                }
                fprintf(stderr, "\n");
            }
            int fieldGuard = 0;
            for (uint32_t item = Resolve(ctx, base, list + 28); fields && item && Plausible(item) && fieldGuard < 64;
                item = Resolve(ctx, base, item + 40), fieldGuard++)
            {
                if (h3::GuestRef<uint32_t>(item + 8) != kTypeListItem)
                    continue;
                auto rect = [](uint32_t at) {
                    static char text[4][64];
                    static int next = 0;
                    char* out = text[next++ & 3];
                    snprintf(out, 64, "%g %g %g %g", float(h3::GuestRef<float>(at)), float(h3::GuestRef<float>(at + 4)),
                        float(h3::GuestRef<float>(at + 8)), float(h3::GuestRef<float>(at + 12)));
                    return out;
                };
                fprintf(stderr, "[menu] fields list %08X item %08X: D8 [%s] E8 [%s]", list, item, rect(item + 0xD8), rect(item + 0xE8));
                uint32_t child = Resolve(ctx, base, item + 28);
                for (int i = 0; child && Plausible(child) && i < 8; child = Resolve(ctx, base, child + 40), i++)
                    if (h3::GuestRef<uint32_t>(child + 8) == kTypeText)
                    {
                        fprintf(stderr, " text %08X E4 [%s] F4 [%s]", child, rect(child + 0xE4), rect(child + 0xF4));
                        break;
                    }
                fprintf(stderr, "\n");
            }
            int logGuard = 0;
            for (uint32_t item = Resolve(ctx, base, list + 28); item && Plausible(item) && logGuard < 64;
                item = Resolve(ctx, base, item + 40), logGuard++)
            {
                if (h3::GuestRef<uint32_t>(item + 8) != kTypeListItem)
                    continue;
                Rect rect;
                TextBounds(ctx, base, item, rect, 2);
                fprintf(stderr, "[menu] screen %08X list %08X item %08X box %.0f..%.0f x %.0f..%.0f\n", inputScreen, list, item,
                    rect.x0, rect.x1, rect.y0, rect.y1);
            }
        }

        if (!(cursor >> 63))
            return;
        float x = float(uint32_t(cursor) & 0xFFFF) / 65535.0f * kUiWidth - kUiWidth / 2;
        float y = float(uint32_t(cursor >> 16) & 0xFFFF) / 65535.0f * kUiHeight - kUiHeight / 2;

        // The item under the cursor; where text boxes overlap (taller than
        // a row), the lowest-starting one, since text sits at the top.
        // Items without data (the unused rows of a list longer than its
        // entries: no text, but boxes overlapping their neighbours') don't
        // count.
        uint32_t hit = 0, hitElement = 0xFFFFFFFF;
        Rect hitRect;
        bool hitSlider = false;
        int guard = 0;
        static const bool noDrawn = h3::EnvFlag("H3_MENU_CURSOR_BOUNDS_ONLY");
        int64_t now = NowNs();
        Reference reference = noDrawn ? Reference{} : ScreenReference(ctx, base, screen, now);
        for (uint32_t item = Resolve(ctx, base, list + 28); item && Plausible(item) && guard < 64;
            item = Resolve(ctx, base, item + 40), guard++)
        {
            if (h3::GuestRef<uint32_t>(item + 8) != kTypeListItem)
                continue;
            Rect rect;
            bool slider = false;
            TextBounds(ctx, base, item, rect, 2, &slider, &reference, now);
            if (rect.Empty() || !rect.Contains(x, y) || (hit && rect.y0 <= hitRect.y0))
                continue;
            uint32_t element = Virtual(ctx, base, item, 20);
            if (element == 0xFFFFFFFF)
                continue;
            hit = item;
            hitElement = element;
            hitRect = rect;
            hitSlider = slider;
        }
        if (!hit)
            return;
        if (hitSlider)
            g_overSliderNs.store(NowNs());
        bool click = g_clickRequested.exchange(false);

        // Focus it when the cursor has moved (so the keyboard and pad
        // still move focus while the mouse rests) or on a click.
        if (moves == handledMoves && !click)
            return;
        handledMoves = moves;

        uint32_t focused = h3::GuestRef<uint8_t>(screen + kScreenNothingFocused) ? 0 : uint32_t(h3::GuestRef<uint32_t>(screen + kScreenFocusedItem));
        if (focused == hit)
        {
            g_clickReady.store(click);
            return;
        }
        uint32_t element = hitElement;
        if (log)
            fprintf(stderr, "[menu] cursor %.0f,%.0f%s: list %08X item %08X (%.0f..%.0f x %.0f..%.0f) element %08X%s\n", x, y,
                click ? " (click)" : "", list, hit, hitRect.x0, hitRect.x1, hitRect.y0, hitRect.y1, element, hitSlider ? " slider" : "");
        if (Call(ctx, base, sub_824E2D20, list, element, 0) & 0xFF)
        {
            Call(ctx, base, sub_82486700, screen, list, element, 1, 1);
            // The D-pad's navigation sound, for a move onto an item (a click
            // plays A's own sound next).
            if (!click)
                Call(ctx, base, sub_82483698, 1, 0xFFFFFFFF);
            g_clickReady.store(click);
        }
    }
}

namespace h3::hooks
{
    void SetMenuCursor(bool inside, float x, float y, bool moved)
    {
        uint64_t packed = (inside ? 1ull << 63 : 0) | uint64_t(uint32_t(std::clamp(y, 0.0f, 1.0f) * 65535.0f)) << 16 |
                          uint32_t(std::clamp(x, 0.0f, 1.0f) * 65535.0f);
        if (g_cursor.exchange(packed) != packed && moved)
            g_cursorMoves.fetch_add(1);
    }

    bool MenuOpen()
    {
        return NowNs() - g_lastListNs.load() < 150'000'000;
    }

    bool MenuCursorOverSlider()
    {
        return NowNs() - g_overSliderNs.load() < 150'000'000;
    }

    void RequestMenuClick()
    {
        g_clickReady.store(false);
        g_clickRequested.store(true);
    }

    bool TakeMenuClick()
    {
        return g_clickReady.exchange(false);
    }

    void CancelMenuClick()
    {
        g_clickRequested.store(false);
        g_clickReady.store(false);
    }
}

PPC_FUNC_IMPL(__imp__sub_824E1B00);
PPC_FUNC(sub_824E1B00)
{
    uint32_t list = ctx.r3.u32;
    __imp__sub_824E1B00(ctx, base);
    UpdateList(ctx, base, list);
}

// Text widget render data (vtable +104): r3 the widget, r4 the render data
// it fills (base widget part from sub_8248B420 first).
// H3_LOG_TEXT_RENDER=1 (debugging): each text widget's bounds (+0xF4) and
// its render data's floats, every 600th render of it.
PPC_FUNC_IMPL(__imp__sub_824E5910);
PPC_FUNC(sub_824E5910)
{
    static const bool log = h3::EnvFlag("H3_LOG_TEXT_RENDER");
    uint32_t widget = ctx.r3.u32, data = ctx.r4.u32;
    __imp__sub_824E5910(ctx, base);
    // The drawn quad's top left (the render data's corners: x at +8, +16,
    // +24, +32 and y at +12, +20, +28, +36) against the bounds'.
    float bx0 = h3::GuestRef<float>(widget + kTextBounds), bx1 = h3::GuestRef<float>(widget + kTextBounds + 4);
    float by0 = h3::GuestRef<float>(widget + kTextBounds + 8), by1 = h3::GuestRef<float>(widget + kTextBounds + 12);
    float qx = h3::GuestRef<float>(data + 8), qy = h3::GuestRef<float>(data + 12);
    for (uint32_t corner = 1; corner < 4; corner++)
    {
        qx = std::min<float>(qx, h3::GuestRef<float>(data + 8 + corner * 8));
        qy = std::min<float>(qy, h3::GuestRef<float>(data + 12 + corner * 8));
    }
    if (bx0 < bx1 && by0 < by1 && std::isfinite(qx) && std::isfinite(qy) && std::fabs(qx) < 1e5f && std::fabs(qy) < 1e5f)
    {
        std::lock_guard<std::mutex> lock(g_drawnLock);
        if (g_drawn.size() > 8192)
            g_drawn.clear();
        g_drawn[widget] = { qx - bx0, qy - by0, NowNs() };
    }
    if (!log)
        return;
    static std::map<uint32_t, uint32_t> seen;
    if (seen[widget]++ % 600 != 0)
        return;
    fprintf(stderr, "[text] %08X bounds %g %g %g %g data", widget, float(h3::GuestRef<float>(widget + kTextBounds)),
        float(h3::GuestRef<float>(widget + kTextBounds + 4)), float(h3::GuestRef<float>(widget + kTextBounds + 8)),
        float(h3::GuestRef<float>(widget + kTextBounds + 12)));
    for (uint32_t offset = 0; offset < 112; offset += 4)
    {
        uint32_t word = h3::GuestRef<uint32_t>(data + offset);
        float v;
        memcpy(&v, &word, 4);
        if (word != 0)
            fprintf(stderr, " +%u=%g", offset, std::fabs(v) < 1e-6f || std::fabs(v) > 1e7f ? double(int32_t(word)) : double(v));
    }
    fprintf(stderr, "\n");
}
