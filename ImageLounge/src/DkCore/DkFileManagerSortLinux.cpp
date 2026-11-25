/*******************************************************************************************************
 DkFileManagerSortLinux.cpp
 Created on:	25.11.2026

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

#if defined(Q_OS_UNIX) && !defined(Q_OS_MACOS)

#include <QDebug>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QHash>
#include <QStandardPaths>
#include <QXmlStreamReader>

#include <optional>

#ifdef Q_OS_LINUX
#include <sys/xattr.h>
#endif

#ifdef WITH_GIO
// glib uses "signals" as an identifier
#pragma push_macro("signals")
#undef signals
#include <gio/gio.h>
#pragma pop_macro("signals")
#endif

namespace nmc
{

namespace
{

using Order = DkFileManagerSortOrder;

enum class FileManager {
    unknown,
    nautilus,
    nemo,
    caja,
    thunar,
    dolphin,
    pcmanfm_qt,
};

QString displayName(FileManager fm)
{
    switch (fm) {
    case FileManager::nautilus:
        return QStringLiteral("Files");
    case FileManager::nemo:
        return QStringLiteral("Nemo");
    case FileManager::caja:
        return QStringLiteral("Caja");
    case FileManager::thunar:
        return QStringLiteral("Thunar");
    case FileManager::dolphin:
        return QStringLiteral("Dolphin");
    case FileManager::pcmanfm_qt:
        return QStringLiteral("PCManFM-Qt");
    default:
        return {};
    }
}

FileManager fromName(const QString &name)
{
    const QString n = name.toLower();
    if (n.contains("nautilus"))
        return FileManager::nautilus;
    if (n.contains("nemo"))
        return FileManager::nemo;
    if (n.contains("caja"))
        return FileManager::caja;
    if (n.contains("thunar"))
        return FileManager::thunar;
    if (n.contains("dolphin"))
        return FileManager::dolphin;
    if (n.contains("pcmanfm-qt"))
        return FileManager::pcmanfm_qt;
    return FileManager::unknown;
}

FileManager fromDesktop(const QString &desktops)
{
    for (const QString &d : desktops.toUpper().split(':', Qt::SkipEmptyParts)) {
        if (d == "GNOME" || d == "UNITY" || d == "BUDGIE" || d == "PANTHEON")
            return FileManager::nautilus;
        if (d == "KDE")
            return FileManager::dolphin;
        if (d == "XFCE")
            return FileManager::thunar;
        if (d == "X-CINNAMON" || d == "CINNAMON")
            return FileManager::nemo;
        if (d == "MATE")
            return FileManager::caja;
        if (d == "LXQT")
            return FileManager::pcmanfm_qt;
    }
    return FileManager::unknown;
}

/**
 * The file manager the user browses folders with: the default handler of folders,
 * else the file manager of the desktop environment.
 **/
FileManager detectFileManager()
{
#ifdef WITH_GIO
    if (GAppInfo *app = g_app_info_get_default_for_type("inode/directory", FALSE)) {
        const FileManager fm = fromName(QString::fromUtf8(g_app_info_get_id(app)));
        g_object_unref(app);
        if (fm != FileManager::unknown)
            return fm;
    }
#endif
    return fromDesktop(qEnvironmentVariable("XDG_CURRENT_DESKTOP"));
}

/**
 * Maps the sort key of a file manager to a nomacs sort mode
 **/
std::optional<int> toSortMode(QString key)
{
    key = key.toLower();
    key.remove("thunar_column_");

    if (key == "name" || key == "text")
        return DkSettings::sort_filename;
    if (key == "date_modified" || key == "modification date" || key == "mtime" || key == "date_modified_with_time"
        || key == "modificationtime" || key == "date")
        return DkSettings::sort_date_modified;
    if (key == "date_created" || key == "creation date" || key == "btime" || key == "date_created_with_time"
        || key == "creationtime" || key == "crtime")
        return DkSettings::sort_date_created;
    if (key == "size" || key == "size_in_bytes")
        return DkSettings::sort_file_size;

    return {};
}

/**
 * A readable name for a sort key we do not support, e.g. THUNAR_COLUMN_MIME_TYPE -> "mime type"
 **/
QString keyDisplayName(QString key)
{
    key.remove("THUNAR_COLUMN_");
    return key.toLower().replace('_', ' ');
}

Order makeOrder(FileManager fm, const QString &key, bool ascending)
{
    if (const auto mode = toSortMode(key))
        return Order::make(displayName(fm), key, *mode, ascending);
    return Order::makeUnsupported(displayName(fm), keyDisplayName(key));
}

bool toBool(const QString &val, bool defaultValue = false)
{
    if (val.isEmpty())
        return defaultValue;
    return val == "true" || val == "1";
}

