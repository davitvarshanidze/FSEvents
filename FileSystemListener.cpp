#include "FileSystemListener.h"

#include <CoreServices/CoreServices.h>
#include <dispatch/dispatch.h>
#include <sys/stat.h>

#include <atomic>
#include <cerrno>
#include <map>
#include <set>
#include <stdexcept>
#include <system_error>
#include <utility>

namespace fs = std::filesystem;

struct FileSystemListener::Impl
{
    struct Entry
    {
        struct stat info{};

        using Id = std::pair<dev_t, ino_t>;

        Id Identity() const
        {
            return {info.st_dev, info.st_ino};
        }

        bool IsDirectory() const
        {
            return S_ISDIR(info.st_mode);
        }

        bool Changed(const Entry &other) const
        {
            if (IsDirectory())
                return false;

            return info.st_size != other.info.st_size ||
                   info.st_mode != other.info.st_mode ||
                   info.st_mtimespec.tv_sec != other.info.st_mtimespec.tv_sec ||
                   info.st_mtimespec.tv_nsec != other.info.st_mtimespec.tv_nsec;
        }
    };

    using Snapshot = std::map<fs::path, Entry>;

    fs::path directory;
    Snapshot snapshot;

    std::atomic<bool> dirty{false};
    std::atomic<bool> rootChanged{false};

    dispatch_queue_t queue = nullptr;
    FSEventStreamRef stream = nullptr;

    bool started = false;

    explicit Impl(const fs::path &path)
        : directory(fs::canonical(path))
    {
        if (!fs::is_directory(directory))
            throw std::runtime_error(
                "Not a directory: " + directory.string());
    }

    ~Impl()
    {
        if (stream)
        {
            if (started)
                FSEventStreamStop(stream);

            FSEventStreamInvalidate(stream);

            if (queue)
                dispatch_sync_f(
                    queue,
                    nullptr,
                    [](void *) {});

            FSEventStreamRelease(stream);
        }

        if (queue)
            dispatch_release(queue);
    }

    static Entry Read(const fs::path &path)
    {
        Entry entry;

        if (lstat(path.c_str(), &entry.info) != 0)
        {
            throw fs::filesystem_error(
                "Could not read file",
                path,
                std::error_code(
                    errno,
                    std::generic_category()));
        }

        return entry;
    }

    Snapshot Scan() const
    {
        Snapshot result;

        for (const auto &item :
             fs::recursive_directory_iterator(directory))
        {
            result.emplace(
                item.path(),
                Read(item.path()));
        }

        return result;
    }

    static void Callback(
        ConstFSEventStreamRef,
        void *context,
        size_t count,
        void *,
        const FSEventStreamEventFlags flags[],
        const FSEventStreamEventId[]) noexcept
    {
        auto &self =
            *static_cast<Impl *>(context);

        self.dirty.store(
            true,
            std::memory_order_relaxed);

        for (size_t i = 0; i < count; ++i)
        {
            if (flags[i] &
                kFSEventStreamEventFlagRootChanged)
            {
                self.rootChanged.store(
                    true,
                    std::memory_order_relaxed);
            }
        }
    }

    void Start()
    {
        queue = dispatch_queue_create(
            "MacFileListener",
            DISPATCH_QUEUE_SERIAL);

        if (!queue)
            throw std::runtime_error(
                "Could not create dispatch queue");

        CFStringRef path =
            CFStringCreateWithFileSystemRepresentation(
                nullptr,
                directory.c_str());

        if (!path)
            throw std::runtime_error(
                "Could not create path");

        const void *values[] = {path};

        CFArrayRef paths =
            CFArrayCreate(
                nullptr,
                values,
                1,
                &kCFTypeArrayCallBacks);

        CFRelease(path);

        if (!paths)
            throw std::runtime_error(
                "Could not create paths array");

        FSEventStreamContext context{};
        context.info = this;

        stream = FSEventStreamCreate(
            nullptr,
            Callback,
            &context,
            paths,
            kFSEventStreamEventIdSinceNow,
            0.1,
            kFSEventStreamCreateFlagWatchRoot);

        CFRelease(paths);

        if (!stream)
            throw std::runtime_error(
                "Could not create FSEvent stream");

        FSEventStreamSetDispatchQueue(
            stream,
            queue);

        started = FSEventStreamStart(stream);

        if (!started)
            throw std::runtime_error(
                "Could not start FSEvent stream");

        snapshot = Scan();
    }

