/*
 * Copyright (C) 2016 ~ 2018 Deepin Technology Co., Ltd.
 *               2016 ~ 2018 dragondjf
 *
 * Author:     dragondjf<dingjiangfeng@deepin.com>
 *
 * Maintainer: dragondjf<dingjiangfeng@deepin.com>
 *             zccrs<zhangjide@deepin.com>
 *             Tangtong<tangtong@deepin.com>
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses/>.
 */

#include "dfileinfo.h"
#include "private/dfileinfo_p.h"
#include "app/define.h"

#include "shutil/fileutils.h"

#include "controllers/pathmanager.h"
#include "controllers/filecontroller.h"

#include "app/define.h"
#include "singleton.h"
#include "usershare/usersharemanager.h"
#include "deviceinfo/udisklistener.h"
#include "tag/tagmanager.h"

#include "dfileservices.h"
#include "dthumbnailprovider.h"
#include "dfileiconprovider.h"
#include "dmimedatabase.h"
#include "dabstractfilewatcher.h"

#include <QDateTime>
#include <QDir>
#include <QPainter>
#include <QApplication>
#include <QtConcurrent>

#include <sys/stat.h>

DFM_USE_NAMESPACE

#define REQUEST_THUMBNAIL_DEALY 500

class RequestEP : public QThread
{
    Q_OBJECT

public:
    static RequestEP *instance();

    ~RequestEP();

    // Request get the file extension propertys
    QQueue<QPair<DUrl, DFileInfoPrivate*>> requestEPFiles;
    QReadWriteLock requestEPFilesLock;
    QSet<DFileInfoPrivate*> dirtyFileInfos;

    void run() override;
    void requestEP(const DUrl &url, DFileInfoPrivate *info);
    void cancelRequestEP(DFileInfoPrivate *info);

Q_SIGNALS:
    void requestEPFinished(const DUrl &url, const QVariantHash &ep);

 private Q_SLOTS:
    void processEPChanged(const DUrl &url, DFileInfoPrivate *info, const QVariantHash &ep);

private:
    explicit RequestEP(QObject *parent = 0);
};

RequestEP::RequestEP(QObject *parent)
    : QThread(parent)
{
    QMetaType::registerEqualsComparator<QList<QColor>>();
    qRegisterMetaType<DFileInfoPrivate*>();

    connect(this, &RequestEP::finished, this, [this] {
        dirtyFileInfos.clear();
    });
}

RequestEP *RequestEP::instance()
{
    // RequestEP对象必须和线程相关，不然线程销毁后，QObject::thread()为nullptr
    // 导致使用QCoreApplication::postEvent给此对象发送事件无效
    // 间接导致 run 中调用  QMetaObject::invokeMethod(this, "processEPChanged", Qt::QueuedConnection, Q_ARG(DUrl, url), Q_ARG(DFileInfoPrivate*, file_info.second), Q_ARG(QVariantHash, ep));
    // 时无效，因为Qt::QueueConnection参数会将此调用交给事件循环处理
    thread_local static RequestEP eq;

    return &eq;
}

RequestEP::~RequestEP()
{

}

void RequestEP::run()
{
    forever {
        requestEPFilesLock.lockForRead();
        if (requestEPFiles.isEmpty()) {
            requestEPFilesLock.unlock();
            return;
        }
        requestEPFilesLock.unlock();
        requestEPFilesLock.lockForWrite();
        auto file_info = requestEPFiles.dequeue();
        requestEPFilesLock.unlock();

        const DUrl &url = file_info.first;
        const QStringList &tag_list = DFileService::instance()->getTagsThroughFiles(nullptr, {url});

        QVariantHash ep;

        if (!tag_list.isEmpty()) {
            ep["tag_name_list"] = tag_list;
        }

        QList<QColor> colors;

        for (const QColor &color : TagManager::instance()->getTagColor(tag_list)) {
            colors << color;
        }

        if (!colors.isEmpty()) {
            ep["colored"] = QVariant::fromValue(colors);
        }

        QMetaObject::invokeMethod(this, "processEPChanged", Qt::QueuedConnection,
                                  Q_ARG(DUrl, url), Q_ARG(DFileInfoPrivate*, file_info.second), Q_ARG(QVariantHash, ep));
    }
}

