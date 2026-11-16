// Tests for src/platform.cpp, run on a private D-Bus session bus
// (see run_ui_flow_test.sh):
//   - launch-at-login via ~/.config/autostart (the non-Flatpak path)
//   - launch-at-login via the Background portal, against a fake portal
//     that follows the xdg-desktop-portal spec - including answering
//     BEFORE the method call returns, the race a late subscriber loses
//   - single instance, with a real second process

#include <QCoreApplication>
#include <QDBusConnection>
#include <QDBusContext>
#include <QDBusMessage>
#include <QDBusObjectPath>
#include <QElapsedTimer>
#include <QFile>
#include <QProcess>
#include <QSettings>
#include <QThread>
#include <cstdio>
#include <functional>

#include "platform.h"

namespace {
int failures = 0;
void check(bool ok, const char *what)
{
    std::printf("  %s - %s\n", ok ? "ok  " : "FAIL", what);
    if (!ok)
        ++failures;
}
bool waitUntil(const std::function<bool()> &cond, int ms)
{
    QElapsedTimer t;
    t.start();
    while (!cond()) {
        if (t.elapsed() > ms)
            return false;
        QCoreApplication::processEvents(QEventLoop::AllEvents, 20);
        QThread::msleep(5);
    }
    return true;
}
} // namespace

// Stand-in for xdg-desktop-portal's Background interface, on its own bus connection.
class FakePortal : public QObject, protected QDBusContext {
    Q_OBJECT
    Q_CLASSINFO("D-Bus Interface", "org.freedesktop.portal.Background")
public:
    explicit FakePortal(QDBusConnection conn) : m_conn(conn) {}
    uint answer = 0; // 0 = allow, 1 = user cancelled
    QVariantMap lastOptions;
    int calls = 0;

public slots:
    QDBusObjectPath RequestBackground(const QString &, const QVariantMap &options)
    {
        ++calls;
        lastOptions = options;
        QString sender = message().service().mid(1);
        sender.replace('.', '_');
        const QString handle = QStringLiteral("/org/freedesktop/portal/desktop/request/%1/%2")
                                   .arg(sender, options.value("handle_token").toString());
        // Answer immediately - before our reply to this call is even sent.
        QDBusMessage response = QDBusMessage::createSignal(handle, "org.freedesktop.portal.Request", "Response");
        response << answer
                 << QVariantMap{{"background", answer == 0}, {"autostart", answer == 0 && options.value("autostart").toBool()}};
        m_conn.send(response);
        return QDBusObjectPath(handle);
    }

private:
    QDBusConnection m_conn;
};

int main(int argc, char *argv[])
{
    QCoreApplication app(argc, argv);
    app.setApplicationName("lowacal");
    app.setOrganizationName("lowacal");

    // Child-process mode for the single-instance test.
    if (app.arguments().contains("--second") || app.arguments().contains("--second-hidden")) {
        SingleInstance instance;
        const bool primary = instance.claim(!app.arguments().contains("--second-hidden"));
        return primary ? 1 : 0; // 0 = correctly refused to become a second primary
    }

    std::printf("-- launch at login, no sandbox (autostart file) --\n");
    qputenv("LOWACAL_FORCE_PORTAL", "0");
    bool done = false, ok = false, nowOn = false;
    auto cb = [&](bool o, bool on, const QString &) { ok = o; nowOn = on; done = true; };
    Autostart::setEnabled(true, "test", &app, cb);
    waitUntil([&] { return done; }, 2000);
    QFile f(Autostart::desktopFilePath());
    const bool opened = f.open(QIODevice::ReadOnly);
    const QString content = opened ? QString::fromUtf8(f.readAll()) : QString();
    std::printf("    %s\n", qPrintable(Autostart::desktopFilePath()));
    check(ok && nowOn && Autostart::isEnabled(), "enabled: autostart entry written");
    check(content.contains(" --hidden") && content.contains("Exec=/"), "entry starts this binary by absolute path, hidden");
    done = false;
    Autostart::setEnabled(false, "test", &app, cb);
    waitUntil([&] { return done; }, 2000);
    check(ok && !QFile::exists(Autostart::desktopFilePath()) && !Autostart::isEnabled(), "disabled: entry removed");

    std::printf("-- launch at login, Flatpak (Background portal) --\n");
    qputenv("LOWACAL_FORCE_PORTAL", "1");
    QDBusConnection portalConn = QDBusConnection::connectToBus(QDBusConnection::SessionBus, "fakeportal");
    FakePortal portal(portalConn);
    check(portalConn.registerService("org.freedesktop.portal.Desktop")
              && portalConn.registerObject("/org/freedesktop/portal/desktop", &portal, QDBusConnection::ExportAllSlots),
          "fake portal registered on the session bus");

    done = false;
    Autostart::setEnabled(true, "Show reminders", &app, cb);
    check(waitUntil([&] { return done; }, 5000), "answer received (portal replied before the call returned)");
    check(ok && nowOn && Autostart::isEnabled(), "portal granted autostart -> enabled");
    check(portal.lastOptions.value("autostart").toBool(), "request asked for autostart=true");
    check(portal.lastOptions.value("commandline").toStringList() == QStringList({"lowacal", "--hidden"}),
          "autostart command line is `lowacal --hidden`");
    check(!portal.lastOptions.value("reason").toString().isEmpty(), "a user-visible reason is sent");

    portal.answer = 1; // user says no
    done = false;
    Autostart::setEnabled(true, "Show reminders", &app, cb);
    waitUntil([&] { return done; }, 5000);
    check(done && !ok, "user refusal is reported as failure, not silently ignored");

    portal.answer = 0;
    done = false;
    Autostart::setEnabled(false, "Show reminders", &app, cb);
    waitUntil([&] { return done; }, 5000);
    check(ok && !nowOn && !Autostart::isEnabled(), "turning it off goes through the portal too");

    std::printf("-- single instance --\n");
    SingleInstance primary;
    int activations = 0;
    QObject::connect(&primary, &SingleInstance::activateRequested, [&] { ++activations; });
    check(primary.claim(true), "first process becomes the primary instance");

    QProcess second;
    second.start(app.applicationFilePath(), {"--second"});
    // The child's activate() call lands on our event loop: keep it spinning.
    waitUntil([&] { return second.state() == QProcess::NotRunning; }, 10000);
    check(second.exitCode() == 0, "second launch refuses to start a second copy");
    check(waitUntil([&] { return activations == 1; }, 3000), "...and asks the running copy to show its window");

    QProcess hidden;
    hidden.start(app.applicationFilePath(), {"--second-hidden"});
    waitUntil([&] { return hidden.state() == QProcess::NotRunning; }, 10000);
    QCoreApplication::processEvents();
    check(hidden.exitCode() == 0 && activations == 1, "a --hidden (autostart) launch just exits, no window pops up");

    std::printf("\n%s (%d failure%s)\n", failures ? "SOME CHECKS FAILED" : "ALL CHECKS PASSED", failures,
                failures == 1 ? "" : "s");
    return failures ? 1 : 0;
}

#include "platform_test.moc"
