/****************************************************************************
**
** Copyright (C) 2015 The Qt Company Ltd.
** Contact: http://www.qt.io/licensing/
**
** This file is part of the Qt Messaging Framework.
**
** $QT_BEGIN_LICENSE:LGPL21$
** Commercial License Usage
** Licensees holding valid commercial Qt licenses may use this file in
** accordance with the commercial license agreement provided with the
** Software or, alternatively, in accordance with the terms contained in
** a written agreement between you and The Qt Company. For licensing terms
** and conditions see http://www.qt.io/terms-conditions. For further
** information use the contact form at http://www.qt.io/contact-us.
**
** GNU Lesser General Public License Usage
** Alternatively, this file may be used under the terms of the GNU Lesser
** General Public License version 2.1 or version 3 as published by the Free
** Software Foundation and appearing in the file LICENSE.LGPLv21 and
** LICENSE.LGPLv3 included in the packaging of this file. Please review the
** following information to ensure the GNU Lesser General Public License
** requirements will be met: https://www.gnu.org/licenses/lgpl.html and
** http://www.gnu.org/licenses/old-licenses/lgpl-2.1.html.
**
** As a special exception, The Qt Company gives you certain additional
** rights. These rights are described in The Qt Company LGPL Exception
** version 1.1, included in the file LGPL_EXCEPTION.txt in this package.
**
** $QT_END_LICENSE$
**
****************************************************************************/

#include "imapclient.h"
#include "imapauthenticator.h"
#include "imapconfiguration.h"
#include "imapstrategy.h"
#include "imaplog.h"
#include <longstream_p.h>
#include <qmailmessagebuffer.h>
#include <qmailfolder.h>
#include <qmailnamespace.h>
#include <qmaildisconnected.h>
#include <qmailcodec.h>
#include <qmailcrypto.h>
#include <limits.h>
#include <QFile>
#include <QDir>

#ifdef QT_QMF_HAVE_ZLIB
#define QMFALLOWCOMPRESS 1
#else
#define QMFALLOWCOMPRESS 0
#endif

class MessageFlushedWrapper : public QMailMessageBufferFlushCallback
{
    ImapStrategyContext *context;
public:
    MessageFlushedWrapper(ImapStrategyContext *_context)
        : context(_context)
    {
    }

    void messageFlushed(QMailMessage *message) override
    {
        context->messageFlushed(*message);
        context->client()->removeAllFromBuffer(message);
    }
};

class DataFlushedWrapper : public QMailMessageBufferFlushCallback
{
    ImapStrategyContext *context;
    QString uid;
    QString section;
public:
    DataFlushedWrapper(ImapStrategyContext *_context, const QString &_uid, const QString &_section)
        : context(_context)
        , uid(_uid)
        , section(_section)
    {
    }

    void messageFlushed(QMailMessage *message) override
    {
        context->dataFlushed(*message, uid, section);
        context->client()->removeAllFromBuffer(message);
    }
};

namespace {

    struct FlagInfo
    {
        FlagInfo(const QStringList &flagNames, quint64 flag, QMailFolder::StandardFolder standardFolder, quint64 messageFlag)
            : _flagNames(flagNames), _flag(flag), _standardFolder(standardFolder), _messageFlag(messageFlag) {}

        QStringList _flagNames;
        quint64 _flag;
        QMailFolder::StandardFolder _standardFolder;
        quint64 _messageFlag;
    };

    static void setFolderFlags(QMailAccount *account, QMailFolder *folder, const QString &flags, bool setStandardFlags)
    {
        // Set permitted flags
        bool childCreationPermitted(!flags.contains("\\NoInferiors", Qt::CaseInsensitive));
        bool messagesPermitted(!flags.contains("\\NoSelect", Qt::CaseInsensitive));
        folder->setStatus(QMailFolder::ChildCreationPermitted, childCreationPermitted);
        folder->setStatus(QMailFolder::MessagesPermitted, messagesPermitted);
        if (!folder->id().isValid()) {
            qCWarning(lcIMAP) << "setFolderFlags must be called on folder in store " << folder->id();
            return;
        }

        if (!setStandardFlags)
            return;

        // Set standard folder flags
        QList<FlagInfo> flagInfoList;
        flagInfoList << FlagInfo(QStringList() << "\\Inbox", QMailFolder::Incoming, QMailFolder::InboxFolder, QMailMessage::Incoming)
            << FlagInfo(QStringList() << "\\Drafts", QMailFolder::Drafts, QMailFolder::DraftsFolder, QMailMessage::Draft)
            << FlagInfo(QStringList() << "\\Trash", QMailFolder::Trash, QMailFolder::TrashFolder, QMailMessage::Trash)
            << FlagInfo(QStringList() << "\\Sent", QMailFolder::Sent, QMailFolder::SentFolder, QMailMessage::Sent)
            << FlagInfo(QStringList() << "\\Spam" << "\\Junk", QMailFolder::Junk, QMailFolder::JunkFolder, QMailMessage::Junk);

        for (int i = 0; i < flagInfoList.count(); ++i) {
            QStringList flagNames(flagInfoList[i]._flagNames);
            quint64 flag(flagInfoList[i]._flag);
            QMailFolder::StandardFolder standardFolder(flagInfoList[i]._standardFolder);
            quint64 messageFlag(flagInfoList[i]._messageFlag);
            bool isFlagged = false;

            foreach (const QString &flagName, flagNames) {
                if (flags.contains(flagName, Qt::CaseInsensitive)) {
                    isFlagged = true;
                    break;
                }
            }

            folder->setStatus(flag, isFlagged);
            if (isFlagged) {
                QMailFolderId oldFolderId = account->standardFolder(standardFolder);
                if (oldFolderId.isValid() && (oldFolderId != folder->id())) {
                    QMailFolder oldFolder(oldFolderId);
                    oldFolder.setStatus(flag, false);
                    // Do the updates in the right order, so if there is a crash
                    // there will be a graceful recovery next time folders are list.
                    // It is expected that no disconnected move operations will be outstanding
                    // otherwise flags for those messages may be updated incorrectly.
                    // So call exportUpdates before retrieveFolderList
                    QMailMessageKey oldFolderKey(QMailMessageKey::parentFolderId(oldFolderId));
                    if (!QMailStore::instance()->updateMessagesMetaData(oldFolderKey, messageFlag, false)) {
                        qCWarning(lcMailStore) << "Unable to update messages in folder" << oldFolderId << "to remove flag" << flagNames;
                    }
                    if (!QMailStore::instance()->updateFolder(&oldFolder)) {
                        qCWarning(lcMailStore) << "Unable to update folder" << oldFolderId << "to remove flag" << flagNames;
                    }
                }
                if (!oldFolderId.isValid() || (oldFolderId != folder->id())) {
                    account->setStandardFolder(standardFolder, folder->id());
                    if (!QMailStore::instance()->updateAccount(account)) {
                        qCWarning(lcMailStore) << "Unable to update account" << account->id() << "to set flag" << flagNames;
                    }
                    QMailMessageKey folderKey(QMailMessageKey::parentFolderId(folder->id()));
                    if (!QMailStore::instance()->updateMessagesMetaData(folderKey, messageFlag, true)) {
                        qCWarning(lcMailStore) << "Unable to update messages in folder" << folder->id() << "to set flag" << flagNames;
                    }
                }
            }
        }
    }
}

class IdleProtocol : public ImapProtocol {
    Q_OBJECT

public:
    IdleProtocol(ImapClient *client, const QMailFolder &folder, QMailCredentialsInterface *credentials);
    virtual ~IdleProtocol() {}

    bool open(const ImapConfiguration& config, qint64 bufferSize = 10*1024) override;

signals:
    void idleNewMailNotification(QMailFolderId);
    void idleFlagsChangedNotification(QMailFolderId);
    void transportError();

protected slots:
    virtual void idleContinuation(ImapCommand, const QString &);
    virtual void idleCommandTransition(ImapCommand, OperationStatus);
    virtual void idleRenew();
    virtual void idleTransportError();
    void logIn();
    void onCredentialsStatusChanged();

protected:
    ImapClient *_client;
    QMailFolder _folder;

private:
    QTimer _timeoutTimer; // Check command hasn't hung
    QMailCredentialsInterface *_credentials;
};

