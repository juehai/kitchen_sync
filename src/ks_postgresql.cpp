#include "endpoint.h"

#include <stdexcept>
#include <set>
#include <cctype>
#include <libpq-fe.h>

#include "schema.h"
#include "database_client_traits.h"
#include "sql_functions.h"
#include "row_printer.h"
#include "ewkb.h"

struct TypeMap {
	set<Oid> geometry;
};

enum PostgreSQLColumnConversion {
	encode_raw,
	encode_bool,
	encode_sint,
	encode_bytea,
	encode_geom,
};

class PostgreSQLRes {
public:
	PostgreSQLRes(PGresult *res, const TypeMap &type_map);
	~PostgreSQLRes();

	inline PGresult *res() { return _res; }
	inline ExecStatusType status() { return PQresultStatus(_res); }
	inline size_t rows_affected() const { return atoi(PQcmdTuples(_res)); }
	inline int n_tuples() const  { return _n_tuples; }
	inline int n_columns() const { return _n_columns; }
	inline PostgreSQLColumnConversion conversion_for(int column_number) { if (conversions.empty()) populate_conversions(); return conversions[column_number]; }

private:
	void populate_conversions();
	PostgreSQLColumnConversion conversion_for_type(Oid typid);

	PGresult *_res;
	const TypeMap &_type_map;
	int _n_tuples;
	int _n_columns;
	vector<PostgreSQLColumnConversion> conversions;
};

PostgreSQLRes::PostgreSQLRes(PGresult *res, const TypeMap &type_map): _res(res), _type_map(type_map) {
	_n_tuples = PQntuples(_res);
	_n_columns = PQnfields(_res);
}

PostgreSQLRes::~PostgreSQLRes() {
	if (_res) {
		PQclear(_res);
	}
}

void PostgreSQLRes::populate_conversions() {
	conversions.resize(_n_columns);

	for (size_t i = 0; i < _n_columns; i++) {
		Oid typid = PQftype(_res, i);
		conversions[i] = conversion_for_type(typid);
	}
}

// from pg_type.h, which isn't available/working on all distributions.
#define BOOLOID			16
#define BYTEAOID		17
#define CHAROID			18
#define INT2OID			21
#define INT4OID			23
#define INT8OID			20
#define TEXTOID			25

PostgreSQLColumnConversion PostgreSQLRes::conversion_for_type(Oid typid) {
	switch (typid) {
		case BOOLOID:
			return encode_bool;

		case INT2OID:
		case INT4OID:
		case INT8OID:
			return encode_sint;

		case BYTEAOID:
			return encode_bytea;

		case CHAROID:
		case TEXTOID:
			return encode_raw; // so this is actually just an optimised version of the default block below

		default:
			// because the Geometry type comes from the PostGIS extension, its OID isn't a constant, so we can't use it in a case statement. we've also
			// used a set instead of a scalar in case there's somehow more than one OID found (presumably from different installs of the extension).
			if (_type_map.geometry.count(typid)) {
				return encode_geom;
			} else {
				return encode_raw;
			}
	}
}


class PostgreSQLRow {
public:
	inline PostgreSQLRow(PostgreSQLRes &res, int row_number): _res(res), _row_number(row_number) { }
	inline const PostgreSQLRes &results() const { return _res; }

	inline         int n_columns() const { return _res.n_columns(); }

	inline        bool   null_at(int column_number) const { return PQgetisnull(_res.res(), _row_number, column_number); }
	inline const char *result_at(int column_number) const { return PQgetvalue (_res.res(), _row_number, column_number); }
	inline         int length_of(int column_number) const { return PQgetlength(_res.res(), _row_number, column_number); }
	inline      string string_at(int column_number) const { return string(result_at(column_number), length_of(column_number)); }
	inline        bool   bool_at(int column_number) const { return (strcmp(result_at(column_number), "t") == 0); }
	inline     int64_t    int_at(int column_number) const { return strtoll(result_at(column_number), nullptr, 10); }
	inline    uint64_t   uint_at(int column_number) const { return strtoull(result_at(column_number), nullptr, 10); }

