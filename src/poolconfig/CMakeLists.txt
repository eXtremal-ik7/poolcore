add_library(poolconfig STATIC config.cpp)
target_include_directories(poolconfig PUBLIC ${RAPIDJSON_INCLUDE_DIRECTORY})
