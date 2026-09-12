# keel_rt: the C floor every Keel program links against - allocation, abort, and eventually print.
#
# C rather than C++, and a separate project rather than part of keelc, because it is not part of the
# compiler: it is data the compiler ships. It is built here so that a change to it is caught by the
# same build as a change to keelc, which is the whole argument for one repository.
add_subdirectory(src)
add_subdirectory(test)