	template <typename Packer>
	inline void pack_column_into(Packer &packer, int column_number) const {
		if (null_at(column_number)) {
			packer << nullptr;
		} else {
			switch (_res.conversion_for(column_number)) {
				case encode_bool:
					packer << bool_at(column_number);
					break;

				case encode_sint:
					packer << int_at(column_number);
					break;

				case encode_bytea: {
					size_t decoded_length;
					void *decoded = PQunescapeBytea((const unsigned char *)result_at(column_number), &decoded_length);
					packer << uncopied_byte_string(decoded, decoded_length);
					PQfreemem(decoded);
					break;
				}

				case encode_geom:
					packer << ewkb_hex_to_standard_geom_bin(result_at(column_number), length_of(column_number));
					break;

				case encode_raw:
					packer << uncopied_byte_string(result_at(column_number), length_of(column_number));
					break;
			}
		}
	}

	template <typename Packer>
	void pack_row_into(Packer &packer) const {
		pack_array_length(packer, n_columns());

		for (size_t column_number = 0; column_number < n_columns(); column_number++) {
			pack_column_into(packer, column_number);
		}
	}

private:
	PostgreSQLRes &_res;
	int _row_number;
};


class PostgreSQLClient: public GlobalKeys, public SequenceColumns, public DropKeysWhenColumnsDropped, public SetNullability {
public:
	typedef PostgreSQLRow RowType;

	PostgreSQLClient(
		const string &database_host,
		const string &database_port,
		const string &database_name,
		const string &database_username,
		const string &database_password,
		const string &variables);
	~PostgreSQLClient();

	void disable_referential_integrity();
	void enable_referential_integrity();
	string export_snapshot();
	void import_snapshot(const string &snapshot);
	void unhold_snapshot();
	void start_read_transaction();
	void start_write_transaction();
	void commit_transaction();
	void rollback_transaction();
	void populate_database_schema(Database &database);
	void convert_unsupported_database_schema(Database &database);
	string escape_string_value(const string &value);
	string &append_escaped_string_value_to(string &result, const string &value);
	string &append_escaped_bytea_value_to(string &result, const string &value);
	string &append_escaped_spatial_value_to(string &result, const string &value);
	string &append_escaped_column_value_to(string &result, const Column &column, const string &value);
	string column_type(const Column &column);
	string column_sequence_name(const Table &table, const Column &column);
	string column_default(const Table &table, const Column &column);
	string column_definition(const Table &table, const Column &column);

	inline string quote_identifier(const string &name) { return ::quote_identifier(name, '"'); };
	inline ColumnFlags supported_flags() const { return ColumnFlags::time_zone; }

	size_t execute(const string &sql);
	string select_one(const string &sql);

	template <typename RowFunction>
	size_t query(const string &sql, RowFunction &row_handler) {
		PostgreSQLRes res(PQexecParams(conn, sql.c_str(), 0, nullptr, nullptr, nullptr, nullptr, 0 /* text-format results only */), type_map);

		if (res.status() != PGRES_TUPLES_OK) {
			throw runtime_error(sql_error(sql));
		}

		for (int row_number = 0; row_number < res.n_tuples(); row_number++) {
			PostgreSQLRow row(res, row_number);
			row_handler(row);
		}

		return res.n_tuples();
	}

protected:
	string sql_error(const string &sql);

private:
	PGconn *conn;
	TypeMap type_map;

	// forbid copying
	PostgreSQLClient(const PostgreSQLClient &_) = delete;
	PostgreSQLClient &operator=(const PostgreSQLClient &_) = delete;
};

