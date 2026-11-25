/*******************************************************************************************************
 DkFileManagerSortWin.cpp
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

#ifdef Q_OS_WIN

#include <QDebug>
#include <QDir>
#include <QFileInfo>

#include <optional>
#include <string>
#include <utility>
#include <vector>

#include <windows.h>

#include <exdisp.h>
#include <knownfolders.h>
#include <propsys.h>
#include <servprov.h>
#include <shlguid.h>
#include <shlobj.h>
#include <shobjidl.h>

namespace nmc
{

namespace
{

using Order = DkFileManagerSortOrder;
const QString explorer = QStringLiteral("Explorer");

// minimal COM smart pointer (MinGW lacks WRL)
template<typename T>
class ComPtr
{
public:
    ComPtr() = default;
    ~ComPtr()
    {
        if (mPtr)
            mPtr->Release();
    }
    ComPtr(const ComPtr &) = delete;
    ComPtr &operator=(const ComPtr &) = delete;

    T *operator->() const
    {
        return mPtr;
    }
    T *get() const
    {
        return mPtr;
    }
    T **put()
    {
        if (mPtr)
            mPtr->Release();
        mPtr = nullptr;
        return &mPtr;
    }
    void **putVoid()
    {
        return reinterpret_cast<void **>(put());
    }
    explicit operator bool() const
    {
        return mPtr != nullptr;
    }
    void swap(ComPtr &other)
    {
        std::swap(mPtr, other.mPtr);
    }

    template<typename U>
    bool queryInterface(ComPtr<U> &other) const
    {
        return mPtr && SUCCEEDED(mPtr->QueryInterface(__uuidof(U), other.putVoid()));
    }

private:
    T *mPtr = nullptr;
};

// initializes COM on this thread if needed
class ComScope
{
public:
    ComScope()
    {
        mHr = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    }
    ~ComScope()
    {
        if (SUCCEEDED(mHr)) // also S_FALSE (already initialized)
            CoUninitialize();
    }

private:
    HRESULT mHr;
};

// property keys of the Explorer columns nomacs can sort by, see propkey.h
// {B725F130-47EF-101A-A5F1-02608C9EEBAC}
constexpr GUID FMTID_Storage_ = {0xB725F130, 0x47EF, 0x101A, {0xA5, 0xF1, 0x02, 0x60, 0x8C, 0x9E, 0xEB, 0xAC}};
constexpr DWORD PID_ItemNameDisplay = 10;
constexpr DWORD PID_Size = 12;
constexpr DWORD PID_DateModified = 14;
constexpr DWORD PID_DateCreated = 15;

QString propertyDisplayName(const PROPERTYKEY &key)
{
    ComPtr<IPropertyDescription> desc;
    QString name;

    if (SUCCEEDED(PSGetPropertyDescription(key, __uuidof(IPropertyDescription), desc.putVoid()))) {
        LPWSTR label = nullptr;
        if (SUCCEEDED(desc->GetDisplayName(&label)) && label) {
            name = QString::fromWCharArray(label);
            CoTaskMemFree(label);
        }
    }

    if (name.isEmpty()) {
        wchar_t canonical[256] = {};
        if (SUCCEEDED(PSStringFromPropertyKey(key, canonical, ARRAYSIZE(canonical))))
            name = QString::fromWCharArray(canonical);
    }

    return name;
}

Order toOrder(const PROPERTYKEY &key, int direction)
{
    std::optional<int> mode;

    if (IsEqualGUID(key.fmtid, FMTID_Storage_)) {
        switch (key.pid) {
        case PID_ItemNameDisplay:
            mode = DkSettings::sort_filename;
            break;
        case PID_Size:
            mode = DkSettings::sort_file_size;
            break;
        case PID_DateModified:
            mode = DkSettings::sort_date_modified;
            break;
        case PID_DateCreated:
            mode = DkSettings::sort_date_created;
            break;
        default:
            break;
        }
    }

    const QString name = propertyDisplayName(key);
    if (!mode)
        return Order::makeUnsupported(explorer, name);

    // SORT_ASCENDING = 1, SORT_DESCENDING = -1
    return Order::make(explorer, name, *mode, direction != SORT_DESCENDING);
}

bool samePath(const QString &a, const QString &b)
{
    return QDir::cleanPath(QDir::fromNativeSeparators(a))
               .compare(QDir::cleanPath(QDir::fromNativeSeparators(b)), Qt::CaseInsensitive)
        == 0;
}

QString pidlPath(PCIDLIST_ABSOLUTE pidl)
{
    wchar_t path[MAX_PATH * 4] = {};
    if (!pidl || !SHGetPathFromIDListEx(pidl, path, ARRAYSIZE(path), GPFIDL_DEFAULT))
        return {};
    return QString::fromWCharArray(path);
}

/**
 * Sort order of an Explorer window (or tab) that currently shows dir.
 * This is the usual case: the user double-clicked an image in Explorer.
 **/