void RequestEP::requestEP(const DUrl &url, DFileInfoPrivate *info)
{
    requestEPFilesLock.lockForRead();

    for (int i = 0; i < requestEPFiles.count(); ++i) {
        auto file_info = requestEPFiles.at(i);

        if (file_info.second == info) {
            requestEPFilesLock.unlock();
            return;
        }
    }

    requestEPFilesLock.unlock();
    requestEPFilesLock.lockForWrite();
    requestEPFiles << qMakePair(url, info);
    requestEPFilesLock.unlock();

    if (!isRunning()) {
        start();
    }
}

void RequestEP::cancelRequestEP(DFileInfoPrivate *info)
{
    requestEPFilesLock.lockForRead();

    for (int i = 0; i < requestEPFiles.count(); ++i) {
        auto file_info = requestEPFiles.at(i);

        if (file_info.second == info) {
            requestEPFilesLock.unlock();
            requestEPFilesLock.lockForWrite();
            requestEPFiles.removeAt(i);
            requestEPFilesLock.unlock();
            info->requestEP = nullptr;
            return;
        }
    }

    requestEPFilesLock.unlock();
    dirtyFileInfos << info;
}

void RequestEP::processEPChanged(const DUrl &url, DFileInfoPrivate *info, const QVariantHash &ep)
{
    Q_EMIT requestEPFinished(url, ep);

    QVariantHash oldEP;

    if (!dirtyFileInfos.contains(info)) {
        oldEP = info->extensionPropertys;
        info->extensionPropertys = ep;
        info->epInitialized = true;
        info->requestEP = nullptr;
    } else {
        dirtyFileInfos.remove(info);
        info = nullptr;
    }

    if (!ep.isEmpty() || oldEP != ep) {
        DAbstractFileWatcher::ghostSignal(url.parentUrl(), &DAbstractFileWatcher::fileAttributeChanged, url);

        if (info) {
            // ###(zccrs): DFileSystemModel中收到通知后会调用DAbstractFileInfo::refresh，导致会重新获取扩展属性
            info->epInitialized = true;
        }
    }
}

DFileInfoPrivate::DFileInfoPrivate(const DUrl &url, DFileInfo *qq, bool hasCache)
    : DAbstractFileInfoPrivate (url, qq, hasCache)
{
    fileInfo.setFile(url.toLocalFile());
}

DFileInfoPrivate::~DFileInfoPrivate()
{
    if (getIconTimer) {
        getIconTimer->stop();
        getIconTimer->deleteLater();
    }

    if (getEPTimer) {
        getEPTimer->stop();
        getEPTimer->deleteLater();
    }

    if (requestEP)
        requestEP->cancelRequestEP(this);
}

DFileInfo::DFileInfo(const QString &filePath, bool hasCache)
    : DFileInfo(DUrl::fromLocalFile(filePath), hasCache)
{

}

DFileInfo::DFileInfo(const DUrl &fileUrl, bool hasCache)
    : DAbstractFileInfo(*new DFileInfoPrivate(fileUrl, this, hasCache))
{

}

DFileInfo::DFileInfo(const QFileInfo &fileInfo, bool hasCache)
    : DFileInfo(DUrl::fromLocalFile(fileInfo.absoluteFilePath()), hasCache)
{

}

DFileInfo::~DFileInfo()
{

}

bool DFileInfo::exists(const DUrl &fileUrl)
{
    return QFileInfo::exists(fileUrl.toLocalFile());
}

QMimeType DFileInfo::mimeType(const QString &filePath, QMimeDatabase::MatchMode mode)
{
    DMimeDatabase db;

    return db.mimeTypeForFile(filePath, mode);
}

bool DFileInfo::exists() const
{
    Q_D(const DFileInfo);

    return d->fileInfo.exists() || d->fileInfo.isSymLink();
}

bool DFileInfo::isPrivate() const
{
    Q_D(const DFileInfo);

    return FileController::privateFileMatch(d->fileInfo.absolutePath(), d->fileInfo.fileName());
}

QString DFileInfo::path() const
{
    Q_D(const DFileInfo);

    return d->fileInfo.path();
}

QString DFileInfo::filePath() const
{
    Q_D(const DFileInfo);

    return d->fileInfo.filePath();
}

QString DFileInfo::absolutePath() const
{
    Q_D(const DFileInfo);

    return d->fileInfo.absolutePath();
}

QString DFileInfo::absoluteFilePath() const
{
    Q_D(const DFileInfo);

    return d->fileInfo.absoluteFilePath();
}

