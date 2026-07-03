# Registers this extension with the DuckDB build (via extension-ci-tools).
duckdb_extension_load(rekordbox
    SOURCE_DIR ${CMAKE_CURRENT_LIST_DIR}
    LOAD_TESTS
)