PostgreSQLClient::PostgreSQLClient(
	const string &database_host,
	const string &database_port,
	const string &database_name,
	const string &database_username,
	const string &database_password,
	const string &variables) {

	const char *keywords[] = { "host",                "port",                "dbname",              "user",                    "password",                nullptr };
	const char *values[]   = { database_host.c_str(), database_port.c_str(), database_name.c_str(), database_username.c_str(), database_password.c_str(), nullptr };

	conn = PQconnectdbParams(keywords, values, 1 /* allow expansion */);

	if (PQstatus(conn) != CONNECTION_OK) {
		throw runtime_error(PQerrorMessage(conn));
	}
	if (PQsetClientEncoding(conn, "SQL_ASCII")) {
		throw runtime_error(PQerrorMessage(conn));
	}

	execute("SET client_min_messages TO WARNING");

	if (!variables.empty()) {
		execute("SET " + variables);
	}
}

PostgreSQLClient::~PostgreSQLClient() {
	if (conn) {
		PQfinish(conn);
	}
}

size_t PostgreSQLClient::execute(const string &sql) {
    PostgreSQLRes res(PQexec(conn, sql.c_str()), type_map);

    if (res.status() != PGRES_COMMAND_OK && res.status() != PGRES_TUPLES_OK) {
		throw runtime_error(sql_error(sql));
    }

    return res.rows_affected();
}

string PostgreSQLClient::select_one(const string &sql) {
	PostgreSQLRes res(PQexecParams(conn, sql.c_str(), 0, nullptr, nullptr, nullptr, nullptr, 0 /* text-format results only */), type_map);

	if (res.status() != PGRES_TUPLES_OK) {
		throw runtime_error(sql_error(sql));
	}

	if (res.n_tuples() != 1 || res.n_columns() != 1) {
		throw runtime_error("Expected query to return only one row with only one column\n" + sql);
	}

	return PostgreSQLRow(res, 0).string_at(0);
}

string PostgreSQLClient::sql_error(const string &sql) {
	if (sql.size() < 200) {
		return PQerrorMessage(conn) + string("\n") + sql;
	} else {
		return PQerrorMessage(conn) + string("\n") + sql.substr(0, 200) + "...";
	}
}

void PostgreSQLClient::start_read_transaction() {
	execute("START TRANSACTION READ ONLY ISOLATION LEVEL REPEATABLE READ");
}

void PostgreSQLClient::start_write_transaction() {
	execute("START TRANSACTION ISOLATION LEVEL READ COMMITTED");
}

void PostgreSQLClient::commit_transaction() {
	execute("COMMIT");
}

void PostgreSQLClient::rollback_transaction() {
	execute("ROLLBACK");
}

string PostgreSQLClient::export_snapshot() {
	// postgresql has transactional DDL, so by starting our transaction before we've even looked at the tables,
	// we'll get a 100% consistent view.
	execute("START TRANSACTION READ ONLY ISOLATION LEVEL REPEATABLE READ");
	return select_one("SELECT pg_export_snapshot()");
}

void PostgreSQLClient::import_snapshot(const string &snapshot) {
	execute("START TRANSACTION READ ONLY ISOLATION LEVEL REPEATABLE READ");
	execute("SET TRANSACTION SNAPSHOT '" + escape_string_value(snapshot) + "'");
}

void PostgreSQLClient::unhold_snapshot() {
	// do nothing - only needed for lock-based systems like mysql
}

void PostgreSQLClient::disable_referential_integrity() {
	execute("SET CONSTRAINTS ALL DEFERRED");

	/* TODO: investigate the pros and cons of disabling triggers - this blocks if there's a read transaction open
	for (const Table &table : database.tables) {
		execute("ALTER TABLE " + client.quote_identifier(table.name) + " DISABLE TRIGGER ALL");
	}
	*/
}

void PostgreSQLClient::enable_referential_integrity() {
	/* TODO: investigate the pros and cons of disabling triggers - this blocks if there's a read transaction open
	for (const Table &table : database.tables) {
		execute("ALTER TABLE " + client.quote_identifier(table.name) + " ENABLE TRIGGER ALL");
	}
	*/
}

