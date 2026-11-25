/*******************************************************************************************************
 DkFileManagerSortMac.cpp
 Created on:	24.11.2026

 nomacs is a fast and small image viewer with the capability of synchronizing multiple instances

 Copyright (C) 2011-2026 nomacs authors

 This file is part of nomacs.

 nomacs is free software: you can redistribute it and/or modify
 it under the terms of the GNU General Public License as published by
 the Free Software Foundation, either version 3 of the License, or
 (at your option) any later version.

 nomacs is distributed in the hope that it will be useful,
 but WITHOUT ANY WARRANTY; without even the implied warranty of
 MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 GNU General Public License for more details.

 You should have received a copy of the GNU General Public License
 along with this program.  If not, see <http://www.gnu.org/licenses/>.

 *******************************************************************************************************/

#include "DkFileManagerSort.h"

#ifdef Q_OS_MACOS

#include <QDebug>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QHash>
#include <QtEndian>

#include <CoreFoundation/CoreFoundation.h>

namespace nmc
{

namespace
{

// .DS_Store records we care about, of one file name
using DsRecords = QHash<QByteArray, QByteArray>; // record code -> blob/type data

/**
 * Minimal reader of the .DS_Store format (a B-tree stored in a "buddy allocator" file).
 * See https://metacpan.org/dist/Mac-Finder-DSStore/view/DSStoreFormat.pod
 **/
class DsStoreReader
{
public:
    explicit DsStoreReader(const QByteArray &file)
    {
        // the file starts with a 4 byte alignment header (0x00000001), all offsets are relative to it
        if (file.size() < 36 || qFromBigEndian<quint32>(file.constData()) != 1)
            return;

        mData = file.mid(4);
        if (mData.left(4) != "Bud1")
            return;

        quint32 rootOffset, rootSize;
        if (!readU32(4, rootOffset) || !readU32(8, rootSize))
            return;

        // root block: block addresses followed by the table of contents
        quint32 numBlocks;
        if (!readU32(rootOffset, numBlocks) || numBlocks > 1000000)
            return;

        qsizetype pos = rootOffset + 8;
        for (quint32 i = 0; i < numBlocks; i++) {
            quint32 addr;
            if (!readU32(pos + i * 4, addr))
                return;
            mBlocks << addr;
        }
        pos += ((numBlocks + 255) / 256) * 256 * 4;

        quint32 numToc;
        if (!readU32(pos, numToc))
            return;
        pos += 4;

        for (quint32 i = 0; i < numToc; i++) {
            if (pos >= mData.size())
                return;
            const int len = static_cast<quint8>(mData.at(pos));
            const QByteArray name = mData.mid(pos + 1, len);
            quint32 id;
            if (!readU32(pos + 1 + len, id))
                return;
            pos += 1 + len + 4;

            if (name == "DSDB")
                mDsdbBlock = static_cast<int>(id);
        }
    }

    /**
     * Returns all records of the file named fileName ("." is the folder holding the .DS_Store)
     **/
    DsRecords records(const QString &fileName)
    {
        DsRecords result;

        qsizetype offset, size;
        quint32 rootNode;
        if (mDsdbBlock < 0 || !block(mDsdbBlock, offset, size) || !readU32(offset, rootNode))
            return result;

        mWanted = fileName.normalized(QString::NormalizationForm_C);
        mResult = &result;
        readNode(rootNode, 0);
        mResult = nullptr;

        return result;
    }

private:
    QByteArray mData;
    QVector<quint32> mBlocks;
    int mDsdbBlock = -1;

    QString mWanted;
    DsRecords *mResult = nullptr;

    bool readU32(qsizetype pos, quint32 &val) const
    {
        if (pos < 0 || pos + 4 > mData.size())
            return false;
        val = qFromBigEndian<quint32>(mData.constData() + pos);
        return true;
    }

    bool block(int id, qsizetype &offset, qsizetype &size) const
    {
        if (id < 0 || id >= mBlocks.size())
            return false;
        const quint32 addr = mBlocks[id];
        offset = addr & ~0x1fu;
        size = qsizetype(1) << (addr & 0x1f);
        return offset + size <= mData.size();
    }

    bool readNode(quint32 id, int depth)
    {
        qsizetype offset, size;
        if (depth > 32 || !block(static_cast<int>(id), offset, size))
            return false;

        quint32 rightChild, count;
        if (!readU32(offset, rightChild) || !readU32(offset + 4, count))
            return false;

        qsizetype pos = offset + 8;
        for (quint32 i = 0; i < count; i++) {
            if (rightChild) { // internal node: every record is preceded by its left child
                quint32 child;
                if (!readU32(pos, child) || !readNode(child, depth + 1))
                    return false;
                pos += 4;
            }
            if (!readRecord(pos))
                return false;
        }

        if (rightChild)
            return readNode(rightChild, depth + 1);

        return true;
    }

