#pragma once

#include <QList>
#include <QMetaType>
#include <QString>

// One configured calendar account. Lives in the SQLite `accounts` table
// (everything except the password) and in the system keychain (the
// password only, via QtKeychain - see keychain.h). Travels between the
// GUI thread (which owns keychain access) and SyncWorker's thread (which
// owns network access) through queued signals, hence the metatype
// registration at the bottom.
struct AccountConfig {
    QString id; // stable local id ("acc-xxxxxxxx"), prefixes calendar ids in the DB; never contains ':'
    QString kind; // "icloud" | "caldav"
    QString label; // shown in the sidebar
    QString baseUrl; // the *resolved* CalDAV endpoint (after .well-known/redirect discovery), not what the user typed
    QString username;
    QString password; // only ever in memory + keychain, never in the DB
};

Q_DECLARE_METATYPE(AccountConfig)
Q_DECLARE_METATYPE(QList<AccountConfig>)