QString DFileInfo::fileName() const
{
    Q_D(const DFileInfo);

    if (d->fileInfo.absoluteFilePath().endsWith(QDir::separator()))
        return QFileInfo(d->fileInfo.absolutePath()).fileName();

    return d->fileInfo.fileName();
}

QString DFileInfo::fileSharedName() const
{
    const ShareInfo &info = userShareManager->getShareInfoByPath(absoluteFilePath());

    return info.shareName();
}

bool DFileInfo::canRename() const
{
    if (systemPathManager->isSystemPath(absoluteFilePath()))
        return false;

    bool canRename = DFileInfo(absolutePath()).isWritable();

//    canRenameCacheMap[fileUrl()] = canRename;

    return canRename;
}

bool DFileInfo::canShare() const
{
    if (isDir() && isReadable()) {
        if (absoluteFilePath().startsWith(QDir::homePath())) {
            return true;
        }

        UDiskDeviceInfoPointer info = deviceListener->getDeviceByFilePath(filePath());

        if (info) {
            if (info->getMediaType() != UDiskDeviceInfo::unknown && info->getMediaType() !=UDiskDeviceInfo::network)
                return true;
        }
    }

    return false;
}

bool DFileInfo::canFetch() const
{
    if (isPrivate())
        return false;

    return isDir() || FileUtils::isArchive(absoluteFilePath());
}

bool DFileInfo::isReadable() const
{
    if (isPrivate())
        return false;

    Q_D(const DFileInfo);

    if (FileUtils::isGvfsMountFile(absoluteFilePath())) {
        return true;
    } else {
        return d->fileInfo.isReadable();
    }
}

bool DFileInfo::isWritable() const
{
    if (isPrivate())
        return false;

    Q_D(const DFileInfo);

    if (FileUtils::isGvfsMountFile(absoluteFilePath())) {
        return true;
    } else {
        return d->fileInfo.isWritable();
    }
}

bool DFileInfo::isExecutable() const
{
    Q_D(const DFileInfo);
    if (FileUtils::isGvfsMountFile(absoluteFilePath())){
        return true;
    }else{
        return d->fileInfo.isExecutable();
    }
}

bool DFileInfo::isHidden() const
{
    Q_D(const DFileInfo);

    return d->fileInfo.isHidden() || FileController::customHiddenFileMatch(d->fileInfo.absolutePath(), d->fileInfo.fileName());
}

bool DFileInfo::isRelative() const
{
    Q_D(const DFileInfo);

    return d->fileInfo.isRelative();
}

bool DFileInfo::isAbsolute() const
{
    Q_D(const DFileInfo);

    return d->fileInfo.isAbsolute();
}

bool DFileInfo::isShared() const
{
    return userShareManager->isShareFile(absoluteFilePath());
}

bool DFileInfo::isWritableShared() const
{
    const ShareInfo &info = userShareManager->getShareInfoByPath(absoluteFilePath());

    return info.isWritable();
}

bool DFileInfo::isAllowGuestShared() const
{
    const ShareInfo &info = userShareManager->getShareInfoByPath(absoluteFilePath());

    return info.isGuestOk();
}

bool DFileInfo::makeAbsolute()
{
    Q_D(DFileInfo);

    return d->fileInfo.makeAbsolute();
}

bool DFileInfo::isFile() const
{
    Q_D(const DFileInfo);

    return d->fileInfo.isFile();
}

bool DFileInfo::isDir() const
{
    Q_D(const DFileInfo);

    return d->fileInfo.isDir();
}

bool DFileInfo::isSymLink() const
{
    Q_D(const DFileInfo);

    return d->fileInfo.isSymLink();
}

DUrl DFileInfo::symLinkTarget() const
{
    Q_D(const DFileInfo);

    if (d->fileInfo.isSymLink())
        return DUrl::fromLocalFile(d->fileInfo.symLinkTarget());

    return DAbstractFileInfo::symLinkTarget();
}

QString DFileInfo::owner() const
{
    Q_D(const DFileInfo);

    return d->fileInfo.owner();
}

uint DFileInfo::ownerId() const
{
    Q_D(const DFileInfo);

    return d->fileInfo.ownerId();
}

QString DFileInfo::group() const
{
    Q_D(const DFileInfo);

    return d->fileInfo.group();
}

uint DFileInfo::groupId() const
{
    Q_D(const DFileInfo);

    return d->fileInfo.groupId();
}

