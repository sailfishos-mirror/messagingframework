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

#ifndef IMAPSTRATEGY_H
#define IMAPSTRATEGY_H

#include "imapprotocol.h"
#include "integerregion.h"

#include <QString>
#include <QStringList>
#include <QList>
#include <QMap>

#include <qmailfolder.h>
#include <qmailmessagebuffer.h>

struct SectionProperties {
    enum MinimumType {
        All = -1,
        HeadersOnly = -2
    };

    SectionProperties(const QMailMessagePart::Location &location = QMailMessagePart::Location(),
                      int minimum = All)
        :  _location(location),
          _minimum(minimum)
    {
    }

    bool isEmpty() const
    {
        return (!_location.isValid() && (_minimum == All));
    }

    QMailMessagePart::Location _location;
    int _minimum;
};

struct MessageSelector
{
    MessageSelector(uint uid, const QMailMessageId &messageId, const SectionProperties &properties)
        : _uid(uid),
          _messageId(messageId),
          _properties(properties)
    {
    }

    QString uidString(const QString &mailbox) const
    {
        if (_uid == 0) {
            return ("id:" + QString::number(_messageId.toULongLong()));
        } else {
            return (mailbox + QString::number(_uid));
        }
    }

    uint _uid;
    QMailMessageId _messageId;
    SectionProperties _properties;
};

typedef QList<MessageSelector> FolderSelections;
typedef QMap<QMailFolderId, FolderSelections> SelectionMap;

class QMailAccount;
class QMailAccountConfiguration;
class ImapClient;

class ImapStrategyContextBase
{
public:
    ImapStrategyContextBase(ImapClient *client) { _client = client; }
    virtual ~ImapStrategyContextBase()  {}

    ImapClient *client();
    ImapProtocol &protocol();
    const ImapMailboxProperties &mailbox();
    QMailAccountId accountId();

    void folderModified(const QMailFolderId &folderId) { _modifiedFolders.insert(folderId); }

    void updateStatus(const QString &);
    void progressChanged(uint, uint);
    void completedMessageAction(const QString &uid);
    void completedMessageCopy(QMailMessage &message, const QMailMessage &original);
    void operationCompleted();
    void matchingMessageIds(const QMailMessageIdList &msgs);
    void remainingMessagesCount(uint count);
    void messagesCount(uint count);

private:
    ImapClient *_client;
    QSet<QMailFolderId> _modifiedFolders;
};

class ImapStrategy
{
public:
    ImapStrategy() {}
    virtual ~ImapStrategy() {}

    virtual void newConnection(ImapStrategyContextBase *context);
    virtual void transition(ImapStrategyContextBase*, const ImapCommand, const OperationStatus) = 0;

    virtual void mailboxListed(ImapStrategyContextBase *context, QMailFolder& folder, const QString &flags);
    virtual bool messageFetched(ImapStrategyContextBase *context, QMailMessage &message);
    virtual void messageFlushed(ImapStrategyContextBase *context, QMailMessage &message);
    virtual void dataFetched(ImapStrategyContextBase *context, QMailMessage &message, const QString &uid, const QString &section);
    virtual void dataFlushed(ImapStrategyContextBase *context, QMailMessage &message, const QString &uid, const QString &section);
    virtual void nonexistentUid(ImapStrategyContextBase *context, const QString &uid);
    virtual void messageStored(ImapStrategyContextBase *context, const QString &uid);
    virtual void messageCopied(ImapStrategyContextBase *context, const QString &copiedUid, const QString &createdUid);
    virtual void messageCreated(ImapStrategyContextBase *context, const QMailMessageId &id, const QString &uid);
    virtual void downloadSize(ImapStrategyContextBase *context, const QString &uid, int length);
    virtual void urlAuthorized(ImapStrategyContextBase *context, const QString &url);
    virtual void folderCreated(ImapStrategyContextBase *context, const QString &folder, bool success);
    virtual void folderDeleted(ImapStrategyContextBase *context, const QMailFolder &folder, bool success);
    virtual void folderRenamed(ImapStrategyContextBase *context, const QMailFolder &folder, const QString &newName, bool success);
    virtual void folderMoved(ImapStrategyContextBase *context, const QMailFolder &folder, const QString &newName, const QMailFolderId &newParentId, bool success);
    virtual void selectFolder(ImapStrategyContextBase *context, const QMailFolder &folder);

    void clearError() { _error = false; }
    bool error() { return _error; }
    QString baseFolder() { return _baseFolder; }

protected:
    virtual void initialAction(ImapStrategyContextBase *context);

