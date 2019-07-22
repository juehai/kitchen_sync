#ifndef SCHEMA_H
#define SCHEMA_H

#include <iostream>
#include <string>
#include <vector>
#include <map>
#include "message_pack/packed_value.h"

using namespace std;

typedef vector<size_t> ColumnIndices;
typedef vector<PackedValue> ColumnValues;

namespace ColumnTypes {
	const string BLOB = "BLOB";
	const string TEXT = "TEXT";
	const string VCHR = "VARCHAR";
	const string FCHR = "CHAR";
	const string JSON = "JSON";
	const string UUID = "UUID";
	const string BOOL = "BOOL";
	const string SINT = "INT";
	const string UINT = "INT UNSIGNED";
	const string REAL = "REAL";
	const string DECI = "DECIMAL";
	const string DATE = "DATE";
	const string TIME = "TIME";
	const string DTTM = "DATETIME";
	const string SPAT = "SPATIAL";
	const string ENUM = "ENUM";

	const string UNKN = "UNKNOWN";
}

enum class DefaultType {
	// these flags are serialized by name not value, so the values here can be changed if required
	no_default = 0,
	sequence = 1, // used for AUTO_INCREMENT, SERIAL, GENERATED BY DEFAULT AS IDENTITY, and GENERATED ALWAYS AS IDENTITY
	default_value = 2,
	default_expression = 3,
};

namespace ColumnFlags {
	// these flags are serialized by name not value, so the values here can be changed if required
	typedef uint32_t flag_t;
	const flag_t nothing = 0;
	const flag_t mysql_timestamp = 1;
	const flag_t mysql_on_update_timestamp = 2;
	const flag_t time_zone = 4;
	const flag_t simple_geometry = 8;
	const flag_t identity_generated_always = 16;
};

struct Column {
	string name;
	bool nullable;
	string column_type;
	size_t size;
	size_t scale;
	DefaultType default_type;
	string default_value;
	ColumnFlags::flag_t flags;
	string type_restriction;
	string reference_system;
	vector<string> enumeration_values;

	// serialized but not compared; used only for passing along unknown column types so you get an intelligible error, and non-portable
	string db_type_def;

	// the following member isn't serialized currently (could be, but not required):
	string filter_expression;

	inline Column(const string &name, bool nullable, DefaultType default_type, string default_value, string column_type, size_t size = 0, size_t scale = 0, ColumnFlags::flag_t flags = ColumnFlags::nothing, const string &type_restriction = "", const string &reference_system = "", const string &db_type_def = ""): name(name), nullable(nullable), default_type(default_type), default_value(default_value), column_type(column_type), size(size), scale(scale), flags(flags), type_restriction(type_restriction), reference_system(reference_system), db_type_def(db_type_def) {}
	inline Column(): nullable(true), size(0), scale(0), default_type(DefaultType::no_default), flags(ColumnFlags::nothing) {}

	inline bool operator ==(const Column &other) const {
		return (name == other.name &&
				nullable == other.nullable &&
				column_type == other.column_type &&
				size == other.size &&
				scale == other.scale &&
				default_type == other.default_type &&
				default_value == other.default_value &&
				flags == other.flags &&
				type_restriction == other.type_restriction &&
				reference_system == other.reference_system &&
				enumeration_values == other.enumeration_values);
	}
	inline bool operator !=(const Column &other) const { return (!(*this == other)); }
};

typedef vector<Column> Columns;
typedef vector<string> ColumnNames;

enum class KeyType {
	unique_key = 0,
	standard_key = 1,
	spatial_key = 2,
};

struct Key {
	string name;
	KeyType key_type;
	ColumnIndices columns;

	inline Key(const string &name, KeyType key_type): name(name), key_type(key_type) {}
	inline Key(): key_type(KeyType::standard_key) {}

	inline bool unique() const { return (key_type == KeyType::unique_key); }
	inline bool spatial() const { return (key_type == KeyType::spatial_key); }

	inline bool operator <(const Key &other) const { return (key_type != other.key_type ? key_type < other.key_type : name < other.name); }
	inline bool operator ==(const Key &other) const { return (name == other.name && key_type == other.key_type && columns == other.columns); }
	inline bool operator !=(const Key &other) const { return (!(*this == other)); }
};

typedef vector<Key> Keys;

enum class PrimaryKeyType {
	no_available_key = 0,
	explicit_primary_key = 1,
	suitable_unique_key = 2,
};

struct Table {
	string name;
	Columns columns;
	ColumnIndices primary_key_columns;
	PrimaryKeyType primary_key_type = PrimaryKeyType::no_available_key;
	Keys keys;

	// the following member isn't serialized currently (could be, but not required):
	string where_conditions;

	inline Table(const string &name): name(name) {}
	inline Table() {}

	inline bool operator <(const Table &other) const { return (name < other.name); }
	inline bool operator ==(const Table &other) const { return (name == other.name && columns == other.columns && same_primary_key_as(other) && keys == other.keys); }
	inline bool operator !=(const Table &other) const { return (!(*this == other)); }
	size_t index_of_column(const string &name) const;

protected:
	inline bool same_primary_key_as(const Table &other) const {
		size_t this_explicit_columns = primary_key_type == PrimaryKeyType::explicit_primary_key ? primary_key_columns.size() : 0;
		size_t that_explicit_columns = other.primary_key_type == PrimaryKeyType::explicit_primary_key ? other.primary_key_columns.size() : 0;
		return (this_explicit_columns == that_explicit_columns && equal(primary_key_columns.begin(), primary_key_columns.begin() + this_explicit_columns, other.primary_key_columns.begin()));
	}
};

typedef vector<Table> Tables;

struct Database {
	Tables tables;
};

#endif
