#include "config.h"

#include <QDir>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QProcess>
#include <QStandardPaths>
#include <QDebug>

QString Config::configPath()
{
    const QString base = QStandardPaths::writableLocation(QStandardPaths::ConfigLocation);
    return base + "/lowacal/accounts.json";
}

namespace {

// Returns the stored password, or an empty string (with a warning
// logged) if secret-tool isn't installed, the entry doesn't exist, or
// the lookup otherwise fails.
QString lookupPassword(const QString &username)
{
    QProcess proc;
    proc.start("secret-tool", {"lookup", "service", "lowacal", "username", username});
    if (!proc.waitForStarted(2000))
        return QString(); // secret-tool not installed (e.g. inside Flatpak) - nothing to migrate from
    if (!proc.waitForFinished(5000)) {
        qWarning() << "Config: secret-tool timed out looking up" << username;
        return QString();
    }
    if (proc.exitCode() != 0) {
        qWarning() << "Config: no keyring entry for" << username
                    << "- store one with: secret-tool store --label=lowacal service lowacal username" << username;
        return QString();
    }
    // secret-tool prints the secret followed by a trailing newline.
    return QString::fromUtf8(proc.readAllStandardOutput()).trimmed();
}

} // namespace

QList<AccountConfig> Config::loadAccounts()
{
    QList<AccountConfig> out;
    const QString path = configPath();

    QFile file(path);
    if (!file.exists()) {
        qWarning() << "Config: no accounts.json yet at" << path
                    << "- copy accounts.example.json there and fill it in";
        return out;
    }
    if (!file.open(QIODevice::ReadOnly)) {
        qWarning() << "Config: could not open" << path << file.errorString();
        return out;
    }

    QJsonParseError err;
    const QJsonDocument doc = QJsonDocument::fromJson(file.readAll(), &err);
    if (err.error != QJsonParseError::NoError) {
        qWarning() << "Config: malformed JSON in" << path << err.errorString();
        return out;
    }

    const QJsonArray accounts = doc.object().value("accounts").toArray();
    for (const QJsonValue &v : accounts) {
        const QJsonObject obj = v.toObject();
        AccountConfig account;
        account.id = obj.value("id").toString();
        account.kind = obj.value("kind").toString();
        account.label = obj.value("label").toString();
        account.baseUrl = obj.value("baseUrl").toString();
        account.username = obj.value("username").toString();

        if (account.id.isEmpty() || account.username.isEmpty() || account.baseUrl.isEmpty()) {
            qWarning() << "Config: skipping account entry missing id/username/baseUrl";
            continue;
        }

        account.password = lookupPassword(account.username);
        if (account.password.isEmpty()) {
            qWarning() << "Config: skipping account" << account.id << "- no password available";
            continue;
        }

        out.append(account);
    }

    return out;
}

QString Config::legacyLookupPassword(const QString &username)
{
    return lookupPassword(username);
}
