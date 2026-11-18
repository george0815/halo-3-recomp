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

namespace
{
    constexpr uint32_t kTypeText = 0, kTypeListItem = 5;
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

    int64_t NowNs()
    {
        return std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now().time_since_epoch()).count();
    }

    // Calls a recompiled function with up to five integer arguments from
    // inside a hook (same thread, same stack: the callee allocates below
    // r1). Returns r3.
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

    // The union of the bounds of the text under a widget (depth-limited).
    void TextBounds(PPCContext& ctx, uint8_t* base, uint32_t widget, Rect& rect, int depth)
    {
        int guard = 0;
        for (uint32_t child = Resolve(ctx, base, widget + 28); child && Plausible(child) && guard < 64;
            child = Resolve(ctx, base, child + 40), guard++)
        {
            if (h3::GuestRef<uint32_t>(child + 8) == kTypeText)
            {
                float x0 = h3::GuestRef<float>(child + kTextBounds), x1 = h3::GuestRef<float>(child + kTextBounds + 4);
                float y0 = h3::GuestRef<float>(child + kTextBounds + 8), y1 = h3::GuestRef<float>(child + kTextBounds + 12);
                if (x0 < x1 && y0 < y1 && x1 - x0 < 4 * kUiWidth && y1 - y0 < 4 * kUiHeight)
                {
                    rect.x0 = std::min(rect.x0, x0);
                    rect.x1 = std::max(rect.x1, x1);
                    rect.y0 = std::min(rect.y0, y0);
                    rect.y1 = std::max(rect.y1, y1);
                }
            }
            if (depth > 0)
                TextBounds(ctx, base, child, rect, depth - 1);
        }
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

        uint64_t cursor = g_cursor.load();
        if (!(cursor >> 63))
            return;
        float x = float(uint32_t(cursor) & 0xFFFF) / 65535.0f * kUiWidth - kUiWidth / 2;
        float y = float(uint32_t(cursor >> 16) & 0xFFFF) / 65535.0f * kUiHeight - kUiHeight / 2;

        // The item under the cursor; where text boxes overlap (taller than
        // a row), the lowest-starting one, since text sits at the top.
        uint32_t hit = 0;
        Rect hitRect;
        int guard = 0;
        for (uint32_t item = Resolve(ctx, base, list + 28); item && Plausible(item) && guard < 64;
            item = Resolve(ctx, base, item + 40), guard++)
        {
            if (h3::GuestRef<uint32_t>(item + 8) != kTypeListItem)
                continue;
            Rect rect;
            TextBounds(ctx, base, item, rect, 2);
            if (!rect.Empty() && rect.Contains(x, y) && (!hit || rect.y0 > hitRect.y0))
            {
                hit = item;
                hitRect = rect;
            }
        }
        if (!hit)
            return;
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
        uint32_t element = Virtual(ctx, base, hit, 20);
        if (element == 0xFFFFFFFF)
            return;
        if (log)
            fprintf(stderr, "[menu] cursor %.0f,%.0f%s: list %08X item %08X (%.0f..%.0f x %.0f..%.0f) element %08X\n", x, y,
                click ? " (click)" : "", list, hit, hitRect.x0, hitRect.x1, hitRect.y0, hitRect.y1, element);
        if (Call(ctx, base, sub_824E2D20, list, element, 0) & 0xFF)
        {
            Call(ctx, base, sub_82486700, screen, list, element, 1, 1);
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
