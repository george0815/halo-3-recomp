// The launcher window (launcher_window.h): pages, input, painting, and
// starting the game.

#include "launcher_window.h"
#include "key_names.h"

#include <QCoreApplication>
#include <QDir>
#include <QFileDialog>
#include <QFileInfo>
#include <QKeyEvent>
#include <QLinearGradient>
#include <QPainter>
#include <QProcess>
#include <QRadialGradient>
#include <QSettings>
#include <QStandardPaths>
#include <QTime>

#include <SDL3/SDL.h>

#include <algorithm>
#include <array>
#include <cmath>

namespace h3::launcher
{
    namespace
    {
        // --- Look ------------------------------------------------------------

        // The background is assets/launcher_background_hd.jpg: a 16:9 band
        // of the promo art (assets/launcher_background.jpg, 491x625, rows
        // 30-305: Cortana's eyes and the sky, the Chief's head and torso)
        // upscaled 4x with Real-ESRGAN (x4plus model), since the original is
        // too small and compressed to scale up sharply.

        // Halo 3's main menu palette: the art is turned to grey and mapped
        // through these blues (dark to light), like the menu's tinted scenery.
        struct TintStop
        {
            float position;
            QColor color;
        };
        const std::array<TintStop, 4> kTint = { {
            { 0.00f, QColor(2, 7, 14) },
            { 0.35f, QColor(16, 56, 96) },
            { 0.70f, QColor(96, 160, 212) },
            { 1.00f, QColor(224, 242, 255) },
        } };

        const QColor kTitleColor(220, 239, 255);
        const QColor kTextColor(159, 198, 234);
        const QColor kSelectedTextColor(255, 255, 255);
        const QColor kDimColor(111, 147, 184);
        const QColor kAccentColor(191, 226, 255);
        const QColor kHighlightColor(100, 165, 230);
        const QColor kLineColor(150, 200, 255, 90);
        const QColor kWarningColor(255, 196, 120);

        // --- Layout (see UpdateMetrics; the fonts are bitmaps, so text
        // sizes step between the game's sizes rather than scaling) --------
        constexpr int kDefaultWidth = 1280, kDefaultHeight = 720;
        constexpr int kMinWidth = 960, kMinHeight = 600;
        constexpr int kLargeWidth = 1600, kLargeHeight = 1000;  // from here on, one font size up
        constexpr int kMinMenuWidth = 720;     // room for the longest control label and three slots
        constexpr int kMaxMenuWidth = 1100;
        constexpr float kMenuWidthFraction = 0.5f;
        constexpr float kMarginFraction = 0.06f;
        constexpr int kMinMargin = 48, kMaxMargin = 160;
        constexpr float kTitleFraction = 0.16f;  // title baseline, as a fraction of the height
        constexpr int kMinTitleBaseline = 100;
        constexpr int kTitleToRows = 52;       // title baseline to the first row
        constexpr int kRowPadding = 9;         // row height beyond the item font's height
        constexpr int kTextInset = 18;         // label inset inside a row
        constexpr int kLabelGap = 24;          // between the longest binding label and the slots
        constexpr int kMinSlotWidth = 80;      // one binding slot, at least
        constexpr int kSlotGap = 6;
        constexpr int kPictureSize = 64;       // gamer picture on the profile card
        constexpr int kHintGap = 36;           // between two button hints

        // Gamepad navigation: polled from SDL; held directions repeat.
        constexpr int kPadPollMs = 16;
        constexpr int kPadRepeatDelayMs = 400;
        constexpr int kPadRepeatMs = 110;
        constexpr int kStickThreshold = 16000;  // of 32767: stick pushed far enough to count

        // How long a binding slot waits for a key before giving up.
        constexpr int kCaptureTimeoutMs = 5000;
        constexpr int kBlinkMs = 500;

        // The game's button icons in conduit-16 (private use characters).
        constexpr char16_t kIconA = 0xE100;
        constexpr char16_t kIconB = 0xE101;
        constexpr char16_t kIconY = 0xE103;
        constexpr char16_t kIconUp = 0xE108;
        constexpr char16_t kIconDown = 0xE109;

        // Window sizes offered on the video page.
        constexpr std::array<QSize, 6> kWindowSizes = { {
            { 1280, 720 }, { 1366, 768 }, { 1600, 900 }, { 1920, 1080 }, { 2560, 1440 }, { 3840, 2160 },
        } };

        // Halo 3 renders 1280x720; render scale multiplies that.
        constexpr int kNativeWidth = 1280, kNativeHeight = 720;

        // Mouse sensitivity choices.
        constexpr float kSensitivityStep = 0.25f;
        constexpr float kMinSensitivity = 0.25f, kMaxSensitivity = 5.0f;

        // Where the launcher remembers default.xex (QSettings scope).
        constexpr const char* kOrganization = "h3recomp";
        constexpr const char* kApplication = "launcher";
        constexpr const char* kXexKey = "xex";

        QString OnOff(bool value) { return value ? QStringLiteral("On") : QStringLiteral("Off"); }

        // The art is dark; grey levels are lifted by this gamma before the
        // palette so the mid-tones (Cortana's face, the sky) show.
        constexpr float kTintGamma = 0.75f;

        // The 256-entry palette for kTint, indexed by grey level.
        std::array<QRgb, 256> TintTable()
        {
            std::array<QRgb, 256> table{};
            for (int i = 0; i < 256; i++)
            {
                float t = std::pow(i / 255.0f, kTintGamma);
                size_t stop = 1;
                while (stop + 1 < kTint.size() && t > kTint[stop].position)
                    stop++;
                const TintStop& a = kTint[stop - 1];
                const TintStop& b = kTint[stop];
                float f = std::clamp((t - a.position) / (b.position - a.position), 0.0f, 1.0f);
                auto mix = [&](int x, int y) { return int(std::lround(x + (y - x) * f)); };
                table[i] = qRgb(mix(a.color.red(), b.color.red()), mix(a.color.green(), b.color.green()),
                    mix(a.color.blue(), b.color.blue()));
            }
            return table;
        }

        // Looks for default.xex where a source checkout keeps it
        // (private/default.xex) above the launcher's own folder and the
        // current directory.
        QString FindXex()
        {
            QStringList starts = { QCoreApplication::applicationDirPath(), QDir::currentPath() };
            for (const QString& start : starts)
            {
                QDir dir(start);
                for (int depth = 0; depth < 5; depth++)
                {
                    QString candidate = dir.filePath(QStringLiteral("private/default.xex"));
                    if (QFileInfo::exists(candidate))
                        return QFileInfo(candidate).absoluteFilePath();
                    if (!dir.cdUp())
                        break;
                }
            }
            return {};
        }