    enum TransferState { Init, List, Search, Preview, Complete, Copy };

    TransferState _transferState;
    QString _baseFolder;
    bool _error;
    QMap<QString,bool> _folder;
};

class ImapCreateFolderStrategy : public ImapStrategy
{
public:
    ImapCreateFolderStrategy(): _inProgress(0) { }
    virtual ~ImapCreateFolderStrategy() {}

    void transition(ImapStrategyContextBase *, const ImapCommand, const OperationStatus) override;
    virtual void createFolder(const QMailFolderId &folder, const QString &name, bool matchFoldersRequired);
    void folderCreated(ImapStrategyContextBase *context, const QString &folder, bool success) override;
protected:
    virtual void handleCreate(ImapStrategyContextBase *context);
    virtual void handleLogin(ImapStrategyContextBase *context);
    virtual void process(ImapStrategyContextBase *context);

    QList<QPair<QMailFolderId, QString> > _folders;
    int _inProgress;

private:
    bool _matchFoldersRequired;
};


class ImapDeleteFolderStrategy : public ImapStrategy
{
public:
    ImapDeleteFolderStrategy() : _inProgress(0) { }
    virtual ~ImapDeleteFolderStrategy() {}

    void transition(ImapStrategyContextBase *, const ImapCommand, const OperationStatus) override;
    virtual void deleteFolder(const QMailFolderId &folderId);
    void folderDeleted(ImapStrategyContextBase *context, const QMailFolder &folder, bool success) override;
protected:
    virtual void handleLogin(ImapStrategyContextBase *context);
    virtual void handleDelete(ImapStrategyContextBase *context);
    virtual void process(ImapStrategyContextBase *context);
    virtual void deleteFolder(const QMailFolderId &folderId, ImapStrategyContextBase *context);
    QList<QMailFolderId> _folderIds;
    int _inProgress;
};

class ImapRenameFolderStrategy : public ImapStrategy
{
public:
    ImapRenameFolderStrategy() : _inProgress(0) { }
    virtual ~ImapRenameFolderStrategy() {}

    void transition(ImapStrategyContextBase *, const ImapCommand, const OperationStatus) override;
    virtual void renameFolder(const QMailFolderId &folderId, const QString &newName);
    void folderRenamed(ImapStrategyContextBase *context, const QMailFolder &folder,
                       const QString &name, bool success) override;
protected:
    virtual void handleLogin(ImapStrategyContextBase *context);
    virtual void handleRename(ImapStrategyContextBase *context);
    virtual void process(ImapStrategyContextBase *context);
    QList<QPair<QMailFolderId, QString> > _folderNewNames;
    int _inProgress;
};

class ImapMoveFolderStrategy : public ImapStrategy
{
public:
    ImapMoveFolderStrategy() : _inProgress(0) { }
    virtual ~ImapMoveFolderStrategy() {}

    void transition(ImapStrategyContextBase *, const ImapCommand, const OperationStatus) override;
    virtual void moveFolder(const QMailFolderId &folderId, const QMailFolderId &newParentId);
    void folderMoved(ImapStrategyContextBase *context, const QMailFolder &folder,
                     const QString &newPath, const QMailFolderId &newParentId, bool success) override;
protected:
    virtual void handleLogin(ImapStrategyContextBase *context);
    virtual void handleMove(ImapStrategyContextBase *context);
    virtual void process(ImapStrategyContextBase *context);
    QList<QPair<QMailFolderId, QMailFolderId> > _folderNewParents;
    int _inProgress;
};

class ImapPrepareMessagesStrategy : public ImapStrategy
{
public:
    ImapPrepareMessagesStrategy() {}
    virtual ~ImapPrepareMessagesStrategy() {}

    virtual void setUnresolved(const QList<QPair<QMailMessagePart::Location, QMailMessagePart::Location> > &ids, bool external);

    void newConnection(ImapStrategyContextBase *context) override;
    void transition(ImapStrategyContextBase*, const ImapCommand, const OperationStatus) override;

    void urlAuthorized(ImapStrategyContextBase *context, const QString &url) override;

protected:
    virtual void handleLogin(ImapStrategyContextBase *context);
    virtual void handleGenUrlAuth(ImapStrategyContextBase *context);

    virtual void nextMessageAction(ImapStrategyContextBase *context);
    virtual void messageListCompleted(ImapStrategyContextBase *context);

