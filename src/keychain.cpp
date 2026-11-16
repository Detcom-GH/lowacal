#include "keychain.h"

#include <qt6keychain/keychain.h>

namespace {
const QString kService = QStringLiteral("lowacal");

QString keyFor(const QString &accountId)
{
    return QStringLiteral("account/%1").arg(accountId);
}
} // namespace

void Keychain::readPassword(const QString &accountId, QObject *context,
                            std::function<void(bool, const QString &, const QString &)> callback)
{
    auto *job = new QKeychain::ReadPasswordJob(kService);
    job->setAutoDelete(true);
    job->setKey(keyFor(accountId));
    QObject::connect(job, &QKeychain::Job::finished, context, [callback](QKeychain::Job *j) {
        auto *readJob = static_cast<QKeychain::ReadPasswordJob *>(j);
        if (readJob->error() == QKeychain::NoError)
            callback(true, readJob->textData(), QString());
        else
            callback(false, QString(), readJob->errorString());
    });
    job->start();
}

void Keychain::writePassword(const QString &accountId, const QString &password, QObject *context,
                             std::function<void(bool, const QString &)> callback)
{
    auto *job = new QKeychain::WritePasswordJob(kService);
    job->setAutoDelete(true);
    job->setKey(keyFor(accountId));
    job->setTextData(password);
    QObject::connect(job, &QKeychain::Job::finished, context, [callback](QKeychain::Job *j) {
        if (j->error() == QKeychain::NoError)
            callback(true, QString());
        else
            callback(false, j->errorString());
    });
    job->start();
}

void Keychain::deletePassword(const QString &accountId)
{
    auto *job = new QKeychain::DeletePasswordJob(kService);
    job->setAutoDelete(true);
    job->setKey(keyFor(accountId));
    job->start();
}
