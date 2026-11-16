#include "traycontroller.h"

#include "branding.h"

#include <QAction>
#include <QDBusConnection>
#include <QDBusInterface>
#include <QIcon>
#include <QMenu>
#include <QDebug>

TrayController::TrayController(QObject *parent) : QObject(parent)
{
    m_tray = new QSystemTrayIcon(Branding::trayIcon(), this);
    m_tray->setToolTip(QStringLiteral("Lowacal"));

    m_menu = new QMenu();

    QAction *showAction = m_menu->addAction(tr("Show window"));
    connect(showAction, &QAction::triggered, this, &TrayController::showWindowRequested);

    QAction *syncAction = m_menu->addAction(tr("Sync now"));
    connect(syncAction, &QAction::triggered, this, &TrayController::syncRequested);

    m_menu->addSeparator();

    QAction *quitAction = m_menu->addAction(tr("Quit"));
    connect(quitAction, &QAction::triggered, this, &TrayController::quitRequested);

    m_tray->setContextMenu(m_menu);

    connect(m_tray, &QSystemTrayIcon::activated, this, [this](QSystemTrayIcon::ActivationReason reason) {
        if (reason == QSystemTrayIcon::Trigger) {
            emit showWindowRequested();
        }
    });

    m_tray->show();
}

void TrayController::showNotification(const QString &title, const QString &body)
{
    // Real freedesktop notification rather than QSystemTrayIcon's
    // showMessage balloon: the balloon is transient and never lands in
    // the desktop's notification history (KDE's popup applet, etc.),
    // which made reminders easy to miss entirely if you glanced away.
    // This goes through the system notification daemon, so it behaves
    // like every other app's notifications - including staying in
    // history.
    QDBusInterface notifications("org.freedesktop.Notifications", "/org/freedesktop/Notifications",
                                  "org.freedesktop.Notifications");
    if (notifications.isValid()) {
        // The desktop-entry hint is what lets Plasma attribute this
        // notification to an installed application - without it the
        // popup shows but never lands in the notification history
        // (verified live: popups appeared, history stayed empty).
        // Needs lowacal.desktop present in ~/.local/share/applications
        // or /usr/share/applications - see README.
        QVariantMap hints;
        hints.insert(QStringLiteral("desktop-entry"), QStringLiteral(LOWACAL_APP_ID));

        const QVariantList args{
            QStringLiteral("Lowacal"), // app_name
            quint32(0), // replaces_id: 0 = always a new notification
            QStringLiteral(LOWACAL_APP_ID), // app_icon: our own icon, installed into the icon theme
            title, // summary
            body, // body
            QStringList(), // actions
            hints,
            int(-1), // expire_timeout: -1 = daemon's default
        };
        notifications.callWithArgumentList(QDBus::NoBlock, "Notify", args);
        return;
    }

    qWarning() << "TrayController: org.freedesktop.Notifications unavailable, falling back to tray balloon";
    m_tray->showMessage(title, body, QSystemTrayIcon::NoIcon, 10000);
}
