#include "appwiring.h"

#include <QCoreApplication>
#include <QLibraryInfo>
#include <QLocale>
#include <QSettings>
#include <QTranslator>

#include "account.h"
#include "calendarbridge.h"
#include "syncworker.h"

void registerAppMetaTypes()
{
    // AccountConfig crosses threads in queued signals.
    qRegisterMetaType<AccountConfig>();
    qRegisterMetaType<QList<AccountConfig>>();
}

void setupLanguage()
{
    const QString chosen = QSettings().value(QStringLiteral("ui/language")).toString();
    const QLocale locale = chosen.isEmpty() ? QLocale::system() : QLocale(chosen);
    QLocale::setDefault(locale);

    auto *app = QCoreApplication::instance();
    auto *ownTranslator = new QTranslator(app);
    if (ownTranslator->load(locale, QStringLiteral("lowacal"), QStringLiteral("_"), QStringLiteral(":/i18n")))
        QCoreApplication::installTranslator(ownTranslator);
    auto *qtTranslator = new QTranslator(app);
    if (qtTranslator->load(locale, QStringLiteral("qtbase"), QStringLiteral("_"),
                           QLibraryInfo::path(QLibraryInfo::TranslationsPath)))
        QCoreApplication::installTranslator(qtTranslator);
}

void wireBridgeAndWorker(CalendarBridge *bridge, SyncWorker *worker)
{
    QObject::connect(bridge, &CalendarBridge::requestSyncNow, worker, &SyncWorker::requestSyncNow);
    QObject::connect(bridge, &CalendarBridge::requestCreateEvent, worker, &SyncWorker::requestCreateEvent);
    QObject::connect(bridge, &CalendarBridge::requestDeleteEvent, worker, &SyncWorker::requestDeleteEvent);
    QObject::connect(bridge, &CalendarBridge::requestUpdateEvent, worker, &SyncWorker::requestUpdateEvent);
    QObject::connect(bridge, &CalendarBridge::reminderOffsetsChanged, worker, &SyncWorker::setReminderOffsets);
    QObject::connect(worker, &SyncWorker::syncStatusChanged, bridge, &CalendarBridge::onSyncFinished);

    // --- accounts: keychain on the GUI thread (bridge), network + DB on the worker ---
    QObject::connect(bridge, &CalendarBridge::requestSetAccounts, worker, &SyncWorker::setAccounts);
    QObject::connect(bridge, &CalendarBridge::requestVerifyAccount, worker, &SyncWorker::verifyAccount);
    QObject::connect(bridge, &CalendarBridge::requestCommitAccount, worker, &SyncWorker::commitAccount);
    QObject::connect(bridge, &CalendarBridge::requestRemoveAccount, worker, &SyncWorker::removeAccount);
    QObject::connect(worker, &SyncWorker::accountVerified, bridge, &CalendarBridge::onAccountVerified);
    QObject::connect(worker, &SyncWorker::accountVerifyFailed, bridge, &CalendarBridge::onAccountVerifyFailed);
    QObject::connect(worker, &SyncWorker::accountsChanged, bridge, &CalendarBridge::onAccountsChanged);
}