string PostgreSQLClient::escape_string_value(const string &value) {
	string result;
	result.resize(value.size()*2 + 1);
	size_t result_length = PQescapeStringConn(conn, (char*)result.data(), value.c_str(), value.size(), nullptr);
	result.resize(result_length);
	return result;
}

string &PostgreSQLClient::append_escaped_string_value_to(string &result, const string &value) {
	string buffer;
	buffer.resize(value.size()*2 + 1);
	size_t result_length = PQescapeStringConn(conn, (char*)buffer.data(), value.c_str(), value.size(), nullptr);
	result += '\'';
	result.append(buffer, 0, result_length);
	result += '\'';
	return result;
}

string &PostgreSQLClient::append_escaped_bytea_value_to(string &result, const string &value) {
	size_t encoded_length;
	const unsigned char *encoded = PQescapeByteaConn(conn, (const unsigned char *)value.c_str(), value.size(), &encoded_length);
	result += '\'';
	result.append(encoded, encoded + encoded_length - 1); // encoded_length includes the null terminator
	result += '\'';
	PQfreemem((void *)encoded);
	return result;
}

string &PostgreSQLClient::append_escaped_spatial_value_to(string &result, const string &value) {
	result.append("ST_GeomFromWKB(");
	append_escaped_bytea_value_to(result, value.substr(4));
	result.append(",");
	result.append(to_string(*(uint32_t*)value.c_str()));
	result.append(")");
	return result;
}

string &PostgreSQLClient::append_escaped_column_value_to(string &result, const Column &column, const string &value) {
	if (column.column_type == ColumnTypes::BLOB) {
		return append_escaped_bytea_value_to(result, value);
	} else if (column.column_type == ColumnTypes::SPAT) {
		return append_escaped_spatial_value_to(result, value);
	} else {
		return append_escaped_string_value_to(result, value);
	}
}

void PostgreSQLClient::convert_unsupported_database_schema(Database &database) {
	for (Table &table : database.tables) {
		for (Column &column : table.columns) {
			if (column.column_type == ColumnTypes::UINT) {
				// postgresql doesn't support unsigned columns; to make migration from databases that do
				// easier, we don't reject unsigned columns, we just convert them to the signed equivalent
				// and rely on it raising if we try to insert an invalid value
				column.column_type = ColumnTypes::SINT;
			}

			if (column.column_type == ColumnTypes::SINT && column.size == 1) {
				// not used by postgresql; smallint is the nearest equivalent
				column.size = 2;
			}

			if (column.column_type == ColumnTypes::SINT && column.size == 3) {
				// not used by postgresql; integer is the nearest equivalent
				column.size = 4;
			}

			if (column.column_type == ColumnTypes::TEXT || column.column_type == ColumnTypes::BLOB) {
				// postgresql doesn't have different sized TEXT/BLOB columns, they're all equivalent to mysql's biggest type
				column.size = 0;
			}
		}

		for (Key &key : table.keys) {
			if (key.name.size() >= 63) {
				// postgresql has a hardcoded limit of 63 characters for index names
				key.name = key.name.substr(0, 63);
			}
		}
	}
}