        // The H3Recomp executable: beside the launcher, in the build tree's
        // H3Recomp/ folder next to H3Launcher/, or on PATH.
        QString FindGameExecutable()
        {
            QDir dir(QCoreApplication::applicationDirPath());
            for (const QString& candidate : { dir.filePath("H3Recomp"), dir.filePath("../H3Recomp/H3Recomp") })
                if (QFileInfo(candidate).isExecutable())
                    return QFileInfo(candidate).absoluteFilePath();
            return QStandardPaths::findExecutable(QStringLiteral("H3Recomp"));
        }

        // The game's disc folder for a default.xex, as main.cpp picks it.
        QString ContentRoot(const QString& xexPath)
        {
            QDir dir = QFileInfo(xexPath).absoluteDir();
            return dir.exists(QStringLiteral("game")) ? dir.filePath(QStringLiteral("game")) : dir.path();
        }

        QString FromStd(const std::string& text) { return QString::fromStdString(text); }
    }

    LauncherWindow::LauncherWindow(const QString& xexPath)
    {
        setWindowTitle(QStringLiteral("Halo 3 Recomp"));
        setMinimumSize(kMinWidth, kMinHeight);
        resize(kDefaultWidth, kDefaultHeight);
        setMouseTracking(true);
        setFocusPolicy(Qt::StrongFocus);

        captureTimeout_.setSingleShot(true);
        connect(&captureTimeout_, &QTimer::timeout, this, [this] { FinishCapture({}); });
        blink_.setInterval(kBlinkMs);
        connect(&blink_, &QTimer::timeout, this, qOverload<>(&QWidget::update));
        blink_.start();

        // Gamepads, through SDL (Qt 6 has no gamepad support). No SDL window
        // exists, so allow events without one having focus.
        SDL_SetHint(SDL_HINT_JOYSTICK_ALLOW_BACKGROUND_EVENTS, "1");
        if (SDL_InitSubSystem(SDL_INIT_GAMEPAD))
        {
            connect(&gamepadPoll_, &QTimer::timeout, this, [this] { PollGamepad(); });
            gamepadPoll_.start(kPadPollMs);
        }
        else
            qWarning("SDL gamepad support unavailable: %s", SDL_GetError());

        settings_ = config::Defaults();
        QString path = xexPath;
        if (path.isEmpty())
            path = QSettings(kOrganization, kApplication).value(kXexKey).toString();
        if (path.isEmpty() || !QFileInfo::exists(path))
            path = FindXex();
        SetXexPath(path);

        BuildBackground();
        UpdateMetrics();
        Push(MainPage());
    }

    // --- Game files and settings ---------------------------------------------

    void LauncherWindow::SetXexPath(const QString& path)
    {
        xexPath_ = path;
        status_.clear();
        if (xexPath_.isEmpty())
        {
            status_ = QStringLiteral("default.xex not found. Select LAUNCH to locate it.");
            return;
        }
        QSettings(kOrganization, kApplication).setValue(kXexKey, xexPath_);

        // The settings live next to the XEX; the fonts come from the disc.
        config::Load(SettingsPath().toStdString(), settings_);
        QString fontPath = ContentRoot(xexPath_) + QStringLiteral("/maps/fonts/font_package.bin");
        QString error;
        haveFonts_ = fonts_.Load(fontPath, &error) && fonts_.Find("conduit-16") && fonts_.Find("conduit-23") &&
            fonts_.Find("conduit-32");
        if (!haveFonts_)
            qWarning("Halo 3 fonts unavailable (%s); using a system font", qPrintable(error));
        picture_ = QImage(FromStd(settings_.profile.picture));
        UpdateMetrics();  // font heights set the row height
    }

    bool LauncherWindow::ChooseXex()
    {
        QString path = QFileDialog::getOpenFileName(this, QStringLiteral("Locate default.xex"), QDir::currentPath(),
            QStringLiteral("Xbox 360 executable (default.xex *.xex)"));
        if (path.isEmpty())
            return false;
        SetXexPath(path);
        if (pages_.size() == 1)
        {
            int selected = Current().selected;
            pages_.back() = MainPage();  // the GAME LOCATION description names the path
            Current().selected = selected;
        }
        update();
        return true;
    }

    QString LauncherWindow::SettingsPath() const
    {
        return QFileInfo(xexPath_).absoluteDir().filePath(QString::fromLatin1(config::kFileName));
    }

    void LauncherWindow::Save()
    {
        if (xexPath_.isEmpty())
            return;  // nowhere to put it yet; kept in memory until the XEX is found
        if (!config::Save(SettingsPath().toStdString(), settings_))
            status_ = QStringLiteral("Can't write %1").arg(SettingsPath());
    }

    void LauncherWindow::Launch()
    {
        if (xexPath_.isEmpty() && !ChooseXex())
            return;
        Save();
        QString game = FindGameExecutable();
        if (game.isEmpty())
        {
            status_ = QStringLiteral("H3Recomp not found next to the launcher. Build it first.");
            update();
            return;
        }
        // Run from the folder above private/ (the checkout root), as when
        // starting H3Recomp by hand; its paths all follow the XEX anyway.
        QDir workingDir = QFileInfo(xexPath_).absoluteDir();
        workingDir.cdUp();
        if (!QProcess::startDetached(game, { xexPath_ }, workingDir.path()))
        {
            status_ = QStringLiteral("Couldn't start %1").arg(game);
            update();
            return;
        }
        close();
    }

    // --- Pages ---------------------------------------------------------------

    LauncherWindow::Page LauncherWindow::MainPage()
    {
        Page page{ QStringLiteral("HALO 3 RECOMP") };
        auto open = [this](Page (LauncherWindow::*build)()) { return [this, build] { Push((this->*build)()); }; };
        page.items.push_back({ MenuItem::Kind::Action, QStringLiteral("LAUNCH"),
            QStringLiteral("Save the settings and start Halo 3."), [this] { Launch(); } });
        page.items.push_back({ MenuItem::Kind::Action, QStringLiteral("VIDEO"),
            QStringLiteral("Window size, fullscreen and rendering resolution."), open(&LauncherWindow::VideoPage) });
        page.items.push_back({ MenuItem::Kind::Action, QStringLiteral("CONTROLS"),
            QStringLiteral("Keyboard and mouse bindings. Controllers always use the standard layout."),
            open(&LauncherWindow::ControlsPage) });
        page.items.push_back({ MenuItem::Kind::Action, QStringLiteral("PROFILE"),
            QStringLiteral("Gamertag, gamer picture and profile options."), open(&LauncherWindow::ProfilePage) });
        page.items.push_back({ MenuItem::Kind::Action, QStringLiteral("DEBUG"),
            QStringLiteral("Logging and tracing options, and unlocking every campaign mission."), open(&LauncherWindow::DebugPage) });
        page.items.push_back({ MenuItem::Kind::Action, QStringLiteral("GAME LOCATION"),
            xexPath_.isEmpty() ? QStringLiteral("Choose the default.xex to run.")
                               : QStringLiteral("Running %1. Select to choose another default.xex.").arg(xexPath_),
            [this] { ChooseXex(); } });
        page.items.push_back({ MenuItem::Kind::Action, QStringLiteral("QUIT"), QStringLiteral("Close the launcher."),
            [this] { close(); } });
        return page;
    }

