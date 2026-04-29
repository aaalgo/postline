#pragma once
#include <unistd.h>
#include <fcntl.h>
#include <unordered_set>
#include "common.h"

namespace postline {

using journal_replay_fn = std::function<void(Message &&)>;

class Journal {

    std::string chain_head_path_;
    std::vector<int> fds_;
    int write_fd_;          // write_fd_ is fds_.back(), don't close it separately
    unsigned last_segment_; // fds_.size() - 1
    off_t offset_;          // keeping track of write_fd_ size

    void create_new_segment(std::string const& new_segment_path,
                            std::string const& chain_head_path)
    {
        int fd = ::open(new_segment_path.c_str(),
                        O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC,
                        0644);
        CHECK_FD(fd);
        protocol::journal::Root::make(chain_head_path).write(fd);
        ::close(fd);
    }

    void discover_chain () {
        std::vector<int> reverse_fds;
        std::unordered_set<std::string> seen;

        std::string path = chain_head_path_;
        while (!path.empty()) {
            log::info("Discovered journal segment: {}", path);
            CHECK(!seen.contains(path));
            seen.insert(path);
            int fd = ::open(path.c_str(), O_RDONLY | O_CLOEXEC);
            CHECK_FD(fd);
            protocol::journal::Root root(Message::read(fd));
            reverse_fds.push_back(fd);
            path = root.prev;
        }
        fds_.assign(reverse_fds.rbegin(), reverse_fds.rend());
    }

public:
    Journal(std::string const& new_segment_path,
            std::string const& chain_head_path,
            journal_replay_fn replay)
    {
        if (!new_segment_path.empty()) {
            log::info("Creating new journal segment: {}", new_segment_path);
            create_new_segment(new_segment_path, chain_head_path);
            chain_head_path_ = new_segment_path;
        }
        else {
            chain_head_path_ = chain_head_path;
        }
        CHECK(!chain_head_path_.empty());
        discover_chain();

        // replay
        for (uint32_t segment = 0; segment < fds_.size(); ++segment) {
            int fd = fds_[segment];

            off_t end = ::lseek(fd, 0, SEEK_END);
            CHECK(end >= 0);

            off_t off = 0;

            while (off < end) {
                size_t read_size;
                Message message = Message::read(fd, off, segment, &read_size);
                if (off > 0) {
                    replay(std::move(message));
                }
                off += read_size;
            }
        }

        // reopen the last fd for append
        ::close(fds_.back());
        write_fd_ = ::open(chain_head_path_.c_str(), O_RDWR | O_CLOEXEC);
        CHECK_FD(write_fd_);
        fds_.back() = write_fd_;
        last_segment_ = fds_.size() - 1;
        offset_ = ::lseek(write_fd_, 0, SEEK_END);
        CHECK(offset_ > 0);
    }

    ~Journal() {
        for (int fd : fds_) ::close(fd);
    }

    Journal(Journal const&) = delete;
    Journal& operator=(Journal const&) = delete;

    AccessID append(Message const &message) {
        AccessID access_id = make_access_id(last_segment_, offset_);
        offset_ += message.write(write_fd_);
        return access_id;
    }

    Message read(AccessID access_id) const {
        CHECK(access_id >= 0);
        uint32_t segment = 0;
        uint64_t offset = 0;
        split_access_id(access_id, &segment, &offset);
        CHECK(segment < fds_.size());
        return Message::read(fds_[segment], offset, segment);
    }
};

} // namespace postline