    QList<QPair<QMailMessagePart::Location, QMailMessagePart::Location> > _locations;
    bool _external;
};

class ImapMessageListStrategy : public ImapStrategy
{
public:
    ImapMessageListStrategy() {}
    virtual ~ImapMessageListStrategy() {}

    virtual void clearSelection();
    virtual void selectedMailsAppend(const QMailMessageIdList &ids);
    virtual void selectedSectionsAppend(const QMailMessagePart::Location &location);
    void newConnection(ImapStrategyContextBase *context) override;
    void transition(ImapStrategyContextBase*, const ImapCommand, const OperationStatus) override;

    void initialAction(ImapStrategyContextBase *context) override;

protected:
    enum { DefaultBatchSize = 50 };
    enum { MaxPipeliningDepth = 4 };

    virtual void handleLogin(ImapStrategyContextBase *context);
    virtual void checkUidValidity(ImapStrategyContextBase *context);
    virtual void handleSelect(ImapStrategyContextBase *context);
    virtual void handleCreate(ImapStrategyContextBase *context);
    virtual void handleDelete(ImapStrategyContextBase *context);
    virtual void handleRename(ImapStrategyContextBase *context);
    virtual void handleMove(ImapStrategyContextBase *context);
    virtual void handleClose(ImapStrategyContextBase *context);

    virtual void messageListFolderAction(ImapStrategyContextBase *context);
    virtual void messageListMessageAction(ImapStrategyContextBase *context) = 0;
    virtual void messageListCompleted(ImapStrategyContextBase *context);

    virtual void resetMessageListTraversal();
    virtual bool selectNextMessageSequence(ImapStrategyContextBase *context, int maximum = DefaultBatchSize, bool folderActionPermitted = true);

    virtual void setCurrentMailbox(const QMailFolderId &id);

    virtual bool messageListFolderActionRequired();

    SelectionMap _selectionMap;
    SelectionMap::Iterator _folderItr;
    FolderSelections::ConstIterator _selectionItr;
    QMailFolder _currentMailbox;
    QString _currentModSeq;
    QStringList _messageUids;
    QMailMessagePart::Location _msgSection;
    int _sectionStart;
    int _sectionEnd;
};

class ImapFetchSelectedMessagesStrategy : public ImapMessageListStrategy
{
public:
    ImapFetchSelectedMessagesStrategy() : _listSize(0), _totalRetrievalSize(0) {}
    virtual ~ImapFetchSelectedMessagesStrategy() {}

    virtual void setOperation(ImapStrategyContextBase *context,
                              QMailRetrievalAction::RetrievalSpecification spec);
    void clearSelection() override;
    void selectedMailsAppend(const QMailMessageIdList &ids) override;
    virtual void selectedSectionsAppend(const QMailMessagePart::Location &, int = -1);
    virtual void prepareCompletionList(
                                  ImapStrategyContextBase *context,
                                  const QMailMessage &message,
                                  QMailMessageIdList &completionList,
                                  QList<QPair<QMailMessagePart::Location, int> > &completionSectionList);

    void newConnection(ImapStrategyContextBase *context) override;
    void transition(ImapStrategyContextBase*, const ImapCommand, const OperationStatus) override;

    void downloadSize(ImapStrategyContextBase*, const QString &uid, int length) override;
    bool messageFetched(ImapStrategyContextBase *context, QMailMessage &message) override;
    void messageFlushed(ImapStrategyContextBase *context, QMailMessage &message) override;
    void dataFetched(ImapStrategyContextBase *context, QMailMessage &message, const QString &uid, const QString &section) override;
    void dataFlushed(ImapStrategyContextBase *context, QMailMessage &message, const QString &uid, const QString &section) override;

protected:
    void handleLogin(ImapStrategyContextBase *context) override;
    virtual void handleUidFetch(ImapStrategyContextBase *context);

    void messageListMessageAction(ImapStrategyContextBase *context) override;

    virtual void itemFetched(ImapStrategyContextBase *context, const QString &uid);

    virtual void metaDataAnalysis(ImapStrategyContextBase *context,
                                  const QMailMessagePartContainer &partContainer,
                                  const QList<QMailMessagePartContainer::Location> &attachmentLocations,
                                  const QMailMessagePartContainer::Location &signedPartLocation,
                                  QList<QPair<QMailMessagePart::Location, uint> > &sectionList,
                                  QList<QPair<QMailMessagePart::Location, int> > &completionSectionList,
                                  QMailMessagePart::Location &preferredBody,
                                  uint &bytesLeft);

