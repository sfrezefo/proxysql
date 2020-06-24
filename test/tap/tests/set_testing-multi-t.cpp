#include <unistd.h>
#include <stdlib.h>
#include <stdio.h>
#include <mysql.h>
#include <string.h>
#include <string>
#include <time.h>
#include <sys/socket.h>
#include <arpa/inet.h>
#include <pthread.h>
#include <sstream>
#include <iostream>
#include <fstream>
#include <mutex>
#include "json.hpp"

#include "tap.h"
#include "utils.h"
#include "command_line.h"

std::vector<std::string> split(const std::string& s, char delimiter)
{
	std::vector<std::string> tokens;
	std::string token;
	std::istringstream tokenStream(s);
	while (std::getline(tokenStream, token, delimiter))
	{
		tokens.push_back(token);
	}
	return tokens;
}

using nlohmann::json;

struct TestCase {
	std::string command;
	json expected_vars;
};

std::vector<TestCase> testCases;

#define MAX_LINE 1024

int readTestCases(const std::string& fileName) {
	FILE* fp = fopen(fileName.c_str(), "r");
	if (!fp) return 0;

	char buf[MAX_LINE], col1[MAX_LINE], col2[MAX_LINE];
	int n = 0;
	for(;;) {
		if (fgets(buf, sizeof(buf), fp) == NULL) break;
		n = sscanf(buf, " \"%[^\"]\", \"%[^\"]\"", col1, col2);
		if (n == 0) break;

		char *p = col2;
		while(*p++) if(*p == '\'') *p = '\"';

		json vars = json::parse(col2);
		testCases.push_back({col1, vars});
	}

	fclose(fp);
	return 1;
}

unsigned long long monotonic_time() {
	struct timespec ts;
	//clock_gettime(CLOCK_MONOTONIC_COARSE, &ts); // this is faster, but not precise
	clock_gettime(CLOCK_MONOTONIC, &ts);
	return (((unsigned long long) ts.tv_sec) * 1000000) + (ts.tv_nsec / 1000);
}

struct cpu_timer
{
	cpu_timer() {
		begin = monotonic_time();
	}
	~cpu_timer()
	{
		unsigned long long end = monotonic_time();
		std::cerr << double( end - begin ) / 1000000 << " secs.\n" ;
		begin=end-begin;
	};
	unsigned long long begin;
};

std::string bn = "";
int queries_per_connections=1;
unsigned int num_threads=1;
int count=0;
char *username=NULL;
char *password=NULL;
char *host=(char *)"localhost";
int port=3306;
int multiport=1;
char *schema=(char *)"information_schema";
int silent = 0;
int sysbench = 0;
int local=0;
int queries=0;
int uniquequeries=0;
int histograms=-1;
int multi_users=0;

bool is_mariadb = false;
bool is_cluster = false;
unsigned int g_connect_OK=0;
unsigned int g_connect_ERR=0;
unsigned int g_select_OK=0;
unsigned int g_select_ERR=0;

unsigned int g_passed=0;
unsigned int g_failed=0;

unsigned int status_connections = 0;
unsigned int connect_phase_completed = 0;
unsigned int query_phase_completed = 0;

__thread int g_seed;
std::mutex mtx_;

inline int fastrand() {
	g_seed = (214013*g_seed+2531011);
	return (g_seed>>16)&0x7FFF;
}

void parseResultJsonColumn(MYSQL_RES *result, json& j) {
	if(!result) return;
	MYSQL_ROW row;

	while ((row = mysql_fetch_row(result)))
		j = json::parse(row[0]);
}

void parseResult(MYSQL_RES *result, json& j) {
	if(!result) return;
	MYSQL_ROW row;

	while ((row = mysql_fetch_row(result))) {
		j[row[0]] = row[1];
	}
}

void dumpResult(MYSQL_RES *result) {
	if(!result) return;
	MYSQL_ROW row;

	int num_fields = mysql_num_fields(result);

	while ((row = mysql_fetch_row(result)))
	{
		for(int i = 0; i < num_fields; i++)
		{
			printf("%s ", row[i] ? row[i] : "NULL");
		}
		printf("\n");
	}
}

