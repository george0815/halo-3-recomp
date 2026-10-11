// H3Launcher: the settings front end for H3Recomp (see launcher_window.h).
//
// Usage: H3Launcher [path to default.xex]
// Without a path it uses the last one chosen, or looks for
// private/default.xex above the launcher and the current directory.
//
// H3_LAUNCHER_SCREENSHOT=out.png renders the window into out.png and exits
// (for documentation; works with QT_QPA_PLATFORM=offscreen). Optional
// H3_LAUNCHER_KEYS="Down,Return" sends those keys first, to reach a page,
// and H3_LAUNCHER_SIZE=1920x1080 sets the window size.

#include "launcher_window.h"

#include <QApplication>
#include <QKeyEvent>
#include <QKeySequence>
#include <QTimer>

int main(int argc, char* argv[])
{
    QApplication app(argc, argv);
    QApplication::setApplicationName(QStringLiteral("H3Launcher"));

    QStringList arguments = QApplication::arguments();
    h3::launcher::LauncherWindow window(arguments.size() > 1 ? arguments[1] : QString());
    if (QStringList size = qEnvironmentVariable("H3_LAUNCHER_SIZE").split('x'); size.size() == 2)
        window.resize(size[0].toInt(), size[1].toInt());
    window.show();

    if (QString screenshot = qEnvironmentVariable("H3_LAUNCHER_SCREENSHOT"); !screenshot.isEmpty())
    {
        QTimer::singleShot(200, &window, [&window, screenshot]
        {
            for (const QString& name : qEnvironmentVariable("H3_LAUNCHER_KEYS").split(',', Qt::SkipEmptyParts))
            {
                QKeySequence sequence(name.trimmed());
                if (sequence.isEmpty())
                    continue;
                QKeyEvent press(QEvent::KeyPress, sequence[0].key(), sequence[0].keyboardModifiers());
                QApplication::sendEvent(&window, &press);
            }
            window.grab().save(screenshot);
            QApplication::quit();
        });
    }
    return QApplication::exec();
}
