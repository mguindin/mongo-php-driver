#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <stdarg.h>
#include <sys/time.h>

#include "types.h"
#include "utils.h"
#include "manager.h"
#include "connections.h"
#include "collection.h"
#include "parse.h"
#include "read_preference.h"

/* Helpers */
static int authenticate_connection(mongo_con_manager *manager, mongo_connection *con, char *database, char *username, char *password, char **error_message)
{
	char *nonce;
	int   retval = 0;

	nonce = mongo_connection_getnonce(manager, con, error_message);
	if (!nonce) {
		return 0;
	}

	retval = mongo_connection_authenticate(manager, con, database, username, password, nonce, error_message);
	free(nonce);

	return retval;
}

static mongo_connection *mongo_get_connection_single(mongo_con_manager *manager, mongo_server_def *server, int connection_flags, char **error_message)
{
	char *hash;
	mongo_connection *con = NULL;

	hash = mongo_server_create_hash(server);
	con = mongo_manager_connection_find_by_hash(manager, hash);
	if (!con && !(connection_flags & MONGO_CON_FLAG_DONT_CONNECT)) {
		con = mongo_connection_create(manager, server, error_message);
		if (con) {
			/* Store hash */
			con->hash = strdup(hash);
			/* Do authentication if requested */
			if (server->db && server->username && server->password) {
				mongo_manager_log(manager, MLOG_CON, MLOG_INFO, "get_connection_single: authenticating %s", hash);
				if (!authenticate_connection(manager, con, server->db, server->username, server->password, error_message)) {
					mongo_connection_destroy(manager, con);
					con = NULL;
					goto bailout;
				}
			}
			/* Do the ping */
			if (!mongo_connection_ping(manager, con, error_message)) {
				mongo_connection_destroy(manager, con);
				con = NULL;
				goto bailout;
			}
			/* Register the connection */
			mongo_manager_connection_register(manager, con);
		}
	} else if (!(connection_flags & MONGO_CON_FLAG_DONT_CONNECT)) {
		/* Do the ping */
		if (!mongo_connection_ping(manager, con, error_message)) {
			mongo_manager_connection_deregister(manager, con);
			con = NULL;
			goto bailout;
		}
	}
bailout:
	free(hash);
	return con;
}

/* Topology discovery */

/* - Helpers */
static void mongo_discover_topology(mongo_con_manager *manager, mongo_servers *servers)
{
	int i, j;
	char *hash;
	mongo_connection *con;
	char *error_message;
	char *repl_set_name = servers->repl_set_name ? strdup(servers->repl_set_name) : NULL;
	int nr_hosts;
	char **found_hosts = NULL;
	char *tmp_hash;
	int   res;

	for (i = 0; i < servers->count; i++) {
		hash = mongo_server_create_hash(servers->server[i]);
		mongo_manager_log(manager, MLOG_CON, MLOG_FINE, "discover_topology: checking ismaster for %s", hash);
		con = mongo_manager_connection_find_by_hash(manager, hash);

		if (!con) {
			mongo_manager_log(manager, MLOG_CON, MLOG_WARN, "discover_topology: couldn't create a connection for %s", hash);
			free(hash);
			continue;
		}
		
		res = mongo_connection_ismaster(manager, con, (char**) &repl_set_name, (int*) &nr_hosts, (char***) &found_hosts, (char**) &error_message, servers->server[i]);
		switch (res) {
			case 0:
				/* Something is wrong with the connection, we need to remove
				 * this from our list */
				mongo_manager_log(manager, MLOG_CON, MLOG_WARN, "discover_topology: ismaster return with an error for %s:%d: [%s]", servers->server[i]->host, servers->server[i]->port, error_message);
				free(error_message);
				mongo_manager_connection_deregister(manager, con);
				break;

			case 3:
				mongo_manager_log(manager, MLOG_CON, MLOG_WARN, "discover_topology: ismaster worked, but we need to remove the seed host's connection");
				mongo_manager_connection_deregister(manager, con);
				/* Break intentionally missing */

			case 1:
				mongo_manager_log(manager, MLOG_CON, MLOG_INFO, "discover_topology: ismaster worked");
				for (j = 0; j < nr_hosts; j++) {
					mongo_server_def *tmp_def;
					mongo_connection *new_con;
					char *con_error_message = NULL;

					/* Create a temp server definition to create a new connection */
					tmp_def = calloc(1, sizeof(mongo_server_def));
					tmp_def->username = servers->server[i]->username ? strdup(servers->server[i]->username) : NULL;
					tmp_def->password = servers->server[i]->password ? strdup(servers->server[i]->password) : NULL;
					tmp_def->db = servers->server[i]->db ? strdup(servers->server[i]->db) : NULL;
					tmp_def->host = strndup(found_hosts[j], strchr(found_hosts[j], ':') - found_hosts[j]);
					tmp_def->port = atoi(strchr(found_hosts[j], ':') + 1);
					
					/* Create a hash so that we can check whether we already have a
					 * connection for this server definition. If we don't create
					 * the connection, register it (done in
					 * mongo_get_connection_single) and add it to the list of
					 * servers that we're processing so we might use this host to
					 * find more servers. */
					tmp_hash = mongo_server_create_hash(tmp_def);
					if (!mongo_manager_connection_find_by_hash(manager, tmp_hash)) {
						mongo_manager_log(manager, MLOG_CON, MLOG_INFO, "discover_topology: found new host: %s:%d", tmp_def->host, tmp_def->port);
						new_con = mongo_get_connection_single(manager, tmp_def, MONGO_CON_FLAG_WRITE, (char **) &con_error_message);
						if (new_con) {
							servers->server[servers->count] = tmp_def;
							servers->count++;
						} else {
							mongo_manager_log(manager, MLOG_CON, MLOG_INFO, "discover_topology: could not connect to new host: %s:%d: %s", tmp_def->host, tmp_def->port, con_error_message);
							free(con_error_message);
						}
					} else {
						mongo_server_def_dtor(tmp_def);
					}
					free(tmp_hash);

					/* Cleanup */
					free(found_hosts[j]);
				}
				free(found_hosts);
				found_hosts = NULL;
				break;

			case 2:
				mongo_manager_log(manager, MLOG_CON, MLOG_FINE, "discover_topology: ismaster got skipped");
				break;
		}

		free(hash);
	}
	if (repl_set_name) {
		free(repl_set_name);
	}
}