string PostgreSQLClient::column_type(const Column &column) {
	if (column.column_type == ColumnTypes::BLOB) {
		return "bytea";

	} else if (column.column_type == ColumnTypes::TEXT) {
		return "text";

	} else if (column.column_type == ColumnTypes::VCHR) {
		string result("character varying");
		if (column.size > 0) {
			result += '(';
			result += to_string(column.size);
			result += ')';
		}
		return result;

	} else if (column.column_type == ColumnTypes::FCHR) {
		string result("character(");
		result += to_string(column.size);
		result += ')';
		return result;

	} else if (column.column_type == ColumnTypes::UUID) {
		return "uuid";

	} else if (column.column_type == ColumnTypes::BOOL) {
		return "boolean";

	} else if (column.column_type == ColumnTypes::SINT) {
		switch (column.size) {
			case 2:
				return "smallint";

			case 4:
				return "integer";

			case 8:
				return "bigint";

			default:
				throw runtime_error("Don't know how to create integer column " + column.name + " of size " + to_string(column.size));
		}

	} else if (column.column_type == ColumnTypes::REAL) {
		return (column.size == 4 ? "real" : "double precision");

	} else if (column.column_type == ColumnTypes::DECI) {
		if (column.size) {
			string result("numeric(");
			result += to_string(column.size);
			result += ',';
			result += to_string(column.scale);
			result += ')';
			return result;
		} else {
			return "numeric";
		}

	} else if (column.column_type == ColumnTypes::DATE) {
		return "date";

	} else if (column.column_type == ColumnTypes::TIME) {
		if (column.flags & ColumnFlags::time_zone) {
			return "time with time zone";
		} else {
			return "time without time zone";
		}

	} else if (column.column_type == ColumnTypes::DTTM) {
		if (column.flags & ColumnFlags::time_zone) {
			return "timestamp with time zone";
		} else {
			return "timestamp without time zone";
		}

	} else if (column.column_type == ColumnTypes::SPAT) {
		// note that we have made the assumption that all the mysql geometry types should be mapped to
		// PostGIS GEOMETRY objects, rather than to the built-in geometric types such as POINT, because
		// postgresql's built-in geometric types don't support spatial reference systems (SRIDs), don't
		// have any equivalent to the multi* types, the built-in POLYGON type doesn't support 'holes' (as
		// created using the two-argument form on mysql). we haven't yet looked at the geography types.
		string result("geometry");
		if (!column.reference_system.empty()) {
			result += '(';
			result += (column.type_restriction.empty() ? string("geometry") : column.type_restriction);
			result += ',';
			result += column.reference_system;
			result += ')';
		} else if (!column.type_restriction.empty()) {
			result += '(';
			result += column.type_restriction;
			result += ')';
		}
		return result;

	} else {
		throw runtime_error("Don't know how to express column type of " + column.name + " (" + column.column_type + ")");
	}
}

string PostgreSQLClient::column_sequence_name(const Table &table, const Column &column) {
	// name to match what postgresql creates for serial columns
	return table.name + "_" + column.name + "_seq";
}

string PostgreSQLClient::column_default(const Table &table, const Column &column) {
	string result(" DEFAULT ");

	switch (column.default_type) {
		case DefaultType::no_default:
			result += "NULL";
			break;

		case DefaultType::sequence:
			result += "nextval('";
			result += escape_string_value(column_sequence_name(table, column));
			result += "'::regclass)";
			break;

		case DefaultType::default_value:
			if (column.column_type == ColumnTypes::BOOL ||
				column.column_type == ColumnTypes::SINT ||
				column.column_type == ColumnTypes::UINT ||
				column.column_type == ColumnTypes::REAL ||
				column.column_type == ColumnTypes::DECI) {
				result += column.default_value;
			} else {
				append_escaped_column_value_to(result, column, column.default_value);
			}
			break;

		case DefaultType::default_expression:
			result += column.default_value;

		default:
			throw runtime_error("Don't know how to express default of " + column.name + " (" + to_string(column.default_type) + ")");
	}

	return result;
}

string PostgreSQLClient::column_definition(const Table &table, const Column &column) {
	string result;
	result += quote_identifier(column.name);
	result += ' ';

	result += column_type(column);

	if (!column.nullable) {
		result += " NOT NULL";
	}

	if (column.default_type) {
		result += column_default(table, column);
	}

	return result;
}

struct PostgreSQLColumnLister {
	inline PostgreSQLColumnLister(Table &table): table(table) {}