    LauncherWindow::Page LauncherWindow::VideoPage()
    {
        Page page{ QStringLiteral("VIDEO") };
        config::VideoSettings& video = settings_.video;

        MenuItem size{ MenuItem::Kind::Choice, QStringLiteral("Window size"),
            QStringLiteral("The window's size when not fullscreen. The picture keeps its 16:9 shape.") };
        size.value = [&video] { return QStringLiteral("%1 x %2").arg(video.width).arg(video.height); };
        size.step = [&video](int delta)
        {
            // Move to the next preset larger/smaller than the current size
            // (a hand-edited size steps to its neighbour).
            int area = video.width * video.height, index = -1;
            for (int i = 0; i < int(kWindowSizes.size()); i++)
                if (kWindowSizes[i].width() * kWindowSizes[i].height() <= area)
                    index = i;
            bool exact = index >= 0 && kWindowSizes[index] == QSize(video.width, video.height);
            int next = delta > 0 ? index + 1 : (exact ? index - 1 : index);
            next = std::clamp(next, 0, int(kWindowSizes.size()) - 1);
            video.width = kWindowSizes[next].width();
            video.height = kWindowSizes[next].height();
        };
        page.items.push_back(size);

        MenuItem fullscreen{ MenuItem::Kind::Choice, QStringLiteral("Fullscreen"),
            QStringLiteral("Borderless fullscreen at the desktop resolution. F11 or Alt+Enter switches in game.") };
        fullscreen.value = [&video] { return OnOff(video.fullscreen); };
        fullscreen.step = [&video](int) { video.fullscreen = !video.fullscreen; };
        page.items.push_back(fullscreen);

        MenuItem vsync{ MenuItem::Kind::Choice, QStringLiteral("VSync"),
            QStringLiteral("Wait for the display's refresh. Off can tear but lowers latency.") };
        vsync.value = [&video] { return OnOff(video.vsync); };
        vsync.step = [&video](int) { video.vsync = !video.vsync; };
        page.items.push_back(vsync);

        MenuItem scale{ MenuItem::Kind::Choice, QStringLiteral("Render resolution"),
            QStringLiteral("Draw the game at a multiple of its native 1280x720, then fit it to the window. "
                           "Higher is sharper and slower.") };
        scale.value = [&video]
        {
            return QStringLiteral("%1x  (%2 x %3)").arg(video.renderScale).arg(kNativeWidth * video.renderScale).arg(kNativeHeight * video.renderScale);
        };
        scale.step = [&video](int delta) { video.renderScale = std::clamp(video.renderScale + delta, 1, config::kMaxRenderScale); };
        page.items.push_back(scale);
        return page;
    }

    LauncherWindow::Page LauncherWindow::ControlsPage()
    {
        Page page{ QStringLiteral("CONTROLS") };
        config::ControlSettings& controls = settings_.controls;

        MenuItem sensitivity{ MenuItem::Kind::Choice, QStringLiteral("Mouse sensitivity"),
            QStringLiteral("How fast the mouse turns the view.") };
        sensitivity.value = [&controls] { return QString::number(controls.mouseSensitivity, 'f', 2); };
        sensitivity.step = [&controls](int delta)
        {
            float steps = std::round(controls.mouseSensitivity / kSensitivityStep) + float(delta);
            controls.mouseSensitivity = std::clamp(steps * kSensitivityStep, kMinSensitivity, kMaxSensitivity);
        };
        page.items.push_back(sensitivity);

        MenuItem invert{ MenuItem::Kind::Choice, QStringLiteral("Invert mouse"),
            QStringLiteral("Push the mouse forward to look down.") };
        invert.value = [&controls] { return OnOff(controls.invertMouse); };
        invert.step = [&controls](int) { controls.invertMouse = !controls.invertMouse; };
        page.items.push_back(invert);

        for (size_t action = 0; action < config::kActionCount; action++)
        {
            const config::ActionInfo& info = config::Describe(config::Action(action));
            MenuItem binding{ MenuItem::Kind::Binding, QString::fromLatin1(info.label),
                QStringLiteral("%1. Enter or click a slot, then press a key or mouse button. Delete clears it.")
                    .arg(QString::fromLatin1(info.pad)) };
            binding.action = action;
            page.items.push_back(binding);
        }

        page.items.push_back({ MenuItem::Kind::Action, QStringLiteral("Restore default controls"),
            QStringLiteral("Put every binding and the mouse options back to the defaults."), [this]
            {
                settings_.controls = config::Defaults().controls;
                Save();
            } });
        return page;
    }

    LauncherWindow::Page LauncherWindow::ProfilePage()
    {
        Page page{ QStringLiteral("PROFILE") };
        config::ProfileSettings& profile = settings_.profile;

        MenuItem gamertag{ MenuItem::Kind::Text, QStringLiteral("Gamertag"),
            QStringLiteral("Your name in game (up to 15 characters). Enter to edit.") };
        gamertag.text = &profile.gamertag;
        gamertag.maxLength = config::kMaxGamertag;
        page.items.push_back(gamertag);

        MenuItem picture{ MenuItem::Kind::Action, QStringLiteral("Gamer picture"),
            QStringLiteral("An image for your profile card. Halo 3 itself shows your emblem, set in game.") };
        picture.value = [&profile] { return profile.picture.empty() ? QStringLiteral("None") : QFileInfo(FromStd(profile.picture)).fileName(); };
        picture.activate = [this, &profile]
        {
            QString path = QFileDialog::getOpenFileName(this, QStringLiteral("Gamer picture"), QDir::homePath(),
                QStringLiteral("Images (*.png *.jpg *.jpeg *.bmp *.webp)"));
            if (path.isEmpty())
                return;
            profile.picture = path.toStdString();
            picture_ = QImage(path);
            Save();
        };
        page.items.push_back(picture);

        // The standard 360 profile options. Halo 3 copies them into a new
        // player profile; its own settings menu wins after that.
        auto toggle = [&page](const QString& label, const QString& description, bool& value)
        {
            MenuItem item{ MenuItem::Kind::Choice, label, description };
            item.value = [&value] { return OnOff(value); };
            item.step = [&value](int) { value = !value; };
            page.items.push_back(item);
        };
        const QString note = QStringLiteral(" Used when Halo 3 creates your player profile.");
        toggle(QStringLiteral("Invert look"), QStringLiteral("Push forward to look down.") + note, profile.invertLook);
        toggle(QStringLiteral("Vibration"), QStringLiteral("Controller rumble.") + note, profile.vibration);
        toggle(QStringLiteral("Auto-aim"), QStringLiteral("Aim assist.") + note, profile.autoAim);
        toggle(QStringLiteral("Auto-center"), QStringLiteral("Level the view while moving.") + note, profile.autoCenter);
        return page;
    }

