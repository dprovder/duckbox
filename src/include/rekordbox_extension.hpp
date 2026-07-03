#pragma once
#include "duckdb.hpp"

namespace duckdb {

class RekordboxExtension : public Extension {
public:
	void Load(ExtensionLoader &loader) override;
	std::string Name() override { return "rekordbox"; }
	std::string Version() const override { return DefaultVersion(); }
};

} // namespace duckdb
