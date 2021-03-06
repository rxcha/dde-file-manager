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

#include "fmstatemanager.h"

#include "qobjecthelper.h"

#include "app/define.h"
#include "app/filesignalmanager.h"
#include "shutil/fileutils.h"

#include <QJsonParseError>
#include <QJsonObject>
#include <QFile>
#include <QDir>

QMap<DUrl, QPair<int, int>> FMStateManager::SortStates;


FMStateManager::FMStateManager(QObject *parent)
    : QObject(parent)
    , BaseManager()
{
    m_fmState = new FMState(this);
    initConnect();
}

FMStateManager::~FMStateManager()
{

}

void FMStateManager::initConnect()
{

}

QString FMStateManager::cacheFilePath()
{
//    return QString("%1/%2").arg(StandardPath::getCachePath(), "FMState.json");
    return getConfigPath("fmstate");
}

QString FMStateManager::sortCacheFilePath()
{
//    return QString("%1/%2").arg(StandardPath::getCachePath(), "sort.json");
    return getConfigPath("sort");
}

void FMStateManager::loadCache()
{
    //Migration for old config files, and rmove that codes for further
    FileUtils::migrateConfigFileFromCache("FMState");

    QString cache = readCacheFromFile(cacheFilePath());
    if (!cache.isEmpty()){
        QObjectHelper::json2qobject(cache, m_fmState);
    }
    loadSortCache();
}


void FMStateManager::saveCache()
{
    QString content = QObjectHelper::qobject2json(m_fmState);
    writeCacheToFile(cacheFilePath(), content);
}

void FMStateManager::loadSortCache()
{
    //Migration for old config files, and rmove that codes for further
    FileUtils::migrateConfigFileFromCache("sort");

    QString cache = readCacheFromFile(sortCacheFilePath());
    if (!cache.isEmpty()){
        QJsonParseError error;
        QJsonDocument doc=QJsonDocument::fromJson(cache.toLocal8Bit(),&error);
        if (error.error == QJsonParseError::NoError){
            QJsonObject obj = doc.object();
            foreach (QString key, obj.keys()) {
                const QStringList &list = obj.value(key).toString().split(",");

                if (list.count() == 2)
                    FMStateManager::SortStates.insert(DUrl(key), qMakePair(list.first().toInt(), list.last().toInt()));
            }
        }else{
            qDebug() << "load cache file: " << sortCacheFilePath() << error.errorString();
        }
    }

    qDebug() << FMStateManager::SortStates;
}

void FMStateManager::saveSortCache()
{
    QVariantMap sortCache;
    foreach (const DUrl& url, FMStateManager::SortStates.keys()) {
        const QPair<int, int> &sort = FMStateManager::SortStates.value(url);

        sortCache.insert(url.toString(), QString("%1,%2").arg(sort.first).arg(sort.second));
    }

    QJsonDocument doc(QJsonObject::fromVariantMap(sortCache));
    writeCacheToFile(sortCacheFilePath(), doc.toJson());

}

void FMStateManager::cacheSortState(const DUrl &url, int role, Qt::SortOrder order)
{
    FMStateManager::SortStates.insert(url, QPair<int, int>(role, order));
    FMStateManager::saveSortCache();
}


FMState *FMStateManager::fmState() const
{
    return m_fmState;
}

void FMStateManager::setFmState(FMState *fmState)
{
    m_fmState = fmState;
}

