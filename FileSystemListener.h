#pragma once

#include <filesystem>
#include <memory>
#include <optional>
#include <vector>

class FileSystemListener
{
public:
    enum class ChangeType
    {
        Created,
        Deleted,
        Renamed,
        Modified
    };

    struct FileEvent
    {
        ChangeType type;
        std::filesystem::path path;
        std::optional<std::filesystem::path> oldPath;
        bool isDirectory;
    };

    explicit FileSystemListener(const std::filesystem::path& directory);
    ~FileSystemListener();

    const std::filesystem::path& Directory() const;
    std::vector<FileEvent> Poll();

private:
    struct Impl;
    std::unique_ptr<Impl> m_impl;
};
