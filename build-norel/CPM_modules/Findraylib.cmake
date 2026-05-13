include("/Users/rjungemann/Projects/turmeric/build-norel/cmake/CPM.cmake")
CPMAddPackage("NAME;raylib;GITHUB_REPOSITORY;raysan5/raylib;GIT_TAG;5.0;OPTIONS;BUILD_EXAMPLES OFF;BUILD_GAMES    OFF;WITH_PIC       ON")
set(raylib_FOUND TRUE)