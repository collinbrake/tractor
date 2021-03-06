find_package(Boost REQUIRED system filesystem)
find_package(gflags REQUIRED COMPONENTS shared)
find_package(Glog REQUIRED)

add_library(farm_ng_core SHARED
  blobstore.cpp
  blobstore.h
  event_log_reader.cpp
  event_log_reader.h
  event_log.cpp
  event_log.h
  init.cpp
  init.h
  ipc.cpp
  ipc.h
  thread_pool.cpp
  thread_pool.h
  )

target_include_directories(farm_ng_core PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/../..)
target_link_libraries(
  farm_ng_core
  farm_ng_core_protobuf
  ${Boost_FILESYSTEM_LIBRARY}
  ${Boost_SYSTEM_LIBRARY}
  dl
  pthread
  gflags
  ${GLOG_LIBRARIES}
)

# TODO add tests!
enable_testing()
include(GoogleTest)
