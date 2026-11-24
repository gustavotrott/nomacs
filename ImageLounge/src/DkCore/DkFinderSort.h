/*******************************************************************************************************
 DkFinderSort.h
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

#include <QString>

#include <optional>

namespace nmc
{

/**
 * Sort order of a folder as configured in the macOS Finder.
 * sortMode is a DkSettings::sortMode, sortDir a DkSettings::sortDir
 **/
struct DkFinderSortOrder {
    int sortMode;
    int sortDir;
};

/**
 * Reads the sort order the Finder uses for the folder dirPath.
 *
 * The per-folder view settings are read from the .DS_Store of the parent folder (or of the folder itself),
 * falling back to Finder's default view settings. Returns nothing if the Finder sort order
 * cannot be represented in nomacs (e.g. sorted by kind) or if not running on macOS.
 **/
std::optional<DkFinderSortOrder> finderSortOrder(const QString &dirPath);

}