static mongo_connection *mongo_get_read_write_connection_replicaset(mongo_con_manager *manager, mongo_servers *servers, int connection_flags, char **error_message)
{
	mongo_connection *con = NULL;
	mongo_connection *tmp;
	mcon_collection  *collection;
	char             *con_error_message = NULL;
	char             *auth_hash = NULL;
	int i;
	int found_connected_server = 0;

	/* Create a connection to every of the servers in the seed list */
	for (i = 0; i < servers->count; i++) {
		tmp = mongo_get_connection_single(manager, servers->server[i], connection_flags, (char **) &con_error_message);

		if (tmp) {
			found_connected_server = 1;
		} else if (!(connection_flags & MONGO_CON_FLAG_DONT_CONNECT)) {
			mongo_manager_log(manager, MLOG_CON, MLOG_WARN, "Couldn't connect to '%s:%d': %s", servers->server[i]->host, servers->server[i]->port, con_error_message);
			free(con_error_message);
		}
	}
	if (!found_connected_server && (connection_flags & MONGO_CON_FLAG_DONT_CONNECT)) {
		return NULL;
	}

	/* Discover more nodes. This also adds a connection to "servers" for each
	 * new node */
	mongo_discover_topology(manager, servers);
	/* Create the authentication hash to filter connections */
	if (servers->server[0]->username && servers->server[0]->password) {
		auth_hash = mongo_server_create_hashed_password(servers->server[0]->username, servers->server[0]->password);
	}
	/* Depending on whether we want a read or a write connection, run the correct algorithms */
	if (connection_flags & MONGO_CON_FLAG_WRITE) {
		mongo_read_preference tmp_rp;

		mongo_read_preference_copy(&servers->read_pref, &tmp_rp);
		tmp_rp.type = MONGO_RP_PRIMARY;
		collection = mongo_find_candidate_servers(manager, &tmp_rp, auth_hash);
		mongo_read_preference_dtor(&tmp_rp);
	} else {
		collection = mongo_find_candidate_servers(manager, &servers->read_pref, auth_hash);
	}
	if (!collection || collection->count == 0) {
		*error_message = strdup("No candidate servers found");
		goto bailout;
	}
	collection = mongo_sort_servers(manager, collection, &servers->read_pref);
	collection = mongo_select_nearest_servers(manager, collection, &servers->read_pref);
	con = mongo_pick_server_from_set(manager, collection, &servers->read_pref);

bailout:
	/* Cleaning up */
	mcon_collection_free(collection);	
	return con;
}


