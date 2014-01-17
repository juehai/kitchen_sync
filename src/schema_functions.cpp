#include "schema_functions.h"

#include <algorithm>
#include "sql_functions.h"

template <typename T>
bool name_equal(const T &a, const T &b) {
	return (a.name == b.name);
};

template <typename T>
struct name_is {
	const string &name;
	name_is(const string &name): name(name) {}
	bool operator()(const T& obj) const {
		return (obj.name == name);
	}
};

void report_schema_mismatch(const string &error) {
	// FUTURE: can we implement some kind of non-fatal mismatch handling?
	throw schema_mismatch(error);
}

void check_column_match(const Table &table, const Column &from_column, const Column &to_column) {
	// FUTURE: check column type, collation etc.
}

void check_columns_match(const Table &table, const Columns &from_columns, const Columns &to_columns) {
	Columns::const_iterator to_column = to_columns.begin();
	for (Columns::const_iterator from_column = from_columns.begin(); from_column != from_columns.end(); ++from_column) {
		if (to_column != to_columns.end() && to_column->name == from_column->name) {
			check_column_match(table, *from_column, *to_column);
			++to_column;

		} else if (find_if(to_column, to_columns.end(), name_is<Column>(from_column->name)) == to_columns.end()) {
			report_schema_mismatch("Missing column " + from_column->name + " on table " + table.name);

		} else if (find_if(from_column, from_columns.end(), name_is<Column>(to_column->name)) == from_columns.end()) {
			report_schema_mismatch("Extra column " + to_column->name + " on table " + table.name);

		} else {
			report_schema_mismatch("Misordered column " + from_column->name + " on table " + table.name + ", should have " + to_column->name + " first");
		}
	}
	if (to_column != to_columns.end()) {
		report_schema_mismatch("Extra column " + to_column->name + " on table " + table.name);
	}
}

void check_primary_key_matches(const Table &table, const ColumnIndices &from_primary_key_columns, const ColumnIndices &to_primary_key_columns) {
	if (from_primary_key_columns != to_primary_key_columns) {
		report_schema_mismatch("Mismatching primary key " + columns_list(table.columns, to_primary_key_columns) + " on table " + table.name + ", should have " + columns_list(table.columns, from_primary_key_columns));
	}
}

void check_key_match(const Table &table, const Key &from_key, const Key &to_key) {
	if (from_key.unique != to_key.unique) {
		report_schema_mismatch("Mismatching unique flag on table " + table.name + " key " + from_key.name);
	}
	if (from_key.columns != to_key.columns) {
		report_schema_mismatch("Mismatching columns " + columns_list(table.columns, to_key.columns) + " on table " + table.name + " key " + from_key.name + ", should have " + columns_list(table.columns, from_key.columns));
	}
}

void check_keys_match(const Table &table, Keys from_keys, Keys to_keys) {
	// the keys should already be given in a consistent sorted order, but our algorithm requires it, so we quickly enforce it here
	sort(from_keys.begin(), from_keys.end());
	sort(  to_keys.begin(),   to_keys.end());

	Keys::const_iterator to_key = to_keys.begin();
	for (Keys::const_iterator from_key = from_keys.begin(); from_key != from_keys.end(); ++from_key) {
		if (to_key == to_keys.end() || to_key->name > from_key->name) {
			report_schema_mismatch("Missing key " + from_key->name + " on table " + table.name);

		} else if (to_key->name < from_key->name) {
			report_schema_mismatch("Extra key " + to_key->name + " on table " + table.name);

		} else {
			check_key_match(table, *from_key, *to_key);
			++to_key;
		}
	}
	if (to_key != to_keys.end()) {
		report_schema_mismatch("Extra key " + to_key->name + " on table " + table.name);
	}
}

void check_table_match(const Table &from_table, const Table &to_table) {
	check_columns_match(from_table, from_table.columns, to_table.columns);
	check_primary_key_matches(from_table, from_table.primary_key_columns, to_table.primary_key_columns);
	check_keys_match(from_table, from_table.keys, to_table.keys);
	// FUTURE: check collation etc.
}

Tables::const_iterator skip_ignored_tables(Tables::const_iterator table, const Tables::const_iterator &end, const set<string> &ignore_tables, const set<string> &only_tables) {
	while (table != end && (ignore_tables.count(table->name) || (!only_tables.empty() && !only_tables.count(table->name)))) {
		++table;
	}
	return table;
}

void check_tables_match(Tables from_tables, Tables to_tables, const set<string> &ignore_tables, const set<string> &only_tables) {
	// databases typically return the tables in sorted order, but our algorithm requires it, so we quickly enforce it here
	sort(from_tables.begin(), from_tables.end());
	sort(  to_tables.begin(),   to_tables.end());

	Tables::const_iterator to_table = skip_ignored_tables(to_tables.begin(), to_tables.end(), ignore_tables, only_tables);
	for (Tables::const_iterator from_table = skip_ignored_tables(from_tables.begin(), from_tables.end(), ignore_tables, only_tables);
		 from_table != from_tables.end();
		 from_table = skip_ignored_tables(++from_table, from_tables.end(), ignore_tables, only_tables)) {
		
		if (to_table == to_tables.end() || to_table->name > from_table->name) {
			report_schema_mismatch("Missing table " + from_table->name);

		} else if (to_table->name < from_table->name) {
			report_schema_mismatch("Extra table " + to_table->name);

		} else {
			check_table_match(*from_table, *to_table);
			to_table = skip_ignored_tables(++to_table, to_tables.end(), ignore_tables, only_tables);
		}
	}
	if (to_table != to_tables.end()) {
		report_schema_mismatch("Extra table " + to_table->name);
	}
}

void check_schema_match(const Database &from_database, const Database &to_database, const set<string> &ignore_tables, const set<string> &only_tables) {
	// currently we only pay attention to tables, but in the future we might support other schema items
	check_tables_match(from_database.tables, to_database.tables, ignore_tables, only_tables);
}
