
#include "inotify.h"

#include <cstdio>

#include <errno.h>
#include <sys/types.h>
#include <sys/select.h>
#include <sys/stat.h>
#include <sys/ioctl.h>
#include <dirent.h>
#include <unistd.h>
#include <linux/limits.h>
#include <string.h>

#include <system_error>
#include <memory>
#include <limits>

#ifdef DEBUG
#include <iostream>
#endif

namespace linux
{

namespace {

#define CHECK_LINUX_FUN_RETURN_OR_THROW(VAR)                \
  do {                                                      \
  if(-1 == VAR)                                             \
    throw std::system_error(errno, std::system_category()); \
  } while(0)

#define THROW_API_CALL_ERROR()                              \
  do {                                                      \
    throw std::system_error(errno, std::system_category()); \
  } while(0)

bool IsDirectory(const std::string &dir) {
  struct stat s;

  if(!lstat(dir.c_str(), &s)) {
    return S_ISDIR(s.st_mode) && !S_ISLNK(s.st_mode);
  }

  return false;
}

bool OneOrTwoDotsDir(struct dirent *entry) {

  return std::string(".")  == entry->d_name ||
         std::string("..") == entry->d_name;

  return false;
}

void DetermineDetails(const char *path) {
  if(EACCES == errno) {
    /// Permission denied.
    std::fprintf(stderr, "Can't open %s: %s", path, strerror(errno));
  } else {
    THROW_API_CALL_ERROR();
  }
}

std::string CombineToFullPath(const std::string &path,
                              const std::string &file) {

  std::string new_path;

  if(file.size() == 0) return path;

  if('/' != path.back()  && '/' != file.front()) {
    new_path = path + "/" + file;
  } else if('/' == path.back()  && '/' == file.front()) {
    new_path = path;
    new_path.append(file.begin() + 1, file.end());
  } else {
    new_path = path + file;
  }

  return std::move(new_path);
}

}

Inotify::Inotify(int flag) {
  this->fd_ = ::inotify_init1(flag);
  CHECK_LINUX_FUN_RETURN_OR_THROW(this->fd_);
}

int Inotify::WatchFile(const char *pathname, uint32_t events) {

  int fd = ::inotify_add_watch(this->fd_, pathname, events);
  CHECK_LINUX_FUN_RETURN_OR_THROW(fd);

#ifdef DEBUG
  std::cerr << "fd: " << fd << ", path: " << pathname << std::endl;
#endif

  return fd;
}

void Inotify::WatchRecursively(const char *path,
                               uint32_t    events,
                               int32_t     max_depth /* = -1 */) {

  if(nullptr == path) {
    throw std::invalid_argument("path can not be null");
  }

  if(max_depth < 0) max_depth = std::numeric_limits<int32_t>::max();

  int fd = this->WatchFile(path, events);

  if(0 == max_depth) {
    return;
  }

  std::shared_ptr<DIR> dir(opendir(path), closedir);
  if(!dir) {
    return;
  } else {
    this->wd_dir_map[fd] = std::string(path);
  }

  struct dirent *entry = nullptr;

  while(nullptr !=(entry = readdir(dir.get()))) {

    std::string entry_path = CombineToFullPath(path, entry->d_name);

    if(IsDirectory(entry_path)) {
      if(OneOrTwoDotsDir(entry)) continue;
      this->WatchRecursively(entry_path.c_str(),
                             events, max_depth - 1);
    }

  } /// while(entry)

}

bool Inotify::RemoveWatch(int wd) {
  auto ret = ::inotify_rm_watch(this->fd_, wd);

  CHECK_LINUX_FUN_RETURN_OR_THROW(ret);

  this->wd_dir_map.erase(wd);

  return ret == 0;
}

std::vector<InotifyEvent> Inotify::ReadEvents(int timeout_sec) {

  std::vector<InotifyEvent> events;

  fd_set read_fds;
  FD_ZERO(&read_fds);
  FD_SET(this->fd_, &read_fds);

  struct timeval read_timeout;
  read_timeout.tv_sec  = timeout_sec;
  read_timeout.tv_usec = 0;

  int nready = select(this->fd_ + 1,
                      &read_fds,
                      nullptr,
                      nullptr,
                      timeout_sec < 0 ? nullptr : &read_timeout);

  if(nready > 0) {

    int bytes_to_read = 0;
    if(ioctl(this->fd_, FIONREAD, &bytes_to_read)) {
      THROW_API_CALL_ERROR();
    }

    std::unique_ptr<char[]> read_buffer(new char[bytes_to_read]);
    read(this->fd_, read_buffer.get(), bytes_to_read);
    this->ParseInotifyEvents(read_buffer.get(), bytes_to_read, events);
  }

  return events;
}

Inotify::~Inotify() {
  if(-1 == this->fd_) {
    ::close(this->fd_);
    this->fd_ = 0;
  }
}

void Inotify::ParseInotifyEvents(char *buf,
                                 int size,
                                 std::vector<InotifyEvent> &events) {

  size_t unread_bytes = size;
  inotify_event* event = reinterpret_cast<inotify_event*>(buf);

  do {

    if(unread_bytes < sizeof(struct inotify_event)) {
#ifdef DEBUG
      std::cerr << "unread size less than size of inotify_event"
                << std::endl;
#endif
      break;
    }

    uint32_t name_length = event->len;
    uint32_t event_size = sizeof(inotify_event) + name_length;

    if(unread_bytes < event_size) {
#ifdef DEBUG
      std::cerr << "unread bytes less than expected."
                << std::endl;
#endif
      break;
    }

    std::string name;
    if(name_length > 0) {
      event->name[name_length] = '\0';
      name = std::move(std::string(event->name));
    }

    if(0 == event->wd) {
      InotifyEvent ev(events.back().wd(),
                      event->mask,
                      event->cookie,
                      events.back().file(),
                      events.back().dir());

      events.push_back(ev);

    } else {

      InotifyEvent ev(event->wd,
                      event->mask,
                      event->cookie,
                      name,
                      this->wd_dir_map[event->wd]);

      events.push_back(ev);

    }


    unread_bytes -= event_size;
    event   = reinterpret_cast<inotify_event*>(
                reinterpret_cast<char*>(event) + event_size
              );

  } while(unread_bytes > 0);

}

} /// ns infra