IdleProtocol::IdleProtocol(ImapClient *client, const QMailFolder &folder, QMailCredentialsInterface *credentials)
    : _client(client)
    , _folder(folder)
    , _credentials(credentials)
{
    connect(this, SIGNAL(continuationRequired(ImapCommand, QString)),
            this, SLOT(idleContinuation(ImapCommand, QString)) );
    connect(this, SIGNAL(completed(ImapCommand, OperationStatus)),
            this, SLOT(idleCommandTransition(ImapCommand, OperationStatus)) );
    connect(this, SIGNAL(connectionError(int,QString)),
            this, SLOT(idleTransportError()) );
    connect(this, SIGNAL(connectionError(QMailServiceAction::Status::ErrorCode,QString)),
            this, SLOT(idleTransportError()) );
    connect(_client, SIGNAL(sessionError()),
            this, SLOT(idleTransportError()) );

    _timeoutTimer.setSingleShot(true);
    _timeoutTimer.setInterval(30 * 1000);
    connect(&_timeoutTimer, &QTimer::timeout, this, &IdleProtocol::idleTransportError);
}

bool IdleProtocol::open(const ImapConfiguration& config, qint64 bufferSize)
{
    _timeoutTimer.start();
    return ImapProtocol::open(config, bufferSize);
}

void IdleProtocol::logIn()
{
    if (_credentials->status() == QMailCredentialsInterface::Ready) {
        sendLogin(QMailAccountConfiguration(_client->account()), _credentials);
    } else if (_credentials->status() == QMailCredentialsInterface::Fetching) {
        connect(_credentials, &QMailCredentialsInterface::statusChanged,
                this, &IdleProtocol::onCredentialsStatusChanged);
    } else {
        qCWarning(lcIMAP) << objectName() << "credential retrieval failed with:" << _credentials->lastError();
        idleTransportError();
    }
}

void IdleProtocol::onCredentialsStatusChanged()
{
    qCDebug(lcIMAP) << objectName() << "Got credential status changed" << _credentials->status();

    disconnect(_credentials, &QMailCredentialsInterface::statusChanged,
               this, &IdleProtocol::onCredentialsStatusChanged);
    switch (_credentials->status()) {
    case (QMailCredentialsInterface::Ready):
        sendLogin(QMailAccountConfiguration(_client->account()), _credentials);
        break;
    case (QMailCredentialsInterface::Failed):
        if (inUse()) {
            idleTransportError();
        }
        break;
    default:
        break;
    }
}

void IdleProtocol::idleContinuation(ImapCommand command, const QString &type)
{
    if (command == IMAP_Idle) {
        if (type == QString("idling")) {
            qCDebug(lcIMAP) << objectName() << "IDLE: Idle connection established.";

            // We are now idling
            _timeoutTimer.stop();
            connect(_client, &ImapClient::renewPushEmail,
                    this, &IdleProtocol::idleRenew);

            _client->setIdlingForFolder(_folder.id());
        } else if (type == QString("newmail")) {
            qCDebug(lcIMAP) << objectName() << "IDLE: new mail event occurred";
            // A new mail event occurred during idle
            emit idleNewMailNotification(_folder.id());
        } else if (type == QString("flagschanged")) {
            qCDebug(lcIMAP) << objectName() << "IDLE: flags changed event occurred";
            // A flags changed event occurred during idle
            emit idleFlagsChangedNotification(_folder.id());
        } else {
            qCWarning(lcIMAP) << "idleContinuation: unknown continuation event";
        }
    }
}

void IdleProtocol::idleCommandTransition(const ImapCommand command, const OperationStatus status)
{
    if ( status != OpOk ) {
        idleTransportError();
        // Notify the client that idle session is not blocking the
        // main session login anymore for this folder.
        _client->setIdlingForFolder(_folder.id());
        return;
    }

    QMailAccountConfiguration config(_client->account());
    switch ( command ) {
        case IMAP_Init:
        {
            if (receivedCapabilities()) {
                // Already received capabilities in unsolicited response, no need to request them again
                setReceivedCapabilities(false);
                idleCommandTransition(IMAP_Capability, status);
                return;
            }
            // We need to request the capabilities
            sendCapability();
            return;
        }
        case IMAP_Capability:
        {
            if (!encrypted()) {
                if (ImapAuthenticator::useEncryption(ImapConfiguration(config), capabilities())) {
                    // Switch to encrypted mode
                    sendStartTLS();
                    break;
                }
            }
            // We are now connected
            logIn();
            return;
        }
        case IMAP_StartTLS:
        {
            logIn();
            break;
        }
        case IMAP_Login: // Fall through
        case IMAP_Compress:
        {
            bool compressCapable(capabilities().contains("COMPRESS=DEFLATE", Qt::CaseInsensitive));
            if (!encrypted() && QMFALLOWCOMPRESS && compressCapable && !compress()) {
                // Server supports COMPRESS and we are not yet compressing
                sendCompress(); // Must not pipeline compress
                return;
            }

            // Server does not support COMPRESS or already compressing
            sendSelect(_folder);
            return;
        }
        case IMAP_Select:
        {
            sendIdle();
            return;
        }
        case IMAP_Idle:
        {
            // Restart idling (TODO: unless we're closing)
            sendIdle();
            return;
        }
        case IMAP_Logout:
        {
            // Ensure connection is closed on logout
            close();
            return;
        }
        default:        //default = all critical messages
        {
            qCWarning(lcIMAP) << objectName() << "IDLE: IMAP Idle unknown command response: " << command;
            return;
        }
    }
}

void IdleProtocol::idleRenew()
{
    _timeoutTimer.start(); // Detect an unresponsive server
    disconnect(_client, &ImapClient::renewPushEmail,
               this, &IdleProtocol::idleRenew);
    sendIdleDone();
}

void IdleProtocol::idleTransportError()
{
    qCWarning(lcIMAP) << objectName()
                   << "IDLE: An IMAP IDLE related error occurred.";

    if (inUse())
        close();

    _timeoutTimer.stop();
    emit transportError();
}