    namespace
    {
        // A debug switch on the Debug page: an H3_* variable set to 1.
        struct DebugFlag
        {
            const char* variable;
            const char* label;
            const char* description;
        };
        constexpr DebugFlag kDebugFlags[] = {
            { "H3_LOG_FPS", "Log frame rate", "Frames per second and the slowest frame, once a second." },
            { "H3_LOG_SLOW", "Log slow work", "Shader translations, pipeline creations and texture uploads that take 20 ms or more." },
            { "H3_LOG_GPU", "Log GPU setup", "Ring buffer setup, the first use of each PM4 opcode, and frame counts." },
            { "H3_LOG_AUDIO", "Log audio", "Audio frames submitted and their peak level, once a second." },
            { "H3_LOG_INPUT", "Log input", "Every controller state change." },
            { "H3_LOG_USER", "Log profile calls", "Profile, sign-in and profile setting calls." },
            { "H3_LOG_CONTENT", "Log content calls", "Content package (save) calls." },
            { "H3_LOG_NOTIFY", "Log notifications", "System notifications sent to the game." },
            { "H3_LOG_FILES", "Log file opens", "File opens, attribute queries and symbolic links." },
            { "H3_LOG_READS", "Log file reads", "Every file read. Very verbose." },
            { "H3_LOG_MEMORY", "Log memory", "Physical memory allocations." },
            { "H3_LOG_WAITS", "Log kernel waits", "Time each thread spends waiting, every 150 frames." },
            { "H3_LOG_RESOLVES", "Log resolves", "Every render target resolve. Very verbose." },
            { "H3_LOG_CLEARS", "Log clears", "D3D clears with their rectangles and colors. Very verbose." },
            { "H3_STRICT_IMPORTS", "Strict imports", "Stop at the first kernel function that isn't implemented." },
        };

        // A debug setting with a few preset values (first = off).
        struct DebugChoice
        {
            const char* variable;
            const char* label;
            const char* description;
            std::array<const char*, 5> values;  // empty = off; null = unused
        };
        constexpr DebugChoice kDebugChoices[] = {
            { "H3_GPU_CAPTURE", "GPU capture frame", "Write gpu_capture.txt: every draw, resolve and shader load in that frame.",
                { "", "300", "900", "1800", "3600" } },
            { "H3_PROFILE", "CPU profile frames", "Sample host CPU time between those frames into profile.txt (tools/h3profile.py).",
                { "", "600-900", "1800-2100", "3600-3900", nullptr } },
            { "H3_SNAPSHOT_EVERY", "Snapshot every", "Save every n-th presented frame as frame_<n>.png in the working directory.",
                { "", "30", "150", "600", nullptr } },
        };
    }

    LauncherWindow::Page LauncherWindow::DebugPage()
    {
        Page page{ QStringLiteral("DEBUG") };
        config::DebugSettings& debug = settings_.debug;

        MenuItem unlock{ MenuItem::Kind::Choice, QStringLiteral("Unlock all missions"),
            QStringLiteral("Every campaign mission can be chosen in the campaign lobby. Completion records stay "
                           "as they are; if the game saves your profile while this is on, the unlocks are kept.") };
        unlock.value = [&debug] { return OnOff(debug.unlockAllLevels); };
        unlock.step = [&debug](int) { debug.unlockAllLevels = !debug.unlockAllLevels; };
        page.items.push_back(unlock);

        MenuItem logFile{ MenuItem::Kind::Choice, QStringLiteral("Write log file"),
            QStringLiteral("Send the game's console output to %1 next to default.xex instead of the terminal.")
                .arg(QString::fromLatin1(config::kLogFileName)) };
        logFile.value = [&debug] { return OnOff(debug.logToFile); };
        logFile.step = [&debug](int) { debug.logToFile = !debug.logToFile; };
        page.items.push_back(logFile);

        for (const DebugFlag& flag : kDebugFlags)
        {
            MenuItem item{ MenuItem::Kind::Choice, QString::fromLatin1(flag.label),
                QStringLiteral("%1 (%2)").arg(QString::fromLatin1(flag.description), QString::fromLatin1(flag.variable)) };
            std::string variable = flag.variable;
            item.value = [&debug, variable] { return OnOff(debug.environment.count(variable) != 0); };
            item.step = [&debug, variable](int)
            {
                if (!debug.environment.erase(variable))
                    debug.environment[variable] = "1";
            };
            page.items.push_back(item);
        }

        for (const DebugChoice& choice : kDebugChoices)
        {
            MenuItem item{ MenuItem::Kind::Choice, QString::fromLatin1(choice.label),
                QStringLiteral("%1 (%2)").arg(QString::fromLatin1(choice.description), QString::fromLatin1(choice.variable)) };
            std::string variable = choice.variable;
            int count = 0;
            while (count < int(choice.values.size()) && choice.values[count])
                count++;
            const DebugChoice* preset = &choice;
            auto index = [&debug, variable, preset, count]
            {
                auto found = debug.environment.find(variable);
                for (int i = 1; found != debug.environment.end() && i < count; i++)
                    if (found->second == preset->values[i])
                        return i;
                return 0;
            };
            item.value = [index, preset]
            {
                int i = index();
                return i == 0 ? QStringLiteral("Off") : QString::fromLatin1(preset->values[i]);
            };
            item.step = [&debug, variable, preset, count, index](int delta)
            {
                int i = (index() + delta + count) % count;
                if (i == 0)
                    debug.environment.erase(variable);
                else
                    debug.environment[variable] = preset->values[i];
            };
            page.items.push_back(item);
        }

        page.items.push_back({ MenuItem::Kind::Action, QStringLiteral("Turn everything off"),
            QStringLiteral("Clear every debug option on this page."), [this]
            {
                settings_.debug = config::DebugSettings();
                Save();
            } });
        return page;
    }

    // --- Gamepad ---------------------------------------------------------------

