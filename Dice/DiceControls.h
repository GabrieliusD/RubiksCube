#pragma once
// The config as data (B12): every tunable by its reference name, for JSON (presets, hot reload)
// and the runtime debug panel, which is generated from kControls exactly as the reference's
// panel is generated from its CONTROLS list (same keys, labels, ranges, steps and which ones
// rebuild the board).
#include "DiceConfig.h"
#include <string>
#include <vector>

namespace dice
{
	// One tunable: its B12 name and where it lives in DiceConfig (exactly one pointer is set).
	struct Field
	{
		const char* key;
		float DiceConfig::* f = nullptr;
		int DiceConfig::* i = nullptr;
		bool DiceConfig::* b = nullptr;
		int kind = 0;   // enums: 1 layout, 2 boardShape, 3 collisionMode
	};
	// All of B12, in its order.
	const std::vector<Field>& Fields();

	// Enum values as the reference's strings ('square', 'hex', ...).
	const char* EnumName(int kind, int value);
	int EnumCount(int kind);
	int GetEnum(const DiceConfig& c, int kind);
	void SetEnum(DiceConfig& c, int kind, int value);

	// JSON text of the whole config, with B12 names; and back. Missing keys keep their current
	// value; unknown keys and bad values are listed in `problems`.
	std::string ConfigToJson(const DiceConfig& config);
	bool ConfigFromJson(const std::string& text, DiceConfig& config, std::vector<std::string>& problems);

	// A panel control (the reference's CONTROLS entries).
	struct Control
	{
		enum Kind { Header, Range, Select, Check } kind;
		const char* key;        // B12 name (headers: nullptr)
		const char* label;
		float min = 0, max = 0, step = 0;
		bool rebuild = false;   // changing it rebuilds the board
		std::vector<const char*> options;   // Select: shown names, in enum order
	};
	const std::vector<Control>& Controls();
	const Field* FindField(const char* key);

	// An HTML range input's value: snapped to min + k x step and clamped to [min, max].
	double SnapToStep(double value, double min, double max, double step);
}
