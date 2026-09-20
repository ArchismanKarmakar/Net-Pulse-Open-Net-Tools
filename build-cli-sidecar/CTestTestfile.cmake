# CMake generated Testfile for 
# Source directory: C:/Users/Archisman/Downloads/NetPulse-cpp-web/33/NetPulse-cpp-web
# Build directory: C:/Users/Archisman/Downloads/NetPulse-cpp-web/33/NetPulse-cpp-web/build-cli-sidecar
# 
# This file includes the relevant testing commands required for 
# testing this directory and lists subdirectories to be tested as well.
if(CTEST_CONFIGURATION_TYPE MATCHES "^([Dd][Ee][Bb][Uu][Gg])$")
  add_test(core "C:/Users/Archisman/Downloads/NetPulse-cpp-web/33/NetPulse-cpp-web/build-cli-sidecar/Debug/netpulse_tests.exe")
  set_tests_properties(core PROPERTIES  _BACKTRACE_TRIPLES "C:/Users/Archisman/Downloads/NetPulse-cpp-web/33/NetPulse-cpp-web/CMakeLists.txt;69;add_test;C:/Users/Archisman/Downloads/NetPulse-cpp-web/33/NetPulse-cpp-web/CMakeLists.txt;0;")
elseif(CTEST_CONFIGURATION_TYPE MATCHES "^([Rr][Ee][Ll][Ee][Aa][Ss][Ee])$")
  add_test(core "C:/Users/Archisman/Downloads/NetPulse-cpp-web/33/NetPulse-cpp-web/build-cli-sidecar/Release/netpulse_tests.exe")
  set_tests_properties(core PROPERTIES  _BACKTRACE_TRIPLES "C:/Users/Archisman/Downloads/NetPulse-cpp-web/33/NetPulse-cpp-web/CMakeLists.txt;69;add_test;C:/Users/Archisman/Downloads/NetPulse-cpp-web/33/NetPulse-cpp-web/CMakeLists.txt;0;")
elseif(CTEST_CONFIGURATION_TYPE MATCHES "^([Mm][Ii][Nn][Ss][Ii][Zz][Ee][Rr][Ee][Ll])$")
  add_test(core "C:/Users/Archisman/Downloads/NetPulse-cpp-web/33/NetPulse-cpp-web/build-cli-sidecar/MinSizeRel/netpulse_tests.exe")
  set_tests_properties(core PROPERTIES  _BACKTRACE_TRIPLES "C:/Users/Archisman/Downloads/NetPulse-cpp-web/33/NetPulse-cpp-web/CMakeLists.txt;69;add_test;C:/Users/Archisman/Downloads/NetPulse-cpp-web/33/NetPulse-cpp-web/CMakeLists.txt;0;")
elseif(CTEST_CONFIGURATION_TYPE MATCHES "^([Rr][Ee][Ll][Ww][Ii][Tt][Hh][Dd][Ee][Bb][Ii][Nn][Ff][Oo])$")
  add_test(core "C:/Users/Archisman/Downloads/NetPulse-cpp-web/33/NetPulse-cpp-web/build-cli-sidecar/RelWithDebInfo/netpulse_tests.exe")
  set_tests_properties(core PROPERTIES  _BACKTRACE_TRIPLES "C:/Users/Archisman/Downloads/NetPulse-cpp-web/33/NetPulse-cpp-web/CMakeLists.txt;69;add_test;C:/Users/Archisman/Downloads/NetPulse-cpp-web/33/NetPulse-cpp-web/CMakeLists.txt;0;")
else()
  add_test(core NOT_AVAILABLE)
endif()
subdirs("cli")
