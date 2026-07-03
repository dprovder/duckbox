#pragma once
#include "duckdb.hpp"
#include "duckdb/function/copy_function.hpp"

namespace duckdb {

// Registers:  COPY <tracks> TO '<usb-root>' (FORMAT rekordbox)
// Writes <usb-root>/PIONEER/rekordbox/export.pdb + PIONEER/USBANLZ/**/ANLZ*.DAT
struct RekordboxCopyFunction {
	static CopyFunction Get();
};

} // namespace duckdb
