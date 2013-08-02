#include <iostream>
#include <unistd.h>

#include "schema.h"
#include "schema_functions.h"

template<class T>
void sync_to(T &client) {
	const int PROTOCOL_VERSION_SUPPORTED = 1;

	// tell the other end what protocol we speak
	Stream stream(STDIN_FILENO);
	cout << Command("protocol", PROTOCOL_VERSION_SUPPORTED);
	int protocol;
	stream.read_and_unpack(protocol);

	// get its schema
	cout << Command("schema");
	Database from_database;
	stream.read_and_unpack(from_database);

	// get our end's schema
	Database to_database(client.database_schema());

	// check they match
	check_schema_match(from_database, to_database);

	cout << Command("quit");
	close(STDOUT_FILENO);
	close(STDIN_FILENO);
}