ImapClient::ImapClient(const QMailAccountId &id, QObject* parent)
    : QObject(parent),
      _accountId(id),
      _closeCount(0),
      _qresyncEnabled(false),
      _requestRapidClose(false),
      _rapidClosing(false),
      _pushConnectionsReserved(0),
      _credentials(QMailCredentialsFactory::getCredentialsHandlerForAccount(QMailAccountConfiguration(id))),
      _loginFailed(false)
{
    static int count(0);
    ++count;

    _protocol.setObjectName(QString("%1").arg(count));
    _strategyContext = new ImapStrategyContext(this);
    _strategyContext->setStrategy(&_strategyContext->synchronizeAccountStrategy);
    connect(&_protocol, SIGNAL(completed(ImapCommand, OperationStatus)),
            this, SLOT(commandCompleted(ImapCommand, OperationStatus)) );
    connect(&_protocol, SIGNAL(mailboxListed(QString,QString)),
            this, SLOT(mailboxListed(QString,QString)));
    connect(&_protocol, SIGNAL(messageFetched(QMailMessage&, const QString &, bool)),
            this, SLOT(messageFetched(QMailMessage&, const QString &, bool)) );
    connect(&_protocol, SIGNAL(dataFetched(QString, QString, QString, int)),
            this, SLOT(dataFetched(QString, QString, QString, int)) );
    connect(&_protocol, SIGNAL(partHeaderFetched(QString, QString, QString, int)),
            this, SLOT(partHeaderFetched(QString, QString, QString, int)) );
    connect(&_protocol, SIGNAL(nonexistentUid(QString)),
            this, SLOT(nonexistentUid(QString)) );
    connect(&_protocol, SIGNAL(messageStored(QString)),
            this, SLOT(messageStored(QString)) );
    connect(&_protocol, SIGNAL(messageCopied(QString, QString)),
            this, SLOT(messageCopied(QString, QString)) );
    connect(&_protocol, SIGNAL(messageCreated(QMailMessageId, QString)),
            this, SLOT(messageCreated(QMailMessageId, QString)) );
    connect(&_protocol, SIGNAL(downloadSize(QString, int)),
            this, SLOT(downloadSize(QString, int)) );
    connect(&_protocol, SIGNAL(urlAuthorized(QString)),
            this, SLOT(urlAuthorized(QString)) );
    connect(&_protocol, SIGNAL(folderCreated(QString, bool)),
            this, SLOT(folderCreated(QString, bool)));
    connect(&_protocol, SIGNAL(folderDeleted(QMailFolder, bool)),
            this, SLOT(folderDeleted(QMailFolder, bool)));
    connect(&_protocol, SIGNAL(folderRenamed(QMailFolder, QString, bool)),
            this, SLOT(folderRenamed(QMailFolder, QString, bool)));
    connect(&_protocol, SIGNAL(folderMoved(QMailFolder, QString, QMailFolderId, bool)),
            this, SLOT(folderMoved(QMailFolder, QString, QMailFolderId, bool)));
    connect(&_protocol, SIGNAL(updateStatus(QString)),
            this, SLOT(transportStatus(QString)) );
    connect(&_protocol, SIGNAL(connectionError(int,QString)),
            this, SLOT(transportError(int,QString)) );
    connect(&_protocol, SIGNAL(connectionError(QMailServiceAction::Status::ErrorCode,QString)),
            this, SLOT(transportError(QMailServiceAction::Status::ErrorCode,QString)) );

    _inactiveTimer.setSingleShot(true);
    connect(&_inactiveTimer, SIGNAL(timeout()),
            this, SLOT(connectionInactive()));

    _idleTimer.setSingleShot(true);
    _idleTimer.setInterval(28 * 60 * 1000);
    connect(&_idleTimer, &QTimer::timeout, this, &ImapClient::renewPushEmail);

    connect(QMailMessageBuffer::instance(), SIGNAL(flushed()), this, SLOT(messageBufferFlushed()));

    setupAccount();
}

ImapClient::~ImapClient()
{
    if (_protocol.inUse()) {
        _protocol.close();
    }
    stopIdleConnections();
    for (QMailMessageBufferFlushCallback *callback : callbacks) {
        QMailMessageBuffer::instance()->removeCallback(callback);
    }
    delete _strategyContext;
    delete _credentials;
}

// Called to begin executing a strategy
void ImapClient::newConnection()
{
    if (_protocol.loggingOut())
        _protocol.close();
    if (!_protocol.inUse()) {
        _qresyncEnabled = false;
    }
    if (_requestRapidClose && !_inactiveTimer.isActive())
        _rapidClosing = true; // Don't close the connection rapidly if interactive checking has recently occurred
    _requestRapidClose = false;
    _inactiveTimer.stop();

    QMailAccountConfiguration config(_accountId);
    ImapConfiguration imapCfg(config);
    if ( imapCfg.mailServer().isEmpty() ) {
        operationFailed(QMailServiceAction::Status::ErrConfiguration, tr("Cannot open connection without IMAP server configuration"));
        return;
    }

    if (!_credentials || (!_protocol.inUse() && !_credentials->init(imapCfg))) {
        operationFailed(QMailServiceAction::Status::ErrConfiguration,
                        _credentials ? _credentials->lastError() : QLatin1String("No credentials available"));
        return;
    }

    _strategyContext->newConnection();
}

ImapStrategyContext *ImapClient::strategyContext()
{
    return _strategyContext;
}

ImapStrategy *ImapClient::strategy() const
{
    return _strategyContext->strategy();
}

void ImapClient::setStrategy(ImapStrategy *strategy)
{
    _strategyContext->setStrategy(strategy);
}

void ImapClient::commandCompleted(ImapCommand command, OperationStatus status)
{
    checkCommandResponse(command, status);
    if (status == OpOk)
        commandTransition(command, status);
}

void ImapClient::checkCommandResponse(ImapCommand command, OperationStatus status)
{
    if ( status != OpOk ) {
        switch ( command ) {
            case IMAP_Enable:
            {
                // Couldn't enable QRESYNC, remove capability and continue
                qCWarning(lcIMAP) << _protocol.objectName() << "unable to enable QRESYNC";
                QStringList capa(_protocol.capabilities());
                capa.removeAll("QRESYNC");
                capa.removeAll("CONDSTORE");
                _protocol.setCapabilities(capa);
                commandTransition(command, OpOk);
                break;
            }
            case IMAP_UIDStore:
            {
                // Couldn't set a flag, ignore as we can stil continue
                qCWarning(lcIMAP) << _protocol.objectName() << "could not store message flag";
                commandTransition(command, OpOk);
                break;
            }

            case IMAP_Login:
            {
                if (!_loginFailed) {
                    _loginFailed = true;
                    _protocol.close();
                    newConnection();
                } else {
                    _credentials->invalidate(QStringLiteral("messageserver5"));
                    operationFailed(QMailServiceAction::Status::ErrLoginFailed, _protocol.lastError());
                }
                return;
            }

            case IMAP_Full:
            {
                operationFailed(QMailServiceAction::Status::ErrFileSystemFull, _protocol.lastError());
                return;
            }

            default:        //default = all critical messages
            {
                QString msg;
                if (_accountId.isValid()) {
                    QMailAccountConfiguration config(_accountId);
                    ImapConfiguration imapCfg(config);
                    msg = imapCfg.mailServer() + ": ";
                }
                msg.append(_protocol.lastError());

                operationFailed(QMailServiceAction::Status::ErrUnknownResponse, msg);
                return;
            }
        }
    }

    switch (command) {
        case IMAP_Full:
            qFatal( "Logic error, IMAP_Full" );
            break;
        case IMAP_Unconnected:
            operationFailed(QMailServiceAction::Status::ErrNoConnection, _protocol.lastError());
            return;
        case IMAP_Login:
            _loginFailed = (status != OpOk);
            break;
        default:
            break;
    }

}