bool isDownloadsFolder(const QString &dir)
{
    const QString downloads = QStandardPaths::writableLocation(QStandardPaths::DownloadLocation);
    return !downloads.isEmpty() && QDir(dir) == QDir(downloads) && QDir(downloads) != QDir::home();
}

// --------------------------------------------------------------------------------------- INI files

using IniGroups = QHash<QString, QHash<QString, QString>>;

/**
 * Minimal parser for INI/KConfig/GKeyFile text: [group] followed by key=value lines.
 * QSettings is not usable because group names can be paths.
 **/
IniGroups parseIni(const QByteArray &data)
{
    IniGroups groups;
    QString group;

    for (const QByteArray &rawLine : data.split('\n')) {
        const QString line = QString::fromUtf8(rawLine).trimmed();
        if (line.isEmpty() || line.startsWith('#') || line.startsWith(';'))
            continue;

        if (line.startsWith('[') && line.endsWith(']')) {
            group = line.mid(1, line.size() - 2);
            continue;
        }

        const qsizetype eq = line.indexOf('=');
        if (eq > 0)
            groups[group].insert(line.left(eq).trimmed(), line.mid(eq + 1).trimmed());
    }

    return groups;
}

IniGroups readIni(const QString &filePath)
{
    QFile file(filePath);
    if (!file.open(QIODevice::ReadOnly) || file.size() > 4 * 1024 * 1024)
        return {};
    return parseIni(file.readAll());
}

QByteArray readXattr(const QString &path, const char *name)
{
#ifdef Q_OS_LINUX
    const QByteArray p = QFile::encodeName(path);
    const ssize_t size = getxattr(p.constData(), name, nullptr, 0);
    if (size <= 0 || size > 4 * 1024 * 1024)
        return {};

    QByteArray value(size, Qt::Uninitialized);
    const ssize_t read = getxattr(p.constData(), name, value.data(), value.size());
    if (read < 0)
        return {};
    value.resize(read);
    return value;
#else
    Q_UNUSED(path)
    Q_UNUSED(name)
    return {};
#endif
}

QString configPath(const QString &relativePath)
{
    return QDir(QStandardPaths::writableLocation(QStandardPaths::GenericConfigLocation)).filePath(relativePath);
}

// --------------------------------------------------------------------------------------- GIO

using Metadata = QHash<QString, QString>;

/**
 * GVfs metadata of a folder (attributes "metadata::*" without the prefix)
 **/
Metadata gvfsMetadata(const QString &dir)
{
    Metadata md;
#ifdef WITH_GIO
    GFile *file = g_file_new_for_path(QFile::encodeName(dir).constData());
    GError *error = nullptr;
    GFileInfo *info = g_file_query_info(file, "metadata::*", G_FILE_QUERY_INFO_NONE, nullptr, &error);

    if (info) {
        if (char **attributes = g_file_info_list_attributes(info, "metadata")) {
            for (char **a = attributes; *a; a++) {
                char *value = g_file_info_get_attribute_as_string(info, *a);
                md.insert(QString::fromUtf8(*a).remove("metadata::"), QString::fromUtf8(value));
                g_free(value);
            }
            g_strfreev(attributes);
        }
        g_object_unref(info);
    } else if (error) {
        qDebug() << "[DkFileManagerSort] no GVfs metadata for" << dir << error->message;
        g_error_free(error);
    }

    g_object_unref(file);
#else
    Q_UNUSED(dir)
#endif
    return md;
}

/**
 * Reads a string (or enum) GSettings key; nothing if the schema or key is not installed
 **/
std::optional<QString> gsettingsString(const char *schemaId, const char *key)
{
#ifdef WITH_GIO
    GSettingsSchemaSource *source = g_settings_schema_source_get_default();
    GSettingsSchema *schema = source ? g_settings_schema_source_lookup(source, schemaId, TRUE) : nullptr;
    if (!schema)
        return {};

    std::optional<QString> result;
    if (g_settings_schema_has_key(schema, key)) {
        GSettings *settings = g_settings_new_full(schema, nullptr, nullptr);
        GVariant *value = g_settings_get_value(settings, key);

        if (g_variant_is_of_type(value, G_VARIANT_TYPE_STRING))
            result = QString::fromUtf8(g_variant_get_string(value, nullptr));
        else if (g_variant_is_of_type(value, G_VARIANT_TYPE_BOOLEAN))
            result = g_variant_get_boolean(value) ? QStringLiteral("true") : QStringLiteral("false");

        g_variant_unref(value);
        g_object_unref(settings);
    }
    g_settings_schema_unref(schema);
    return result;
#else
    Q_UNUSED(schemaId)
    Q_UNUSED(key)
    return {};
#endif
}