void queryVariables(MYSQL *mysql, json& j) {
	std::stringstream query;
	if (is_mariadb) {
		query << "SELECT /* mysql " << mysql << " */ lower(variable_name), variable_value FROM information_schema.session_variables WHERE variable_name IN "
			" ('hostname', 'sql_log_bin', 'sql_mode', 'init_connect', 'time_zone', 'sql_auto_is_null', "
			" 'sql_safe_updates', 'max_join_size', 'net_write_timeout', 'sql_select_limit', "
			" 'sql_select_limit', 'character_set_results', 'tx_isolation', 'tx_read_only', "
			" 'sql_auto_is_null', 'collation_connection', 'character_set_connection', 'character_set_client', 'character_set_database', 'group_concat_max_len');";
	}
	if (is_cluster) {
		query << "SELECT /* mysql " << mysql << " */ * FROM performance_schema.session_variables WHERE variable_name IN "
			" ('hostname', 'sql_log_bin', 'sql_mode', 'init_connect', 'time_zone', 'sql_auto_is_null', "
			" 'sql_safe_updates', 'session_track_gtids', 'max_join_size', 'net_write_timeout', 'sql_select_limit', "
			" 'sql_select_limit', 'character_set_results', 'transaction_isolation', 'transaction_read_only', "
			" 'sql_auto_is_null', 'collation_connection', 'character_set_connection', 'character_set_client', 'character_set_database', 'wsrep_sync_wait', 'group_concat_max_len');";
	}
	if (!is_mariadb && !is_cluster) {
		query << "SELECT /* mysql " << mysql << " */ * FROM performance_schema.session_variables WHERE variable_name IN "
			" ('hostname', 'sql_log_bin', 'sql_mode', 'init_connect', 'time_zone', 'sql_auto_is_null', "
			" 'sql_safe_updates', 'session_track_gtids', 'max_join_size', 'net_write_timeout', 'sql_select_limit', "
			" 'sql_select_limit', 'character_set_results', 'transaction_isolation', 'transaction_read_only', "
			" 'sql_auto_is_null', 'collation_connection', 'character_set_connection', 'character_set_client', 'character_set_database', 'group_concat_max_len');";
	}
	//fprintf(stderr, "TRACE : QUERY 3 : variables %s\n", query.str().c_str());
	if (mysql_query(mysql, query.str().c_str())) {
		if (silent==0) {
			fprintf(stderr,"ERROR while running -- \"%s\" :  (%d) %s\n", query.str().c_str(), mysql_errno(mysql), mysql_error(mysql));
		}
	} else {
		MYSQL_RES *result = mysql_store_result(mysql);
		parseResult(result, j);

		mysql_free_result(result);
		__sync_fetch_and_add(&g_select_OK,1);
	}
}