void ImapClient::commandTransition(ImapCommand command, OperationStatus status)
{
    switch ( command ) {
        case IMAP_Init:
        {
            // We need to request the capabilities. Even in the case that an unsolicited response
            // has been received, as some servers report incomplete info in unsolicited responses,
            // missing the IDLE capability.
            // Currently idle connections must be established before main connection will
            // service requests.
            emit updateStatus( tr("Checking capabilities" ) );
            _protocol.sendCapability();
            break;
        }

        case IMAP_Capability:
        {
            if (_protocol.authenticated()) {
                // Received capabilities after logging in, continue with post login process
                Q_ASSERT(_protocol.receivedCapabilities());
                _protocol.setReceivedCapabilities(true);
                commandTransition(IMAP_Login, status);
                return;
            }

            if (!_protocol.encrypted()) {
                QMailAccountConfiguration config(_accountId);
                if (ImapAuthenticator::useEncryption(ImapConfiguration(config),
                                                     _protocol.capabilities())) {
                    // Switch to encrypted mode
                    emit updateStatus( tr("Starting TLS" ) );
                    _protocol.sendStartTLS();
                    break;
                }
            }

            // We are now connected
            QMailFolderIdList idleFolders;
            if (_protocol.supportsCapability("IDLE")
                && _pushConnectionsReserved) {
                idleFolders = configurationIdleFolderIds();
            }
            // Start / stop idle connections, to keep only idles
            monitor(idleFolders);

            if (isPushEmailEstablished()) {
                // Login is postponed after idle connections, see
                // IMAP_Idle_Continuation
                logIn();
            }
            break;
        }

        case IMAP_Idle_Continuation:
        {
            logIn();
            break;
        }

        case IMAP_StartTLS:
        {
            // Check capabilities for encrypted mode
            _protocol.sendCapability();
            break;
        }

        case IMAP_Login:
        {
            // After logging in server capabilities reported may change  so we need to request
            // capabilities again, unless already received in an unsolicited response
            if (!_protocol.receivedCapabilities()) {
                emit updateStatus( tr("Checking capabilities" ) );
                _protocol.sendCapability();
                return;
            }

            // Now that we know the capabilities, check for Reference and idle support
            QMailAccount account(_accountId);
            QMailAccountConfiguration config(_accountId);
            ImapConfiguration imapCfg(config);
            bool supportsReferences(_protocol.capabilities().contains("URLAUTH", Qt::CaseInsensitive) &&
                                    _protocol.capabilities().contains("CATENATE", Qt::CaseInsensitive)
#if !defined(QT_NO_SSL)
                                    // No FWOD support for IMAPS
                                    && (static_cast<QMailTransport::EncryptType>(imapCfg.mailEncryption()) != QMailTransport::Encrypt_SSL)
#endif
                                   );

            if (((account.status() & QMailAccount::CanReferenceExternalData) && !supportsReferences) ||
                (!(account.status() & QMailAccount::CanReferenceExternalData) && supportsReferences) ||
                (imapCfg.pushCapable() != _protocol.supportsCapability("IDLE")) ||
                (imapCfg.capabilities() != _protocol.capabilities())) {
                account.setStatus(QMailAccount::CanReferenceExternalData, supportsReferences);
                imapCfg.setPushCapable(_protocol.supportsCapability("IDLE"));
                imapCfg.setCapabilities(_protocol.capabilities());
                if (!QMailStore::instance()->updateAccount(&account, &config)) {
                    qCWarning(lcMailStore) << "Unable to update account" << account.id() << "to set imap4 configuration";
                }
            }
            // After logging in server capabilities reported may change so we need to
            // check if IDLE is already established, when enabled
            if (_protocol.supportsCapability("IDLE")
                && _pushConnectionsReserved) {
                // This is a noop when IDLE is already established
                // for the given folders
                monitor(configurationIdleFolderIds());
                emit updateStatus( tr("Logging in idle connection" ) );
            }

            bool compressCapable(_protocol.capabilities().contains("COMPRESS=DEFLATE", Qt::CaseInsensitive));
            if (!_protocol.encrypted() && QMFALLOWCOMPRESS && compressCapable && !_protocol.compress()) {
                _protocol.sendCompress(); // MUST not pipeline compress
                return;
            }
            // Server does not support compression, continue with post compress step
            commandTransition(IMAP_Compress, status);
            return;
        }

        case IMAP_Compress:
        {
            // Sent a compress, or logged in and server doesn't support compress
            if (!_protocol.capabilities().contains("QRESYNC", Qt::CaseInsensitive)) {
                _strategyContext->commandTransition(IMAP_Login, status);
            } else {
                if (!_qresyncEnabled) {
                    _protocol.sendEnable("QRESYNC CONDSTORE");
                    _qresyncEnabled = true;
                }
            }
            break;
        }

        case IMAP_Enable:
        {
            // Equivalent to having just logged in
            _strategyContext->commandTransition(IMAP_Login, status);
            break;
        }

        case IMAP_Noop:
        {
            QMailAccountConfiguration config(_accountId);
            _inactiveTimer.start(_closeCount ? MaxTimeBeforeNoop : ImapConfiguration(config).timeTillLogout() % MaxTimeBeforeNoop);
            break;
        }

        case IMAP_Logout:
        {
            // Ensure connection is closed on logout
            _protocol.close();
            return;
        }

        case IMAP_Select:
        case IMAP_Examine:
        case IMAP_QResync:
        {
            if (_protocol.mailbox().isSelected()) {
                const ImapMailboxProperties &properties(_protocol.mailbox());

                // See if we have anything to record about this mailbox
                QMailFolder folder(properties.id);

                bool modified(false);
                if ((folder.serverCount() != properties.exists) || (folder.serverUnreadCount() != properties.unseen)) {
                    folder.setServerCount(properties.exists);
                    folder.setServerUnreadCount(properties.unseen);
                    modified = true;

                    // See how this compares to the local mailstore count
                    updateFolderCountStatus(&folder);
                }

                QString supportsForwarded(properties.permanentFlags.contains("$Forwarded", Qt::CaseInsensitive) ? "true" : QString());
                if (folder.customField("qmf-supports-forwarded") != supportsForwarded) {
                    if (supportsForwarded.isEmpty()) {
                        folder.removeCustomField("qmf-supports-forwarded");
                    } else {
                        folder.setCustomField("qmf-supports-forwarded", supportsForwarded);
                    }
                    modified = true;
                }

                if (modified) {
                    if (!QMailStore::instance()->updateFolder(&folder)) {
                        qCWarning(lcMailStore) << "Unable to update folder" << folder.id() << "to update server counts!";
                    }
                }
            }
        }
        // fall through

        default:
        {
            _strategyContext->commandTransition(command, status);
            break;
        }
    }
}

/*  Mailboxes retrieved from the server goes here.  If the INBOX mailbox
    is new, it means it is the first time the account is used.
*/
void ImapClient::mailboxListed(const QString &flags, const QString &path)
{
    QMailFolderId parentId;
    QMailFolderId boxId;

    QMailAccount account(_accountId);

    QString mailboxPath;

    if (_protocol.delimiterUnknown())
        qCWarning(lcIMAP) << "Delimiter has not yet been discovered, which is essential to know the structure of a mailbox";

    QStringList list = _protocol.flatHierarchy() ? QStringList(path) : path.split(_protocol.delimiter());

    for ( QStringList::Iterator it = list.begin(); it != list.end(); ++it ) {

        if (it->isEmpty())
            continue; // Skip empty folder names

        if (!mailboxPath.isEmpty())
            mailboxPath.append(_protocol.delimiter());
        mailboxPath.append(*it);

        boxId = mailboxId(mailboxPath);
        if (boxId.isValid()) {
            // This element already exists
            if (mailboxPath == path) {
                QMailFolder folder(boxId);
                QMailFolder folderOriginal(folder);
                setFolderFlags(&account, &folder, flags, _protocol.capabilities().contains("XLIST"));

                if (folder.status() != folderOriginal.status()) {
                    if (!QMailStore::instance()->updateFolder(&folder)) {
                        qCWarning(lcMailStore) << "Unable to update folder for account:" << folder.parentAccountId() << "path:" << folder.path();
                    }
                }

                _strategyContext->mailboxListed(folder, flags);
            }

            parentId = boxId;
        } else {
            // This element needs to be created
            QMailFolder folder(mailboxPath, parentId, _accountId);
            folder.setDisplayName(QMailCodec::decodeModifiedUtf7(*it));
            folder.setStatus(QMailFolder::Incoming, true);
            // Set synchronization flag the same as parent folder, or true if there's no parent
            bool synchronize = parentId.isValid() ?
                        QMailFolder(parentId).status() & QMailFolder::SynchronizationEnabled : true;
            folder.setStatus(QMailFolder::SynchronizationEnabled, synchronize);

            // The reported flags pertain to the listed folder only
            QString folderFlags;
            if (mailboxPath == path) {
                folderFlags = flags;
            }

            if (QString::compare(path, "INBOX", Qt::CaseInsensitive) == 0) {
                //don't let inbox be deleted/renamed
                folder.setStatus(QMailFolder::DeletionPermitted, false);
                folder.setStatus(QMailFolder::RenamePermitted, false);
                folderFlags.append(" \\Inbox");
            } else {
                folder.setStatus(QMailFolder::DeletionPermitted, true);
                folder.setStatus(QMailFolder::RenamePermitted, true);
            }

            // Only folders beneath the base folder are relevant
            QString path(folder.path());
            QString baseFolder(_strategyContext->baseFolder());

            if (baseFolder.isEmpty() ||
                (path.startsWith(baseFolder, Qt::CaseInsensitive) && (path.length() == baseFolder.length())) ||
                (path.startsWith(baseFolder + _protocol.delimiter(), Qt::CaseInsensitive))) {
                if (!QMailStore::instance()->addFolder(&folder)) {
                    qCWarning(lcMailStore) << "Unable to add folder for account:" << folder.parentAccountId() << "path:" << folder.path();
                }
                else {
                    //set inbox as standardFolder
                    if (QString::compare(path, "INBOX", Qt::CaseInsensitive) == 0) {
                        account.setStandardFolder(QMailFolder::InboxFolder, folder.id());
                        if (!QMailStore::instance()->updateAccount(&account)) {
                            qCWarning(lcMailStore) << "Unable to update account" << account.id();
                        }
                    }
                }
            }

            setFolderFlags(&account, &folder, folderFlags, _protocol.capabilities().contains("XLIST")); // requires valid folder.id()
            _strategyContext->mailboxListed(folder, folderFlags);

            if (!QMailStore::instance()->updateFolder(&folder)) {
                qCWarning(lcMailStore) << "Unable to update folder for account:" << folder.parentAccountId() << "path:" << folder.path();
            }

            boxId = mailboxId(mailboxPath);
            parentId = boxId;
        }
    }
}