bool DFileInfo::permission(QFileDevice::Permissions permissions) const
{
    Q_D(const DFileInfo);

    if (isPrivate())
        return false;

    return d->fileInfo.permission(permissions);
}

QFileDevice::Permissions DFileInfo::permissions() const
{
    Q_D(const DFileInfo);

    if (isPrivate())
        return QFileDevice::Permissions();

    return d->fileInfo.permissions();
}

qint64 DFileInfo::size() const
{
    Q_D(const DFileInfo);

    return d->fileInfo.size();
}

int DFileInfo::filesCount() const
{
    if (isDir())
        return FileUtils::filesCount(absoluteFilePath());

    return -1;
}

QDateTime DFileInfo::created() const
{
    Q_D(const DFileInfo);

    return d->fileInfo.created();
}

QDateTime DFileInfo::lastModified() const
{
    Q_D(const DFileInfo);

    if (isSymLink() && !exists()) {
        struct stat attrib;

        if (lstat(d->fileInfo.filePath().toLocal8Bit().constData(), &attrib) >= 0)
            return QDateTime::fromTime_t(attrib.st_mtime);
    }

    return d->fileInfo.lastModified();
}

QDateTime DFileInfo::lastRead() const
{
    Q_D(const DFileInfo);

    return d->fileInfo.lastRead();
}

QMimeType DFileInfo::mimeType(QMimeDatabase::MatchMode mode) const
{
    Q_D(const DFileInfo);

    if (!d->mimeType.isValid() || d->mimeTypeMode != mode) {
        d->mimeType = mimeType(absoluteFilePath(), mode);
        d->mimeTypeMode = mode;
    }

    return d->mimeType;
}

bool DFileInfo::canIteratorDir() const
{
    return true;
}

QString DFileInfo::subtitleForEmptyFloder() const
{
    if (!exists()) {
        return QObject::tr("File has been moved or deleted");
    } else if (!isReadable()) {
        return QObject::tr("You do not have permission to access this folder");
    }

    return QObject::tr("Folder is empty");
}

QString DFileInfo::fileDisplayName() const
{
    if (systemPathManager->isSystemPath(toLocalFile())) {
        const QString &displayName = systemPathManager->getSystemPathDisplayNameByPath(filePath());

        if (!displayName.isEmpty())
            return displayName;
    } else if (deviceListener->isDeviceFolder(toLocalFile())) {
        const UDiskDeviceInfoPointer &deviceInfo = deviceListener->getDeviceByPath(filePath());

        if (deviceInfo && !deviceInfo->fileDisplayName().isEmpty())
            return deviceInfo->fileDisplayName();
    }

    return fileName();
}

void DFileInfo::refresh()
{
    Q_D(DFileInfo);

    d->fileInfo.refresh();
    d->icon = QIcon();
    d->epInitialized = false;
}

DUrl DFileInfo::goToUrlWhenDeleted() const
{
    if (deviceListener->isInDeviceFolder(absoluteFilePath()))
        return DUrl::fromLocalFile(QDir::homePath());

    return DAbstractFileInfo::goToUrlWhenDeleted();
}

void DFileInfo::makeToActive()
{
    Q_D(DFileInfo);

    d->fileInfo.refresh();
    DAbstractFileInfo::makeToActive();
}

void DFileInfo::makeToInactive()
{
    Q_D(DFileInfo);

    DAbstractFileInfo::makeToInactive();

    if (d->getIconTimer) {
        d->getIconTimer->stop();
        d->getIconTimer->deleteLater();
    } else if (d->requestingThumbnail) {
        d->requestingThumbnail = false;
        DThumbnailProvider::instance()->removeInProduceQueue(d->fileInfo, DThumbnailProvider::Large);
    }

    if (d->getEPTimer) {
        d->getEPTimer->stop();
        d->getEPTimer->deleteLater();
        d->requestEP = nullptr;
        d->epInitialized = false;
    }
}