    QMailRetrievalAction::RetrievalSpecification _retrievalSpec;

    uint _headerLimit;
    int _listSize;
    int _messageCount;
    int _messageCountIncremental;
    int _outstandingFetches;

    // RetrievalMap maps uid -> ((units, bytes) to be retrieved, percentage retrieved)
    typedef QMultiMap<QString, QPair< QPair<uint, uint>, uint> > RetrievalMap;
    RetrievalMap _retrievalSize;
    uint _progressRetrievalSize;
    uint _totalRetrievalSize;
};

class ImapFolderListStrategy : public ImapFetchSelectedMessagesStrategy
{
public:
    ImapFolderListStrategy() : _processed(0), _processable(0) {}
    virtual ~ImapFolderListStrategy() {}

    void clearSelection() override;
    virtual void selectedFoldersAppend(const QMailFolderIdList &ids);

    void newConnection(ImapStrategyContextBase *context) override;
    void transition(ImapStrategyContextBase*, const ImapCommand, const OperationStatus) override;

    void mailboxListed(ImapStrategyContextBase *context, QMailFolder& folder, const QString &flags) override;

protected:
    void handleLogin(ImapStrategyContextBase *context) override;
    virtual void handleList(ImapStrategyContextBase *context);
    void handleSelect(ImapStrategyContextBase *context) override;
    virtual void handleSearch(ImapStrategyContextBase *context);

    virtual void folderListFolderAction(ImapStrategyContextBase *context);
    virtual void folderListCompleted(ImapStrategyContextBase *context);

    virtual void processNextFolder(ImapStrategyContextBase *context);
    virtual bool nextFolder();
    virtual void processFolder(ImapStrategyContextBase *context);

    void updateUndiscoveredCount(ImapStrategyContextBase *context);

    enum FolderStatus
    {
        NoInferiors = (1 << 0),
        NoSelect = (1 << 1),
        Marked = (1 << 2),
        Unmarked = (1 << 3),
        HasChildren = (1 << 4),
        HasNoChildren = (1 << 5)
    };

    QMailFolderIdList _mailboxIds;
    QMap<QMailFolderId, FolderStatus> _folderStatus;

    int _processed;
    int _processable;
};

class ImapUpdateMessagesFlagsStrategy : public ImapFolderListStrategy
{
public:
    ImapUpdateMessagesFlagsStrategy() {}
    virtual ~ImapUpdateMessagesFlagsStrategy() {}

    void clearSelection() override;
    void selectedMailsAppend(const QMailMessageIdList &messageIds) override;
    virtual QMailMessageIdList selectedMails();

    void transition(ImapStrategyContextBase*, const ImapCommand, const OperationStatus) override;

protected:
    void handleLogin(ImapStrategyContextBase *context) override;
    virtual void handleUidSearch(ImapStrategyContextBase *context);

    void folderListFolderAction(ImapStrategyContextBase *context) override;
    void folderListCompleted(ImapStrategyContextBase *context) override;

    bool nextFolder() override;
    void processFolder(ImapStrategyContextBase *context) override;

    virtual void processUidSearchResults(ImapStrategyContextBase *context);

private:
    QMailMessageIdList _selectedMessageIds;
    QMap<QMailFolderId, QStringList> _folderMessageUids;
    QStringList _serverUids;
    QString _filter;
    enum SearchState { Seen, Unseen, Flagged };
    SearchState _searchState;

    QStringList _seenUids;
    QStringList _unseenUids;
    QStringList _flaggedUids;
};

class ImapSynchronizeBaseStrategy : public ImapFolderListStrategy
{
public:
    ImapSynchronizeBaseStrategy() : _ignoreSyncFlag(false) {}
    virtual ~ImapSynchronizeBaseStrategy() {}

    void newConnection(ImapStrategyContextBase *context) override;

    bool messageFetched(ImapStrategyContextBase *context, QMailMessage &message) override;
    void messageFlushed(ImapStrategyContextBase *context, QMailMessage &message) override;
    virtual void setIgnoreSyncFlag(bool ignoreSyncFlag);

protected:
    void handleLogin(ImapStrategyContextBase *context) override;
    void handleSelect(ImapStrategyContextBase *context) override;
    void handleUidFetch(ImapStrategyContextBase *context) override;

    bool nextFolder() override;
    virtual bool synchronizationEnabled(const QMailFolder &folder) const;