void ImapClient::messageFetched(QMailMessage& mail, const QString &detachedFilename, bool structureOnly)
{
    if (structureOnly) {
        mail.setParentAccountId(_accountId);

        // Some properties are inherited from the folder
        const ImapMailboxProperties &properties(_protocol.mailbox());

        mail.setParentFolderId(properties.id);

        if (properties.status & QMailFolder::Incoming) {
            mail.setStatus(QMailMessage::Incoming, true);
        }
        if (properties.status & QMailFolder::Outgoing) {
            mail.setStatus(QMailMessage::Outgoing, true);
        }
        if (properties.status & QMailFolder::Drafts) {
            mail.setStatus(QMailMessage::Draft, true);
        }
        if (properties.status & QMailFolder::Sent) {
            mail.setStatus(QMailMessage::Sent, true);
        }
        if (properties.status & QMailFolder::Trash) {
            mail.setStatus(QMailMessage::Trash, true);
        }
        if (properties.status & QMailFolder::Junk) {
            mail.setStatus(QMailMessage::Junk, true);
        }
        mail.setStatus(QMailMessage::CalendarInvitation, mail.hasCalendarInvitation());
        mail.setStatus(QMailMessage::HasSignature, (QMailCryptographicService::findSignedContainer(&mail) != 0));
        mail.setStatus(QMailMessage::CalendarCancellation, mail.hasCalendarCancellation());

        // Disable Notification when getting older message
        QMailFolder folder(properties.id);
        bool ok1, ok2; // toUint returns 0 on error, which is an invalid IMAP uid
        int clientMax(folder.customField("qmf-max-serveruid").toUInt(&ok1));
        int serverUid(ImapProtocol::uid(mail.serverUid()).toUInt(&ok2));
        if (ok1 && ok2 && clientMax && (serverUid < clientMax)) {
            // older message
            mail.setStatus(QMailMessage::NoNotification, true);
        }

    } else {
        // We need to update the message from the existing data
        QMailMessageMetaData existing(mail.serverUid(), _accountId);
        if (existing.id().isValid()) {
            // Record the status fields that may have been updated
            bool replied(mail.status() & QMailMessage::Replied);
            bool readElsewhere(mail.status() & QMailMessage::ReadElsewhere);
            bool importantElsewhere(mail.status() & QMailMessage::ImportantElsewhere);
            bool contentAvailable(mail.status() & QMailMessage::ContentAvailable);
            bool partialContentAvailable(mail.status() & QMailMessage::PartialContentAvailable);

            mail.setId(existing.id());
            mail.setParentAccountId(existing.parentAccountId());
            mail.setParentFolderId(existing.parentFolderId());
            mail.setStatus(existing.status());
            mail.setContent(existing.content());
            mail.setReceivedDate(existing.receivedDate());
            QMailDisconnected::copyPreviousFolder(existing, &mail);
            mail.setInResponseTo(existing.inResponseTo());
            mail.setResponseType(existing.responseType());
            mail.setContentScheme(existing.contentScheme());
            mail.setContentIdentifier(existing.contentIdentifier());
            mail.setCustomFields(existing.customFields());
            mail.setParentThreadId(existing.parentThreadId());

            // Preserve the status flags determined by the protocol
            mail.setStatus(QMailMessage::Replied, replied);
            mail.setStatus(QMailMessage::ReadElsewhere, readElsewhere);
            mail.setStatus(QMailMessage::ImportantElsewhere, importantElsewhere);
            if ((mail.status() & QMailMessage::ContentAvailable) || contentAvailable) {
                mail.setStatus(QMailMessage::ContentAvailable, true);
            }
            if ((mail.status() & QMailMessage::PartialContentAvailable) || partialContentAvailable) {
                mail.setStatus(QMailMessage::PartialContentAvailable, true);
            }
        } else {
            qCWarning(lcIMAP) << "Unable to find existing message for uid:" << mail.serverUid() << "account:" << _accountId;
        }
    }
    mail.setCustomField("qmf-detached-filename", detachedFilename);

    _classifier.classifyMessage(mail);


    QMailMessage *bufferMessage(new QMailMessage(mail));
    _bufferedMessages.append(bufferMessage);
    if (_strategyContext->messageFetched(*bufferMessage)) {
        removeAllFromBuffer(bufferMessage);
        return;
    }
    QMailMessageBufferFlushCallback *callback = new MessageFlushedWrapper(_strategyContext);
    callbacks << callback;
    QMailMessageBuffer::instance()->setCallback(bufferMessage, callback);
}


void ImapClient::folderCreated(const QString &folder, bool success)
{
    if (success) {
        mailboxListed(QString(), folder);
    }
    _strategyContext->folderCreated(folder, success);
}

void ImapClient::folderDeleted(const QMailFolder &folder, bool success)
{
    _strategyContext->folderDeleted(folder, success);
}

void ImapClient::folderRenamed(const QMailFolder &folder, const QString &newPath, bool success)
{
    _strategyContext->folderRenamed(folder, newPath, success);
}

void ImapClient::folderMoved(const QMailFolder &folder, const QString &newPath,
                             const QMailFolderId &newParentId, bool success)
{
    _strategyContext->folderMoved(folder, newPath, newParentId, success);
}

static bool updateParts(QMailMessagePart &part, const QByteArray &bodyData)
{
    static const QByteArray newLine(QMailMessage::CRLF);
    static const QByteArray marker("--");
    static const QByteArray bodyDelimiter(newLine + newLine);

    if (part.multipartType() == QMailMessage::MultipartNone) {
        // The body data is for this part only
        part.setBody(QMailMessageBody::fromData(bodyData, part.contentType(), part.transferEncoding(), QMailMessageBody::AlreadyEncoded));
        part.removeHeaderField("X-qmf-internal-partial-content");
    } else {
        const QByteArray &boundary(part.contentType().boundary());

        // TODO: this code is replicated from qmailmessage.cpp (parseMimeMultipart)

        // Separate the body into parts delimited by the boundary, and update them individually
        QByteArray partDelimiter = marker + boundary;
        QByteArray partTerminator = QByteArray(1, QMailMessage::LineFeed) + partDelimiter + marker;

        int startPos = bodyData.indexOf(partDelimiter, 0);
        if (startPos != -1)
            startPos += partDelimiter.length();

        // Subsequent delimiters include the leading newline
        partDelimiter.prepend(newLine);

        const char *baseAddress = bodyData.constData();
        int partIndex = 0;

        int endPos = bodyData.indexOf(partTerminator, 0);
        if (endPos > 0 && bodyData[endPos - 1] == QMailMessage::CarriageReturn) {
            endPos--;
        }
        while ((startPos != -1) && (startPos < endPos)) {
            // Skip the boundary line
            startPos = bodyData.indexOf(newLine, startPos);

            if ((startPos != -1) && (startPos < endPos)) {
                // Parse the section up to the next boundary marker
                int nextPos = bodyData.indexOf(partDelimiter, startPos);

                if (nextPos > 0 && bodyData[nextPos - 1] == QMailMessage::CarriageReturn) {
                    nextPos--;
                }

                // Find the beginning of the part body
                startPos = bodyData.indexOf(bodyDelimiter, startPos);
                if ((startPos != -1) && (startPos < nextPos)) {
                    startPos += bodyDelimiter.length();

                    QByteArray partBodyData(QByteArray::fromRawData(baseAddress + startPos, (nextPos - startPos)));
                    if (!updateParts(part.partAt(partIndex), partBodyData))
                        return false;

                    ++partIndex;
                }

                if (bodyData[nextPos] == QMailMessage::CarriageReturn) {
                    nextPos++;
                }
                // Move to the next part
                startPos = nextPos + partDelimiter.length();
            }
        }
    }

    return true;
}