QIcon DFileInfo::fileIcon() const
{
    Q_D(const DFileInfo);

    if (!d->icon.isNull() && (!d->iconFromTheme || !d->icon.name().isEmpty())) {
        return d->icon;
    }

    d->iconFromTheme = false;

    const DUrl &fileUrl = this->fileUrl();

#ifdef DFM_MINIMUM
    bool has_thumbnail = false;
#else
    bool has_thumbnail = FileUtils::isGvfsMountFile(absoluteFilePath()) || DThumbnailProvider::instance()->hasThumbnail(d->fileInfo);
#endif
    if (has_thumbnail) {
        const QIcon icon(DThumbnailProvider::instance()->thumbnailFilePath(d->fileInfo, DThumbnailProvider::Large));

        if (!icon.isNull()) {
            QPixmap pixmap = icon.pixmap(DThumbnailProvider::Large, DThumbnailProvider::Large);
            QPainter pa(&pixmap);

            pa.setPen(Qt::gray);
            pa.drawRect(pixmap.rect().adjusted(0, 0, -1, -1));
            d->icon.addPixmap(pixmap);
            d->iconFromTheme = false;

            return d->icon;
        }

        if (d->getIconTimer) {
            QMetaObject::invokeMethod(d->getIconTimer, "start", Qt::QueuedConnection);
        } else {
            QTimer *timer = new QTimer();
            const QExplicitlySharedDataPointer<DFileInfo> me(const_cast<DFileInfo*>(this));

            d->getIconTimer = timer;
            timer->setSingleShot(true);
            timer->moveToThread(qApp->thread());
            timer->setInterval(REQUEST_THUMBNAIL_DEALY);

            QObject::connect(timer, &QTimer::timeout, timer, [fileUrl, timer, me] {
                DThumbnailProvider::instance()->appendToProduceQueue(me->d_func()->fileInfo, DThumbnailProvider::Large,
                                                                     [me] (const QString &path) {
                    if (path.isEmpty()) {
                        me->d_func()->iconFromTheme = true;
                    } else {
                        // clean old icon
                        me->d_func()->icon = QIcon();
                    }
                });
                me->d_func()->requestingThumbnail = true;
                timer->deleteLater();
            });

            QMetaObject::invokeMethod(timer, "start", Qt::QueuedConnection);
        }

        d->icon = DFileIconProvider::globalProvider()->icon(*this);

        return d->icon;
    }

    if (isSymLink()) {
        const DUrl &symLinkTarget = this->symLinkTarget();

        if (symLinkTarget != fileUrl) {
            const DAbstractFileInfoPointer &fileInfo = DFileService::instance()->createFileInfo(Q_NULLPTR, symLinkTarget);

            if (fileInfo){
                d->icon = fileInfo->fileIcon();
                d->iconFromTheme = false;

                return d->icon;
            }
        }
    }

    d->icon = DFileIconProvider::globalProvider()->icon(*this);
    d->iconFromTheme = true;

    return d->icon;
}

QString DFileInfo::iconName() const
{
    if (systemPathManager->isSystemPath(absoluteFilePath()))
        return systemPathManager->getSystemPathIconNameByPath(absoluteFilePath());

    return DAbstractFileInfo::iconName();
}

QFileInfo DFileInfo::toQFileInfo() const
{
    Q_D(const DFileInfo);

    return d->fileInfo;
}

QIODevice *DFileInfo::createIODevice() const
{
    return new QFile(absoluteFilePath());
}

QVariantHash DFileInfo::extensionPropertys() const
{
    Q_D(const DFileInfo);

    // ensure extension propertys
    if (!d->epInitialized) {
        d->epInitialized = true;

        const DUrl &url = fileUrl();

        if (!d->getEPTimer) {
            d->getEPTimer = new QTimer();
            d->getEPTimer->setSingleShot(true);
            d->getEPTimer->moveToThread(qApp->thread());
            d->getEPTimer->setInterval(REQUEST_THUMBNAIL_DEALY);
        }

        QObject::connect(d->getEPTimer, &QTimer::timeout, d->getEPTimer, [d, url, this] {
            d->requestEP = RequestEP::instance();
            d->requestEP->requestEP(url, const_cast<DFileInfoPrivate*>(d));
            d->getEPTimer->deleteLater();
        });

        QMetaObject::invokeMethod(d->getEPTimer, "start", Qt::QueuedConnection);
    }

    return d->extensionPropertys;
}

QString DFileInfo::suffix() const
{
    Q_D(const DFileInfo);

    if (d->fileInfo.isDir())
        return QString();

    return d->fileInfo.suffix();
}

QString DFileInfo::completeSuffix() const
{
    Q_D(const DFileInfo);

    if (d->fileInfo.isDir())
        return QString();

    return d->fileInfo.completeSuffix();
}

DFileInfo::DFileInfo(DFileInfoPrivate &dd)
    : DAbstractFileInfo(dd)
{

}

#include "dfileinfo.moc"
