
#include <sys/types.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>
#include <linux/limits.h>
#include <errno.h>

#include <iostream>
#include <fstream>
#include <system_error>
#include <thread>
#include <vector>
#include <memory>
#include <cstring>
#include <array>
#include <algorithm>

#include <tclap/CmdLine.h>

#include "inotify.h"

namespace {

using InotifyEventCollection = std::vector<linux::InotifyEvent>;
using StringArray = std::vector<std::string>;

std::string g_sysrootDir;
std::string g_outputDir;
std::string g_packageName;
bool        g_reserveCopied;
StringArray g_argsToMake;
StringArray g_CopiedItems;
bool        g_canClean = false;


bool g_stop_monitor = false;

bool IsDir(const char *dir);
bool ParseCmdOptions(int argc, char *argv[]);
void WatchInotifyEvents(linux::Inotify &notify,
                        InotifyEventCollection &installed);

int CreateChildProcessAndWait(const std::string &command,
                              const StringArray &argv);

bool InstallAndMonitorSysroot(InotifyEventCollection &installed);
bool CopyInstalledToOutputDir(const InotifyEventCollection &installed);
std::string CombineToFullPath(const std::string &path,
                              const std::string &file);
void CreateDebianPackage();

}


int main(int argc, char *argv[])
{

  struct Cleaner {
    InotifyEventCollection &events;
    Cleaner(InotifyEventCollection &evs) : events(evs){ }
    ~Cleaner() {

      if(!g_canClean || g_reserveCopied) {
        return;
      }

      std::cout << "Cleaning copied items..." << std::endl;

      g_CopiedItems.insert(g_CopiedItems.begin(), "-rf");

      if(!g_CopiedItems.empty()) {
        g_CopiedItems.push_back(CombineToFullPath(g_outputDir, "DEBIAN"));
        CreateChildProcessAndWait("rm", g_CopiedItems);
      }

    }
  };

  if(!ParseCmdOptions(argc, argv)) {
    return 1;
  }

  InotifyEventCollection installed;
  Cleaner cleaner(installed);

  if(InstallAndMonitorSysroot(installed) &&
     CopyInstalledToOutputDir(installed)) {

    CreateDebianPackage();
  }

  g_canClean = true;

  return 0;
}

