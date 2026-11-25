/*******************************************************************************************************
 DkFileManagerSort.h
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

#pragma once

#include "DkSettings.h"

#include <QString>

namespace nmc
{

/**
 * Sort order of a folder as configured in the platform's file manager
 * (macOS Finder, Windows Explorer, or a Linux file manager).
 **/
struct DkFileManagerSortOrder {
    enum Status {
        not_found, // no sort information available
        found, // sortMode and sortDir are valid
        unsupported, // the file manager sorts by sortKey which nomacs cannot do
    };

    Status status = not_found;
    int sortMode = DkSettings::sort_filename; // DkSettings::sortMode
    int sortDir = DkSettings::sort_ascending; // DkSettings::sortDir
    QString fileManager; // e.g. "Finder", "Explorer", "Nautilus"
    QString sortKey; // the file manager's name of the sort key, for messages

    static DkFileManagerSortOrder make(const QString &fileManager, const QString &sortKey, int sortMode, bool ascending)
    {
        DkFileManagerSortOrder o;
        o.status = found;
        o.fileManager = fileManager;
        o.sortKey = sortKey;
        o.sortMode = sortMode;
        o.sortDir = ascending ? DkSettings::sort_ascending : DkSettings::sort_descending;
        return o;
    }

    static DkFileManagerSortOrder makeUnsupported(const QString &fileManager, const QString &sortKey)
    {
        DkFileManagerSortOrder o;
        o.status = unsupported;
        o.fileManager = fileManager;
        o.sortKey = sortKey;
        return o;
    }
};

/**
 * Returns how the folder dirPath is sorted in the file manager.
 *
 * - macOS: Finder view settings (.DS_Store of the folder, or Finder's default view settings)
 * - Windows: Explorer, from an open Explorer window showing the folder, else from the saved folder view state
 * - Linux/BSD: the default file manager (Nautilus, Nemo, Caja, Thunar, Dolphin, PCManFM-Qt)
 *
 * Only sort keys nomacs supports can be mapped (name, date modified, date created, size); other keys
 * are reported as unsupported.
 **/
DkFileManagerSortOrder fileManagerSortOrder(const QString &dirPath);

/**
 * Name of the platform file manager for UI strings, e.g. "Finder"; empty if unknown.
 **/
QString fileManagerName();

}