void queryInternalStatus(MYSQL *mysql, json& j) {
	char *query = (char*)"PROXYSQL INTERNAL SESSION";

	//fprintf(stderr, "TRACE : QUERY 4 : variables %s\n", query);
	if (mysql_query(mysql, query)) {
		if (silent==0) {
			fprintf(stderr,"ERROR while running -- \"%s\" :  (%d) %s\n", query, mysql_errno(mysql), mysql_error(mysql));
		}
	} else {
		MYSQL_RES *result = mysql_store_result(mysql);
		parseResultJsonColumn(result, j);

		mysql_free_result(result);
		__sync_fetch_and_add(&g_select_OK,1);
	}

	// value types in mysql and in proxysql are different
	// we should convert proxysql values to mysql format to compare
	for (auto& el : j.items()) {
		if (el.key() == "conn") {
			std::string sql_log_bin_value;

			// sql_log_bin {0|1}
			if (el.value()["sql_log_bin"] == 1) {
				el.value().erase("sql_log_bin");
				j["conn"]["sql_log_bin"] = "ON";
			}
			else if (el.value()["sql_log_bin"] == 0) {
				el.value().erase("sql_log_bin");
				j["conn"]["sql_log_bin"] = "OFF";
			}

			// sql_auto_is_null {true|false}
			if (!el.value()["sql_auto_is_null"].dump().compare("ON") ||
					!el.value()["sql_auto_is_null"].dump().compare("1") ||
					!el.value()["sql_auto_is_null"].dump().compare("on") ||
					el.value()["sql_auto_is_null"] == 1) {
				el.value().erase("sql_auto_is_null");
				j["conn"]["sql_auto_is_null"] = "ON";
			}
			else if (!el.value()["sql_auto_is_null"].dump().compare("OFF") ||
					!el.value()["sql_auto_is_null"].dump().compare("0") ||
					!el.value()["sql_auto_is_null"].dump().compare("off") ||
					el.value()["sql_auto_is_null"] == 0) {
				el.value().erase("sql_auto_is_null");
				j["conn"]["sql_auto_is_null"] = "OFF";
			}

			// completely remove autocommit test
/*
			// autocommit {true|false}
			if (!el.value()["autocommit"].dump().compare("ON") ||
					!el.value()["autocommit"].dump().compare("1") ||
					!el.value()["autocommit"].dump().compare("on") ||
					el.value()["autocommit"] == 1) {
				el.value().erase("autocommit");
				j["conn"]["autocommit"] = "ON";
			}
			else if (!el.value()["autocommit"].dump().compare("OFF") ||
					!el.value()["autocommit"].dump().compare("0") ||
					!el.value()["autocommit"].dump().compare("off") ||
					el.value()["autocommit"] == 0) {
				el.value().erase("autocommit");
				j["conn"]["autocommit"] = "OFF";
			}
*/
			// sql_safe_updates
			if (!el.value()["sql_safe_updates"].dump().compare("\"ON\"") ||
					!el.value()["sql_safe_updates"].dump().compare("\"1\"") ||
					!el.value()["sql_safe_updates"].dump().compare("\"on\"") ||
					el.value()["sql_safe_updates"] == 1) {
				el.value().erase("sql_safe_updates");
				j["conn"]["sql_safe_updates"] = "ON";
			}
			else if (!el.value()["sql_safe_updates"].dump().compare("\"OFF\"") ||
					!el.value()["sql_safe_updates"].dump().compare("\"0\"") ||
					!el.value()["sql_safe_updates"].dump().compare("\"off\"") ||
					el.value()["sql_safe_updates"] == 0) {
				el.value().erase("sql_safe_updates");
				j["conn"]["sql_safe_updates"] = "OFF";
			}

			std::stringstream ss;
			ss << 0xFFFFFFFFFFFFFFFF;
			// sql_select_limit
			if (!el.value()["sql_select_limit"].dump().compare("\"DEFAULT\"")) {
				el.value().erase("sql_select_limit");
				j["conn"]["sql_select_limit"] = strdup(ss.str().c_str());
			}

			if (!is_mariadb) {
				// transaction_isolation (level)
				if (!el.value()["isolation_level"].dump().compare("\"REPEATABLE READ\"")) {
					el.value().erase("isolation_level");
					j["conn"]["transaction_isolation"] = "REPEATABLE-READ";
				}
				else if (!el.value()["isolation_level"].dump().compare("\"READ COMMITTED\"")) {
					el.value().erase("isolation_level");
					j["conn"]["transaction_isolation"] = "READ-COMMITTED";
				}
				else if (!el.value()["isolation_level"].dump().compare("\"READ UNCOMMITTED\"")) {
					el.value().erase("isolation_level");
					j["conn"]["transaction_isolation"] = "READ-UNCOMMITTED";
				}
				else if (!el.value()["isolation_level"].dump().compare("\"SERIALIZABLE\"")) {
					el.value().erase("isolation_level");
					j["conn"]["transaction_isolation"] = "SERIALIZABLE";
				}
			}
			else {
				// transaction_isolation (level)
				if (!el.value()["isolation_level"].dump().compare("\"REPEATABLE READ\"")) {
					el.value().erase("isolation_level");
					j["conn"]["tx_isolation"] = "REPEATABLE-READ";
				}
				else if (!el.value()["isolation_level"].dump().compare("\"READ COMMITTED\"")) {
					el.value().erase("isolation_level");
					j["conn"]["tx_isolation"] = "READ-COMMITTED";
				}
				else if (!el.value()["isolation_level"].dump().compare("\"READ UNCOMMITTED\"")) {
					el.value().erase("isolation_level");
					j["conn"]["tx_isolation"] = "READ-UNCOMMITTED";
				}
				else if (!el.value()["isolation_level"].dump().compare("\"SERIALIZABLE\"")) {
					el.value().erase("isolation_level");
					j["conn"]["tx_isolation"] = "SERIALIZABLE";
				}
			}

			if (!is_mariadb) {
				// transaction_read (write|only)
				if (!el.value()["transaction_read"].dump().compare("\"ONLY\"")) {
					el.value().erase("transaction_read");
					j["conn"]["transaction_read_only"] = "ON";
				}
				else if (!el.value()["transaction_read"].dump().compare("\"WRITE\"")) {
					el.value().erase("transaction_read");
					j["conn"]["transaction_read_only"] = "OFF";
				}
			} else {
				// transaction_read (write|only)
				if (!el.value()["transaction_read"].dump().compare("\"ONLY\"")) {
					el.value().erase("transaction_read");
					j["conn"]["tx_read_only"] = "ON";
				}
				else if (!el.value()["transaction_read"].dump().compare("\"WRITE\"")) {
					el.value().erase("transaction_read");
					j["conn"]["tx_read_only"] = "OFF";
				}
			}

			if (!is_mariadb) {
				// session_track_gtids
				if (!el.value()["session_track_gtids"].dump().compare("\"OFF\"")) {
					el.value().erase("session_track_gtids");
					j["conn"]["session_track_gtids"] = "OFF";
				}
				else if (!el.value()["session_track_gtids"].dump().compare("\"OWN_GTID\"")) {
					el.value().erase("session_track_gtids");
					j["conn"]["session_track_gtids"] = "OWN_GTID";
				}
				else if (!el.value()["session_track_gtids"].dump().compare("\"ALL_GTIDS\"")) {
					el.value().erase("session_track_gtids");
					j["conn"]["session_track_gtids"] = "ALL_GTIDS";
				}
			}

		}
	}
}