    void LauncherWindow::PollGamepad()
    {
        SDL_Event event;
        while (SDL_PollEvent(&event))
        {
            switch (event.type)
            {
            case SDL_EVENT_GAMEPAD_ADDED:
                if (!gamepad_)
                    gamepad_ = SDL_OpenGamepad(event.gdevice.which);
                update();
                break;
            case SDL_EVENT_GAMEPAD_REMOVED:
                if (gamepad_ && SDL_GetGamepadID(gamepad_) == event.gdevice.which)
                {
                    SDL_CloseGamepad(gamepad_);
                    gamepad_ = nullptr;
                }
                update();
                break;
            case SDL_EVENT_GAMEPAD_BUTTON_DOWN:
                if (gamepad_ && event.gbutton.which == SDL_GetGamepadID(gamepad_))
                    GamepadButton(event.gbutton.button);
                break;
            default:
                break;
            }
        }
        if (!gamepad_)
            return;

        // The d-pad or left stick as a direction, repeating while held.
        auto axis = [this](SDL_GamepadAxis a) { return SDL_GetGamepadAxis(gamepad_, a); };
        int direction = 0;
        if (SDL_GetGamepadButton(gamepad_, SDL_GAMEPAD_BUTTON_DPAD_UP) || axis(SDL_GAMEPAD_AXIS_LEFTY) < -kStickThreshold)
            direction = SDL_GAMEPAD_BUTTON_DPAD_UP;
        else if (SDL_GetGamepadButton(gamepad_, SDL_GAMEPAD_BUTTON_DPAD_DOWN) || axis(SDL_GAMEPAD_AXIS_LEFTY) > kStickThreshold)
            direction = SDL_GAMEPAD_BUTTON_DPAD_DOWN;
        else if (SDL_GetGamepadButton(gamepad_, SDL_GAMEPAD_BUTTON_DPAD_LEFT) || axis(SDL_GAMEPAD_AXIS_LEFTX) < -kStickThreshold)
            direction = SDL_GAMEPAD_BUTTON_DPAD_LEFT;
        else if (SDL_GetGamepadButton(gamepad_, SDL_GAMEPAD_BUTTON_DPAD_RIGHT) || axis(SDL_GAMEPAD_AXIS_LEFTX) > kStickThreshold)
            direction = SDL_GAMEPAD_BUTTON_DPAD_RIGHT;
        if (direction != padDirection_)
        {
            padDirection_ = direction;
            if (direction)
            {
                GamepadButton(direction);
                padRepeat_.start();
                padRepeatDelay_ = kPadRepeatDelayMs;
            }
        }
        else if (direction && padRepeat_.elapsed() >= padRepeatDelay_)
        {
            GamepadButton(direction);
            padRepeat_.start();
            padRepeatDelay_ = kPadRepeatMs;
        }
    }

    void LauncherWindow::GamepadButton(int button)
    {
        if (capturing_)
        {
            // A gamepad can't be bound (it always drives its own buttons).
            if (button == SDL_GAMEPAD_BUTTON_EAST)
                FinishCapture({});
            return;
        }
        if (editing_)
        {
            if (button == SDL_GAMEPAD_BUTTON_SOUTH || button == SDL_GAMEPAD_BUTTON_EAST)
            {
                editing_ = false;
                Save();
                update();
            }
            return;
        }
        MenuItem& item = Current().items[Current().selected];
        switch (button)
        {
        case SDL_GAMEPAD_BUTTON_DPAD_UP: Select(Current().selected - 1); break;
        case SDL_GAMEPAD_BUTTON_DPAD_DOWN: Select(Current().selected + 1); break;
        case SDL_GAMEPAD_BUTTON_DPAD_LEFT: Step(-1); break;
        case SDL_GAMEPAD_BUTTON_DPAD_RIGHT: Step(1); break;
        case SDL_GAMEPAD_BUTTON_SOUTH: Activate(); break;
        case SDL_GAMEPAD_BUTTON_EAST: Back(); break;
        case SDL_GAMEPAD_BUTTON_NORTH:
            if (item.kind == MenuItem::Kind::Binding)
                ClearBinding(slot_);
            break;
        default: break;
        }
    }

    void LauncherWindow::Push(Page page)
    {
        pages_.push_back(std::move(page));
        slot_ = 0;
        update();
    }

    void LauncherWindow::Back()
    {
        if (pages_.size() > 1)
        {
            pages_.pop_back();
            slot_ = 0;
            update();
        }
    }

    // --- Input ---------------------------------------------------------------

    void LauncherWindow::Select(int row)
    {
        Page& page = Current();
        row = std::clamp(row, 0, int(page.items.size()) - 1);
        if (row != page.selected)
            slot_ = 0;
        page.selected = row;
        // Keep the selection on screen.
        int visible = VisibleRows();
        if (row < page.scroll)
            page.scroll = row;
        else if (row >= page.scroll + visible)
            page.scroll = row - visible + 1;
        update();
    }

    void LauncherWindow::Step(int delta)
    {
        MenuItem& item = Current().items[Current().selected];
        if (item.kind == MenuItem::Kind::Binding)
            slot_ = std::clamp(slot_ + delta, 0, SlotCount(item) - 1);
        else if (item.step)
        {
            item.step(delta);
            Save();
        }
        update();
    }

    void LauncherWindow::Activate()
    {
        MenuItem& item = Current().items[Current().selected];
        switch (item.kind)
        {
        case MenuItem::Kind::Action:
            if (item.activate)
                item.activate();
            break;
        case MenuItem::Kind::Choice:
            Step(1);
            break;
        case MenuItem::Kind::Text:
            editing_ = true;
            break;
        case MenuItem::Kind::Binding:
            StartCapture(slot_);
            break;
        }
        update();
    }

    void LauncherWindow::StartCapture(int slot)
    {
        slot_ = slot;
        capturing_ = true;
        captureClock_.start();
        captureTimeout_.start(kCaptureTimeoutMs);
        grabMouse();  // so a click anywhere binds that button
        update();
    }

    void LauncherWindow::FinishCapture(const QString& name)
    {
        if (!capturing_)
            return;
        capturing_ = false;
        captureTimeout_.stop();
        releaseMouse();
        if (!name.isEmpty())
        {
            std::vector<std::string>& bindings = settings_.controls.bindings[Current().items[Current().selected].action];
            std::string binding = name.toStdString();
            // A name appears once per action: rebinding a slot to a key
            // already in another slot just moves it.
            std::erase(bindings, binding);
            size_t slot = std::min<size_t>(slot_, bindings.size());
            if (slot < bindings.size())
                bindings[slot] = binding;
            else if (bindings.size() < config::kMaxBindings)
                bindings.push_back(binding);
            Save();
        }
        update();
    }

    void LauncherWindow::ClearBinding(int slot)
    {
        std::vector<std::string>& bindings = settings_.controls.bindings[Current().items[Current().selected].action];
        if (slot < int(bindings.size()))
        {
            bindings.erase(bindings.begin() + slot);
            slot_ = std::clamp(slot_, 0, SlotCount(Current().items[Current().selected]) - 1);
            Save();
        }
        update();
    }