// --------------------------------------------------------------------------------------- file managers

/**
 * GNOME Files (Nautilus). Since version 43 both views share the "icon-view" keys,
 * older versions stored the list view sort separately.
 **/
Order nautilusSortOrder(const QString &dir)
{
    const char *schema = "org.gnome.nautilus.preferences";
    const Metadata md = gvfsMetadata(dir);

    QString key = md.value("nautilus-icon-view-sort-by");
    QString reversed = md.value("nautilus-icon-view-sort-reversed");
    if (key.isEmpty()) {
        key = md.value("nautilus-list-view-sort-column");
        reversed = md.value("nautilus-list-view-sort-reversed");
    }

    // the Downloads folder shows the newest files first unless the user changed it
    const bool downloads = isDownloadsFolder(dir);

    if (key.isEmpty())
        key = downloads ? QStringLiteral("date_modified")
                        : gsettingsString(schema, "default-sort-order").value_or(QStringLiteral("name"));
    if (reversed.isEmpty())
        reversed = downloads
            ? QStringLiteral("true")
            : gsettingsString(schema, "default-sort-in-reverse-order").value_or(QStringLiteral("false"));

    return makeOrder(FileManager::nautilus, key, !toBool(reversed));
}

/**
 * Nemo and Caja (forks of Nautilus 2.x): separate sort keys for icon and list view
 **/
Order nemoCajaSortOrder(FileManager fm, const QString &dir)
{
    const QString prefix = fm == FileManager::nemo ? "nemo" : "caja";
    const QByteArray schema = fm == FileManager::nemo ? "org.nemo.preferences" : "org.mate.caja.preferences";

    Metadata md;
    if (!toBool(gsettingsString(schema.constData(), "ignore-view-metadata").value_or(QString())))
        md = gvfsMetadata(dir);

    // e.g. "OAFIID:Nemo_File_Manager_List_View"; default from preferences: "icon-view", "list-view", "compact-view"
    QString view = md.value(prefix + "-default-view");
    if (view.isEmpty())
        view = gsettingsString(schema.constData(), "default-folder-viewer").value_or(QString());
    const bool listView = view.contains("list", Qt::CaseInsensitive);

    QString key = listView ? md.value(prefix + "-list-view-sort-column") : md.value(prefix + "-icon-view-sort-by");
    QString reversed = listView ? md.value(prefix + "-list-view-sort-reversed")
                                : md.value(prefix + "-icon-view-sort-reversed");

    if (key.isEmpty())
        key = gsettingsString(schema.constData(), "default-sort-order").value_or(QStringLiteral("name"));
    if (reversed.isEmpty())
        reversed = gsettingsString(schema.constData(), "default-sort-in-reverse-order")
                       .value_or(QStringLiteral("false"));

    return makeOrder(fm, key, !toBool(reversed));
}

/**
 * Thunar: per folder (GVfs metadata) only if "Remember view settings for each folder" is enabled,
 * else the last used sort order (xfconf channel "thunar")
 **/
Order thunarSortOrder(const QString &dir)
{
    // read the xfconf channel from its file
    QHash<QString, QString> xfconf;
    QFile file(configPath("xfce4/xfconf/xfce-perchannel-xml/thunar.xml"));
    if (file.open(QIODevice::ReadOnly)) {
        QXmlStreamReader xml(&file);
        while (!xml.atEnd() && !xml.hasError()) {
            if (xml.readNext() == QXmlStreamReader::StartElement && xml.name() == QLatin1String("property")) {
                const auto attrs = xml.attributes();
                xfconf.insert(attrs.value("name").toString(), attrs.value("value").toString());
            }
        }
    }

    QString key;
    QString order;

    if (toBool(xfconf.value("misc-directory-specific-settings"))) {
        const Metadata md = gvfsMetadata(dir);
        key = md.value("thunar-sort-column");
        order = md.value("thunar-sort-order");
    }

    if (key.isEmpty())
        key = xfconf.value("last-sort-column", QStringLiteral("THUNAR_COLUMN_NAME"));
    if (order.isEmpty())
        order = xfconf.value("last-sort-order", QStringLiteral("GTK_SORT_ASCENDING"));

    return makeOrder(FileManager::thunar, key, order != "GTK_SORT_DESCENDING");
}

/**
 * Dolphin view properties: a [Dolphin] group in a .directory file, or since Dolphin 24.12 the same text
 * in the extended attribute "user.kde.fm.viewproperties#1"
 **/