void * my_conn_thread(void *arg) {
	g_seed = time(NULL) ^ getpid() ^ pthread_self();
	unsigned int select_OK=0;
	unsigned int select_ERR=0;
	int i, j;
	MYSQL **mysqlconns=(MYSQL **)malloc(sizeof(MYSQL *)*count);
	std::vector<json> varsperconn(count);

	if (mysqlconns==NULL) {
		exit(EXIT_FAILURE);
	}

	std::vector<std::string> cs = {"latin1", "utf8", "utf8mb4", "latin2", "latin7"};

	for (i=0; i<count; i++) {
		MYSQL *mysql=mysql_init(NULL);
		std::string nextcs = cs[i%cs.size()];

		mysql_options(mysql, MYSQL_SET_CHARSET_NAME, nextcs.c_str());
		if (mysql==NULL) {
			exit(EXIT_FAILURE);
		}
		MYSQL *rc = NULL;
		if (multi_users==0) {
			rc = mysql_real_connect(mysql, host, username, password, schema, (local ? 0 : ( port + rand()%multiport ) ), NULL, 0);
		} else {
			int i = rand()%multi_users;
			i++;
			std::string u = "sbtest" + std::to_string(i);
			std::string p = "sbtest" + std::to_string(i);
			rc = mysql_real_connect(mysql, host, u.c_str(), p.c_str(), schema, (local ? 0 : ( port + rand()%multiport ) ), NULL, 0);	
		}
		if (rc==NULL) {
			if (silent==0) {
				fprintf(stderr,"%s\n", mysql_error(mysql));
			}
			exit(EXIT_FAILURE);
		}
		mysqlconns[i]=mysql;
		__sync_add_and_fetch(&status_connections,1);
	}
	__sync_fetch_and_add(&connect_phase_completed,1);

	while(__sync_fetch_and_add(&connect_phase_completed,0) != num_threads) {
	}
	MYSQL *mysql=NULL;
	json vars;
	for (j=0; j<queries; j++) {
		int fr = fastrand();
		int r1=fr%count;
		int r2=fastrand()%testCases.size();

		if (j%queries_per_connections==0) {
			mysql=mysqlconns[r1];
			vars = varsperconn[r1];
		}
		if (multi_users || strcmp(username,(char *)"root")) {
			if (strstr(testCases[r2].command.c_str(),"database")) {
				std::lock_guard<std::mutex> lock(mtx_);
				skip(1, "mysql connection [%p], command [%s]", mysql, testCases[r2].command.c_str());
				continue;
			}
			if (strstr(testCases[r2].command.c_str(),"sql_log_bin")) {
				std::lock_guard<std::mutex> lock(mtx_);
				skip(1, "mysql connection [%p], command [%s]", mysql, testCases[r2].command.c_str());
				continue;
			}
		}
		std::vector<std::string> commands = split(testCases[r2].command.c_str(), ';');
		for (auto c : commands) {
			if (multi_users) {
				if (c == " ") {
					c = "DO 1";
				}
			}
			if (mysql_query(mysql, c.c_str())) {
				if (silent==0) {
					fprintf(stderr,"ERROR while running -- \"%s\" :  (%d) %s\n", c.c_str(), mysql_errno(mysql), mysql_error(mysql));
				}
			} else {
				MYSQL_RES *result = mysql_store_result(mysql);
				mysql_free_result(result);
				select_OK++;
				__sync_fetch_and_add(&g_select_OK,1);
			}
		}

		for (auto& el : testCases[r2].expected_vars.items()) {
			if (el.key() == "transaction_isolation") {
				if (is_mariadb) {
					vars["tx_isolation"] = el.value();
				}
				else {
					vars[el.key()] = el.value();
				}
			}
			else if (el.key() == "session_track_gtids") {
				if (!is_mariadb) {
					vars[el.key()] = el.value();
				}
			}
			else if (el.key() == "wsrep_sync_wait") {
				if (is_cluster) {
					vars[el.key()] = el.value();
				}
			}
			else if (el.key() == "transaction_read_only") {
				if (is_mariadb) {
					vars["tx_read_only"] = el.value();
				} else {
					vars[el.key()] = el.value();
				}
			}
			else {
				vars[el.key()] = el.value();
			}
		}

		int sleepDelay = fastrand()%100;
		usleep(sleepDelay * 1000);

		char query[128];
		sprintf(query, "SELECT /* %p */ %d;", mysql, sleepDelay);
		if (mysql_query(mysql,query)) {
			select_ERR++;
			__sync_fetch_and_add(&g_select_ERR,1);
		} else {
			MYSQL_RES *result = mysql_store_result(mysql);
			mysql_free_result(result);
			select_OK++;
			__sync_fetch_and_add(&g_select_OK,1);
		}

		json mysql_vars;
		queryVariables(mysql, mysql_vars);

		json proxysql_vars;
		queryInternalStatus(mysql, proxysql_vars);

		bool testPassed = true;
		int variables_tested = 0;
		for (auto& el : vars.items()) {
			auto k = mysql_vars.find(el.key());
			auto s = proxysql_vars["conn"].find(el.key());

			if (k == mysql_vars.end())
				fprintf(stderr, "Variable %s->%s in mysql resultset was not found.\nmysql data : %s\nproxysql data: %s\ncsv data %s\n",
						el.value().dump().c_str(), el.key().c_str(), mysql_vars.dump().c_str(), proxysql_vars.dump().c_str(), vars.dump().c_str());

			if (s == proxysql_vars["conn"].end())
				fprintf(stderr, "Variable %s->%s in proxysql resultset was not found.\nmysql data : %s\nproxysql data: %s\ncsv data %s\n",
						el.value().dump().c_str(), el.key().c_str(), mysql_vars.dump().c_str(), proxysql_vars.dump().c_str(), vars.dump().c_str());

			if (k.value() != el.value() || s.value() != el.value()) {
				__sync_fetch_and_add(&g_failed, 1);
				testPassed = false;
				fprintf(stderr, "Test failed for this case %s->%s.\n\nmysql data %s\n\n proxysql data %s\n\n csv data %s\n\n\n",
						el.value().dump().c_str(), el.key().c_str(), mysql_vars.dump().c_str(), proxysql_vars.dump().c_str(), vars.dump().c_str());
				ok(testPassed, "mysql connection [%p], thread_id [%lu], command [%s]", mysql, mysql->thread_id, testCases[r2].command.c_str());
				exit(0);
			} else {
				variables_tested++;
			}
		}
		{
			std::lock_guard<std::mutex> lock(mtx_);
			ok(testPassed, "mysql connection [%p], thread_id [%lu], variables_tested [%d], command [%s]", mysql, mysql->thread_id, variables_tested, testCases[r2].command.c_str());
		}
	}
	__sync_fetch_and_add(&query_phase_completed,1);

	return NULL;
}