    void LauncherWindow::keyPressEvent(QKeyEvent* event)
    {
        if (event->isAutoRepeat() && capturing_)
            return;
        if (capturing_)
        {
            QString name = BindingNameForKey(*event);
            if (!name.isEmpty())
                FinishCapture(name);
            return;
        }
        MenuItem& item = Current().items[Current().selected];
        if (editing_)
        {
            std::string& text = *item.text;
            switch (event->key())
            {
            case Qt::Key_Return:
            case Qt::Key_Enter:
            case Qt::Key_Escape:
            case Qt::Key_Up:
            case Qt::Key_Down:
                editing_ = false;
                if (text.empty())
                    text = config::Defaults().profile.gamertag;
                Save();
                break;
            case Qt::Key_Backspace:
                if (!text.empty())
                    text.pop_back();
                break;
            default:
                // Gamertags are letters, digits and spaces.
                for (QChar c : event->text())
                    if (c.unicode() < 0x80 && (c.isLetterOrNumber() || c == ' ') && text.size() < item.maxLength)
                        text.push_back(char(c.unicode()));
                break;
            }
            update();
            return;
        }

        switch (event->key())
        {
        case Qt::Key_Up: Select(Current().selected - 1); break;
        case Qt::Key_Down: Select(Current().selected + 1); break;
        case Qt::Key_PageUp: Select(Current().selected - VisibleRows()); break;
        case Qt::Key_PageDown: Select(Current().selected + VisibleRows()); break;
        case Qt::Key_Home: Select(0); break;
        case Qt::Key_End: Select(int(Current().items.size()) - 1); break;
        case Qt::Key_Left: Step(-1); break;
        case Qt::Key_Right: Step(1); break;
        case Qt::Key_Return:
        case Qt::Key_Enter:
        case Qt::Key_Space: Activate(); break;
        case Qt::Key_Delete:
            if (item.kind == MenuItem::Kind::Binding)
                ClearBinding(slot_);
            break;
        case Qt::Key_Backspace:
            if (item.kind == MenuItem::Kind::Binding)
                ClearBinding(slot_);
            else
                Back();
            break;
        case Qt::Key_Escape: Back(); break;
        default: QWidget::keyPressEvent(event); break;
        }
    }

    void LauncherWindow::mouseMoveEvent(QMouseEvent* event)
    {
        if (capturing_ || editing_)
            return;
        int row = RowAt(event->position().toPoint());
        if (row >= 0 && row != Current().selected)
            Select(row);
    }

    void LauncherWindow::mousePressEvent(QMouseEvent* event)
    {
        if (capturing_)
        {
            FinishCapture(BindingNameForMouse(event->button()));
            return;
        }
        if (event->button() == Qt::BackButton)
        {
            Back();
            return;
        }
        int row = RowAt(event->position().toPoint());
        if (row < 0)
        {
            if (editing_)
                editing_ = false, Save();
            update();
            return;
        }
        Select(row);
        MenuItem& item = Current().items[row];
        QRect rect = RowRect(row);
        QPoint point = event->position().toPoint();
        // Right-click goes back, except on a binding slot, where it clears it.
        if (item.kind == MenuItem::Kind::Binding)
        {
            for (int slot = 0; slot < SlotCount(item); slot++)
                if (SlotRect(rect, slot).contains(point))
                {
                    slot_ = slot;
                    if (event->button() == Qt::RightButton)
                        ClearBinding(slot);
                    else
                        StartCapture(slot);
                    update();
                    return;
                }
        }
        if (event->button() == Qt::RightButton)
        {
            Back();
            return;
        }
        if (event->button() != Qt::LeftButton)
            return;
        if (item.kind == MenuItem::Kind::Choice)
            Step(point.x() < rect.right() - rect.width() / 4 ? -1 : 1);  // left of the value steps back
        else if (item.kind != MenuItem::Kind::Binding)
            Activate();
        update();
    }

    void LauncherWindow::wheelEvent(QWheelEvent* event)
    {
        if (capturing_)
        {
            FinishCapture(QString::fromUtf8(config::kMouseWheel.data(), config::kMouseWheel.size()));
            return;
        }
        Page& page = Current();
        int notches = event->angleDelta().y() / 120;
        int maxScroll = std::max(0, int(page.items.size()) - VisibleRows());
        page.scroll = std::clamp(page.scroll - notches, 0, maxScroll);
        page.selected = std::clamp(page.selected, page.scroll, page.scroll + VisibleRows() - 1);
        update();
    }

    // --- Layout --------------------------------------------------------------

    void LauncherWindow::UpdateMetrics()
    {
        Metrics& m = metrics_;
        m.large = width() >= kLargeWidth && height() >= kLargeHeight;
        m.margin = std::clamp(int(width() * kMarginFraction), kMinMargin, kMaxMargin);
        m.menuWidth = std::clamp(int(width() * kMenuWidthFraction), kMinMenuWidth, kMaxMenuWidth);
        m.menuWidth = std::min(m.menuWidth, width() - 2 * m.margin);
        m.titleBaseline = std::max(kMinTitleBaseline, int(height() * kTitleFraction));
        m.menuTop = m.titleBaseline + kTitleToRows;
        int itemHeight = haveFonts_ ? fonts_.Find(FontName(Style::Item))->Height() : (m.large ? 46 : 33);
        int smallHeight = haveFonts_ ? fonts_.Find(FontName(Style::Small))->Height() : (m.large ? 33 : 22);
        m.rowHeight = itemHeight + kRowPadding;
        m.lineHeight = smallHeight;
        m.footerHeight = 3 * smallHeight + 54;  // two description lines and the hints
    }

    const char* LauncherWindow::FontName(Style style) const
    {
        switch (style)
        {
        case Style::Title: return "conduit-32";
        case Style::Item: return metrics_.large ? "conduit-32" : "conduit-23";
        case Style::Small: return metrics_.large ? "conduit-23" : "conduit-16";
        case Style::Icon: return "conduit-16";
        }
        return "conduit-16";
    }

    QRect LauncherWindow::MenuRect() const
    {
        // Left-aligned; the menu column follows the window's size.
        const Metrics& m = metrics_;
        return QRect(m.margin, m.menuTop, m.menuWidth, height() - m.menuTop - m.footerHeight);
    }

    QRect LauncherWindow::RowRect(int row) const
    {
        QRect menu = MenuRect();
        return QRect(menu.left(), menu.top() + (row - Current().scroll) * metrics_.rowHeight, menu.width(), metrics_.rowHeight);
    }

    int LauncherWindow::VisibleRows() const
    {
        return std::max(1, MenuRect().height() / metrics_.rowHeight);
    }

    int LauncherWindow::RowAt(QPoint point) const
    {
        QRect menu = MenuRect();
        if (!menu.contains(point))
            return -1;
        int row = Current().scroll + (point.y() - menu.top()) / metrics_.rowHeight;
        int last = std::min(int(Current().items.size()), Current().scroll + VisibleRows()) - 1;
        return row <= last ? row : -1;
    }

    int LauncherWindow::SlotCount(const MenuItem& item) const
    {
        // The bindings plus one empty slot to add another, up to the limit.
        size_t bound = settings_.controls.bindings[item.action].size();
        return int(std::min(bound + 1, config::kMaxBindings));
    }