std::optional<Order> openWindowOrder(const QString &dir)
{
    ComPtr<IShellWindows> windows;
    if (FAILED(CoCreateInstance(CLSID_ShellWindows, nullptr, CLSCTX_ALL, __uuidof(IShellWindows), windows.putVoid())))
        return {};

    long count = 0;
    if (FAILED(windows->get_Count(&count)))
        return {};

    std::optional<Order> result;

    for (long i = 0; i < count; i++) {
        VARIANT index;
        VariantInit(&index);
        V_VT(&index) = VT_I4;
        V_I4(&index) = i;

        ComPtr<IDispatch> disp;
        if (FAILED(windows->Item(index, disp.put())) || !disp)
            continue;

        ComPtr<IServiceProvider> provider;
        ComPtr<IShellBrowser> browser;
        ComPtr<IShellView> view;
        ComPtr<IFolderView2> folderView;
        ComPtr<IPersistFolder2> folder;

        if (!disp.queryInterface(provider)
            || FAILED(provider->QueryService(SID_STopLevelBrowser, __uuidof(IShellBrowser), browser.putVoid()))
            || FAILED(browser->QueryActiveShellView(view.put())) || !view.queryInterface(folderView)
            || FAILED(folderView->GetFolder(__uuidof(IPersistFolder2), folder.putVoid())))
            continue;

        PIDLIST_ABSOLUTE pidl = nullptr;
        if (FAILED(folder->GetCurFolder(&pidl)))
            continue;
        const QString path = pidlPath(pidl);
        CoTaskMemFree(pidl);

        if (path.isEmpty() || !samePath(path, dir))
            continue;

        int numColumns = 0;
        if (FAILED(folderView->GetSortColumnCount(&numColumns)) || numColumns < 1)
            continue;

        std::vector<SORTCOLUMN> columns(numColumns);
        if (FAILED(folderView->GetSortColumns(columns.data(), numColumns)))
            continue;

        const Order order = toOrder(columns[0].propkey, columns[0].direction);

        // with File Explorer tabs every tab is a shell window, prefer the visible one
        HWND hwnd = nullptr;
        const bool visible = SUCCEEDED(browser->GetWindow(&hwnd)) && IsWindowVisible(hwnd);

        qInfo() << "[DkFileManagerSort] Explorer window" << path << "sorted by" << order.sortKey
                << (columns[0].direction == SORT_DESCENDING ? "descending" : "ascending") << "visible:" << visible;

        if (!result || visible)
            result = order;
        if (visible)
            break;
    }

    return result;
}

// ------------------------------------------------------------------------------------- saved view state

const wchar_t *shellKey = L"Software\\Classes\\Local Settings\\Software\\Microsoft\\Windows\\Shell";

class RegKey
{
public:
    RegKey() = default;
    RegKey(HKEY parent, const std::wstring &path)
    {
        if (RegOpenKeyExW(parent, path.c_str(), 0, KEY_READ, &mKey) != ERROR_SUCCESS)
            mKey = nullptr;
    }
    ~RegKey()
    {
        if (mKey)
            RegCloseKey(mKey);
    }
    RegKey(const RegKey &) = delete;
    RegKey &operator=(const RegKey &) = delete;
    RegKey(RegKey &&other) noexcept
        : mKey(other.mKey)
    {
        other.mKey = nullptr;
    }
    RegKey &operator=(RegKey &&other) noexcept
    {
        std::swap(mKey, other.mKey);
        return *this;
    }