    virtual void previewDiscoveredMessages(ImapStrategyContextBase *context);
    virtual bool selectNextPreviewFolder(ImapStrategyContextBase *context);

    virtual void fetchNextMailPreview(ImapStrategyContextBase *context);
    virtual void folderPreviewCompleted(ImapStrategyContextBase *context);

    virtual void processUidSearchResults(ImapStrategyContextBase *context);

protected:
    QStringList _newUids;
    QList<QPair<QMailFolderId, QStringList> > _retrieveUids;
    QMailMessageIdList _completionList;
    QList<QPair<QMailMessagePart::Location, int> > _completionSectionList;
    int _outstandingPreviews;

private:
    bool _ignoreSyncFlag;
    uint _progress;
    uint _total;
};

class ImapRetrieveFolderListStrategy : public ImapSynchronizeBaseStrategy
{
public:
    ImapRetrieveFolderListStrategy() : _quickList(false), _descending(false) {}
    virtual ~ImapRetrieveFolderListStrategy() {}

    virtual void setBase(const QMailFolderId &folderId);
    virtual void setQuickList(bool quickList);
    virtual void setDescending(bool descending);

    void newConnection(ImapStrategyContextBase *context) override;

    void mailboxListed(ImapStrategyContextBase *context, QMailFolder& folder, const QString &flags) override;

protected:
    void handleLogin(ImapStrategyContextBase *context) override;
    void handleSearch(ImapStrategyContextBase *context) override;
    void handleList(ImapStrategyContextBase *context) override;

    void folderListCompleted(ImapStrategyContextBase *context) override;

    void removeDeletedMailboxes(ImapStrategyContextBase *context);

    QMailFolderId _baseId;
    bool _quickList;
    bool _descending;
    QStringList _mailboxPaths;
    QSet<QString> _ancestorPaths;
    QStringList _ancestorSearchPaths;
    QMailFolderIdList _mailboxList;
};

class ImapRetrieveMessageListStrategy : public ImapSynchronizeBaseStrategy
{
public:
    ImapRetrieveMessageListStrategy() {}
    virtual ~ImapRetrieveMessageListStrategy() {}

    virtual void setMinimum(uint minimum);
    virtual void setAccountCheck(bool accountCheck);

    void transition(ImapStrategyContextBase*, const ImapCommand, const OperationStatus) override;
    void selectFolder(ImapStrategyContextBase *context, const QMailFolder &folder) override;

protected:
    void handleLogin(ImapStrategyContextBase *context) override;
    virtual void handleUidSearch(ImapStrategyContextBase *context);
    virtual void handleFetchFlags(ImapStrategyContextBase *context);
    virtual void qresyncHandleUidSearch(ImapStrategyContextBase *context);

    void messageListCompleted(ImapStrategyContextBase *context) override;
    void folderListCompleted(ImapStrategyContextBase *context) override;
    void processUidSearchResults(ImapStrategyContextBase *context) override;

    void folderListFolderAction(ImapStrategyContextBase *context) override;
    virtual void qresyncFolderListFolderAction(ImapStrategyContextBase *context);

    bool synchronizationEnabled(const QMailFolder &folder) const override;

    uint _minimum;
    bool _accountCheck;
    bool _fillingGap;
    bool _listAll;
    bool _qresyncListingNew;
    IntegerRegion _qresyncRetrieve;
    int _qresyncVanished;
    QMap<QMailFolderId, IntegerRegion> _newMinMaxMap;
    QMailFolderIdList _updatedFolders;
};

class ImapSynchronizeAllStrategy : public ImapRetrieveFolderListStrategy
{
public:
    enum Options
    {
        RetrieveMail = (1 << 0),
        ImportChanges = (1 << 1),
        ExportChanges = (1 << 2)
    };

    ImapSynchronizeAllStrategy();
    virtual ~ImapSynchronizeAllStrategy() {}

    void setOptions(Options options);

    void transition(ImapStrategyContextBase*, const ImapCommand, const OperationStatus) override;

protected:
    virtual void handleUidSearch(ImapStrategyContextBase *context);
    virtual void handleUidStore(ImapStrategyContextBase *context);
    virtual void handleExpunge(ImapStrategyContextBase *context);

    void folderListFolderAction(ImapStrategyContextBase *context) override;

    void processUidSearchResults(ImapStrategyContextBase *context) override;
    virtual void searchInconclusive(ImapStrategyContextBase *context);

    void folderListCompleted(ImapStrategyContextBase *context) override;