    QRect LauncherWindow::SlotRect(const QRect& row, int slot) const
    {
        // The slots start after the page's longest binding label and share
        // the rest of the row.
        int labels = 0;
        for (const MenuItem& item : Current().items)
            if (item.kind == MenuItem::Kind::Binding)
                labels = std::max(labels, TextWidth(Style::Item, item.label));
        int left = row.left() + kTextInset + labels + kLabelGap;
        int right = row.right() - kTextInset;
        int count = int(config::kMaxBindings);
        int width = std::max(kMinSlotWidth, (right - left - (count - 1) * kSlotGap) / count);
        left = std::min(left, right - count * width - (count - 1) * kSlotGap);
        return QRect(left + slot * (width + kSlotGap), row.top() + 6, width, row.height() - 12);
    }

    // --- Painting ------------------------------------------------------------

    void LauncherWindow::BuildBackground()
    {
        QImage art(QStringLiteral(":/launcher_background.jpg"));
        if (art.isNull())
            return;
        QImage crop = art.convertToFormat(QImage::Format_RGB32);
        // Grey (Rec. 709 luma), then through the menu palette.
        const std::array<QRgb, 256> table = TintTable();
        for (int y = 0; y < crop.height(); y++)
        {
            QRgb* line = reinterpret_cast<QRgb*>(crop.scanLine(y));
            for (int x = 0; x < crop.width(); x++)
            {
                QRgb p = line[x];
                int luma = (qRed(p) * 54 + qGreen(p) * 183 + qBlue(p) * 19) >> 8;
                line[x] = table[std::clamp(luma, 0, 255)];
            }
        }
        tinted_ = crop;
    }

    void LauncherWindow::resizeEvent(QResizeEvent*)
    {
        // Scale the art to cover the window, then darken where text goes.
        QPixmap pixmap(size());
        pixmap.fill(QColor(2, 7, 14));
        QPainter painter(&pixmap);
        painter.setRenderHint(QPainter::SmoothPixmapTransform);
        if (!tinted_.isNull())
        {
            QSize scaled = tinted_.size().scaled(size(), Qt::KeepAspectRatioByExpanding);
            painter.drawImage(QRect(QPoint((width() - scaled.width()) / 2, 0), scaled), tinted_);
        }
        UpdateMetrics();
        // Behind the menu, on the left.
        int menuRight = metrics_.margin + metrics_.menuWidth;
        QLinearGradient side(menuRight * 0.6, 0, menuRight + width() * 0.15, 0);
        side.setColorAt(0, QColor(2, 8, 18, 170));
        side.setColorAt(1, QColor(2, 8, 18, 0));
        painter.fillRect(rect(), side);
        // Behind the description and hints.
        QLinearGradient bottom(0, height() - 170, 0, height());
        bottom.setColorAt(0, QColor(2, 8, 18, 0));
        bottom.setColorAt(1, QColor(2, 8, 18, 200));
        painter.fillRect(rect(), bottom);
        // Vignette.
        QRadialGradient vignette(rect().center(), std::hypot(width(), height()) / 2);
        vignette.setColorAt(0.6, QColor(0, 0, 0, 0));
        vignette.setColorAt(1.0, QColor(0, 0, 0, 120));
        painter.fillRect(rect(), vignette);
        // Faint scan lines, like the game's menu screens.
        constexpr int kScanLineSpacing = 3;
        for (int y = 0; y < height(); y += kScanLineSpacing)
            painter.fillRect(0, y, width(), 1, QColor(0, 0, 0, 16));
        background_ = pixmap;
    }

    namespace
    {
        // A system font standing in for a Conduit size when the game's fonts
        // can't be read ("conduit-23" -> 23 * 1.05 pixels).
        QFont FallbackFont(const char* name)
        {
            QFont font;
            font.setPixelSize(int(QString::fromLatin1(name).section('-', 1).toInt() * 1.05));
            return font;
        }
    }

    int LauncherWindow::Ascent(Style style) const
    {
        if (!haveFonts_)
            return QFontMetrics(FallbackFont(FontName(style))).ascent();
        return fonts_.Find(FontName(style))->ascent;
    }

    int LauncherWindow::TextWidth(Style style, const QString& text) const
    {
        if (haveFonts_)
            return fonts_.Find(FontName(style))->Width(text);
        return QFontMetrics(FallbackFont(FontName(style))).horizontalAdvance(text);
    }

    QStringList LauncherWindow::Wrap(Style style, const QString& text, int width) const
    {
        QStringList lines;
        QString line;
        for (const QString& word : text.split(' ', Qt::SkipEmptyParts))
        {
            QString candidate = line.isEmpty() ? word : line + ' ' + word;
            if (!line.isEmpty() && TextWidth(style, candidate) > width)
            {
                lines.push_back(line);
                line = word;
            }
            else
                line = candidate;
        }
        if (!line.isEmpty())
            lines.push_back(line);
        return lines;
    }

    void LauncherWindow::DrawText(QPainter& painter, Style style, int x, int baseline, const QString& text, const QColor& color,
        Align align) const
    {
        int width = TextWidth(style, text);
        if (align == Align::Right)
            x -= width;
        else if (align == Align::Center)
            x -= width / 2;
        if (haveFonts_)
        {
            fonts_.Find(FontName(style))->Draw(painter, x, baseline, text, color);
            return;
        }
        painter.setFont(FallbackFont(FontName(style)));
        painter.setPen(color);
        painter.drawText(x, baseline, text);
    }

    void LauncherWindow::DrawBindingSlots(QPainter& painter, const MenuItem& item, const QRect& row, bool selected)
    {
        const std::vector<std::string>& bindings = settings_.controls.bindings[item.action];
        for (int slot = 0; slot < int(config::kMaxBindings); slot++)
        {
            QRect box = SlotRect(row, slot);
            bool active = selected && slot == slot_;
            bool bound = slot < int(bindings.size());
            if (slot >= SlotCount(item))
                continue;
            painter.fillRect(box, active ? QColor(150, 205, 255, 70) : QColor(10, 30, 55, 120));
            painter.setPen(active ? kAccentColor : QColor(120, 170, 220, 70));
            painter.drawRect(box.adjusted(0, 0, -1, -1));
            QString text;
            QColor color = bound ? (active ? kSelectedTextColor : kTextColor) : kDimColor;
            if (active && capturing_)
            {
                int left = (kCaptureTimeoutMs - int(captureClock_.elapsed()) + 999) / 1000;
                text = QStringLiteral("Press a key  %1").arg(std::max(left, 0));
                color = kAccentColor;
            }
            else
                text = bound ? FromStd(bindings[slot]) : QStringLiteral("—");
            int baseline = box.center().y() + Ascent(Style::Small) / 2 - 2;
            DrawText(painter, Style::Small, box.center().x(), baseline, text, color, Align::Center);
        }
    }