class TemporaryFile
{
    enum { BufferSize = 4096 };

    QString _fileName;

public:
    TemporaryFile(const QString &fileName) : _fileName(QMail::tempPath() + fileName) {}

    QString fileName() const { return _fileName; }

    bool write(const QMailMessageBody &body)
    {
        QFile file(_fileName);
        if (!file.open(QIODevice::WriteOnly)) {
            qCWarning(lcMailStore) << "Unable to open file for writing:" << _fileName;
            return false;
        }

        // We need to write out the data still encoded - since we're deconstructing
        // server-side data, the meaning of the parameter is reversed...
        QMailMessageBody::EncodingFormat outputFormat(QMailMessageBody::Decoded);

        QDataStream out(&file);
        if (!body.toStream(out, outputFormat)) {
            qCWarning(lcMailStore) << "Unable to write existing body to file:" << _fileName;
            return false;
        }

        file.close();
        return true;
    }

    static bool copyFileData(QFile &srcFile, QFile &dstFile, qint64 maxLength = -1)
    {
        char buffer[BufferSize];

        while (!srcFile.atEnd() && (maxLength != 0)) {
            qint64 readSize = ((maxLength > 0) ? qMin<qint64>(maxLength, BufferSize) : static_cast<qint64>(BufferSize));
            readSize = srcFile.read(buffer, readSize);
            if (readSize == -1) {
                return false;
            }

            if (maxLength > 0) {
                maxLength -= readSize;
            }
            if (dstFile.write(buffer, readSize) != readSize) {
                return false;
            }
        }

        return true;
    }

    bool appendAndReplace(const QString &fileName)
    {
        {
            QFile existingFile(_fileName);
            QFile dataFile(fileName);

            if (!existingFile.exists()) {
                if (!QFile::copy(fileName, _fileName)) {
                    qCWarning(lcMailStore) << "Unable to copy - fileName:" << fileName << "_fileName:" << _fileName;
                    return false;
                }
            } else if (existingFile.open(QIODevice::Append)) {
                // On windows, this file will be unwriteable if it is open elsewhere
                if (dataFile.open(QIODevice::ReadOnly)) {
                    if (!copyFileData(dataFile, existingFile)) {
                        qCWarning(lcMailStore) << "Unable to append data to file:" << _fileName;
                        return false;
                    }
                } else {
                    qCWarning(lcMailStore) << "Unable to open new data for read:" << fileName;
                    return false;
                }
            } else if (existingFile.open(QIODevice::ReadOnly)) {
                if (dataFile.open(QIODevice::WriteOnly)) {
                    qint64 existingLength = QFileInfo(existingFile).size();
                    qint64 dataLength = QFileInfo(dataFile).size();

                    if (!dataFile.resize(existingLength + dataLength)) {
                        qCWarning(lcMailStore) << "Unable to resize data file:" << fileName;
                        return false;
                    } else {
                        QFile readDataFile(fileName);
                        if (!readDataFile.open(QIODevice::ReadOnly)) {
                            qCWarning(lcMailStore) << "Unable to reopen data file for read:" << fileName;
                            return false;
                        }

                        // Copy the data to the end of the file
                        dataFile.seek(existingLength);
                        if (!copyFileData(readDataFile, dataFile, dataLength)) {
                            qCWarning(lcMailStore) << "Unable to copy existing data in file:" << fileName;
                            return false;
                        }
                    }

                    // Copy the existing data before the new data
                    dataFile.seek(0);
                    if (!copyFileData(existingFile, dataFile, existingLength)) {
                        qCWarning(lcMailStore) << "Unable to copy existing data to file:" << fileName;
                        return false;
                    }
                } else {
                    qCWarning(lcMailStore) << "Unable to open new data for write:" << fileName;
                    return false;
                }

                // The complete data is now in the new file
                if (!QFile::remove(_fileName)) {
                    qCWarning(lcMailStore) << "Unable to remove pre-existing:" << _fileName;
                    return false;
                }

                _fileName = fileName;
                return true;
            } else {
                qCWarning(lcMailStore) << "Unable to open:" << _fileName;
                return false;
            }
        }

        if (!QFile::remove(fileName)) {
            qCWarning(lcMailStore) << "Unable to remove:" << fileName;
            return false;
        }
        if (!QFile::rename(_fileName, fileName)) {
            qCWarning(lcMailStore) << "Unable to rename:" << _fileName << fileName;
            return false;
        }

        _fileName = fileName;
        return true;
    }
};

void ImapClient::dataFetched(const QString &uid, const QString &section, const QString &fileName, int size)
{
    static const QString tempDir = QMail::tempPath();

    QMailMessage *mail;
    bool inBuffer = false;

    foreach (QMailMessage *msg, _bufferedMessages) {
        if (msg->serverUid() == uid) {
            mail = msg;
            inBuffer = true;
            break;
        }
    }
    if (!inBuffer) {
        mail = new QMailMessage(uid, _accountId);
    }

    detachedTempFiles.insert(mail->id(),fileName); // multi

    if (mail->id().isValid()) {
        if (section.isEmpty()) {
            // This is the body of the message, or a part thereof
            uint existingSize = 0;
            if (mail->hasBody()) {
                existingSize = mail->body().length();

                // Write the existing data to a temporary file
                TemporaryFile tempFile("mail-" + uid + "-body");
                if (!tempFile.write(mail->body())) {
                    qCWarning(lcMailStore) << "Unable to write existing body to file:" << tempFile.fileName();
                    return;
                }

                if (!tempFile.appendAndReplace(fileName)) {
                    qCWarning(lcMailStore) << "Unable to append data to existing body file:" << tempFile.fileName();
                    return;
                } else {
                    // The appended content file is now named 'fileName'
                }
            }

            // Set the content into the mail
            mail->setBody(QMailMessageBody::fromFile(fileName, mail->contentType(), mail->transferEncoding(), QMailMessageBody::AlreadyEncoded));
            mail->setStatus(QMailMessage::PartialContentAvailable, true);

            const uint totalSize(existingSize + size);
            if (totalSize >= mail->contentSize()) {
                // We have all the data for this message body
                mail->setStatus(QMailMessage::ContentAvailable, true);
            }

        } else {
            // This is data for a sub-part of the message
            QMailMessagePart::Location partLocation(section);
            if (!partLocation.isValid(false)) {
                qCWarning(lcIMAP) << "Unable to locate part for invalid section:" << section;
                return;
            } else if (!mail->contains(partLocation)) {
                qCWarning(lcIMAP) << "Unable to update invalid part for section:" << section;
                return;
            }

            QMailMessagePart &part = mail->partAt(partLocation);

            // Headers for part with undecoded data should have been
            // received already.
            if (part.hasUndecodedData()) {
                QFile file(fileName);
                if (!file.open(QIODevice::ReadOnly)) {
                    qCWarning(lcMailStore) << "Unable to read undecoded data from:" << fileName << "- error:" << file.error();
                    operationFailed(QMailServiceAction::Status::ErrFrameworkFault, tr("Unable to read fetched data"));
                    return;
                }
                part.appendUndecodedData(file.readAll());
            }

            int existingSize = 0;
            if (part.hasBody()) {
                existingSize = part.body().length();

                // Write the existing data to a temporary file
                TemporaryFile tempFile("mail-" + uid + "-part-" + section);
                if (!tempFile.write(part.body())) {
                    qCWarning(lcMailStore) << "Unable to write existing body to file:" << tempFile.fileName();
                    return;
                }

                if (!tempFile.appendAndReplace(fileName)) {
                    qCWarning(lcMailStore) << "Unable to append data to existing body file:" << tempFile.fileName();
                    return;
                } else {
                    // The appended content file is now named 'fileName'
                }
            }

            if (part.multipartType() == QMailMessage::MultipartNone) {
                // The body data is for this part only
                part.setBody(QMailMessageBody::fromFile(fileName, part.contentType(), part.transferEncoding(), QMailMessageBody::AlreadyEncoded));

                const int totalSize(existingSize + size);
                if (totalSize >= part.contentDisposition().size()) {
                    // We have all the data for this part
                    part.removeHeaderField("X-qmf-internal-partial-content");
                } else {
                    // We only have a portion of the part data
                    part.setHeaderField("X-qmf-internal-partial-content", "true");
                }

            } else {
                // Find the part bodies in the retrieved data
                QFile file(fileName);
                if (!file.open(QIODevice::ReadOnly)) {
                    qCWarning(lcMailStore) << "Unable to read fetched data from:" << fileName << "- error:" << file.error();
                    operationFailed(QMailServiceAction::Status::ErrFrameworkFault, tr("Unable to read fetched data"));
                    return;
                }

                uchar *data = file.map(0, size);
                if (!data) {
                    qCWarning(lcMailStore) << "Unable to map fetched data from:" << fileName << "- error:" << file.error();
                    operationFailed(QMailServiceAction::Status::ErrFrameworkFault, tr("Unable to map fetched data"));
                    return;
                }

                // Update the part bodies that we have retrieved
                QByteArray bodyData(QByteArray::fromRawData(reinterpret_cast<const char*>(data), size));
                if (!updateParts(part, bodyData)) {
                    operationFailed(QMailServiceAction::Status::ErrFrameworkFault, tr("Unable to update part body"));
                    return;
                }

                // These updates cannot be effected by storing the data file directly
                if (!mail->customField("qmf-detached-filename").isEmpty()) {
                    QFile::remove(mail->customField("qmf-detached-filename"));
                    mail->removeCustomField("qmf-detached-filename");
                }
            }
        }

        if (inBuffer) {
            return;
        }

        _bufferedMessages.append(mail);
        _strategyContext->dataFetched(*mail, uid, section);
        QMailMessageBufferFlushCallback *callback = new DataFlushedWrapper(_strategyContext, uid, section);
        callbacks << callback;
        QMailMessageBuffer::instance()->setCallback(mail, callback);
    } else {
        qCWarning(lcIMAP) << "Unable to handle dataFetched - uid:" << uid << "section:" << section;
        operationFailed(QMailServiceAction::Status::ErrFrameworkFault, tr("Unable to handle dataFetched without context"));
    }
}

