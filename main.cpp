#include "FileSystemListener.h"

#include <chrono>
#include <csignal>
#include <iostream>
#include <thread>

volatile std::sig_atomic_t running = 1;

void Stop(int)
{
    running = 0;
}

const char *Name(
    FileSystemListener::ChangeType type)
{
    switch (type)
    {
    case FileSystemListener::ChangeType::Created:
        return "CREATED";

    case FileSystemListener::ChangeType::Deleted:
        return "DELETED";

    case FileSystemListener::ChangeType::Renamed:
        return "RENAMED";

    case FileSystemListener::ChangeType::Modified:
        return "MODIFIED";
    }

    return "UNKNOWN";
}

int main(int argc, char **argv)
{
    std::signal(SIGINT, Stop);

    try
    {
        FileSystemListener listener(
            argc > 1
                ? argv[1]
                : "./watched");

        std::cout
            << "Watching: "
            << listener.Directory()
            << '\n';

        while (running)
        {
            for (const auto &event :
                 listener.Poll())
            {
                std::cout
                    << '['
                    << Name(event.type)
                    << "] ";

                if (event.isDirectory)
                    std::cout << "directory ";
                else
                    std::cout << "file ";

                if (event.oldPath)
                {
                    std::cout
                        << *event.oldPath
                        << " -> ";
                }

                std::cout
                    << event.path
                    << '\n';
            }

            std::this_thread::sleep_for(
                std::chrono::milliseconds(100));
        }
    }
    catch (const std::exception &e)
    {
        std::cerr
            << "Error: "
            << e.what()
            << '\n';

        return 1;
    }

    return 0;
}