    virtual bool setNextSeen(ImapStrategyContextBase *context);
    virtual bool setNextNotSeen(ImapStrategyContextBase *context);
    virtual bool setNextImportant(ImapStrategyContextBase *context);
    virtual bool setNextNotImportant(ImapStrategyContextBase *context);
    virtual bool setNextDeleted(ImapStrategyContextBase *context);

    void folderPreviewCompleted(ImapStrategyContextBase *context) override;

protected:
    QStringList _readUids;
    QStringList _unreadUids;
    QStringList _importantUids;
    QStringList _unimportantUids;
    QStringList _removedUids;
    QStringList _storedReadUids;
    QStringList _storedUnreadUids;
    QStringList _storedImportantUids;
    QStringList _storedUnimportantUids;
    QStringList _storedRemovedUids;
    bool _expungeRequired;
    static const int batchSize = 1000;

private:
    Options _options;

    enum SearchState { All, Seen, Unseen, Flagged, Inconclusive };
    SearchState _searchState;

    QStringList _seenUids;
    QStringList _unseenUids;
    QStringList _flaggedUids;
};

class ImapRetrieveAllStrategy : public ImapSynchronizeAllStrategy
{
public:
    ImapRetrieveAllStrategy();
    virtual ~ImapRetrieveAllStrategy() {}
};

class ImapSearchMessageStrategy : public ImapRetrieveFolderListStrategy
{
public:
    ImapSearchMessageStrategy() : _canceled(false) { setBase(QMailFolderId()); setQuickList(true); setDescending(true); }
    virtual ~ImapSearchMessageStrategy() {}

    virtual void cancelSearch();
    virtual void searchArguments(const QMailMessageKey &searchCriteria, const QString &bodyText, quint64 limit, const QMailMessageSortKey &sort, bool count);
    void transition(ImapStrategyContextBase *, const ImapCommand, const OperationStatus) override;
protected:
    void folderListCompleted(ImapStrategyContextBase *context) override;
    virtual void handleSearchMessage(ImapStrategyContextBase *context);
    void handleUidFetch(ImapStrategyContextBase *context) override;
    void folderListFolderAction(ImapStrategyContextBase *context) override;

    bool messageFetched(ImapStrategyContextBase *context, QMailMessage &message) override;
    void messageFlushed(ImapStrategyContextBase *context, QMailMessage &message) override;
    void messageListCompleted(ImapStrategyContextBase *context) override;

    struct SearchData {
        QMailMessageKey criteria;
        QString bodyText;
        QMailMessageSortKey sort;
        uint limit;
        bool count;
    };
    QList<SearchData> _searches;
    QList<QMailMessageId> _fetchedList;
    bool _canceled;
    uint _limit;
    uint _count;
};

class ImapExportUpdatesStrategy : public ImapSynchronizeAllStrategy
{
public:
    ImapExportUpdatesStrategy();
    virtual ~ImapExportUpdatesStrategy() {}

protected:
    void handleLogin(ImapStrategyContextBase *context) override;
    void handleUidSearch(ImapStrategyContextBase *context) override;
    void handleSelect(ImapStrategyContextBase *context) override;

    void folderListFolderAction(ImapStrategyContextBase *context) override;
    bool nextFolder() override;

    void processUidSearchResults(ImapStrategyContextBase *context) override;

    QStringList _serverReportedUids;
    QStringList _clientDeletedUids;
    QStringList _clientReadUids;
    QStringList _clientUnreadUids;
    QStringList _clientImportantUids;
    QStringList _clientUnimportantUids;

    QMap<QMailFolderId, QList<QStringList> > _folderMessageUids;
};

class ImapCopyMessagesStrategy : public ImapFetchSelectedMessagesStrategy
{
public:
    ImapCopyMessagesStrategy() {}
    virtual ~ImapCopyMessagesStrategy() {}

    void clearSelection() override;
    virtual void appendMessageSet(const QMailMessageIdList &ids, const QMailFolderId &destinationId);

    void newConnection(ImapStrategyContextBase *context) override;
    void transition(ImapStrategyContextBase*, const ImapCommand, const OperationStatus) override;