    HKEY get() const
    {
        return mKey;
    }
    explicit operator bool() const
    {
        return mKey != nullptr;
    }

private:
    HKEY mKey = nullptr;
};

/**
 * Finds the BagMRU subkey of one item of an ID list. BagMRU is a tree mirroring the shell namespace:
 * values "0", "1", ... hold the item ID of each child (subkey with the same name).
 **/
std::optional<std::wstring> findBagMruChild(HKEY key, IShellFolder *parent, PCUITEMID_CHILD child)
{
    for (DWORD i = 0;; i++) {
        wchar_t name[64];
        DWORD nameLen = ARRAYSIZE(name);
        DWORD type = 0;
        DWORD dataLen = 0;

        const LONG res = RegEnumValueW(key, i, name, &nameLen, nullptr, &type, nullptr, &dataLen);
        if (res == ERROR_NO_MORE_ITEMS)
            break;
        if (res != ERROR_SUCCESS || type != REG_BINARY || dataLen < sizeof(USHORT) || name[0] < L'0' || name[0] > L'9')
            continue;

        // the stored item ID, null terminated
        std::vector<BYTE> data(dataLen + sizeof(USHORT), 0);
        nameLen = ARRAYSIZE(name);
        if (RegEnumValueW(key, i, name, &nameLen, nullptr, &type, data.data(), &dataLen) != ERROR_SUCCESS)
            continue;

        const auto *stored = reinterpret_cast<PCUITEMID_CHILD>(data.data());
        if (stored->mkid.cb > dataLen)
            continue;

        const HRESULT hr = parent->CompareIDs(0, stored, child);
        if (SUCCEEDED(hr) && static_cast<short>(HRESULT_CODE(hr)) == 0)
            return std::wstring(name);
    }

    return {};
}

/**
 * The NodeSlot of a folder in BagMRU, which is the number of its view state key in Bags
 **/
std::optional<DWORD> bagNodeSlot(PCIDLIST_ABSOLUTE pidl)
{
    ComPtr<IShellFolder> folder;
    if (FAILED(SHGetDesktopFolder(folder.put())))
        return {};

    std::wstring path = std::wstring(shellKey) + L"\\BagMRU";
    RegKey key(HKEY_CURRENT_USER, path);

    for (PCUIDLIST_RELATIVE item = pidl; key && !ILIsEmpty(item); item = ILNext(item)) {
        PITEMID_CHILD child = ILCloneFirst(item);
        if (!child)
            return {};

        const auto name = findBagMruChild(key.get(), folder.get(), child);

        ComPtr<IShellFolder> childFolder;
        const bool bound = name
            && SUCCEEDED(folder->BindToObject(child, nullptr, __uuidof(IShellFolder), childFolder.putVoid()));
        ILFree(child);

        if (!bound)
            return {};

        path += L"\\" + *name;
        key = RegKey(HKEY_CURRENT_USER, path);
        folder.swap(childFolder);
    }

    if (!key)
        return {};

    DWORD slot = 0;
    DWORD size = sizeof(slot);
    DWORD type = 0;
    if (RegQueryValueExW(key.get(), L"NodeSlot", nullptr, &type, reinterpret_cast<BYTE *>(&slot), &size)
            != ERROR_SUCCESS
        || type != REG_DWORD)
        return {};

    return slot;
}

/**
 * Parses a "Sort" value of a saved folder view:
 * 16 byte header, DWORD count, then per column: FMTID (16 bytes), DWORD pid, DWORD direction (1 or -1)
 **/
std::optional<Order> parseSortValue(const std::vector<BYTE> &data)
{
    constexpr size_t header = 16;
    constexpr size_t columnSize = sizeof(GUID) + 2 * sizeof(DWORD);

    if (data.size() < header + sizeof(DWORD) + columnSize)
        return {};

    DWORD count = 0;
    memcpy(&count, data.data() + header, sizeof(count));
    if (count < 1 || data.size() < header + sizeof(DWORD) + count * columnSize)
        return {};

    const BYTE *column = data.data() + header + sizeof(DWORD);
    PROPERTYKEY key;
    LONG direction = 0;
    memcpy(&key.fmtid, column, sizeof(GUID));
    memcpy(&key.pid, column + sizeof(GUID), sizeof(DWORD));
    memcpy(&direction, column + sizeof(GUID) + sizeof(DWORD), sizeof(LONG));

    return toOrder(key, direction);
}