void ImapClient::partHeaderFetched(const QString &uid, const QString &section, const QString &fileName, int /* size */)
{
    static const QString tempDir = QMail::tempPath();

    QMailMessage *mail;
    bool inBuffer = false;

    foreach (QMailMessage *msg, _bufferedMessages) {
        if (msg->serverUid() == uid) {
            mail = msg;
            inBuffer = true;
            break;
        }
    }
    if (!inBuffer) {
        mail = new QMailMessage(uid, _accountId);
    }

    detachedTempFiles.insert(mail->id(),fileName); // multi

    if (mail->id().isValid() && !section.isEmpty()) {
        // This is data for a sub-part of the message
        QMailMessagePart::Location partLocation(section);
        if (!partLocation.isValid(false)) {
            qCWarning(lcIMAP) << "Unable to locate part for invalid section:" << section;
            return;
        } else if (!mail->contains(partLocation)) {
            qCWarning(lcIMAP) << "Unable to update invalid part for section:" << section;
            return;
        }

        QMailMessagePart &part = mail->partAt(partLocation);

        QFile file(fileName);
        if (!file.open(QIODevice::ReadOnly)) {
            qCWarning(lcMailStore) << "Unable to read undecoded data from:" << fileName << "- error:" << file.error();
            operationFailed(QMailServiceAction::Status::ErrFrameworkFault, tr("Unable to read fetched data"));
            return;
        }
        // We start the undecoded part by saving the header.
        // The rest will be appended at later arrival.
        part.setUndecodedData(file.readAll());

        if (inBuffer) {
            return;
        }

        _bufferedMessages.append(mail);
        _strategyContext->dataFetched(*mail, uid, section);
        QMailMessageBufferFlushCallback *callback = new DataFlushedWrapper(_strategyContext, uid, section);
        callbacks << callback;
        QMailMessageBuffer::instance()->setCallback(mail, callback);
    } else {
        qCWarning(lcIMAP) << "Unable to handle partHeaderFetched - uid:" << uid << "section:" << section;
        operationFailed(QMailServiceAction::Status::ErrFrameworkFault, tr("Unable to handle partHeaderFetched without context"));
    }
}

void ImapClient::nonexistentUid(const QString &uid)
{
    _strategyContext->nonexistentUid(uid);
}

void ImapClient::messageStored(const QString &uid)
{
    _strategyContext->messageStored(uid);
}

void ImapClient::messageCopied(const QString &copiedUid, const QString &createdUid)
{
    _strategyContext->messageCopied(copiedUid, createdUid);
}

void ImapClient::messageCreated(const QMailMessageId &id, const QString &uid)
{
    _strategyContext->messageCreated(id, uid);
}

void ImapClient::downloadSize(const QString &uid, int size)
{
    _strategyContext->downloadSize(uid, size);
}

void ImapClient::urlAuthorized(const QString &url)
{
    _strategyContext->urlAuthorized(url);
}

void ImapClient::setupAccount() const
{
    QMailAccount account(_accountId);
    bool updated = false;

    if (!(account.status() & QMailAccount::CanCreateFolders)) {
        account.setStatus(QMailAccount::CanCreateFolders, true);
        updated = true;
    }

    if (!(account.status() & QMailAccount::CanSearchOnServer)) {
        account.setStatus(QMailAccount::CanSearchOnServer, true);
        updated = true;
    }

    // At this point account can't have a persistent connection to the server,
    // if for some reason the status is wrong(crash/abort) we will
    // reset correct status here.
    if (account.status() & QMailAccount::HasPersistentConnection) {
        updated = true;
        account.setStatus(QMailAccount::HasPersistentConnection, false);
    }

    if (updated && !QMailStore::instance()->updateAccount(&account)) {
        qCWarning(lcMailStore) << "Unable to setup account status" << account.id();
    }
}

QMailAccountId ImapClient::account() const
{
    return _accountId;
}

void ImapClient::transportError(int code, const QString &msg)
{
    operationFailed(code, msg);
}

void ImapClient::transportError(QMailServiceAction::Status::ErrorCode code, const QString &msg)
{
    operationFailed(code, msg);
}

void ImapClient::closeConnection()
{
    _inactiveTimer.stop();
    if ( _protocol.inUse() ) {
        _protocol.close();
    }
}

void ImapClient::transportStatus(const QString& status)
{
    emit updateStatus(status);
}

void ImapClient::cancelTransfer(QMailServiceAction::Status::ErrorCode code, const QString &text)
{
    operationFailed(code, text);
}

void ImapClient::retrieveOperationCompleted()
{
    deactivateConnection();

    // This retrieval may have been asynchronous
    emit allMessagesReceived();

    // Or it may have been requested by a waiting client
    emit retrievalCompleted();
}

void ImapClient::deactivateConnection()
{
    QMailAccountConfiguration config(_accountId);
    int time(ImapConfiguration(config).timeTillLogout());
    if (_rapidClosing)
        time = 0;
    _closeCount = time / MaxTimeBeforeNoop;
    _inactiveTimer.start(_closeCount ? MaxTimeBeforeNoop : time);
}

void ImapClient::connectionInactive()
{
    Q_ASSERT(_closeCount >= 0);
    if (_closeCount == 0) {
        _rapidClosing = false;
        if ( _protocol.connected()) {
            emit updateStatus( tr("Logging out") );
            _protocol.sendLogout();
            // Client MUST read tagged response, but if connection hangs in logout state newConnection will autoClose.
        } else {
            closeConnection();
        }
    } else {
        --_closeCount;
        _protocol.sendNoop();
    }
}

void ImapClient::operationFailed(int code, const QString &text)
{
    if (_protocol.inUse())
        _protocol.close();

    emit errorOccurred(code, text);
}

void ImapClient::operationFailed(QMailServiceAction::Status::ErrorCode code, const QString &text)
{
    if (_protocol.inUse())
        _protocol.close();

    emit errorOccurred(code, text);
}