    bool readRecord(qsizetype &pos)
    {
        quint32 nameLen;
        if (!readU32(pos, nameLen) || nameLen > 0xffff || pos + 4 + nameLen * 2 + 8 > mData.size())
            return false;
        pos += 4;

        QString name(nameLen, Qt::Uninitialized);
        for (quint32 i = 0; i < nameLen; i++)
            name[i] = QChar(qFromBigEndian<quint16>(mData.constData() + pos + i * 2));
        pos += nameLen * 2;

        const QByteArray code = mData.mid(pos, 4);
        const QByteArray type = mData.mid(pos + 4, 4);
        pos += 8;

        qsizetype dataPos = pos;
        qsizetype dataLen = 0;
        quint32 len = 0;

        if (type == "long" || type == "shor" || type == "type") {
            dataLen = 4;
        } else if (type == "bool") {
            dataLen = 1;
        } else if (type == "comp" || type == "dutc") {
            dataLen = 8;
        } else if (type == "blob") {
            if (!readU32(pos, len))
                return false;
            dataPos = pos + 4;
            dataLen = len;
            pos += 4;
        } else if (type == "ustr") {
            if (!readU32(pos, len))
                return false;
            dataPos = pos + 4;
            dataLen = qsizetype(len) * 2;
            pos += 4;
        } else {
            qWarning() << "[DkFileManagerSort] unknown .DS_Store record type" << type;
            return false;
        }

        if (dataPos + dataLen > mData.size())
            return false;
        pos += dataLen;

        if (name.normalized(QString::NormalizationForm_C).compare(mWanted, Qt::CaseInsensitive) == 0)
            mResult->insert(code, mData.mid(dataPos, dataLen));

        return true;
    }
};

// RAII for CoreFoundation objects
template<typename T>
class CfRef
{
public:
    explicit CfRef(T ref = nullptr)
        : mRef(ref)
    {
    }
    ~CfRef()
    {
        if (mRef)
            CFRelease(mRef);
    }
    CfRef(const CfRef &) = delete;
    CfRef &operator=(const CfRef &) = delete;

