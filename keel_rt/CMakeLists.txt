# keel_rt: the C floor every Keel program links against - allocation, abort and output.
#
# C rather than C++, and a separate project rather than part of keelc, because it is not part of the
# compiler: it is data the compiler ships. It is built here so that a change to it is caught by the
# same build as a change to keelc, which is the whole argument for one repository.
# /RTC1's run-time checks call into the MSVC runtime, which a Keel program on Windows does not link.
string(REPLACE "/RTC1" "" CMAKE_C_FLAGS_DEBUG "${CMAKE_C_FLAGS_DEBUG}")

add_subdirectory(src)
add_subdirectory(test)
