set(MEMKV_LINK_LIBS
        ${FMT_LIBRARY}
        ${GFLAGS_STATIC_LIB}
        ${PROTOBUF_STATIC_LIBRARY}
        ${BRPC_LIBRARY}
        pthread
        )

set(MEMKV_SOURCES
        memkv_store.cc
        slice.h
        status.cc
        memkv_service.cc
        db.cc
        pb/memkv.pb.cc)

add_library(memkv ${MEMKV_SOURCES})
target_link_libraries(memkv ${MEMKV_LINK_LIBS})
set(MEMKV_LINK_LIBS memkv ${MEMKV_LINK_LIBS})

function(ADD_TEST TEST_NAME)
    add_executable(${TEST_NAME} ${TEST_NAME}.cc ${MEMKV_SOURCES})
    target_link_libraries(${TEST_NAME} ${MEMKV_LINK_LIBS} ${GTEST_LIB} ${GTEST_MAIN_LIB})
endfunction()

ADD_TEST(memkv_store_test)

add_executable(memkv_server memkv_server.cc ${MEMKV_SOURCES})
target_link_libraries(memkv_server ${MEMKV_LINK_LIBS})