    void LauncherWindow::DrawRow(QPainter& painter, const MenuItem& item, const QRect& row, bool selected)
    {
        if (selected)
        {
            // Halo 3's selection: a soft bar fading to the right with a
            // bright edge on the left.
            QLinearGradient bar(row.topLeft(), row.topRight());
            QColor strong = kHighlightColor, faint = kHighlightColor;
            strong.setAlpha(120);
            faint.setAlpha(18);
            bar.setColorAt(0, strong);
            bar.setColorAt(1, faint);
            painter.fillRect(row, bar);
            painter.fillRect(QRect(row.left(), row.top(), 4, row.height()), kAccentColor);
        }
        QColor color = selected ? kSelectedTextColor : kTextColor;
        int baseline = row.center().y() + Ascent(Style::Item) / 2 - 3;
        DrawText(painter, Style::Item, row.left() + kTextInset, baseline, item.label, color);

        int valueBaseline = row.center().y() + Ascent(Style::Small) / 2 - 2;
        int right = row.right() - kTextInset;
        switch (item.kind)
        {
        case MenuItem::Kind::Choice:
        {
            // ‹ value ›
            QString value = item.value();
            DrawText(painter, Style::Small, right, valueBaseline, QStringLiteral("›"), selected ? kAccentColor : kDimColor, Align::Right);
            int valueRight = right - 22;
            DrawText(painter, Style::Small, valueRight, valueBaseline, value, color, Align::Right);
            DrawText(painter, Style::Small, valueRight - TextWidth(Style::Small, value) - 14, valueBaseline, QStringLiteral("‹"),
                selected ? kAccentColor : kDimColor, Align::Right);
            break;
        }
        case MenuItem::Kind::Text:
        {
            QString text = FromStd(*item.text);
            bool caret = editing_ && selected && (QTime::currentTime().msec() < kBlinkMs);
            DrawText(painter, Style::Small, right, valueBaseline, text + (editing_ && selected ? (caret ? "_" : " ") : ""),
                editing_ && selected ? kAccentColor : color, Align::Right);
            break;
        }
        case MenuItem::Kind::Binding:
            DrawBindingSlots(painter, item, row, selected);
            break;
        case MenuItem::Kind::Action:
            if (item.value)
                DrawText(painter, Style::Small, right, valueBaseline, item.value(), color, Align::Right);
            break;
        }
    }

    void LauncherWindow::DrawProfileCard(QPainter& painter)
    {
        // A small gamercard in the bottom right: picture and gamertag.
        constexpr int kMargin = 48;
        QString gamertag = FromStd(settings_.profile.gamertag);
        int cardWidth = kPictureSize + 16 + TextWidth(Style::Item, gamertag);
        QRect picture(width() - kMargin - cardWidth, height() - kMargin - kPictureSize, kPictureSize, kPictureSize);
        painter.fillRect(picture.adjusted(-2, -2, 2, 2), QColor(150, 205, 255, 90));
        if (!picture_.isNull())
        {
            QImage square = picture_.scaled(kPictureSize, kPictureSize, Qt::KeepAspectRatioByExpanding, Qt::SmoothTransformation);
            painter.drawImage(picture, square, QRect((square.width() - kPictureSize) / 2, (square.height() - kPictureSize) / 2,
                kPictureSize, kPictureSize));
        }
        else
            painter.fillRect(picture, QColor(12, 44, 76));
        DrawText(painter, Style::Item, picture.right() + 16, picture.center().y() + Ascent(Style::Item) / 2 - 3, gamertag, kTitleColor);
    }

    void LauncherWindow::DrawHints(QPainter& painter, int x, int baseline)
    {
        // Each hint is a button and what it does: the game's button icons
        // with a gamepad connected, keys and mouse buttons otherwise.
        struct Hint
        {
            QString button;
            bool icon;
            QString action;
        };
        std::vector<Hint> hints;
        bool pad = gamepad_ != nullptr;
        const MenuItem& item = Current().items[Current().selected];
        if (capturing_)
            hints.push_back({ pad ? QString(QChar(kIconB)) : QString(), pad, pad ? QStringLiteral("Cancel")
                                                                             : QStringLiteral("Press a key or mouse button to bind it") });
        else if (editing_)
            hints.push_back({ pad ? QString(QChar(kIconA)) : QStringLiteral("Enter"), pad, QStringLiteral("Done") });
        else
        {
            hints.push_back({ pad ? QString(QChar(kIconA)) : QStringLiteral("Enter / Click"), pad, QStringLiteral("Select") });
            if (item.kind == MenuItem::Kind::Binding)
                hints.push_back({ pad ? QString(QChar(kIconY)) : QStringLiteral("Delete / Right-click"), pad, QStringLiteral("Clear") });
            if (pages_.size() > 1)
                hints.push_back({ pad ? QString(QChar(kIconB)) : QStringLiteral("Esc / Right-click"), pad, QStringLiteral("Back") });
        }
        for (const Hint& hint : hints)
        {
            if (!hint.button.isEmpty())
            {
                Style style = hint.icon ? Style::Icon : Style::Small;
                DrawText(painter, style, x, baseline, hint.button, hint.icon ? kTextColor : kAccentColor);
                x += TextWidth(style, hint.button) + 8;
            }
            DrawText(painter, Style::Small, x, baseline, hint.action, kTextColor);
            x += TextWidth(Style::Small, hint.action) + kHintGap;
        }
    }

    void LauncherWindow::paintEvent(QPaintEvent*)
    {
        QPainter painter(this);
        painter.drawPixmap(0, 0, background_);

        const Page& page = Current();
        QRect menu = MenuRect();

        // Title and the rule under it.
        DrawText(painter, Style::Title, menu.left(), metrics_.titleBaseline, page.title, kTitleColor);
        painter.fillRect(QRect(menu.left(), metrics_.titleBaseline + 18, menu.width(), 1), kLineColor);

        // Rows.
        int visible = VisibleRows();
        for (int i = 0; i < visible && page.scroll + i < int(page.items.size()); i++)
        {
            int index = page.scroll + i;
            DrawRow(painter, page.items[index], RowRect(index), index == page.selected);
        }
        // More rows above/below.
        if (page.scroll > 0)
            DrawText(painter, Style::Icon, menu.right() - 4, menu.top() - 6, QString(QChar(kIconUp)), kAccentColor, Align::Right);
        if (page.scroll + visible < int(page.items.size()))
            DrawText(painter, Style::Icon, menu.right() - 4, menu.top() + visible * metrics_.rowHeight + 16, QString(QChar(kIconDown)),
                kAccentColor, Align::Right);

        // Description (or a problem), wrapped to the menu's width, and the
        // button hints.
        const MenuItem& item = page.items[page.selected];
        bool problem = !status_.isEmpty();
        int line = height() - metrics_.footerHeight + 34;
        for (const QString& text : Wrap(Style::Small, problem ? status_ : item.description, menu.width()))
        {
            DrawText(painter, Style::Small, menu.left(), line, text, problem ? kWarningColor : kTextColor);
            line += metrics_.lineHeight;
        }
        DrawHints(painter, menu.left(), height() - 28);

        DrawProfileCard(painter);
    }
}
