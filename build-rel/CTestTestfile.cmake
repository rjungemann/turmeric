# CMake generated Testfile for 
# Source directory: /Users/rjungemann/Projects/turmeric
# Build directory: /Users/rjungemann/Projects/turmeric/build-rel
# 
# This file includes the relevant testing commands required for 
# testing this directory and lists subdirectories to be tested as well.
add_test([=[tur_tests]=] "bash" "tests/run.sh")
set_tests_properties([=[tur_tests]=] PROPERTIES  DEPENDS "tur" WORKING_DIRECTORY "/Users/rjungemann/Projects/turmeric" _BACKTRACE_TRIPLES "/Users/rjungemann/Projects/turmeric/CMakeLists.txt;19;add_test;/Users/rjungemann/Projects/turmeric/CMakeLists.txt;0;")
add_test([=[tur_cli_tests]=] "bash" "tests/run-cli.sh")
set_tests_properties([=[tur_cli_tests]=] PROPERTIES  DEPENDS "tur" WORKING_DIRECTORY "/Users/rjungemann/Projects/turmeric" _BACKTRACE_TRIPLES "/Users/rjungemann/Projects/turmeric/CMakeLists.txt;24;add_test;/Users/rjungemann/Projects/turmeric/CMakeLists.txt;0;")
add_test([=[tur_span_tests]=] "bash" "tests/check-span-unknown.sh")
set_tests_properties([=[tur_span_tests]=] PROPERTIES  DEPENDS "tur" WORKING_DIRECTORY "/Users/rjungemann/Projects/turmeric" _BACKTRACE_TRIPLES "/Users/rjungemann/Projects/turmeric/CMakeLists.txt;29;add_test;/Users/rjungemann/Projects/turmeric/CMakeLists.txt;0;")
add_test([=[tur_flags_tests]=] "bash" "tests/run-flags.sh")
set_tests_properties([=[tur_flags_tests]=] PROPERTIES  DEPENDS "tur" WORKING_DIRECTORY "/Users/rjungemann/Projects/turmeric" _BACKTRACE_TRIPLES "/Users/rjungemann/Projects/turmeric/CMakeLists.txt;34;add_test;/Users/rjungemann/Projects/turmeric/CMakeLists.txt;0;")
subdirs("src")
subdirs("examples")