int main(int argc, char *argv[]) {
	CommandLine cl;

	if(cl.getEnv())
		return exit_status();

	{
		bn = basename(argv[0]);
		std::string bn = basename(argv[0]);
		std::cerr << "Filename: " << bn << std::endl;
		if (bn == "set_testing-multi-t") {
			multi_users=4;
		}
	}

	std::string fileName(std::string(cl.workdir) + "/set_testing-t.csv");

	MYSQL* mysqladmin = mysql_init(NULL);
	if (!mysqladmin)
		return exit_status();

	if (!mysql_real_connect(mysqladmin, cl.host, cl.admin_username, cl.admin_password, NULL, cl.admin_port, NULL, 0)) {
	    fprintf(stderr, "File %s, line %d, Error: %s\n",
	              __FILE__, __LINE__, mysql_error(mysqladmin));
		return exit_status();
	}
/*
	MYSQL_QUERY(mysqladmin, "update global_variables set variable_value='ONLY_FULL_GROUP_BY,STRICT_TRANS_TABLES,NO_ZERO_IN_DATE,NO_ZERO_DATE,ERROR_FOR_DIVISION_BY_ZERO,NO_ENGINE_SUBSTITUTION' where variable_name='mysql-default_sql_mode'");
	MYSQL_QUERY(mysqladmin, "update global_variables set variable_value='OFF' where variable_name='mysql-default_sql_safe_update'");
	MYSQL_QUERY(mysqladmin, "update global_variables set variable_value='UTF8' where variable_name='mysql-default_character_set_results'");
	MYSQL_QUERY(mysqladmin, "update global_variables set variable_value='REPEATABLE READ' where variable_name='mysql-default_isolation_level'");
	MYSQL_QUERY(mysqladmin, "update global_variables set variable_value='REPEATABLE READ' where variable_name='mysql-default_tx_isolation'");
	MYSQL_QUERY(mysqladmin, "update global_variables set variable_value='utf8_general_ci' where variable_name='mysql-default_collation_connection'");
	MYSQL_QUERY(mysqladmin, "update global_variables set variable_value='true' where variable_name='mysql-enforce_autocommit_on_reads'");
	MYSQL_QUERY(mysqladmin, "load mysql variables to runtime");

*/
	if (multi_users) {
		for (int i=1; i<=multi_users; i++) {
			std::string q = "INSERT OR IGNORE INTO mysql_users (username,password) VALUES ('sbtest" + std::to_string(i) + "','sbtest" + std::to_string(i) + "')";
			std::cerr << bn << ": " << q << std::endl;
			MYSQL_QUERY(mysqladmin, q.c_str());
		}
		std::string q = "LOAD MYSQL USERS TO RUNTIME";
		std::cerr << bn << ": " << q << std::endl;
		MYSQL_QUERY(mysqladmin, q.c_str());
		q = "UPDATE mysql_servers SET max_connections=3 WHERE hostgroup_id=0;";
		std::cerr << bn << ": " << q << std::endl;
		MYSQL_QUERY(mysqladmin, q.c_str());
		q = "LOAD MYSQL SERVERS TO RUNTIME";
		std::cerr << bn << ": " << q << std::endl;
		MYSQL_QUERY(mysqladmin, q.c_str());
	}
	MYSQL* mysql = mysql_init(NULL);
	if (!mysql)
		return exit_status();
	if (!mysql_real_connect(mysql, cl.host, cl.username, cl.password, NULL, cl.port, NULL, 0)) {
		fprintf(stderr, "File %s, line %d, Error: %s\n",
				__FILE__, __LINE__, mysql_error(mysql));
		return exit_status();
	}
	MYSQL_QUERY(mysql, "select @@version");
	MYSQL_RES *result = mysql_store_result(mysql);
	MYSQL_ROW row;
	while ((row = mysql_fetch_row(result)))
	{
		if (strstr(row[0], "Maria")) {
			is_mariadb = true;
		}
		else {
			is_mariadb = false;
		}

		char* first_dash = strstr(row[0], "-");
		if (!first_dash || !strstr(first_dash+1, "-")) {
			is_cluster = false;
		} else {
			is_cluster = true;
		}
	}

	mysql_free_result(result);
	mysql_close(mysql);

	num_threads = 10;
	queries = 1000;
	queries_per_connections = 10;
	count = 10;
	username = cl.username;
	password = cl.password;
	host = cl.host;
	port = cl.port;

	plan(queries * num_threads);
	if (!readTestCases(fileName)) {
		fprintf(stderr, "Cannot read %s\n", fileName.c_str());
		return exit_status();
	}

	if (strcmp(host,"localhost")==0) {
		local = 1;
	}
	if (uniquequeries == 0) {
		if (queries) uniquequeries=queries;
	}
	if (uniquequeries) {
		uniquequeries=(int)sqrt(uniquequeries);
	}

	pthread_t *thi=(pthread_t *)malloc(sizeof(pthread_t)*num_threads);
	if (thi==NULL)
		return exit_status();

	for (unsigned int i=0; i<num_threads; i++) {
		if ( pthread_create(&thi[i], NULL, my_conn_thread , NULL) != 0 )
			perror("Thread creation");
	}
	for (unsigned int i=0; i<num_threads; i++) {
		pthread_join(thi[i], NULL);
	}
	return exit_status();
}
