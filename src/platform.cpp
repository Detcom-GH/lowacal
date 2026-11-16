#include "platform.h"

#include <QCoreApplication>
#include <QDBusConnection>
#include <QDBusConnectionInterface>
#include <QDBusMessage>
#include <QDBusObjectPath>
#include <QDBusPendingCallWatcher>
#include <QDBusPendingReply>
#include <QDir>
#include <QFile>
#include <QPointer>
#include <QRandomGenerator>
#include <QSettings>
#include <QStandardPaths>
#include <QTimer>
#include <QDebug>

namespace {

const QString kAppId = QStringLiteral(LOWACAL_APP_ID);
const QString kPortalService = QStringLiteral("org.freedesktop.portal.Desktop");
const QString kPortalPath = QStringLiteral("/org/freedesktop/portal/desktop");

// One Background-portal request. The portal answers asynchronously with a
// Response signal on a per-request object path (the "handle"); the handle
// is predictable from our bus name + a token we choose, so we subscribe
// BEFORE calling - otherwise a fast portal could answer before we listen.
class PortalRequest : public QObject {
    Q_OBJECT
public:
    using Callback = std::function<void(bool, bool, const QString &)>;

    PortalRequest(QObject *context, Callback callback) : QObject(context), m_callback(std::move(callback)) {}

    void start(bool enabled, const QString &reason)
    {
        QDBusConnection bus = QDBusConnection::sessionBus();
        if (!bus.isConnected()) {
            finish(false, false, QStringLiteral("no D-Bus session bus"));
            return;
        }
        const QString token = QStringLiteral("lowacal%1").arg(QRandomGenerator::global()->generate());
        QString sender = bus.baseService().mid(1); // ":1.42" -> "1.42"
        sender.replace('.', '_');
        listenOn(QStringLiteral("/org/freedesktop/portal/desktop/request/%1/%2").arg(sender, token));

        QDBusMessage call = QDBusMessage::createMethodCall(kPortalService, kPortalPath,
                                                           QStringLiteral("org.freedesktop.portal.Background"),
                                                           QStringLiteral("RequestBackground"));
        const QVariantMap options{
            {QStringLiteral("handle_token"), token},
            {QStringLiteral("reason"), reason},
            {QStringLiteral("autostart"), enabled},
            {QStringLiteral("commandline"), QStringList{QStringLiteral("lowacal"), QStringLiteral("--hidden")}},
        };
        call << QString() << options;

        auto *watcher = new QDBusPendingCallWatcher(bus.asyncCall(call), this);
        connect(watcher, &QDBusPendingCallWatcher::finished, this, [this](QDBusPendingCallWatcher *w) {
            QDBusPendingReply<QDBusObjectPath> reply = *w;
            w->deleteLater();
            if (reply.isError()) {
                finish(false, false, reply.error().message());
                return;
            }
            // Old portal versions ignore handle_token: follow whatever
            // handle they actually returned.
            if (reply.value().path() != m_handle)
                listenOn(reply.value().path());
        });

        // Some desktops ask the user first; don't wait forever if nobody answers.
        QTimer::singleShot(120000, this, [this] { finish(false, false, QStringLiteral("no answer from the portal")); });
    }

private slots:
    void onResponse(uint response, const QVariantMap &results)
    {
        // 0 = success, 1 = user cancelled, 2 = other error
        if (response == 0)
            finish(true, results.value(QStringLiteral("autostart")).toBool(), QString());
        else
            finish(false, false, response == 1 ? QStringLiteral("cancelled") : QStringLiteral("denied"));
    }

private:
    void listenOn(const QString &handle)
    {
        QDBusConnection bus = QDBusConnection::sessionBus();
        if (!m_handle.isEmpty())
            bus.disconnect(QString(), m_handle, QStringLiteral("org.freedesktop.portal.Request"),
                           QStringLiteral("Response"), this, SLOT(onResponse(uint, QVariantMap)));
        m_handle = handle;
        bus.connect(QString(), m_handle, QStringLiteral("org.freedesktop.portal.Request"), QStringLiteral("Response"),
                    this, SLOT(onResponse(uint, QVariantMap)));
    }

    void finish(bool ok, bool enabledNow, const QString &error)
    {
        if (m_done)
            return;
        m_done = true;
        m_callback(ok, enabledNow, error);
        deleteLater();
    }

    Callback m_callback;
    QString m_handle;
    bool m_done = false;
};

} // namespace

bool Autostart::usesPortal()
{
    return QFile::exists(QStringLiteral("/.flatpak-info")) || qEnvironmentVariable("LOWACAL_FORCE_PORTAL") == QLatin1String("1");
}

QString Autostart::desktopFilePath()
{
    return QStandardPaths::writableLocation(QStandardPaths::ConfigLocation) + QStringLiteral("/autostart/") + kAppId
        + QStringLiteral(".desktop");
}

bool Autostart::isEnabled()
{
    if (usesPortal())
        return QSettings().value(QStringLiteral("app/autostart"), false).toBool();
    return QFile::exists(desktopFilePath());
}

void Autostart::setEnabled(bool enabled, const QString &reason, QObject *context,
                           std::function<void(bool, bool, const QString &)> callback)
{
    if (usesPortal()) {
        auto *request = new PortalRequest(context, [callback](bool ok, bool enabledNow, const QString &error) {
            if (ok)
                QSettings().setValue(QStringLiteral("app/autostart"), enabledNow);
            callback(ok, enabledNow, error);
        });
        request->start(enabled, reason);
        return;
    }

    const QString path = desktopFilePath();
    if (!enabled) {
        const bool ok = !QFile::exists(path) || QFile::remove(path);
        callback(ok, !ok, ok ? QString() : QStringLiteral("could not remove %1").arg(path));
        return;
    }

    QDir().mkpath(QFileInfo(path).absolutePath());
    QFile file(path);
    if (!file.open(QIODevice::WriteOnly | QIODevice::Truncate | QIODevice::Text)) {
        callback(false, false, file.errorString());
        return;
    }
    // Absolute path to *this* binary: works no matter whether ~/.local/bin
    // is on the session's PATH at login time.
    QString exe = QCoreApplication::applicationFilePath();
    if (exe.contains(' '))
        exe = '"' + exe + '"';
    file.write(QStringLiteral("[Desktop Entry]\n"
                              "Type=Application\n"
                              "Name=Lowacal\n"
                              "Exec=%1 --hidden\n"
                              "Icon=%2\n"
                              "Terminal=false\n"
                              "X-GNOME-Autostart-enabled=true\n")
                   .arg(exe, kAppId)
                   .toUtf8());
    file.close();
    callback(true, true, QString());
}

SingleInstance::SingleInstance(QObject *parent) : QObject(parent) {}

bool SingleInstance::claim(bool activateExisting)
{
    QDBusConnection bus = QDBusConnection::sessionBus();
    if (!bus.isConnected())
        return true;

    if (bus.registerService(kAppId)) {
        bus.registerObject(QStringLiteral("/SingleInstance"), this, QDBusConnection::ExportScriptableSlots);
        return true;
    }

    if (activateExisting) {
        QDBusMessage call = QDBusMessage::createMethodCall(kAppId, QStringLiteral("/SingleInstance"),
                                                           QStringLiteral("lowacal.SingleInstance"),
                                                           QStringLiteral("activate"));
        bus.call(call, QDBus::Block, 3000);
    }
    return false;
}

void SingleInstance::activate()
{
    emit activateRequested();
}

#include "platform.moc"
