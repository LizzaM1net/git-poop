project(git-poop CXX)

add_executable(git-poop main.cpp)

target_link_libraries(git-poop PRIVATE util)
