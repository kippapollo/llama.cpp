set(TARGET gguf-encrypt)

add_executable(${TARGET}
    gguf-encrypt.cpp
)

target_link_libraries(${TARGET} PRIVATE common llama)

install(TARGETS ${TARGET} RUNTIME)