/**
 * Sort order Explorer saved for dir (when it was last viewed). Explorer keeps one view state per folder type
 * (Generic, Pictures, ...) below Bags\<NodeSlot>\Shell\{folder type}; the most recently written one is used.
 **/
std::optional<Order> savedViewOrder(const QString &dir)
{
    PIDLIST_ABSOLUTE pidl = nullptr;
    if (FAILED(SHParseDisplayName(reinterpret_cast<LPCWSTR>(QDir::toNativeSeparators(dir).utf16()),
                                  nullptr,
                                  &pidl,
                                  0,
                                  nullptr)))
        return {};

    const auto slot = bagNodeSlot(pidl);
    CoTaskMemFree(pidl);
    if (!slot)
        return {};

    const std::wstring bagPath = std::wstring(shellKey) + L"\\Bags\\" + std::to_wstring(*slot) + L"\\Shell";
    RegKey bag(HKEY_CURRENT_USER, bagPath);
    if (!bag)
        return {};

    std::optional<Order> result;
    FILETIME newest = {};

    for (DWORD i = 0;; i++) {
        wchar_t name[128];
        DWORD nameLen = ARRAYSIZE(name);
        FILETIME written = {};
        const LONG res = RegEnumKeyExW(bag.get(), i, name, &nameLen, nullptr, nullptr, nullptr, &written);
        if (res == ERROR_NO_MORE_ITEMS)
            break;
        if (res != ERROR_SUCCESS)
            continue;

        RegKey folderType(bag.get(), name);
        DWORD size = 0;
        DWORD type = 0;
        if (!folderType || RegQueryValueExW(folderType.get(), L"Sort", nullptr, &type, nullptr, &size) != ERROR_SUCCESS
            || type != REG_BINARY)
            continue;

        std::vector<BYTE> data(size);
        if (RegQueryValueExW(folderType.get(), L"Sort", nullptr, &type, data.data(), &size) != ERROR_SUCCESS)
            continue;

        if (result && CompareFileTime(&written, &newest) <= 0)
            continue;

        if (auto order = parseSortValue(data)) {
            result = order;
            newest = written;
        }
    }

    if (result)
        qInfo() << "[DkFileManagerSort] saved Explorer view of" << dir << "sorted by" << result->sortKey;

    return result;
}

bool isDownloadsFolder(const QString &dir)
{
    PWSTR path = nullptr;
    bool downloads = false;
    if (SUCCEEDED(SHGetKnownFolderPath(FOLDERID_Downloads, 0, nullptr, &path)))
        downloads = samePath(QString::fromWCharArray(path), dir);
    CoTaskMemFree(path);
    return downloads;
}

}

QString fileManagerName()
{
    return explorer;
}

DkFileManagerSortOrder fileManagerSortOrder(const QString &dirPath)
{
    const QFileInfo dirInfo(dirPath);
    if (!dirInfo.isDir())
        return {};

    const QString dir = dirInfo.absoluteFilePath();
    ComScope com;

    std::optional<Order> order = openWindowOrder(dir);
    if (!order)
        order = savedViewOrder(dir);

    // Explorer defaults: Downloads is sorted by date modified (newest first), other folders by name
    if (!order) {
        const PROPERTYKEY name = {FMTID_Storage_, PID_ItemNameDisplay};
        const PROPERTYKEY modified = {FMTID_Storage_, PID_DateModified};
        order = isDownloadsFolder(dir) ? toOrder(modified, SORT_DESCENDING) : toOrder(name, SORT_ASCENDING);
    }

    qInfo() << "[DkFileManagerSort]" << dir << "sort key:" << order->sortKey << "status:" << order->status
            << "mode:" << order->sortMode << "dir:" << order->sortDir;

    return *order;
}

}

#endif
