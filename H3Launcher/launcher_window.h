// The launcher's one window: the promo art tinted like Halo 3's main menu,
// and a menu in the game's style (and font) for the settings the game reads
// from settings.ini: window and render resolution, the profile, and the
// keyboard/mouse bindings. LAUNCH saves them and starts H3Recomp.
//
// Everything is painted by hand (no stock widgets) so that all text can use
// the game's bitmap font. The menu is a stack of pages; each page is a list
// of items, and each item is one of a few kinds (see MenuItem). Keyboard:
// up/down select, left/right change, Enter activates, Escape goes back.
// Mouse: hover, click, right-click to go back, wheel to scroll. A gamepad
// (through SDL) works too, and the button hints switch to its icons while
// one is connected.
//
// The layout follows the window: the menu sits on the left at a width
// relative to the window, and tall windows use the next font size up.

#pragma once

#include "halo_font.h"
#include "config/settings.h"

#include <QElapsedTimer>
#include <QImage>
#include <QPixmap>
#include <QTimer>
#include <QWidget>

#include <functional>
#include <vector>

struct SDL_Gamepad;

namespace h3::launcher
{
    // One row of a menu page.
    struct MenuItem
    {
        enum class Kind
        {
            Action,   // Enter runs activate (open a page, launch, ...)
            Choice,   // left/right call step; value() is shown on the right
            Text,     // Enter starts typing into text (up to maxLength)
            Binding,  // the key/mouse bindings of one action (config::Action)
        };
        Kind kind = Kind::Action;
        QString label;
        QString description;  // shown at the bottom while selected
        std::function<void()> activate;
        std::function<QString()> value;
        std::function<void(int delta)> step;
        std::string* text = nullptr;
        size_t maxLength = 0;
        size_t action = 0;  // Binding: index into the settings' bindings
    };

    class LauncherWindow : public QWidget
    {
    public:
        // xexPath: default.xex to launch, or empty to look for it.
        explicit LauncherWindow(const QString& xexPath);

    protected:
        void paintEvent(QPaintEvent* event) override;
        void resizeEvent(QResizeEvent* event) override;
        void keyPressEvent(QKeyEvent* event) override;
        void mouseMoveEvent(QMouseEvent* event) override;
        void mousePressEvent(QMouseEvent* event) override;
        void wheelEvent(QWheelEvent* event) override;

    private:
        // Text styles, each a size of the game's Conduit font.
        enum class Style
        {
            Title,  // conduit-32: page titles
            Item,   // conduit-23: menu rows
            Small,  // conduit-16: values, descriptions, hints
            Icon,   // conduit-16 at every size: the only font with the button icons
        };
        enum class Align
        {
            Left,
            Right,
            Center,
        };

        // Layout for the current window size (UpdateMetrics).
        struct Metrics
        {
            bool large = false;    // tall window: one font size up
            int margin = 0;        // left edge of the menu
            int menuWidth = 0;
            int menuTop = 0;       // first row
            int titleBaseline = 0;
            int rowHeight = 0;
            int footerHeight = 0;  // description and hints below the menu
            int lineHeight = 0;    // line spacing of Small text
        };

        // A page of the menu and where the user is on it.
        struct Page
        {
            QString title;
            std::vector<MenuItem> items;
            int selected = 0;
            int scroll = 0;  // first visible row
        };

        // --- Game files and settings ---------------------------------------
        void SetXexPath(const QString& path);
        bool ChooseXex();
        QString SettingsPath() const;
        void Save();
        void Launch();

        // --- Pages ---------------------------------------------------------
        Page MainPage();
        Page VideoPage();
        Page ControlsPage();
        Page ProfilePage();
        Page DebugPage();
        void Push(Page page);
        void Back();
        Page& Current() { return pages_.back(); }
        const Page& Current() const { return pages_.back(); }

        // --- Input on the current item --------------------------------------
        void Select(int row);
        void Step(int delta);
        void Activate();
        void StartCapture(int slot);
        void FinishCapture(const QString& name);
        void ClearBinding(int slot);

        // --- Gamepad (SDL) ---------------------------------------------------
        void PollGamepad();
        void GamepadButton(int button);

        // --- Painting -------------------------------------------------------
        void BuildBackground();
        void UpdateMetrics();
        // The Conduit size used for a style at the current window size.
        const char* FontName(Style style) const;
        void DrawHints(QPainter& painter, int x, int baseline);
        void DrawText(QPainter& painter, Style style, int x, int baseline, const QString& text, const QColor& color,
            Align align = Align::Left) const;
        int TextWidth(Style style, const QString& text) const;
        // text broken into lines no wider than width (at spaces).
        QStringList Wrap(Style style, const QString& text, int width) const;
        int Ascent(Style style) const;
        void DrawRow(QPainter& painter, const MenuItem& item, const QRect& row, bool selected);
        void DrawBindingSlots(QPainter& painter, const MenuItem& item, const QRect& row, bool selected);
        void DrawProfileCard(QPainter& painter);

        // Layout, from the window size.
        QRect MenuRect() const;
        int VisibleRows() const;
        int RowAt(QPoint point) const;
        QRect SlotRect(const QRect& row, int slot) const;
        QRect RowRect(int row) const;
        int SlotCount(const MenuItem& item) const;

        QString xexPath_;
        config::Settings settings_;
        FontPackage fonts_;
        bool haveFonts_ = false;
        std::vector<Page> pages_;
        Metrics metrics_;

        // The first connected gamepad (null: none) and its d-pad / stick
        // auto-repeat.
        SDL_Gamepad* gamepad_ = nullptr;
        QTimer gamepadPoll_;
        int padDirection_ = 0;  // 0 none, else the GamepadButton it repeats
        QElapsedTimer padRepeat_;
        int padRepeatDelay_ = 0;

        // Typing into a Text item / waiting for a key for a Binding slot.
        bool editing_ = false;
        bool capturing_ = false;
        int slot_ = 0;  // selected binding slot on a Binding row
        QTimer captureTimeout_;
        QElapsedTimer captureClock_;
        QTimer blink_;  // caret and capture prompt animation

        QImage tinted_;      // the cropped, tinted promo art
        QPixmap background_; // tinted_ scaled to the window, with overlays
        QImage picture_;     // the profile's gamer picture, if any
        QString status_;     // a problem to show (no default.xex, launch failed)
    };
}