	inline void operator()(PostgreSQLRow &row) {
		string name(row.string_at(0));
		string db_type(row.string_at(1));
		bool nullable(row.string_at(2) == "f");
		DefaultType default_type(DefaultType::no_default);
		string default_value;

		if (row.string_at(3) == "t") {
			default_type = DefaultType::default_value;
			default_value = row.string_at(4);

			if (default_value.length() > 20 &&
				default_value.substr(0, 9) == "nextval('" &&
				default_value.substr(default_value.length() - 12, 12) == "'::regclass)") {
				default_type = DefaultType::sequence;
				default_value = "";

			} else if (default_value.substr(0, 6) == "NULL::" && db_type.substr(0, default_value.length() - 6) == default_value.substr(6)) {
				// postgresql treats a NULL default as distinct to no default, so we try to respect that by keeping the value as a function,
				// but chop off the type conversion for the sake of portability
				default_type = DefaultType::default_expression;
				default_value = "NULL";

			} else if (default_value.length() > 2 && default_value[0] == '\'') {
				default_value = unescape_string_value(default_value.substr(1, default_value.rfind('\'') - 1));

			} else if (default_value.length() > 0 && default_value != "false" && default_value != "true" && default_value.find_first_not_of("0123456789.") != string::npos) {
				default_type = DefaultType::default_expression;

				// postgresql converts CURRENT_TIMESTAMP to now(); convert it back for portability
				if (default_value == "now()") {
					default_value = "CURRENT_TIMESTAMP";

				// do the same for its conversion of CURRENT_DATE
				} else if (default_value == "('now'::text)::date") {
					default_value = "CURRENT_DATE";

				// other SQL-reserved zero-argument functions come back with quoted identifiers and brackets, see Note on the
				// 'System Information Functions' page; the list here is shorter because some get converted to one of the others by pg
				} else if (default_value == "\"current_schema\"()" || default_value == "\"current_user\"()" || default_value == "\"session_user\"()") {
					default_value = default_value.substr(1, default_value.length() - 4);
				}
			}
		}

		if (db_type == "boolean") {
			table.columns.emplace_back(name, nullable, default_type, default_value, ColumnTypes::BOOL);
		} else if (db_type == "smallint") {
			table.columns.emplace_back(name, nullable, default_type, default_value, ColumnTypes::SINT, 2);
		} else if (db_type == "integer") {
			table.columns.emplace_back(name, nullable, default_type, default_value, ColumnTypes::SINT, 4);
		} else if (db_type == "bigint") {
			table.columns.emplace_back(name, nullable, default_type, default_value, ColumnTypes::SINT, 8);
		} else if (db_type == "real") {
			table.columns.emplace_back(name, nullable, default_type, default_value, ColumnTypes::REAL, 4);
		} else if (db_type == "double precision") {
			table.columns.emplace_back(name, nullable, default_type, default_value, ColumnTypes::REAL, 8);
		} else if (db_type.substr(0, 8) == "numeric(") {
			table.columns.emplace_back(name, nullable, default_type, default_value, ColumnTypes::DECI, extract_column_length(db_type), extract_column_scale(db_type));
		} else if (db_type.substr(0, 7) == "numeric") {
			table.columns.emplace_back(name, nullable, default_type, default_value, ColumnTypes::DECI);
		} else if (db_type.substr(0, 18) == "character varying(") {
			table.columns.emplace_back(name, nullable, default_type, default_value, ColumnTypes::VCHR, extract_column_length(db_type));
		} else if (db_type.substr(0, 18) == "character varying") {
			table.columns.emplace_back(name, nullable, default_type, default_value, ColumnTypes::VCHR /* no length limit */);
		} else if (db_type.substr(0, 10) == "character(") {
			table.columns.emplace_back(name, nullable, default_type, default_value, ColumnTypes::FCHR, extract_column_length(db_type));
		} else if (db_type == "text") {
			table.columns.emplace_back(name, nullable, default_type, default_value, ColumnTypes::TEXT);
		} else if (db_type == "bytea") {
			table.columns.emplace_back(name, nullable, default_type, default_value, ColumnTypes::BLOB);
		} else if (db_type == "uuid") {
			table.columns.emplace_back(name, nullable, default_type, default_value, ColumnTypes::UUID);
		} else if (db_type == "date") {
			table.columns.emplace_back(name, nullable, default_type, default_value, ColumnTypes::DATE);
		} else if (db_type == "time without time zone") {
			table.columns.emplace_back(name, nullable, default_type, default_value, ColumnTypes::TIME);
		} else if (db_type == "time with time zone") {
			table.columns.emplace_back(name, nullable, default_type, default_value, ColumnTypes::TIME, 0, 0, ColumnFlags::time_zone);
		} else if (db_type == "timestamp without time zone") {
			table.columns.emplace_back(name, nullable, default_type, default_value, ColumnTypes::DTTM);
		} else if (db_type == "timestamp with time zone") {
			table.columns.emplace_back(name, nullable, default_type, default_value, ColumnTypes::DTTM, 0, 0, ColumnFlags::time_zone);
		} else if (db_type == "geometry") {
			table.columns.emplace_back(name, nullable, default_type, default_value, ColumnTypes::SPAT);
		} else if (db_type.substr(0, 9) == "geometry(") {
			string type_restriction, reference_system;
			tie(type_restriction, reference_system) = extract_spatial_type_restriction_and_reference_system(db_type.substr(9, db_type.length() - 10));
			table.columns.emplace_back(name, nullable, default_type, default_value, ColumnTypes::SPAT, 0, 0, ColumnFlags::nothing, type_restriction, reference_system);
		} else {
			// not supported, but leave it till sync_to's check_tables_usable to complain about it so that it can be ignored
			table.columns.emplace_back(name, nullable, default_type, default_value, ColumnTypes::UNKN, 0, 0, ColumnFlags::nothing, "", "", db_type);
		}
	}

