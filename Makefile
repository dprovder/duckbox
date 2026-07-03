PROJ_DIR := $(dir $(abspath $(lastword $(MAKEFILE_LIST))))

# Name of the extension
EXT_NAME=rekordbox
EXT_CONFIG=${PROJ_DIR}extension_config.cmake

# Pulls in DuckDB's extension build machinery (targets: release, debug, test, clean).
# Requires the extension-ci-tools + duckdb submodules (see README "Bootstrap").
include extension-ci-tools/makefiles/duckdb_extension.Makefile