static mongo_connection *mongo_get_connection_multiple(mongo_con_manager *manager, mongo_servers *servers, int connection_flags, char **error_message)
{
	mongo_connection *con = NULL;
	mongo_connection *tmp;
	mcon_collection  *collection = NULL;
	char             *con_error_message = NULL;
	char             *auth_hash = NULL;
	mongo_read_preference tmp_rp; /* We only support NEAREST for MULTIPLE right now */
	int i;
	int found_connected_server = 0;
	mcon_str         *messages;

	mcon_str_ptr_init(messages);

	/* Create a connection to every of the servers in the seed list */
	for (i = 0; i < servers->count; i++) {
		tmp = mongo_get_connection_single(manager, servers->server[i], connection_flags, (char **) &con_error_message);

		if (tmp) {
			found_connected_server = 1;
		} else if (!(connection_flags & MONGO_CON_FLAG_DONT_CONNECT)) {
			mongo_manager_log(manager, MLOG_CON, MLOG_WARN, "Couldn't connect to '%s:%d': %s", servers->server[i]->host, servers->server[i]->port, con_error_message);
			if (messages->l) {
				mcon_str_addl(messages, "; ", 2, 0);
			}
			mcon_str_add(messages, "Failed to connect to: ", 0);
			mcon_str_add(messages, servers->server[i]->host, 0);
			mcon_str_addl(messages, ":", 1, 0);
			mcon_str_add_int(messages, servers->server[i]->port);
			mcon_str_addl(messages, ": ", 2, 0);
			mcon_str_add(messages, con_error_message, 1); /* Also frees con_error_message */
		}
	}

	/* If we don't have a connected server then there is no point in continueing */
	if (!found_connected_server && (connection_flags & MONGO_CON_FLAG_DONT_CONNECT)) {
		return NULL;
	}

	/* Create the authentication hash to filter connections */
	if (servers->server[0]->username && servers->server[0]->password) {
		auth_hash = mongo_server_create_hashed_password(servers->server[0]->username, servers->server[0]->password);
	}
	/* Force the RP of NEAREST, which is the only one that makes sense right
	 * now. Technically, read preference tags are also supported, but not
	 * implemented on the mongos side yet. */
	mongo_read_preference_copy(&servers->read_pref, &tmp_rp);
	tmp_rp.type = MONGO_RP_NEAREST;
	collection = mongo_find_candidate_servers(manager, &tmp_rp, auth_hash);
	mongo_read_preference_dtor(&tmp_rp);
	if (!collection || collection->count == 0) {
		if (messages->l) {
			*error_message = strdup(messages->d);
		} else {
			*error_message = strdup("No candidate servers found");
		}
		goto bailout;
	}
	collection = mongo_sort_servers(manager, collection, &servers->read_pref);
	collection = mongo_select_nearest_servers(manager, collection, &servers->read_pref);
	con = mongo_pick_server_from_set(manager, collection, &servers->read_pref);

bailout:
	/* Cleaning up */
	mcon_str_ptr_dtor(messages);
	if (collection) {
		mcon_collection_free(collection);	
	}
	return con;
}

/* API interface to fetch a connection */
mongo_connection *mongo_get_read_write_connection(mongo_con_manager *manager, mongo_servers *servers, int connection_flags, char **error_message)
{
	/* Which connection we return depends on the type of connection we want */
	switch (servers->con_type) {
		case MONGO_CON_TYPE_STANDALONE:
			mongo_manager_log(manager, MLOG_CON, MLOG_INFO, "mongo_get_read_write_connection: finding a STANDALONE connection");
			return mongo_get_connection_multiple(manager, servers, connection_flags, error_message);

		case MONGO_CON_TYPE_REPLSET:
			mongo_manager_log(
				manager, MLOG_CON, MLOG_INFO,
				"mongo_get_read_write_connection: finding a REPLSET connection (%s)",
				connection_flags & MONGO_CON_FLAG_WRITE ? "write" : "read"
			);
			return mongo_get_read_write_connection_replicaset(manager, servers, connection_flags, error_message);

		case MONGO_CON_TYPE_MULTIPLE:
			mongo_manager_log(manager, MLOG_CON, MLOG_FINE, "mongo_get_read_write_connection: finding a MULTIPLE connection");
			return mongo_get_connection_multiple(manager, servers, connection_flags, error_message);

		default:
			mongo_manager_log(manager, MLOG_CON, MLOG_INFO, "mongo_get_read_write_connection: connection type %d is not supported", servers->con_type);
			*error_message = strdup("mongo_get_read_write_connection: Unknown connection type requested");
	}
	return NULL;
}

