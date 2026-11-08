idf_component_register(
    SRCS "src/hcc_backup_stream.cpp" "src/hcc_plist.cpp"
         "src/hcc_backup_upload_request.cpp" "src/hcc_backup_storage.cpp" "src/hcc_backup_receive.cpp" "src/hcc_backup_batch.cpp" "src/hcc_backup_session.cpp" "src/hcc_backup_link.cpp"
    INCLUDE_DIRS "include"
)
target_compile_features(${COMPONENT_LIB} PUBLIC cxx_std_17)