	inline string unescape_string_value(const string &escaped) {
		string result;
		result.reserve(escaped.length());
		for (string::size_type n = 0; n < escaped.length(); n++) {
			// this is by no means a complete unescaping function, it only handles the cases seen in
			// the output of pg_get_expr so far.  note that pg does not interpret regular character
			// escapes such as \t and \n when outputting these default definitions.
			if (escaped[n] == '\\' || escaped[n] == '\'') {
				n += 1;
			}
			result += escaped[n];
		}
		return result;
	}

	inline tuple<string, string> extract_spatial_type_restriction_and_reference_system(string type_restriction) {
		transform(type_restriction.begin(), type_restriction.end(), type_restriction.begin(), [](unsigned char c){ return tolower(c); });

		size_t comma_pos = type_restriction.find(',');
		if (comma_pos == string::npos) {
			return make_tuple(type_restriction, "");
		}

		string reference_system(type_restriction.substr(comma_pos + 1));
		type_restriction.resize(comma_pos);
		if (type_restriction == "geometry") type_restriction.clear();

		return make_tuple(type_restriction, reference_system);
	}

	Table &table;
};

struct PostgreSQLPrimaryKeyLister {
	inline PostgreSQLPrimaryKeyLister(Table &table): table(table) {}

	inline void operator()(PostgreSQLRow &row) {
		string column_name = row.string_at(0);
		size_t column_index = table.index_of_column(column_name);
		table.primary_key_columns.push_back(column_index);
		table.primary_key_type = explicit_primary_key;
	}

	Table &table;
};

struct PostgreSQLKeyLister {
	inline PostgreSQLKeyLister(Table &table): table(table) {}

	inline void operator()(PostgreSQLRow &row) {
		// if we have no primary key, we might need to use another unique key as a surrogate - see PostgreSQLTableLister below
		// furthermore this key must have no NULLable columns, as they effectively make the index not unique
		string key_name = row.string_at(0);
		bool unique = (row.string_at(1) == "t");
		string column_name = row.string_at(2);
		size_t column_index = table.index_of_column(column_name);
		// FUTURE: consider representing collation, index type, partial keys etc.

		if (table.keys.empty() || table.keys.back().name != key_name) {
			table.keys.push_back(Key(key_name, unique));
		}
		table.keys.back().columns.push_back(column_index);
	}

	Table &table;
};

struct PostgreSQLTableLister {
	PostgreSQLTableLister(PostgreSQLClient &client, Database &database): client(client), database(database) {}