namespace {

bool IsDir(const char *dir) {
  struct stat s;

  if(!lstat(dir, &s)) {
    return S_ISDIR(s.st_mode) && !S_ISLNK(s.st_mode);
  }


  return false;
}

bool ParseCmdOptions(int argc, char *argv[]) {

  TCLAP::CmdLine cmd("Make install, "
                     "in cross compile env and generate DEB package.(xinsuiyuer@gmail.com)",
                     ' ',
                     "0.1");

  TCLAP::ValueArg<std::string> sysrootArg(
      "s", "sysroot", "The directory of sysroot",
      true, "", "sysroot");

  cmd.add(sysrootArg);

  TCLAP::SwitchArg reserveArg(
      "r", "reserve",
      "Off default. Whether reserve items that had been copied to output directory. ",
      false);

  cmd.add(reserveArg);

  TCLAP::ValueArg<std::string> outputArg(
      "o", "output",
      "The directory where installed files will be copied to,"
      " and create a DEB package automatically that will be placed in <output>/../<pkg-name>.deb",
      true, ".", "/path/to/output"
      );

  cmd.add(outputArg);

  TCLAP::ValueArg<std::string> packageNameArg(
      "n",
      "pkg-name",
      "the name of the package that will be generated",
      true, "", "package name");

  cmd.add(packageNameArg);

  TCLAP::UnlabeledMultiArg<std::string> toMakeArgs(
      "args",
      "args pass to 'make'. (e.g. -B -f unix.make)"
      "Default contain target named 'install'",
      false, "args pass to make", true);

  cmd.add(toMakeArgs);

  try {

    cmd.parse(argc, argv);
    g_sysrootDir    = sysrootArg.getValue();
    g_outputDir     = outputArg.getValue();
    g_packageName   = packageNameArg.getValue();
    g_argsToMake    = toMakeArgs.getValue();
    g_reserveCopied = reserveArg.getValue();

    if(g_argsToMake.empty()) {
      g_argsToMake.push_back("install");
    }

    if(sysrootArg.isSet() && !IsDir(g_sysrootDir.c_str())) {

      std::cerr << "Invalid sysroot directory: " << g_sysrootDir << std::endl;
      return false;
    }

    if(g_outputDir != "." && !IsDir(g_outputDir.c_str())) {
      std::cerr << "Invalid output directory: " << g_outputDir << std::endl;
      return false;
    }

    if(g_outputDir == ".") {
      char *cwd = ::getcwd(nullptr, 0);
      g_outputDir = std::string(cwd);
      free(cwd);
    }
  }
  catch(TCLAP::ArgException &ex) {
    std::cerr << "error: " << ex.error() << std::endl;
    return false;
  }
  catch(std::exception &ex) {
    std::cout << "error: " << ex.what() << std::endl;
    return false;
  }

  return true;
}

void WatchInotifyEvents(linux::Inotify &notify,
                        InotifyEventCollection &installed) {

  try {

    while(!g_stop_monitor) {
      auto events = notify.ReadEvents(1);
      for(auto &event : events) {

        if(IN_MOVED_FROM & event.mask()) {
          auto deleted = std::find_if(installed.begin(),
                                      installed.end(),
                                      [&](linux::InotifyEvent& e)->bool {
                                        return event.file() == e.file() &&
                                               event.dir()  == e.dir();
                                      });

          installed.erase(deleted);
          continue;
        }

        if(IN_CREATE & event.mask()) {
          installed.push_back(std::move(event));
        }

      } /// end for

    } // end while


  }
  catch(std::exception &ex) {
    std::cerr << ex.what() << std::endl;
  }

}

int CreateChildProcessAndWait(const std::string &command,
                              const StringArray &argv) {

  char* args[32];
  args[0] = const_cast<char*>(command.data());

  for(StringArray::size_type i = 0; i < argv.size() && i < 32; ++i) {
    args[i + 1] = const_cast<char*>(argv[i].data());
  }

  args[argv.size() + 1] = nullptr;

#ifdef DEBUG
  for(StringArray::size_type i = 0; i < argv.size() + 1; ++i)
    std::cout << args[i] << " ";
  std::cout << std::endl;
#endif

  pid_t child = fork();
  if(0 == child) {
    execvp(command.c_str(), args);
    _exit(errno);
  }

  int status = -1;
  waitpid(child, &status, 0);

  return WEXITSTATUS(status);
}

char * chrtostr(char ch) {
        static char str[2] = { '\0', '\0' };
        str[0] = ch;
        return str;
}

char * inotifytools_event_to_str_sep(int events, char sep)
{
        static char ret[1024];
        ret[0] = '\0';
        ret[1] = '\0';

        if ( IN_ACCESS & events ) {
                strcat( ret, chrtostr(sep) );
                strcat( ret, "ACCESS" );
        }
        if ( IN_MODIFY & events ) {
                strcat( ret, chrtostr(sep) );
                strcat( ret, "MODIFY" );
        }
        if ( IN_ATTRIB & events ) {
                strcat( ret, chrtostr(sep) );
                strcat( ret, "ATTRIB" );
        }
        if ( IN_CLOSE_WRITE & events ) {
                strcat( ret, chrtostr(sep) );
                strcat( ret, "CLOSE_WRITE" );
        }
        if ( IN_CLOSE_NOWRITE & events ) {
                strcat( ret, chrtostr(sep) );
                strcat( ret, "CLOSE_NOWRITE" );
        }
        if ( IN_OPEN & events ) {
                strcat( ret, chrtostr(sep) );
                strcat( ret, "OPEN" );
        }
        if ( IN_MOVED_FROM & events ) {
                strcat( ret, chrtostr(sep) );
                strcat( ret, "MOVED_FROM" );
        }
        if ( IN_MOVED_TO & events ) {
                strcat( ret, chrtostr(sep) );
                strcat( ret, "MOVED_TO" );
        }
        if ( IN_CREATE & events ) {
                strcat( ret, chrtostr(sep) );
                strcat( ret, "CREATE" );
        }
        if ( IN_DELETE & events ) {
                strcat( ret, chrtostr(sep) );
                strcat( ret, "DELETE" );
        }
        if ( IN_DELETE_SELF & events ) {
                strcat( ret, chrtostr(sep) );
                strcat( ret, "DELETE_SELF" );
        }
        if ( IN_UNMOUNT & events ) {
                strcat( ret, chrtostr(sep) );
                strcat( ret, "UNMOUNT" );
        }
        if ( IN_Q_OVERFLOW & events ) {
                strcat( ret, chrtostr(sep) );
                strcat( ret, "Q_OVERFLOW" );
        }
        if ( IN_IGNORED & events ) {
                strcat( ret, chrtostr(sep) );
                strcat( ret, "IGNORED" );
        }
        if ( IN_CLOSE & events ) {
                strcat( ret, chrtostr(sep) );
                strcat( ret, "CLOSE" );
        }
        if ( IN_MOVE_SELF & events ) {
                strcat( ret, chrtostr(sep) );
                strcat( ret, "MOVE_SELF" );
        }
        if ( IN_ISDIR & events ) {
                strcat( ret, chrtostr(sep) );
                strcat( ret, "ISDIR" );
        }
        if ( IN_ONESHOT & events ) {
                strcat( ret, chrtostr(sep) );
                strcat( ret, "ONESHOT" );
        }

        return &ret[1];
}

bool InstallAndMonitorSysroot(InotifyEventCollection &installed) {

  try {

    linux::Inotify notify;
    std::cout << std::endl;
    notify.WatchRecursively(g_sysrootDir.c_str(), IN_CREATE | IN_MOVE , 9);
    std::cout << std::endl;

    std::thread monitor(WatchInotifyEvents, std::ref(notify), std::ref(installed));

    int rc = CreateChildProcessAndWait("make", g_argsToMake);
    g_stop_monitor = true;
    monitor.join();

    if(rc != 0) return false;

#ifdef DEBUG
    std::cout << "inotify fd: " << notify.GetDescriptor() << std::endl;
    for(auto &event : installed) {
      std::cout << "fd: " << event.wd() << "cookie: " << event.cookie() << " ---  ";
      if(event.dir().size() > 0) std::cout << event.dir() << "/";
      std::cout << event.file() << "   ";
      std::cout << inotifytools_event_to_str_sep(event.mask(), ' ')
                << std::endl << std::endl;
    }
#endif

  }
  catch(const std::exception &ex) {
    std::cerr << ex.what() << std::endl;
  }

  return true;
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

bool CopyInstalledToOutputDir(const InotifyEventCollection &installed) {

  for(auto &entry : installed) {

    if(false == (IN_CREATE & entry.mask())) continue;

    std::string full_installed_path =
        CombineToFullPath(entry.dir(), entry.file());

    /// miXpkg -s /opt/sysroot
    /// entry.dir() = /opt/sysroot/dira/dircc
    ///                           ^  < - >  ^   => /dira/dircc
    std::string relative_path(full_installed_path.begin() + g_sysrootDir.size(),
                              full_installed_path.end());

    /// miXpkg -o ~/pkg
    /// =>  ~/pkg/dira/dircc
    /// make sure has only one blash between ouput dir and relative_path
    std::string full_output_path = CombineToFullPath(g_outputDir, relative_path);

    /// extract ouput dir, and creat it first.
    std::string::size_type last_blash_pos = full_output_path.find_last_of('/');
    std::string full_output_dir(full_output_path.begin(),
                                full_output_path.begin() + last_blash_pos);

#ifdef DEBUG
    std::cout << std::endl;
    std::cout << "          entry.dir: " << entry.dir()         << std::endl;
    std::cout << "         entry.file: " << entry.file()        << std::endl;
    std::cout << "full_installed_path: " << full_installed_path << std::endl;
    std::cout << "      relative_path: " << relative_path       << std::endl;
    std::cout << "   full_output_path: " << full_output_path    << std::endl;
    std::cout << "    full_output_dir: " << full_output_dir     << std::endl;
#endif

    /// first, create the ouput dir.
    StringArray mkdirArgs{ "-p", full_output_dir};
    CreateChildProcessAndWait("mkdir", mkdirArgs);

    /// second, copy to ouput dir.
    StringArray cpArgs{ "-R", full_installed_path, full_output_path };
    CreateChildProcessAndWait("cp", cpArgs);
    g_CopiedItems.push_back(full_output_path);

  }

  return true;
}

void CreateDebianPackage() {
  int rc = 0;

  /// create DEBIAN directory
  std::string debian_dir = CombineToFullPath(g_outputDir, "DEBIAN");
  StringArray mkdirArgs{ "-p", debian_dir };
  rc = CreateChildProcessAndWait("mkdir", mkdirArgs);
  if(0 != rc) exit(1);

  /// create control file
  std::string deb_control = CombineToFullPath(debian_dir, "control");

  {

    std::fstream control_fs(deb_control, std::ios::out);
    if(!control_fs) {
      std::cerr << "Can't create " << deb_control << std::endl;
      exit(1);
    }

    control_fs << "Package: "      << g_packageName << std::endl
               << "Version: "      << std::endl
               << "Section: "      << std::endl
               << "Architecture: " << std::endl
               << "Maintainer: "   << std::endl
               << "Description: "  << std::endl;

    control_fs.flush();

  }

  const char *editor_env = getenv("EDITOR");
  if(nullptr == editor_env) editor_env = "vim";

  std::string editor(editor_env);
  StringArray editorArgs{ deb_control };
  rc = CreateChildProcessAndWait(editor, editorArgs);
  if(0 != rc) {
    std::cerr << std::endl << "Can't find vim or other editor." << std::endl
              << "You can edit " << deb_control
              << " manually." << std::endl
              << "And then run 'dpkg -b " << g_outputDir << " ' to "
              << " create DEB package for '" << g_packageName
              << "'" << std::endl;

    exit(1);
  }

  StringArray dpkgArgs{ "-b", g_outputDir, g_packageName + ".deb" };
  rc = CreateChildProcessAndWait("dpkg", dpkgArgs);
  if(0 != rc) {

    if(ENOENT == rc) {
      std::cerr << "Can't find dpkg command." << std::endl;
    } else {
      std::cerr << std::endl
                << "Can't create DEB package for '" << g_packageName
                << "'. Please fix '" << deb_control << "'"
                << "and run 'dpkg -b " << g_outputDir << " "
                << g_packageName + ".deb" << "' again."
                << std::endl;
    }

    exit(1);
  }

}


}