    void messageCopied(ImapStrategyContextBase *context, const QString &copiedUid, const QString &createdUid) override;
    void messageCreated(ImapStrategyContextBase *context, const QMailMessageId &id, const QString &createdUid) override;
    bool messageFetched(ImapStrategyContextBase *context, QMailMessage &message) override;
    void messageFlushed(ImapStrategyContextBase *context, QMailMessage &message) override;

protected:
    void handleLogin(ImapStrategyContextBase *context) override;
    void handleSelect(ImapStrategyContextBase *context) override;
    virtual void handleUidCopy(ImapStrategyContextBase *context);
    virtual void handleAppend(ImapStrategyContextBase *context);
    virtual void handleUidSearch(ImapStrategyContextBase *context);
    virtual void handleUidStore(ImapStrategyContextBase *context);
    void handleUidFetch(ImapStrategyContextBase *context) override;

    void messageListFolderAction(ImapStrategyContextBase *context) override;
    void messageListMessageAction(ImapStrategyContextBase *context) override;
    void messageListCompleted(ImapStrategyContextBase *context) override;

    virtual QString copiedMessageFetched(ImapStrategyContextBase *context, QMailMessage &message);
    virtual void updateCopiedMessage(ImapStrategyContextBase *context, QMailMessage &message, const QMailMessage &source);

    virtual void removeObsoleteUids(ImapStrategyContextBase *context);

    virtual void copyNextMessage(ImapStrategyContextBase *context);
    virtual void fetchNextCopy(ImapStrategyContextBase *context);

    virtual void selectMessageSet(ImapStrategyContextBase *context);

    QList<QPair<QMailMessageIdList, QMailFolderId> > _messageSets;
    QMailFolder _destination;
    QMap<QString, QString> _sourceUid;
    QStringList _sourceUids;
    int _sourceIndex;
    QStringList _createdUids;
    int _messagesAdded;
    QStringList _obsoleteDestinationUids;
    QMap<QString,QString> _remember;
};

class ImapMoveMessagesStrategy : public ImapCopyMessagesStrategy
{
public:
    ImapMoveMessagesStrategy() {}
    virtual ~ImapMoveMessagesStrategy() {}

    void transition(ImapStrategyContextBase*, const ImapCommand, const OperationStatus) override;
    void messageFlushed(ImapStrategyContextBase *context, QMailMessage &message) override;

protected:
    void handleUidCopy(ImapStrategyContextBase *context) override;
    void handleUidStore(ImapStrategyContextBase *context) override;
    void handleClose(ImapStrategyContextBase *context) override;
    void handleUidFetch(ImapStrategyContextBase *context) override;
    virtual void handleExamine(ImapStrategyContextBase *context);

    void messageListFolderAction(ImapStrategyContextBase *context) override;
    void messageListMessageAction(ImapStrategyContextBase *context) override;

    void updateCopiedMessage(ImapStrategyContextBase *context, QMailMessage &message, const QMailMessage &source) override;

    QMailFolder _lastMailbox;
    QMap<QString, QMailMessageId> _messagesToRemove;
};

class ImapExternalizeMessagesStrategy : public ImapCopyMessagesStrategy
{
public:
    ImapExternalizeMessagesStrategy() {}
    virtual ~ImapExternalizeMessagesStrategy() {}

    void appendMessageSet(const QMailMessageIdList &ids, const QMailFolderId &destinationId) override;

    void newConnection(ImapStrategyContextBase *context) override;
    void transition(ImapStrategyContextBase*, const ImapCommand, const OperationStatus) override;

    bool messageFetched(ImapStrategyContextBase *context, QMailMessage &message) override;
    void messageFlushed(ImapStrategyContextBase *context, QMailMessage &message) override;
    void urlAuthorized(ImapStrategyContextBase *context, const QString &url) override;

protected:
    virtual void handleGenUrlAuth(ImapStrategyContextBase *context);

    void messageListCompleted(ImapStrategyContextBase *context) override;

    void updateCopiedMessage(ImapStrategyContextBase *context, QMailMessage &message, const QMailMessage &source) override;

    virtual void resolveNextMessage(ImapStrategyContextBase *context);

    QMailMessageIdList _urlIds;
};

class ImapFlagMessagesStrategy : public ImapFetchSelectedMessagesStrategy
{
public:
    ImapFlagMessagesStrategy() {}
    virtual ~ImapFlagMessagesStrategy() {}

    void newConnection(ImapStrategyContextBase *context) override;
    void clearSelection() override;
    virtual void setMessageFlags(MessageFlags flags, bool set);

    void transition(ImapStrategyContextBase*, const ImapCommand, const OperationStatus) override;

protected:
    virtual void handleUidStore(ImapStrategyContextBase *context);

    void messageListMessageAction(ImapStrategyContextBase *context) override;

