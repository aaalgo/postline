#pragma once
#include <unistd.h>
#include <fcntl.h>
#include <unordered_set>
#include "common.h"

namespace postline {

using journal_replay_fn = std::function<void(Message &&)>;


class Journal {

    std::string resume_path_;
    std::vector<int> fds_;
    int write_fd_;          // write_fd_ is fds_.back(), don't close it separately
    unsigned last_segment_; // fds_.size() - 1
    off_t offset_;          // keeping track of write_fd_ size
    bool read_only_;

    void create_new_segment(std::string const& journal_path,
                            std::string const& resume_path)
    {
        int fd = ::open(journal_path.c_str(),
                        O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC,
                        0644);
        CHECK_FD(fd);
        protocol::journal::Root::make(resume_path).write(fd);
        ::close(fd);
    }

    void discover_chain () {
        std::vector<int> reverse_fds;
        std::unordered_set<std::string> seen;

        std::string path = resume_path_;
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
    Journal(std::string const& journal_path,
            std::string const& resume_path,
            journal_replay_fn replay)
        : write_fd_(-1),
          last_segment_(0),
          offset_(0),
          read_only_(true)
    {
        bool read_only = true;
        if (!journal_path.empty()) {
            log::info("Creating new journal segment: {}", journal_path);
            create_new_segment(journal_path, resume_path);
            resume_path_ = journal_path;
            read_only = false;
        }
        else {
            resume_path_ = resume_path;
        }
        read_only_ = read_only;
        CHECK(!resume_path_.empty());
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
        if (!read_only_) {
            ::close(fds_.back());
            write_fd_ = ::open(resume_path_.c_str(), O_RDWR | O_CLOEXEC);
            CHECK_FD(write_fd_);
            fds_.back() = write_fd_;
            offset_ = ::lseek(write_fd_, 0, SEEK_END);
            CHECK(offset_ > 0);
        }
        last_segment_ = fds_.size() - 1;
    }

    ~Journal() {
        for (int fd : fds_) ::close(fd);
    }

    Journal(Journal const&) = delete;
    Journal& operator=(Journal const&) = delete;

    AccessID append(Message &message) {
        if (write_fd_ < 0) return NO_ACCESS_ID;
        AccessID access_id = make_access_id(last_segment_, offset_);
        message.set_access_id(access_id);
        offset_ += message.write(write_fd_);
        return access_id;
    }

    Message read(AccessID access_id) const {
        //CHECK(access_id >= 0);
        uint32_t segment = 0;
        uint64_t offset = 0;
        split_access_id(access_id, &segment, &offset);
        CHECK(segment < fds_.size());
        return Message::read(fds_[segment], offset, segment);
    }
};

} // namespace postline
