app: main.cc inotify.cc
	#g++ -std=c++11 -Wall -g -O0 -o miXpkg main.cc inotify.cc -pthread
	g++ -std=c++11 -DDEBUG -Wall -g -O0 -o miXpkg main.cc inotify.cc -pthread