    static std::vector<FileEvent> Diff(
        const Snapshot &oldSnapshot,
        const Snapshot &newSnapshot)
    {
        std::vector<FileEvent> events;

        std::set<fs::path> oldUsed;
        std::set<fs::path> newUsed;

        for (const auto &pair : newSnapshot)
        {
            const fs::path &path = pair.first;
            const Entry &current = pair.second;

            auto old = oldSnapshot.find(path);

            if (old == oldSnapshot.end())
                continue;

            oldUsed.insert(path);
            newUsed.insert(path);

            if (old->second.Identity() != current.Identity() ||
                old->second.Changed(current))
            {
                events.push_back({ChangeType::Modified,
                                  path,
                                  std::nullopt,
                                  current.IsDirectory()});
            }
        }

        using Id = Entry::Id;

        std::map<Id, std::vector<fs::path>> oldIds;
        std::map<Id, std::vector<fs::path>> newIds;

        for (const auto &pair : oldSnapshot)
        {
            const fs::path &path = pair.first;
            const Entry &entry = pair.second;

            if (oldUsed.count(path) == 0)
                oldIds[entry.Identity()].push_back(path);
        }

        for (const auto &pair : newSnapshot)
        {
            const fs::path &path = pair.first;
            const Entry &entry = pair.second;

            if (newUsed.count(path) == 0)
                newIds[entry.Identity()].push_back(path);
        }

        for (const auto &pair : oldIds)
        {
            const Id &id = pair.first;
            const auto &oldPaths = pair.second;

            auto found = newIds.find(id);

            if (found == newIds.end())
                continue;

            if (oldPaths.size() != 1 ||
                found->second.size() != 1)
                continue;

            const fs::path &oldPath =
                oldPaths.front();

            const fs::path &newPath =
                found->second.front();

            oldUsed.insert(oldPath);
            newUsed.insert(newPath);

            events.push_back({ChangeType::Renamed,
                              newPath,
                              oldPath,
                              newSnapshot.at(newPath).IsDirectory()});
        }

        for (const auto &pair : oldSnapshot)
        {
            const fs::path &path = pair.first;
            const Entry &entry = pair.second;

            if (oldUsed.count(path) == 0)
            {
                events.push_back({ChangeType::Deleted,
                                  path,
                                  std::nullopt,
                                  entry.IsDirectory()});
            }
        }

        for (const auto &pair : newSnapshot)
        {
            const fs::path &path = pair.first;
            const Entry &entry = pair.second;

            if (newUsed.count(path) == 0)
            {
                events.push_back({ChangeType::Created,
                                  path,
                                  std::nullopt,
                                  entry.IsDirectory()});
            }
        }

        return events;
    }
};

FileSystemListener::FileSystemListener(
    const fs::path &directory)
    : m_impl(std::make_unique<Impl>(directory))
{
    m_impl->Start();
}

FileSystemListener::~FileSystemListener() = default;

const fs::path &
FileSystemListener::Directory() const
{
    return m_impl->directory;
}

std::vector<FileSystemListener::FileEvent>
FileSystemListener::Poll()
{
    if (!m_impl->dirty.exchange(
            false,
            std::memory_order_relaxed))
    {
        return {};
    }

    if (m_impl->rootChanged.load(
            std::memory_order_relaxed))
    {
        throw std::runtime_error(
            "Watched directory was moved or deleted");
    }

    try
    {
        auto current =
            m_impl->Scan();

        auto events =
            Impl::Diff(
                m_impl->snapshot,
                current);

        m_impl->snapshot =
            std::move(current);

        return events;
    }
    catch (const fs::filesystem_error &)
    {
        m_impl->dirty.store(
            true,
            std::memory_order_relaxed);

        return {};
    }
}