    MessageFlags _setMask;
    MessageFlags _unsetMask;
    int _outstandingStores;
};

class ImapDeleteMessagesStrategy : public ImapFlagMessagesStrategy
{
public:
    ImapDeleteMessagesStrategy() {}
    virtual ~ImapDeleteMessagesStrategy() {}

    void setLocalMessageRemoval(bool removal);
    void clearSelection() override;

    void transition(ImapStrategyContextBase*, const ImapCommand, const OperationStatus) override;

protected:
    void handleUidStore(ImapStrategyContextBase *context) override;
    void handleClose(ImapStrategyContextBase *context) override;
    virtual void handleExamine(ImapStrategyContextBase *context);

    void messageListFolderAction(ImapStrategyContextBase *context) override;
    void messageListCompleted(ImapStrategyContextBase *context) override;

    bool _removal;
    QStringList _storedList;
    QMailFolder _lastMailbox;
};

class ImapStrategyContext : public ImapStrategyContextBase
{
public:
    ImapStrategyContext(ImapClient *client);

    ImapPrepareMessagesStrategy prepareMessagesStrategy;
    ImapFetchSelectedMessagesStrategy selectedStrategy;
    ImapRetrieveFolderListStrategy foldersOnlyStrategy;
    ImapExportUpdatesStrategy exportUpdatesStrategy;
    ImapUpdateMessagesFlagsStrategy updateMessagesFlagsStrategy;
    ImapSynchronizeAllStrategy synchronizeAccountStrategy;
    ImapCopyMessagesStrategy copyMessagesStrategy;
    ImapMoveMessagesStrategy moveMessagesStrategy;
    ImapExternalizeMessagesStrategy externalizeMessagesStrategy;
    ImapFlagMessagesStrategy flagMessagesStrategy;
    ImapDeleteMessagesStrategy deleteMessagesStrategy;
    ImapRetrieveMessageListStrategy retrieveMessageListStrategy;
    ImapRetrieveAllStrategy retrieveAllStrategy;
    ImapCreateFolderStrategy createFolderStrategy;
    ImapDeleteFolderStrategy deleteFolderStrategy;
    ImapRenameFolderStrategy renameFolderStrategy;
    ImapMoveFolderStrategy moveFolderStrategy;
    ImapSearchMessageStrategy searchMessageStrategy;

    void newConnection() { _strategy->clearError(); _strategy->newConnection(this); }
    void commandTransition(const ImapCommand command, const OperationStatus status) { _strategy->transition(this, command, status); }

    void mailboxListed(QMailFolder& folder, const QString &flags) { _strategy->mailboxListed(this, folder, flags); }
    bool messageFetched(QMailMessage &message) { return _strategy->messageFetched(this, message); }
    void messageFlushed(QMailMessage &message) { _strategy->messageFlushed(this, message); }
    void dataFetched(QMailMessage &message, const QString &uid, const QString &section) { _strategy->dataFetched(this, message, uid, section); }
    void dataFlushed(QMailMessage &message, const QString &uid, const QString &section) { _strategy->dataFlushed(this, message, uid, section); }
    void nonexistentUid(const QString &uid) { _strategy->nonexistentUid(this, uid); }
    void messageStored(const QString &uid) { _strategy->messageStored(this, uid); }
    void messageCopied(const QString &copiedUid, const QString &createdUid) { _strategy->messageCopied(this, copiedUid, createdUid); }
    void messageCreated(const QMailMessageId &id, const QString &uid) { _strategy->messageCreated(this, id, uid); }
    void downloadSize(const QString &uid, int length) { _strategy->downloadSize(this, uid, length); }
    void urlAuthorized(const QString &url) { _strategy->urlAuthorized(this, url); }
    void folderCreated(const QString &folder, bool success) { _strategy->folderCreated(this, folder, success); }
    void folderDeleted(const QMailFolder &folder, bool success) { _strategy->folderDeleted(this, folder, success); }
    void folderRenamed(const QMailFolder &folder, const QString &name, bool success) {
        _strategy->folderRenamed(this, folder, name, success);
    }
    void folderMoved(const QMailFolder &folder, const QString &name, const QMailFolderId &newParentId, bool success) {
        _strategy->folderMoved(this, folder, name, newParentId, success);
    }
    QString baseFolder() { return _strategy->baseFolder(); }

    ImapStrategy *strategy() const { return _strategy; }
    void setStrategy(ImapStrategy *strategy) { _strategy = strategy; }

private:
    ImapStrategy *_strategy;
};

#endif