	void operator()(PostgreSQLRow &row) {
		Table table(row.string_at(0));

		PostgreSQLColumnLister column_lister(table);
		client.query(
			"SELECT attname, format_type(atttypid, atttypmod), attnotnull, atthasdef, pg_get_expr(adbin, adrelid) "
			  "FROM pg_attribute "
			  "JOIN pg_class ON attrelid = pg_class.oid "
			  "JOIN pg_type ON atttypid = pg_type.oid "
			  "LEFT JOIN pg_attrdef ON adrelid = attrelid AND adnum = attnum "
			 "WHERE attnum > 0 AND "
			       "NOT attisdropped AND "
			       "relname = '" + client.escape_string_value(table.name) + "' "
			 "ORDER BY attnum",
			column_lister);

		PostgreSQLPrimaryKeyLister primary_key_lister(table);
		client.query(
			"SELECT column_name "
			  "FROM information_schema.table_constraints, "
			       "information_schema.key_column_usage "
			 "WHERE information_schema.table_constraints.table_name = '" + client.escape_string_value(table.name) + "' AND "
			       "information_schema.key_column_usage.table_name = information_schema.table_constraints.table_name AND "
			       "constraint_type = 'PRIMARY KEY' "
			 "ORDER BY ordinal_position",
			primary_key_lister);

		PostgreSQLKeyLister key_lister(table);
		client.query(
			"SELECT indexname, indisunique, attname "
			  "FROM (SELECT table_class.oid AS table_oid, index_class.relname AS indexname, pg_index.indisunique, generate_series(1, array_length(indkey, 1)) AS position, unnest(indkey) AS attnum "
			          "FROM pg_class table_class, pg_class index_class, pg_index "
			         "WHERE table_class.relname = '" + client.escape_string_value(table.name) + "' AND "
			               "table_class.relkind = 'r' AND "
			               "index_class.relkind = 'i' AND "
			               "pg_index.indrelid = table_class.oid AND "
			               "pg_index.indexrelid = index_class.oid AND "
			               "NOT pg_index.indisprimary) index_attrs,"
			       "pg_attribute "
			 "WHERE pg_attribute.attrelid = table_oid AND "
			       "pg_attribute.attnum = index_attrs.attnum "
			 "ORDER BY indexname, index_attrs.position",
			key_lister);

		sort(table.keys.begin(), table.keys.end()); // order is arbitrary for keys, but both ends must be consistent, so we sort the keys by name

		database.tables.push_back(table);
	}

	PostgreSQLClient &client;
	Database &database;
};

struct PostgreSQLTypeMapCollector {
	PostgreSQLTypeMapCollector(TypeMap &type_map): type_map(type_map) {}

	void operator()(PostgreSQLRow &row) {
		uint32_t oid(row.uint_at(0));
		string typname(row.string_at(1));

		if (typname == "geometry") {
			type_map.geometry.insert(oid);
		}
	}

	TypeMap &type_map;
};

void PostgreSQLClient::populate_database_schema(Database &database) {
	PostgreSQLTableLister table_lister(*this, database);
	query("SELECT pg_class.relname "
		    "FROM pg_class, pg_namespace "
		   "WHERE pg_class.relnamespace = pg_namespace.oid AND "
		         "pg_namespace.nspname = ANY (current_schemas(false)) AND "
		         "relkind = 'r' "
		"ORDER BY pg_relation_size(pg_class.oid) DESC, relname ASC",
		 table_lister);

	PostgreSQLTypeMapCollector type_collector(type_map);
	query("SELECT pg_type.oid, pg_type.typname "
		    "FROM pg_type, pg_namespace "
		   "WHERE pg_type.typnamespace = pg_namespace.oid AND "
		         "pg_namespace.nspname = ANY (current_schemas(false)) AND "
		         "pg_type.typname IN ('geometry')",
		  type_collector);
}


int main(int argc, char *argv[]) {
	return endpoint_main<PostgreSQLClient>(argc, argv);
}
