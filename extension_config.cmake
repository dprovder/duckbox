# Registers this extension with the DuckDB build (via extension-ci-tools).
duckdb_extension_load(rekordbox
    SOURCE_DIR ${CMAKE_CURRENT_LIST_DIR}
    LOAD_TESTS
)

# HTTP API server (community extension, built in-tree so it works on this
# custom v0.0.1 dev binary where INSTALL FROM community 404s). Provides
# httpserve_start/httpserve_stop -> SQL-over-HTTP JSON for the local DJ UI.
# Source is vendored + symlinked to this duckdb at ../deps/httpserver.
duckdb_extension_load(httpserver
    SOURCE_DIR ${CMAKE_CURRENT_LIST_DIR}/../deps/httpserver
)