QMailFolderId ImapClient::mailboxId(const QString &path) const
{
    QMailFolderIdList folderIds = QMailStore::instance()->queryFolders(QMailFolderKey::parentAccountId(_accountId) & QMailFolderKey::path(path));
    if (folderIds.count() == 1)
        return folderIds.first();

    return QMailFolderId();
}

QMailFolderIdList ImapClient::mailboxIds() const
{
    return QMailStore::instance()->queryFolders(QMailFolderKey::parentAccountId(_accountId), QMailFolderSortKey::path(Qt::AscendingOrder));
}

QStringList ImapClient::serverUids(const QMailFolderId &folderId) const
{
    // We need to list both the messages in the mailbox, and those moved to the
    // Trash folder which are still in the mailbox as far as the server is concerned
    return serverUids(messagesKey(folderId) | trashKey(folderId));
}

QStringList ImapClient::serverUids(const QMailFolderId &folderId, quint64 messageStatusFilter, bool set) const
{
    QMailMessageKey statusKey(QMailMessageKey::status(messageStatusFilter, QMailDataComparator::Includes));
    return serverUids((messagesKey(folderId) | trashKey(folderId)) & (set ? statusKey : ~statusKey));
}

QStringList ImapClient::serverUids(QMailMessageKey key) const
{
    QStringList uidList;

    for (const QMailMessageMetaData& r : QMailStore::instance()->messagesMetaData(key, QMailMessageKey::ServerUid))
        if (!r.serverUid().isEmpty())
            uidList.append(r.serverUid());

    return uidList;
}

QMailMessageKey ImapClient::messagesKey(const QMailFolderId &folderId) const
{
    return (QMailMessageKey::parentAccountId(_accountId) &
            QMailDisconnected::sourceKey(folderId));
}

QMailMessageKey ImapClient::trashKey(const QMailFolderId &folderId) const
{
    return (QMailMessageKey::parentAccountId(_accountId) &
            QMailDisconnected::sourceKey(folderId) &
            QMailMessageKey::status(QMailMessage::Trash));
}

QStringList ImapClient::deletedMessages(const QMailFolderId &folderId) const
{
    QStringList serverUidList;

    for (const QMailMessageRemovalRecord& r : QMailStore::instance()->messageRemovalRecords(_accountId, folderId))
        if (!r.serverUid().isEmpty())
            serverUidList.append(r.serverUid());

    return serverUidList;
}

void ImapClient::updateFolderCountStatus(QMailFolder *folder)
{
    // Find the local mailstore count for this folder
    QMailMessageKey folderContent(QMailDisconnected::sourceKey(folder->id()));
    folderContent &= ~QMailMessageKey::status(QMailMessage::Removed);

    uint count = QMailStore::instance()->countMessages(folderContent);
    folder->setStatus(QMailFolder::PartialContent, (count < folder->serverCount()));
}

bool ImapClient::isPushEmailEstablished()
{
    QMailAccountConfiguration config(account());
    ImapConfiguration imapCfg(config);
    if (!imapCfg.pushEnabled() || configurationIdleFolderIds().isEmpty())
        return true;

    return !_monitored.isEmpty() && _waitingForIdleFolderIds.isEmpty();
}

void ImapClient::setIdlingForFolder(const QMailFolderId &id)
{
    if (_monitored.value(id)->connected()
        && !_idleTimer.isActive()) {
        _idleTimer.start();
    }
    if (!_waitingForIdleFolderIds.isEmpty()) {
        _waitingForIdleFolderIds.removeOne(id);
        if (_waitingForIdleFolderIds.isEmpty()) {
            commandCompleted(IMAP_Idle_Continuation, OpOk);
        }
    }
}

QMailFolderIdList ImapClient::configurationIdleFolderIds()
{
    QMailAccountConfiguration config(account());
    ImapConfiguration imapCfg(config);
    QMailFolderIdList folderIds;
    if (!imapCfg.pushEnabled())
        return folderIds;
    foreach(QString folderName, imapCfg.pushFolders()) {
        QMailFolderId idleFolderId(mailboxId(folderName));
        if (idleFolderId.isValid()) {
            folderIds.append(idleFolderId);
        }
    }
    return folderIds;
}

void ImapClient::monitor(const QMailFolderIdList &mailboxIds)
{
    static int count(0);

    if (mailboxIds.isEmpty()) {
        _idleTimer.stop();
    }

    foreach(const QMailFolderId &id, _monitored.keys()) {
        if (!mailboxIds.contains(id)) {
            qCDebug(lcIMAP) << "stop monitoring folder" << id;
            IdleProtocol *protocol = _monitored.take(id);
            protocol->close(); // Instead of closing could reuse below in some cases
            delete protocol;
            _waitingForIdleFolderIds.removeOne(id);
        }
    }

    QMailAccountConfiguration config(account());
    ImapConfiguration imapCfg(config);
    if (!_protocol.supportsCapability("IDLE")
        || !imapCfg.pushEnabled()) {
        return;
    }

    foreach(QMailFolderId id, mailboxIds) {
        if (!_monitored.contains(id)) {
            qCDebug(lcIMAP) << "start monitoring folder" << id;
            ++count;
            IdleProtocol *protocol = new IdleProtocol(this, QMailFolder(id), _credentials);
            protocol->setObjectName(QString("I:%1").arg(count));
            _monitored.insert(id, protocol);
            _waitingForIdleFolderIds.append(id);
            connect(protocol, SIGNAL(idleNewMailNotification(QMailFolderId)),
                    this, SIGNAL(idleNewMailNotification(QMailFolderId)));
            connect(protocol, SIGNAL(idleFlagsChangedNotification(QMailFolderId)),
                    this, SIGNAL(idleFlagsChangedNotification(QMailFolderId)));
            connect(protocol, &IdleProtocol::transportError,
                    this, &ImapClient::pushEmailError);
            protocol->open(imapCfg);
        }
    }
}

void ImapClient::messageBufferFlushed()
{
    // We know this is now empty
    callbacks.clear();
}

void ImapClient::removeAllFromBuffer(QMailMessage *message)
{
    if (message) {
        QMultiMap<QMailMessageId, QString>::const_iterator i = detachedTempFiles.find(message->id());
        while (i != detachedTempFiles.end() && i.key() == message->id()) {
            if (!(*i).isEmpty() && QFile::exists(*i)) {
                QFile::remove(*i);
            }
            ++i;
        }
        detachedTempFiles.remove(message->id());
    }
    int i = 0;
    while ((i = _bufferedMessages.indexOf(message, i)) != -1) {
        delete _bufferedMessages.at(i);
        _bufferedMessages.remove(i);
    }
}

void ImapClient::logIn()
{
    emit updateStatus( tr("Logging in" ) );
    if (_credentials->status() == QMailCredentialsInterface::Ready) {
        QMailAccountConfiguration config(_accountId);
        _protocol.sendLogin(config, _credentials);
    } else if (_credentials->status() == QMailCredentialsInterface::Fetching) {
        connect(_credentials, &QMailCredentialsInterface::statusChanged,
                this, &ImapClient::onCredentialsStatusChanged);
    } else {
        qCWarning(lcIMAP) << "credential retrieval failed with:" << _credentials->lastError();
        operationFailed(QMailServiceAction::Status::ErrConfiguration,
                        _credentials->lastError());
    }
}

void ImapClient::onCredentialsStatusChanged()
{
    qCDebug(lcIMAP)  << "Got credential status changed" << _credentials->status();

    disconnect(_credentials, &QMailCredentialsInterface::statusChanged,
               this, &ImapClient::onCredentialsStatusChanged);
    if (_protocol.inUse()) {
        switch (_credentials->status()) {
        case (QMailCredentialsInterface::Ready): {
            QMailAccountConfiguration config(_accountId);
            _protocol.sendLogin(config, _credentials);
            break;
        }
        case (QMailCredentialsInterface::Failed):
            operationFailed(QMailServiceAction::Status::ErrLoginFailed, _credentials->lastError());
            break;
        default:
            break;
        }
    }
}

#include "imapclient.moc"
