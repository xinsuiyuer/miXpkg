
#ifndef LINUX_INOTIFY_H_
#define LINUX_INOTIFY_H_

#include <unistd.h>
#include <sys/inotify.h>

#include <string>
#include <map>
#include <vector>

namespace linux
{

class InotifyEvent {
 public:
  InotifyEvent(int wd, uint32_t mask, uint32_t cookie,
               const std::string& file, const std::string& dir)
    : wd_(wd), mask_(mask), cookie_(cookie), file_(file), dir_(dir) {

  }


  int wd() const { return this->wd_; }
  uint32_t mask() const { return this->mask_; }
  uint32_t cookie() const { return this->cookie_; }
  const std::string& file() const { return this->file_; }
  const std::string& dir() const { return this->dir_; }

 private:
  int         wd_;
  uint32_t    mask_;
  uint32_t    cookie_;
  std::string file_;
  std::string dir_;
};

class Inotify final {
 public:

  /**
   * @brief create a inotify install by calling inotify_init1().
   * @exception system_error if inotify_init1 return -1, an exception
   * throwed to indicate the error.
   *
   * @param flag Default is 0. see man inotify_init1
   */
  explicit Inotify(int flag = 0);

  ~Inotify();

 private:
  Inotify(const Inotify&) = delete;
  Inotify& operator=(const Inotify&) = delete;


 public:

  int GetDescriptor() const {
    return this->fd_;
  }

  /**
   * @brief adds a new watch, or modifies an existing watch.(man
   * inotify_add_watch)
   *
   * @exception system_error Indicates the error if inotify_add_watch() failed.
   *
   * @param filename Watchs location that is specified in filename. May be 
   * a file or dir.
   * @param events
   *
   * @return a nonnegative watch descriptor.
   */
  int WatchFile(const char *filename, uint32_t events);

  /**
   * @brief watch the path recursively.
   *
   * @param path path of directory to watch. If the path is a file then
   * the behavior is same as WatchFile(...).
   * @param events Inotify events to watch for.
   * @param max_depth less than 0 means ulimit. 0 will be same as WatchFile.
   * @return true on success, false on failure.
   */
  void WatchRecursively(const char *path,
                        uint32_t events,
                        int32_t max_depth = -1);

  /**
   * @brief remove an exsiting watch from an inofity instance.
   *
   * @exception system_error Indicate the error.
   *
   * @param wd The watch descriptor to be removed.
   *
   * @return true if success.
   */
  bool RemoveWatch(int wd);

  /**
   * @brief Read events from the inotify
   *
   * @exception system_error Indicate the error.
   * 
   * @param timeout_sec In second. If 0 will return immediately.
   *
   * @return 
   */
  std::vector<InotifyEvent> ReadEvents(int timeout_sec);

 private:

  void ParseInotifyEvents(char *buf,
                          int size,
                          std::vector<InotifyEvent> &events);

  int fd_;
  std::map<int, std::string> wd_dir_map;

};


} // end of linux ns

#endif /* end of include guard: LINUX_INOTIFY_H_ */