    T get() const
    {
        return mRef;
    }

private:
    T mRef;
};

CFStringRef cfString(const char *str)
{
    return CFStringCreateWithCString(kCFAllocatorDefault, str, kCFStringEncodingUTF8);
}

CFTypeRef dictValue(CFTypeRef dict, const char *key)
{
    if (!dict || CFGetTypeID(dict) != CFDictionaryGetTypeID())
        return nullptr;

    CfRef<CFStringRef> cfKey(cfString(key));
    return CFDictionaryGetValue(static_cast<CFDictionaryRef>(dict), cfKey.get());
}

QString toQString(CFTypeRef str)
{
    if (!str || CFGetTypeID(str) != CFStringGetTypeID())
        return {};
    return QString::fromCFString(static_cast<CFStringRef>(str));
}

std::optional<bool> toBool(CFTypeRef val)
{
    if (!val)
        return {};
    if (CFGetTypeID(val) == CFBooleanGetTypeID())
        return CFBooleanGetValue(static_cast<CFBooleanRef>(val));
    if (CFGetTypeID(val) == CFNumberGetTypeID()) {
        int i = 0;
        CFNumberGetValue(static_cast<CFNumberRef>(val), kCFNumberIntType, &i);
        return i != 0;
    }
    return {};
}

/**
 * Translates a Finder sort key ("arrangeBy" or "sortColumn") to nomacs.
 * The direction is the Finder default for that key unless given by ascending.
 **/
using Order = DkFileManagerSortOrder;
const QString finder = QStringLiteral("Finder");

Order toSortOrder(const QString &key, std::optional<bool> ascending = {})
{
    int mode;
    bool defaultAscending = false; // Finder shows newest/biggest first

    if (key == "name" || key == "none" || key == "grid") { // unsorted icon view is not arranged, name is closest
        mode = DkSettings::sort_filename;
        defaultAscending = true;
    } else if (key == "dateModified") {
        mode = DkSettings::sort_date_modified;
    } else if (key == "dateCreated") {
        mode = DkSettings::sort_date_created;
    } else if (key == "size") {
        mode = DkSettings::sort_file_size;
    } else {
        // kind, dateAdded, dateLastOpened, label, ... have no nomacs equivalent
        return Order::makeUnsupported(finder, key);
    }

    return Order::make(finder, key, mode, ascending.value_or(defaultAscending));
}

/**
 * Sort order of Finder list view settings. The column list is either an array (lsvC, Finder defaults)
 * or a dictionary keyed by column identifier (lsvp, lsvP).
 **/
Order listViewSortOrder(CFTypeRef settings)
{
    const QString column = toQString(dictValue(settings, "sortColumn"));
    if (column.isEmpty())
        return {};

    std::optional<bool> ascending;
    CFTypeRef columns = dictValue(settings, "columns");

    if (columns && CFGetTypeID(columns) == CFArrayGetTypeID()) {
        CFArrayRef arr = static_cast<CFArrayRef>(columns);
        for (CFIndex i = 0; i < CFArrayGetCount(arr); i++) {
            CFTypeRef col = CFArrayGetValueAtIndex(arr, i);
            if (toQString(dictValue(col, "identifier")) == column) {
                ascending = toBool(dictValue(col, "ascending"));
                break;
            }
        }
    } else if (columns) {
        ascending = toBool(dictValue(dictValue(columns, column.toUtf8().constData()), "ascending"));
    }

    return toSortOrder(column, ascending);
}

Order arrangeSortOrder(CFTypeRef settings)
{
    const QString key = toQString(dictValue(settings, "arrangeBy"));
    if (key.isEmpty())
        return {};
    return toSortOrder(key);
}

CFPropertyListRef parsePlist(const QByteArray &data)
{
    if (data.isEmpty())
        return nullptr;

    CfRef<CFDataRef> cfData(CFDataCreate(kCFAllocatorDefault, //
                                         reinterpret_cast<const UInt8 *>(data.constData()),
                                         data.size()));
    return CFPropertyListCreateWithData(kCFAllocatorDefault, cfData.get(), kCFPropertyListImmutable, nullptr, nullptr);
}

enum class ViewStyle {
    icon,
    list,
    column,
    gallery,
};

std::optional<ViewStyle> toViewStyle(const QByteArray &code)
{
    if (code == "icnv")
        return ViewStyle::icon;
    if (code == "Nlsv")
        return ViewStyle::list;
    if (code == "clmv")
        return ViewStyle::column;
    if (code == "glyv" || code == "Flwv")
        return ViewStyle::gallery;
    return {};
}

/**
 * Sort order from the records of a folder in a .DS_Store
 **/
Order recordSortOrder(const DsRecords &records, ViewStyle style)
{
    if (style == ViewStyle::list) {
        for (const char *code : {"lsvC", "lsvP", "lsvp"}) {
            CfRef<CFPropertyListRef> plist(parsePlist(records.value(code)));
            Order order = listViewSortOrder(plist.get());
            if (order.status != Order::not_found)
                return order;
        }
        return {};
    }

    const char *code = style == ViewStyle::gallery ? "glvp" : "icvp";
    CfRef<CFPropertyListRef> plist(parsePlist(records.value(code)));
    return arrangeSortOrder(plist.get());
}

/**
 * Sort order of Finder's default view settings (Finder > View > Show View Options > Use as Defaults)
 **/
Order defaultSortOrder(ViewStyle style)
{
    CfRef<CFStringRef> app(cfString("com.apple.finder"));
    CfRef<CFStringRef> key(cfString("StandardViewSettings"));
    CfRef<CFPropertyListRef> settings(CFPreferencesCopyAppValue(key.get(), app.get()));

    switch (style) {
    case ViewStyle::list:
        for (const char *name : {"ExtendedListViewSettingsV2", "ExtendedListViewSettings", "ListViewSettings"}) {
            Order order = listViewSortOrder(dictValue(settings.get(), name));
            if (order.status != Order::not_found)
                return order;
        }
        return {};
    case ViewStyle::gallery:
        return arrangeSortOrder(dictValue(settings.get(), "GalleryViewSettings"));
    default:
        return arrangeSortOrder(dictValue(settings.get(), "IconViewSettings"));
    }
}

ViewStyle defaultViewStyle()
{
    CfRef<CFStringRef> app(cfString("com.apple.finder"));
    CfRef<CFStringRef> key(cfString("FXPreferredViewStyle"));
    CfRef<CFPropertyListRef> style(CFPreferencesCopyAppValue(key.get(), app.get()));

    return toViewStyle(toQString(style.get()).toLatin1()).value_or(ViewStyle::icon);
}

DsRecords readRecords(const QString &dsStorePath, const QString &fileName)
{
    QFile file(dsStorePath);
    if (!file.open(QIODevice::ReadOnly))
        return {};

    // the file has a few KB normally, but guard against nonsense
    if (file.size() > 32 * 1024 * 1024)
        return {};

    return DsStoreReader(file.readAll()).records(fileName);
}

}

QString fileManagerName()
{
    return finder;
}

DkFileManagerSortOrder fileManagerSortOrder(const QString &dirPath)
{
    const QFileInfo dirInfo(dirPath);
    if (!dirInfo.isDir())
        return {};

    const QString dir = dirInfo.absoluteFilePath();
    const QString name = dirInfo.fileName();

    // the Finder stores the view settings of a folder in its parent's .DS_Store,
    // except where it cannot (e.g. home folder, volume roots) where it uses its own .DS_Store with name "."
    DsRecords records;
    if (!name.isEmpty())
        records = readRecords(QDir(dirInfo.absolutePath()).filePath(".DS_Store"), name);
    if (records.isEmpty())
        records = readRecords(QDir(dir).filePath(".DS_Store"), ".");

    const ViewStyle style = toViewStyle(records.value("vstl")).value_or(defaultViewStyle());

    Order order = recordSortOrder(records, style);
    if (order.status == Order::not_found)
        order = defaultSortOrder(style);

    qInfo() << "[DkFileManagerSort]" << dir << "Finder view style:" << int(style) << "sort key:" << order.sortKey
            << "status:" << order.status << "mode:" << order.sortMode << "dir:" << order.sortDir;

    return order;
}

}

#endif
