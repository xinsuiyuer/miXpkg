miXpkg is tool to generate a DEB package, the package contains files that were installed by command "make install".
It is useful in cross-compile enviroment. Sometimes after compiling from source, you have to install the app to sysroot on host,
and then install on target as well.

usage:
1. make
Compile sources firstly.
2. miXpkg -s /path/to/sysroot -o /path/to/place/copied/installed/files -n package-name [args pass to make, e.g. install var1=val1]
3. The generated DEB package placed in /path/to/place/copied/installed/files/../package-name.deb



How does it work?
1. Beforce miXpkg runs 'make [install | args pass to make]', it watchs at sysroot by using inotify mechanism.
2. Run 'make [install | args pass to make]'
3. Stop watching at sysroot, and copys files or directorys that were created into path specified by -o option.
4. Create DEB's control file path/DEBIAN/control( path specified by -o option).
5. Run editor specified in EDITOR enviroment variable(or vim default.)
6. After editor exit, uses dpkg -b to generate DEB package.
