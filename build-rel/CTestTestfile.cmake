# CMake generated Testfile for 
# Source directory: /Users/rjungemann/Projects/turmeric2
# Build directory: /Users/rjungemann/Projects/turmeric2/build-rel
# 
# This file includes the relevant testing commands required for 
# testing this directory and lists subdirectories to be tested as well.
add_test([=[tur_tests]=] "bash" "tests/run.sh")
set_tests_properties([=[tur_tests]=] PROPERTIES  DEPENDS "tur" WORKING_DIRECTORY "/Users/rjungemann/Projects/turmeric2" _BACKTRACE_TRIPLES "/Users/rjungemann/Projects/turmeric2/CMakeLists.txt;19;add_test;/Users/rjungemann/Projects/turmeric2/CMakeLists.txt;0;")
add_test([=[tur_cli_tests]=] "bash" "tests/run-cli.sh")
set_tests_properties([=[tur_cli_tests]=] PROPERTIES  DEPENDS "tur" WORKING_DIRECTORY "/Users/rjungemann/Projects/turmeric2" _BACKTRACE_TRIPLES "/Users/rjungemann/Projects/turmeric2/CMakeLists.txt;24;add_test;/Users/rjungemann/Projects/turmeric2/CMakeLists.txt;0;")
add_test([=[tur_span_tests]=] "bash" "tests/check-span-unknown.sh")
set_tests_properties([=[tur_span_tests]=] PROPERTIES  DEPENDS "tur" WORKING_DIRECTORY "/Users/rjungemann/Projects/turmeric2" _BACKTRACE_TRIPLES "/Users/rjungemann/Projects/turmeric2/CMakeLists.txt;29;add_test;/Users/rjungemann/Projects/turmeric2/CMakeLists.txt;0;")
add_test([=[tur_flags_tests]=] "bash" "tests/run-flags.sh")
<<<<<<< HEAD
set_tests_properties([=[tur_flags_tests]=] PROPERTIES  DEPENDS "tur" WORKING_DIRECTORY "/Users/rjungemann/Projects/turmeric2" _BACKTRACE_TRIPLES "/Users/rjungemann/Projects/turmeric2/CMakeLists.txt;34;add_test;/Users/rjungemann/Projects/turmeric2/CMakeLists.txt;0;")
add_test([=[tur_repl_tests]=] "bash" "tests/turi/eval-test.sh")
set_tests_properties([=[tur_repl_tests]=] PROPERTIES  DEPENDS "tur" WORKING_DIRECTORY "/Users/rjungemann/Projects/turmeric2" _BACKTRACE_TRIPLES "/Users/rjungemann/Projects/turmeric2/CMakeLists.txt;41;add_test;/Users/rjungemann/Projects/turmeric2/CMakeLists.txt;0;")
add_test([=[tur_repl_smoke]=] "bash" "tests/turi/repl-smoke.sh")
set_tests_properties([=[tur_repl_smoke]=] PROPERTIES  DEPENDS "tur" WORKING_DIRECTORY "/Users/rjungemann/Projects/turmeric2" _BACKTRACE_TRIPLES "/Users/rjungemann/Projects/turmeric2/CMakeLists.txt;47;add_test;/Users/rjungemann/Projects/turmeric2/CMakeLists.txt;0;")
add_test([=[tur_eval_import]=] "bash" "tests/turi/eval-import.sh")
set_tests_properties([=[tur_eval_import]=] PROPERTIES  DEPENDS "tur" ENVIRONMENT "TUR_CC_FLAGS=-O2 -std=c99 -Wall -I/Users/rjungemann/Projects/turmeric2/src -L/Users/rjungemann/Projects/turmeric2/build-rel/src" WORKING_DIRECTORY "/Users/rjungemann/Projects/turmeric2" _BACKTRACE_TRIPLES "/Users/rjungemann/Projects/turmeric2/CMakeLists.txt;53;add_test;/Users/rjungemann/Projects/turmeric2/CMakeLists.txt;0;")
add_test([=[tur_eval_basic]=] "/Users/rjungemann/Projects/turmeric2/build-rel/tur_eval_basic")
set_tests_properties([=[tur_eval_basic]=] PROPERTIES  DEPENDS "tur" WORKING_DIRECTORY "/Users/rjungemann/Projects/turmeric2" _BACKTRACE_TRIPLES "/Users/rjungemann/Projects/turmeric2/CMakeLists.txt;69;add_test;/Users/rjungemann/Projects/turmeric2/CMakeLists.txt;0;")
||||||| parent of 6e96c3bb (WIP: Fix failing tests)
set_tests_properties([=[tur_flags_tests]=] PROPERTIES  DEPENDS "tur" WORKING_DIRECTORY "/Users/rjungemann/Projects/turmeric" _BACKTRACE_TRIPLES "/Users/rjungemann/Projects/turmeric/CMakeLists.txt;34;add_test;/Users/rjungemann/Projects/turmeric/CMakeLists.txt;0;")
=======
set_tests_properties([=[tur_flags_tests]=] PROPERTIES  DEPENDS "tur" WORKING_DIRECTORY "/Users/rjungemann/Projects/turmeric" _BACKTRACE_TRIPLES "/Users/rjungemann/Projects/turmeric/CMakeLists.txt;34;add_test;/Users/rjungemann/Projects/turmeric/CMakeLists.txt;0;")
add_test([=[tur_repl_tests]=] "bash" "tests/turi/eval-test.sh")
set_tests_properties([=[tur_repl_tests]=] PROPERTIES  DEPENDS "tur" WORKING_DIRECTORY "/Users/rjungemann/Projects/turmeric" _BACKTRACE_TRIPLES "/Users/rjungemann/Projects/turmeric/CMakeLists.txt;41;add_test;/Users/rjungemann/Projects/turmeric/CMakeLists.txt;0;")
add_test([=[tur_repl_smoke]=] "bash" "tests/turi/repl-smoke.sh")
set_tests_properties([=[tur_repl_smoke]=] PROPERTIES  DEPENDS "tur" WORKING_DIRECTORY "/Users/rjungemann/Projects/turmeric" _BACKTRACE_TRIPLES "/Users/rjungemann/Projects/turmeric/CMakeLists.txt;47;add_test;/Users/rjungemann/Projects/turmeric/CMakeLists.txt;0;")
add_test([=[tur_eval_import]=] "bash" "tests/turi/eval-import.sh")
set_tests_properties([=[tur_eval_import]=] PROPERTIES  DEPENDS "tur" ENVIRONMENT "TUR_CC_FLAGS=-O2 -std=c99 -Wall -I/Users/rjungemann/Projects/turmeric/src -L/Users/rjungemann/Projects/turmeric/build-rel/src" WORKING_DIRECTORY "/Users/rjungemann/Projects/turmeric" _BACKTRACE_TRIPLES "/Users/rjungemann/Projects/turmeric/CMakeLists.txt;53;add_test;/Users/rjungemann/Projects/turmeric/CMakeLists.txt;0;")
add_test([=[tur_eval_basic]=] "/Users/rjungemann/Projects/turmeric/build-rel/tur_eval_basic")
set_tests_properties([=[tur_eval_basic]=] PROPERTIES  DEPENDS "tur" WORKING_DIRECTORY "/Users/rjungemann/Projects/turmeric" _BACKTRACE_TRIPLES "/Users/rjungemann/Projects/turmeric/CMakeLists.txt;69;add_test;/Users/rjungemann/Projects/turmeric/CMakeLists.txt;0;")
>>>>>>> 6e96c3bb (WIP: Fix failing tests)
subdirs("src")