std::optional<Order> dolphinPropsOrder(const QString &propsDir)
{
    IniGroups props = readIni(QDir(propsDir).filePath(".directory"));
    if (!props.contains("Dolphin"))
        props = parseIni(readXattr(propsDir, "user.kde.fm.viewproperties#1"));
    if (!props.contains("Dolphin"))
        return {};

    const QHash<QString, QString> dolphin = props.value("Dolphin");
    const int version = dolphin.value("Version", "0").toInt();
    QString role = dolphin.value("SortRole", QStringLiteral("text"));

    // legacy names
    if (version < 2 && role == "name")
        role = QStringLiteral("text");
    if (version < 4 && role == "date")
        role = QStringLiteral("modificationtime");

    const bool ascending = dolphin.value("SortOrder", "0").toInt() == Qt::AscendingOrder;
    return makeOrder(FileManager::dolphin, role, ascending);
}

Order dolphinSortOrder(const QString &dir)
{
    const IniGroups dolphinrc = readIni(configPath("dolphinrc"));
    const bool globalViewProps = toBool(dolphinrc.value("General").value("GlobalViewProps"), true);

    const QString viewProps = QDir(QStandardPaths::writableLocation(QStandardPaths::GenericDataLocation))
                                  .filePath("dolphin/view_properties");

    std::optional<Order> order;

    if (globalViewProps) {
        order = dolphinPropsOrder(QDir(viewProps).filePath("global"));
    } else {
        // in the folder itself, or for folders that cannot store it (not writable, outside home)
        // under view_properties/local/<absolute path>
        order = dolphinPropsOrder(dir);
        if (!order)
            order = dolphinPropsOrder(QDir(viewProps).filePath("local") + QDir(dir).absolutePath());
    }

    if (order)
        return *order;

    // defaults: the Downloads folder is sorted by date (newest first), others by name
    if (isDownloadsFolder(dir))
        return makeOrder(FileManager::dolphin, "modificationtime", false);
    return makeOrder(FileManager::dolphin, "text", true);
}

/**
 * PCManFM-Qt: folder settings are stored if "Preserve settings for this folder" is checked,
 * in .directory or dir-settings.conf (possibly inherited from a parent with Recursive=true)
 **/
Order pcmanfmQtSortOrder(const QString &dir)
{
    const QString profileDir = configPath("pcmanfm-qt/default");

    auto toOrder = [](const QHash<QString, QString> &group) -> std::optional<Order> {
        const QString column = group.value("SortColumn");
        if (column.isEmpty())
            return {};
        return makeOrder(FileManager::pcmanfm_qt, column, group.value("SortOrder") != "descending");
    };

    const QHash<QString, QString> dotDirectory = readIni(QDir(dir).filePath(".directory")).value("File Manager");
    if (auto order = toOrder(dotDirectory))
        return *order;

    IniGroups dirSettings = readIni(QDir(profileDir).filePath("dir-settings.conf"));
    if (dirSettings.isEmpty())
        dirSettings = readIni(configPath("libfm/dir-settings.conf"));

    const QString absDir = QDir(dir).absolutePath();
    if (auto order = toOrder(dirSettings.value(absDir)))
        return *order;

    // inherited from a parent folder
    QDir parent(absDir);
    while (parent.cdUp()) {
        const auto group = dirSettings.value(parent.absolutePath());
        if (toBool(group.value("Recursive"))) {
            if (auto order = toOrder(group))
                return *order;
        }
        if (parent.isRoot())
            break;
    }

    const QHash<QString, QString> folderView = readIni(QDir(profileDir).filePath("settings.conf")).value("FolderView");
    if (auto order = toOrder(folderView))
        return *order;

    return makeOrder(FileManager::pcmanfm_qt, "name", true);
}

}

QString fileManagerName()
{
    // menus use a generic name since several file managers can be installed
    return {};
}

DkFileManagerSortOrder fileManagerSortOrder(const QString &dirPath)
{
    const QFileInfo dirInfo(dirPath);
    if (!dirInfo.isDir())
        return {};

    const QString dir = dirInfo.absoluteFilePath();
    const FileManager fm = detectFileManager();

    Order order;
    switch (fm) {
    case FileManager::nautilus:
        order = nautilusSortOrder(dir);
        break;
    case FileManager::nemo:
    case FileManager::caja:
        order = nemoCajaSortOrder(fm, dir);
        break;
    case FileManager::thunar:
        order = thunarSortOrder(dir);
        break;
    case FileManager::dolphin:
        order = dolphinSortOrder(dir);
        break;
    case FileManager::pcmanfm_qt:
        order = pcmanfmQtSortOrder(dir);
        break;
    default:
        break;
    }

    qInfo() << "[DkFileManagerSort]" << dir << "file manager:" << displayName(fm) << "sort key:" << order.sortKey
            << "status:" << order.status << "mode:" << order.sortMode << "dir:" << order.sortDir;

    return order;
}

}

#endif