/* Connection management */

/* - Helpers */
mongo_connection *mongo_manager_connection_find_by_hash(mongo_con_manager *manager, char *hash)
{
	mongo_con_manager_item *ptr = manager->connections;

	while (ptr) {
		if (strcmp(ptr->hash, hash) == 0) {
			mongo_manager_log(manager, MLOG_CON, MLOG_FINE, "found connection %s (looking for %s)", ptr->hash, hash);
			return ptr->connection;
		}
		ptr = ptr->next;
	}
	return NULL;
}

static mongo_con_manager_item *create_new_manager_item(void)
{
	mongo_con_manager_item *tmp = malloc(sizeof(mongo_con_manager_item));
	memset(tmp, 0, sizeof(mongo_con_manager_item));

	return tmp;
}

static inline void free_manager_item(mongo_con_manager *manager, mongo_con_manager_item *item)
{
	mongo_manager_log(manager, MLOG_CON, MLOG_INFO, "freeing connection %s", item->hash);
	free(item->hash);
	free(item);
}

static void destroy_manager_item(mongo_con_manager *manager, mongo_con_manager_item *item)
{
	if (item->next) {
		destroy_manager_item(manager, item->next);
	}
	mongo_connection_destroy(manager, item->connection);
	free_manager_item(manager, item);
}

void mongo_manager_connection_register(mongo_con_manager *manager, mongo_connection *con)
{
	mongo_con_manager_item *ptr = manager->connections;
	mongo_con_manager_item *new;

	/* Setup new entry */
	new = create_new_manager_item();
	new->hash = strdup(con->hash);
	new->connection = con;
	new->next = NULL;

	if (!ptr) { /* No connections at all yet */
		manager->connections = new;
	} else {
		/* Existing connections, so find the last one */
		do {
			if (!ptr->next) {
				ptr->next = new;
				break;
			}
			ptr = ptr->next;
		} while (1);
	}
}

int mongo_manager_connection_deregister(mongo_con_manager *manager, mongo_connection *con)
{
	mongo_con_manager_item *ptr = manager->connections;
	mongo_con_manager_item *prev = NULL;

	/* Remove from manager */
	/* - if there are no connections, simply return false */
	if (!ptr) {
		return 0;
	}
	/* Loop over existing connections and compare hashes */
	do {
		/* The connection is the one we're looking for */
		if (strcmp(ptr->hash, con->hash) == 0) {
			if (prev == NULL) { /* It's the first one in the list... */
				manager->connections = ptr->next;
			} else {
				prev->next = ptr->next;
			}
			/* Free structures */
			mongo_connection_destroy(manager, con);
			free_manager_item(manager, ptr);

			/* Woo! */
			return 1;
		}

		/* Next iteration */
		prev = ptr;
		ptr = ptr->next;
	} while (ptr);

	return 0;
}

/* Logging */
void mongo_manager_log(mongo_con_manager *manager, int module, int level, char *format, ...)
{
	va_list arg;

	va_start(arg, format);
	if (manager->log_function) {
		manager->log_function(module, level, manager->log_context, format, arg);
	}
	va_end(arg);
}

/* Log handler which does nothing */
void mongo_log_null(int module, int level, void *context, char *format, va_list arg)
{
}

/* Log handler which uses printf */
void mongo_log_printf(int module, int level, void *context, char *format, va_list arg)
{
	va_list  tmp;
	char    *message;

	message = malloc(1024);

	va_copy(tmp, arg);
	vsnprintf(message, 1024, format, tmp);
	va_end(tmp);

	printf("%s\n", message);
	free(message);
}

/* Init/deinit */
mongo_con_manager *mongo_init(void)
{
	mongo_con_manager *tmp;

	tmp = malloc(sizeof(mongo_con_manager));
	memset(tmp, 0, sizeof(mongo_con_manager));

	tmp->log_context = NULL;
	tmp->log_function = mongo_log_null;

	tmp->ping_interval = MONGO_MANAGER_DEFAULT_PING_INTERVAL;
	tmp->ismaster_interval = MONGO_MANAGER_DEFAULT_MASTER_INTERVAL;

	return tmp;
}

void mongo_deinit(mongo_con_manager *manager)
{
	if (manager->connections) {
		/* Does this recursively for all cons */
		destroy_manager_item(manager, manager->connections);
	}

	free(manager);
}
