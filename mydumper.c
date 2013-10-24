/*
    This program is free software: you can redistribute it and/or modify
    it under the terms of the GNU General Public License as published by
    the Free Software Foundation, either version 3 of the License, or
    (at your option) any later version.

    This program is distributed in the hope that it will be useful,
    but WITHOUT ANY WARRANTY; without even the implied warranty of
    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
    GNU General Public License for more details.

    You should have received a copy of the GNU General Public License
    along with this program.  If not, see <http://www.gnu.org/licenses/>.

	Authors: 	Domas Mituzas, Facebook ( domas at fb dot com )
			Mark Leith, Oracle Corporation (mark dot leith at oracle dot com)
			Andrew Hutchings, SkySQL (andrew at skysql dot com)
*/

#define _LARGEFILE64_SOURCE
#define _FILE_OFFSET_BITS 64

#include <mysql.h>
#include <unistd.h>
#include <stdio.h>
#include <string.h>
#include <glib.h>
#include <stdlib.h>
#include <stdarg.h>
#include <errno.h>
#include <time.h>
#include <zlib.h>
#include <pcre.h>
#include <signal.h>
#include <glib/gstdio.h>
#include "binlog.h"
#include "server_detect.h"
#include "common.h"
#include "g_unix_signal.h"
#include "config.h"

char *regexstring=NULL;

const char DIRECTORY[]= "export";
const char BINLOG_DIRECTORY[]= "binlog_snapshot";
const char DAEMON_BINLOGS[]= "binlogs";

static GMutex * init_mutex = NULL;

/* Program options */
gchar *output_directory= NULL;
guint statement_size= 1000000;
guint rows_per_file= 0;
guint chunk_filesize = 0;
int longquery= 60;
int build_empty_files= 0;
int skip_tz= 0;
int need_dummy_read= 0;
int compress_output= 0;
int killqueries= 0;
int detected_server= 0;
guint snapshot_interval= 60;
gboolean daemon_mode= FALSE;

gchar *ignore_engines= NULL;
char **ignore= NULL;

gchar *tables_list= NULL;
char **tables= NULL;

gboolean need_binlogs= FALSE;
gchar *binlog_directory= NULL;
gchar *daemon_binlog_directory= NULL;

gchar *logfile= NULL;
FILE *logoutfile= NULL;

gboolean no_schemas= FALSE;
gboolean no_locks= FALSE;
gboolean less_locking = FALSE;
gboolean success_on_1146 = FALSE;

GList *innodb_tables= NULL;
GList *non_innodb_table= NULL;
GList *non_innodb_table_block= NULL;
GList *table_schemas= NULL;
gint non_innodb_table_counter= 0;
gint non_innodb_done= 0;

// For daemon mode, 0 or 1
guint dump_number= 0;
guint binlog_connect_id= 0;
gboolean shutdown_triggered= FALSE;
GAsyncQueue *start_scheduled_dump;
GMainLoop *m1;

int errors;

static GOptionEntry entries[] =
{
	{ "database", 'B', 0, G_OPTION_ARG_STRING, &db, "Database to dump", NULL },
	{ "tables-list", 'T', 0, G_OPTION_ARG_STRING, &tables_list, "Comma delimited table list to dump (does not exclude regex option)", NULL },
	{ "outputdir", 'o', 0, G_OPTION_ARG_FILENAME, &output_directory, "Directory to output files to",  NULL },
	{ "statement-size", 's', 0, G_OPTION_ARG_INT, &statement_size, "Attempted size of INSERT statement in bytes, default 1000000", NULL},
	{ "rows", 'r', 0, G_OPTION_ARG_INT, &rows_per_file, "Try to split tables into chunks of this many rows. This option turns off --chunk-filesize", NULL},
	{ "chunk-filesize", 'F', 0, G_OPTION_ARG_INT, &chunk_filesize, "Split tables into chunks of this output file size. This value is in bytes", NULL },
	{ "compress", 'c', 0, G_OPTION_ARG_NONE, &compress_output, "Compress output files", NULL},
	{ "build-empty-files", 'e', 0, G_OPTION_ARG_NONE, &build_empty_files, "Build dump files even if no data available from table", NULL},
	{ "regex", 'x', 0, G_OPTION_ARG_STRING, &regexstring, "Regular expression for 'db.table' matching", NULL},
	{ "ignore-engines", 'i', 0, G_OPTION_ARG_STRING, &ignore_engines, "Comma delimited list of storage engines to ignore", NULL },
	{ "no-schemas", 'm', 0, G_OPTION_ARG_NONE, &no_schemas, "Do not dump table schemas with the data", NULL },
	{ "no-locks", 'k', 0, G_OPTION_ARG_NONE, &no_locks, "Do not execute the temporary shared read lock.  WARNING: This will cause inconsistent backups", NULL },
	{ "less-locking", 0, 0, G_OPTION_ARG_NONE, &less_locking, "Minimize locking time on InnoDB tables, WARNING: be carefull, please READ documentation first", NULL},
	{ "long-query-guard", 'l', 0, G_OPTION_ARG_INT, &longquery, "Set long query timer in seconds, default 60", NULL },
	{ "kill-long-queries", 'k', 0, G_OPTION_ARG_NONE, &killqueries, "Kill long running queries (instead of aborting)", NULL },
	{ "binlogs", 'b', 0, G_OPTION_ARG_NONE, &need_binlogs, "Get a snapshot of the binary logs as well as dump data",  NULL },
	{ "daemon", 'D', 0, G_OPTION_ARG_NONE, &daemon_mode, "Enable daemon mode", NULL },
	{ "snapshot-interval", 'I', 0, G_OPTION_ARG_INT, &snapshot_interval, "Interval between each dump snapshot (in minutes), requires --daemon, default 60", NULL },
	{ "logfile", 'L', 0, G_OPTION_ARG_FILENAME, &logfile, "Log file name to use, by default stdout is used", NULL },
	{ "tz-utc", 0, 0, G_OPTION_ARG_NONE, NULL, "SET TIME_ZONE='+00:00' at top of dump to allow dumping of TIMESTAMP data when a server has data in different time zones or data is being moved between servers with different time zones, defaults to on use --skip-tz-utc to disable.", NULL },
	{ "skip-tz-utc", 0, 0, G_OPTION_ARG_NONE, &skip_tz, "", NULL },
	{ "success-on-1146", 0, 0, G_OPTION_ARG_NONE, &success_on_1146, "Not increment error count and Warning instead of Critical in case of table doesn't exist", NULL},
	{ NULL, 0, 0, G_OPTION_ARG_NONE,   NULL, NULL, NULL }
};

struct tm tval;

void dump_schema_data(MYSQL *conn, char *database, char *table, char *filename);
void dump_schema(char *database, char *table, struct configuration *conf);
void dump_table(MYSQL *conn, char *database, char *table, struct configuration *conf, gboolean is_innodb);
guint64 dump_table_data(MYSQL *, FILE *, char *, char *, char *, char *);
void dump_database(MYSQL *, char *);
GList * get_chunks_for_table(MYSQL *, char *, char*,  struct configuration *conf);
guint64 estimate_count(MYSQL *conn, char *database, char *table, char *field, char *from, char *to);
void dump_table_data_file(MYSQL *conn, char *database, char *table, char *where, char *filename);
void create_backup_dir(char *directory);
gboolean write_data(FILE *,GString*);
gboolean check_regex(char *database, char *table);
void no_log(const gchar *log_domain, GLogLevelFlags log_level, const gchar *message, gpointer user_data);
void set_verbose(guint verbosity);
MYSQL *reconnect_for_binlog(MYSQL *thrconn);
void start_dump(MYSQL *conn, MYSQL *lock_conn);
MYSQL *create_main_connection();
void *binlog_thread(void *data);
void *exec_thread(void *data);
void write_log_file(const gchar *log_domain, GLogLevelFlags log_level, const gchar *message, gpointer user_data);

void no_log(const gchar *log_domain, GLogLevelFlags log_level, const gchar *message, gpointer user_data) {
	(void) log_domain;
	(void) log_level;
	(void) message;
	(void) user_data;
}

void set_verbose(guint verbosity) {
	if (logfile) {
		logoutfile = g_fopen(logfile, "w");
		if (!logoutfile) {
			g_critical("Could not open log file '%s' for writing: %d", logfile, errno);
			exit(EXIT_FAILURE);
		}
	}

	switch (verbosity) {
		case 0:
			g_log_set_handler(NULL, (GLogLevelFlags)(G_LOG_LEVEL_MASK), no_log, NULL);
			break;
		case 1:
			g_log_set_handler(NULL, (GLogLevelFlags)(G_LOG_LEVEL_WARNING | G_LOG_LEVEL_MESSAGE), no_log, NULL);
			if (logfile)
				g_log_set_handler(NULL, (GLogLevelFlags)(G_LOG_LEVEL_ERROR | G_LOG_LEVEL_CRITICAL), write_log_file, NULL);
			break;
		case 2:
			g_log_set_handler(NULL, (GLogLevelFlags)(G_LOG_LEVEL_MESSAGE), no_log, NULL);
			if (logfile)
				g_log_set_handler(NULL, (GLogLevelFlags)(G_LOG_LEVEL_WARNING | G_LOG_LEVEL_ERROR | G_LOG_LEVEL_WARNING | G_LOG_LEVEL_ERROR | G_LOG_LEVEL_CRITICAL), write_log_file, NULL);
			break;
		default:
			if (logfile)
				g_log_set_handler(NULL, (GLogLevelFlags)(G_LOG_LEVEL_MASK), write_log_file, NULL);
			break;
	}
}

gboolean sig_triggered(gpointer user_data) {
	(void) user_data;

	g_message("Shutting down gracefully");
	shutdown_triggered= TRUE;
	g_main_loop_quit(m1);
	return FALSE;
}

void clear_dump_directory()
{
	GError *error= NULL;
	char* dump_directory= g_strdup_printf("%s/%d", output_directory, dump_number);
	GDir* dir= g_dir_open(dump_directory, 0, &error);

	if (error) {
		g_critical("cannot open directory %s, %s\n", dump_directory, error->message);
		errors++;
		return;
	}

	const gchar* filename= NULL;

	while((filename= g_dir_read_name(dir))) {
		gchar* path= g_build_filename(dump_directory, filename, NULL);
		if (g_unlink(path) == -1) {
			g_critical("error removing file %s (%d)\n", path, errno);
			errors++;
			return;
		}
		g_free(path);
	}

	g_dir_close(dir);
	g_free(dump_directory);
}

gboolean run_snapshot(gpointer *data)
{
	(void) data;

	g_async_queue_push(start_scheduled_dump,GINT_TO_POINTER(1));

	return (shutdown_triggered) ? FALSE : TRUE;
}

/* Check database.table string against regular expression */

gboolean check_regex(char *database, char *table) {
	/* This is not going to be used in threads */
	static pcre *re = NULL;
	int rc;
	int ovector[9]= {0};
	const char *error;
	int erroroffset;

	char *p;

	/* Let's compile the RE before we do anything */
	if (!re) {
		re = pcre_compile(regexstring,PCRE_CASELESS|PCRE_MULTILINE,&error,&erroroffset,NULL);
		if(!re) {
			g_critical("Regular expression fail: %s", error);
			exit(EXIT_FAILURE);
		}
	}

	p=g_strdup_printf("%s.%s",database,table);
	rc = pcre_exec(re,NULL,p,strlen(p),0,0,ovector,9);
	g_free(p);

	return (rc>0)?TRUE:FALSE;
}

/* Write some stuff we know about snapshot, before it changes */
void write_snapshot_info(MYSQL *conn, FILE *file) {
	MYSQL_RES *master=NULL, *slave=NULL;
	MYSQL_FIELD *fields;
	MYSQL_ROW row;

	char *masterlog=NULL;
	char *masterpos=NULL;

	char *slavehost=NULL;
	char *slavelog=NULL;
	char *slavepos=NULL;

	mysql_query(conn,"SHOW MASTER STATUS");
	master=mysql_store_result(conn);
	if (master && (row=mysql_fetch_row(master))) {
		masterlog=row[0];
		masterpos=row[1];
	}

	mysql_query(conn, "SHOW SLAVE STATUS");
	slave=mysql_store_result(conn);
	guint i;
	if (slave && (row=mysql_fetch_row(slave))) {
		fields=mysql_fetch_fields(slave);
		for (i=0; i<mysql_num_fields(slave);i++) {
			if (!strcasecmp("exec_master_log_pos",fields[i].name)) {
				slavepos=row[i];
			} else if (!strcasecmp("relay_master_log_file", fields[i].name)) {
				slavelog=row[i];
			} else if (!strcasecmp("master_host",fields[i].name)) {
				slavehost=row[i];
			}
		}
	}

	if (masterlog) {
		fprintf(file, "SHOW MASTER STATUS:\n\tLog: %s\n\tPos: %s\n\n", masterlog, masterpos);
		g_message("Written master status");
	}

	if (slavehost) {
		fprintf(file, "SHOW SLAVE STATUS:\n\tHost: %s\n\tLog: %s\n\tPos: %s\n\n",
			slavehost, slavelog, slavepos);
		g_message("Written slave status");
	}

	fflush(file);
	if (master)
		mysql_free_result(master);
	if (slave)
		mysql_free_result(slave);
}

void *process_queue(struct thread_data *td) {
	struct configuration *conf= td->conf;
	// mysql_init is not thread safe, especially in Connector/C
	g_mutex_lock(init_mutex);
	MYSQL *thrconn = mysql_init(NULL);
	g_mutex_unlock(init_mutex);

	mysql_options(thrconn,MYSQL_READ_DEFAULT_GROUP,"mydumper");

	if (compress_protocol)
		mysql_options(thrconn,MYSQL_OPT_COMPRESS,NULL);

	if (!mysql_real_connect(thrconn, hostname, username, password, NULL, port, socket_path, 0)) {
		g_critical("Failed to connect to database: %s", mysql_error(thrconn));
		exit(EXIT_FAILURE);
	} else {
		g_message("Thread %d connected using MySQL connection ID %lu", td->thread_id, mysql_thread_id(thrconn));
	}

	if ((detected_server == SERVER_TYPE_MYSQL) && mysql_query(thrconn, "SET SESSION wait_timeout = 2147483")){
		g_warning("Failed to increase wait_timeout: %s", mysql_error(thrconn));
	}
	if (mysql_query(thrconn, "SET SESSION TRANSACTION ISOLATION LEVEL REPEATABLE READ")) {
		g_warning("Failed to set isolation level: %s", mysql_error(thrconn));
	}
	if (mysql_query(thrconn, "START TRANSACTION /*!40108 WITH CONSISTENT SNAPSHOT */")) {
		g_critical("Failed to start consistent snapshot: %s",mysql_error(thrconn));
		errors++;
	}
	if(!skip_tz && mysql_query(thrconn, "/*!40103 SET TIME_ZONE='+00:00' */")){
		g_critical("Failed to set time zone: %s",mysql_error(thrconn));
	}

	/* Unfortunately version before 4.1.8 did not support consistent snapshot transaction starts, so we cheat */
	if (need_dummy_read) {
		mysql_query(thrconn,"SELECT /*!40001 SQL_NO_CACHE */ * FROM mysql.mydumperdummy");
		MYSQL_RES *res=mysql_store_result(thrconn);
		if (res)
			mysql_free_result(res);
	}
	mysql_query(thrconn, "/*!40101 SET NAMES binary*/");

	g_async_queue_push(conf->ready,GINT_TO_POINTER(1));

	struct job* job= NULL;
	struct table_job* tj= NULL;
	struct schema_job* sj= NULL;
	struct binlog_job* bj= NULL;
	for(;;) {
		GTimeVal tv;
		g_get_current_time(&tv);
		g_time_val_add(&tv,1000*1000*1);
		job=(struct job *)g_async_queue_pop(conf->queue);

		if (shutdown_triggered && (job->type != JOB_SHUTDOWN)) {
			continue;
		}

		switch (job->type) {
			case JOB_DUMP:
				tj=(struct table_job *)job->job_data;
				if (tj->where)
					g_message("Thread %d dumping data for `%s`.`%s` where %s", td->thread_id, tj->database, tj->table, tj->where);
				else
					g_message("Thread %d dumping data for `%s`.`%s`", td->thread_id, tj->database, tj->table);
				dump_table_data_file(thrconn, tj->database, tj->table, tj->where, tj->filename);
				if(tj->database) g_free(tj->database);
				if(tj->table) g_free(tj->table);
				if(tj->where) g_free(tj->where);
				if(tj->filename) g_free(tj->filename);
				g_free(tj);
				g_free(job);
				break;
			case JOB_DUMP_NON_INNODB:
				tj=(struct table_job *)job->job_data;
				if (tj->where)
					g_message("Thread %d dumping data for `%s`.`%s` where %s", td->thread_id, tj->database, tj->table, tj->where);
				else
					g_message("Thread %d dumping data for `%s`.`%s`", td->thread_id, tj->database, tj->table);
				dump_table_data_file(thrconn, tj->database, tj->table, tj->where, tj->filename);
				if(tj->database) g_free(tj->database);
				if(tj->table) g_free(tj->table);
				if(tj->where) g_free(tj->where);
				if(tj->filename) g_free(tj->filename);
				g_free(tj);
				g_free(job);
				if (g_atomic_int_dec_and_test(&non_innodb_table_counter) && g_atomic_int_get(&non_innodb_done)) {
					g_async_queue_push(conf->unlock_tables, GINT_TO_POINTER(1));
				}
				break;
			case JOB_SCHEMA:
				sj=(struct schema_job *)job->job_data;
				g_message("Thread %d dumping schema for `%s`.`%s`", td->thread_id, sj->database, sj->table);
				dump_schema_data(thrconn, sj->database, sj->table, sj->filename);
				if(sj->database) g_free(sj->database);
				if(sj->table) g_free(sj->table);
				if(sj->filename) g_free(sj->filename);
				g_free(sj);
				g_free(job);
				break;
			case JOB_BINLOG:
				thrconn= reconnect_for_binlog(thrconn);
				g_message("Thread %d connected using MySQL connection ID %lu (in binlog mode)", td->thread_id, mysql_thread_id(thrconn));
				bj=(struct binlog_job *)job->job_data;
				g_message("Thread %d dumping binary log file %s", td->thread_id, bj->filename);
				get_binlog_file(thrconn, bj->filename, binlog_directory, bj->start_position, bj->stop_position, FALSE);
				if(bj->filename)
					g_free(bj->filename);
				g_free(bj);
				g_free(job);
				break;
			case JOB_SHUTDOWN:
				g_message("Thread %d shutting down", td->thread_id);
				if (thrconn)
					mysql_close(thrconn);
				g_free(job);
				mysql_thread_end();
				return NULL;
				break;
			default:
				g_critical("Something very bad happened!");
				exit(EXIT_FAILURE);
		}
	}
	if (thrconn)
		mysql_close(thrconn);
	mysql_thread_end();
	return NULL;
}

MYSQL *reconnect_for_binlog(MYSQL *thrconn) {
	if (thrconn) {
		mysql_close(thrconn);
	}
	g_mutex_lock(init_mutex);
	thrconn= mysql_init(NULL);
	g_mutex_unlock(init_mutex);

	if (compress_protocol)
		mysql_options(thrconn,MYSQL_OPT_COMPRESS,NULL);

	int timeout= 1;
	mysql_options(thrconn, MYSQL_OPT_READ_TIMEOUT, (const char*)&timeout);

	if (!mysql_real_connect(thrconn, hostname, username, password, NULL, port, socket_path, 0)) {
		g_critical("Failed to re-connect to database: %s", mysql_error(thrconn));
		exit(EXIT_FAILURE);
	}
	return thrconn;
}

int main(int argc, char *argv[])
{
	GError *error = NULL;
	GOptionContext *context;

	g_thread_init(NULL);

	init_mutex = g_mutex_new();

	context = g_option_context_new("multi-threaded MySQL dumping");
	GOptionGroup *main_group= g_option_group_new("main", "Main Options", "Main Options", NULL, NULL);
	g_option_group_add_entries(main_group, entries);
	g_option_group_add_entries(main_group, common_entries);
	g_option_context_set_main_group(context, main_group);
	if (!g_option_context_parse(context, &argc, &argv, &error)) {
		g_print ("option parsing failed: %s, try --help\n", error->message);
		exit (EXIT_FAILURE);
	}
	g_option_context_free(context);

	if (program_version) {
		g_print("mydumper %s, built against MySQL %s\n", VERSION, MYSQL_SERVER_VERSION);
		exit (EXIT_SUCCESS);
	}

	set_verbose(verbose);

	time_t t;
	time(&t);localtime_r(&t,&tval);
	
	//rows chunks have precedence over chunk_filesize 
	if (rows_per_file > 0)
		chunk_filesize = 0;
	
	if (!output_directory)
		output_directory = g_strdup_printf("%s-%04d%02d%02d-%02d%02d%02d",DIRECTORY,
			tval.tm_year+1900, tval.tm_mon+1, tval.tm_mday,
			tval.tm_hour, tval.tm_min, tval.tm_sec);

	create_backup_dir(output_directory);
	if (daemon_mode) {
		pid_t pid, sid;

		pid= fork();
		if (pid < 0)
			exit(EXIT_FAILURE);
		else if (pid > 0)
			exit(EXIT_SUCCESS);

		umask(0);
		sid= setsid();

		if (sid < 0)
			exit(EXIT_FAILURE);

		char *dump_directory= g_strdup_printf("%s/0", output_directory);
		create_backup_dir(dump_directory);
		g_free(dump_directory);
		dump_directory= g_strdup_printf("%s/1", output_directory);
		create_backup_dir(dump_directory);
		g_free(dump_directory);
		daemon_binlog_directory= g_strdup_printf("%s/%s", output_directory, DAEMON_BINLOGS);
		create_backup_dir(daemon_binlog_directory);
	}

	if (need_binlogs) {
		binlog_directory = g_strdup_printf("%s/%s", output_directory, BINLOG_DIRECTORY);
		create_backup_dir(binlog_directory);
	}

	/* Give ourselves an array of engines to ignore */
	if (ignore_engines)
		ignore = g_strsplit(ignore_engines, ",", 0);

	/* Give ourselves an array of tables to dump */
	if (tables_list)
		tables = g_strsplit(tables_list, ",", 0);

	if (daemon_mode) {
		GError* terror;

		GThread *bthread= g_thread_create(binlog_thread, GINT_TO_POINTER(1), FALSE, &terror);
		if (bthread == NULL) {
			g_critical("Could not create binlog thread: %s", terror->message);
			g_error_free(terror);
			exit(EXIT_FAILURE);
		}

		start_scheduled_dump= g_async_queue_new();
		GThread *ethread= g_thread_create(exec_thread, GINT_TO_POINTER(1), FALSE, &terror);
		if (ethread == NULL) {
			g_critical("Could not create exec thread: %s", terror->message);
			g_error_free(terror);
			exit(EXIT_FAILURE);
		}
		// Run initial snapshot
		run_snapshot(NULL);
		#if GLIB_MINOR_VERSION < 14
		g_timeout_add(snapshot_interval*60*1000, (GSourceFunc) run_snapshot, NULL);
		#else
		g_timeout_add_seconds(snapshot_interval*60, (GSourceFunc) run_snapshot, NULL);
		#endif
		guint sigsource= g_unix_signal_add(SIGINT, sig_triggered, NULL);
		sigsource= g_unix_signal_add(SIGTERM, sig_triggered, NULL);
		m1= g_main_loop_new(NULL, TRUE);
		g_main_loop_run(m1);
		g_source_remove(sigsource);
	} else {
		MYSQL *conn= create_main_connection();
		MYSQL *lock_conn= create_main_connection();
		start_dump(conn, lock_conn);
		mysql_close(conn);
		mysql_close(lock_conn);
	}

	sleep(5);
	mysql_thread_end();
	mysql_library_end();
	g_free(output_directory);
	g_strfreev(ignore);
	g_strfreev(tables);

	if (logoutfile) {
		fclose(logoutfile);
	}

	exit(errors ? EXIT_FAILURE : EXIT_SUCCESS);
}

MYSQL *create_main_connection()
{
	MYSQL *conn;
	conn = mysql_init(NULL);
	mysql_options(conn,MYSQL_READ_DEFAULT_GROUP,"mydumper");

	if (!mysql_real_connect(conn, hostname, username, password, db, port, socket_path, 0)) {
		g_critical("Error connecting to database: %s", mysql_error(conn));
		exit(EXIT_FAILURE);
	}
	if ((detected_server == SERVER_TYPE_MYSQL) && mysql_query(conn, "SET SESSION wait_timeout = 2147483")){
		g_warning("Failed to increase wait_timeout: %s", mysql_error(conn));
	}
	if ((detected_server == SERVER_TYPE_MYSQL) && mysql_query(conn, "SET SESSION net_write_timeout = 2147483")){
		g_warning("Failed to increase net_write_timeout: %s", mysql_error(conn));
	}

	detected_server= detect_server(conn);
	switch (detected_server) {
		case SERVER_TYPE_MYSQL:
			g_message("Connected to a MySQL server");
			break;
		case SERVER_TYPE_DRIZZLE:
			g_message("Connected to a Drizzle server");
			break;
		default:
			g_critical("Cannot detect server type");
			exit(EXIT_FAILURE);
			break;
	}

	return conn;
}

void *exec_thread(void *data) {
	(void) data;

	while(1) {
		g_async_queue_pop(start_scheduled_dump);
		clear_dump_directory();
		MYSQL *conn= create_main_connection();
		MYSQL *lock_conn= create_main_connection();
		start_dump(conn,lock_conn);
		mysql_close(conn);
		mysql_close(lock_conn);
		mysql_thread_end();

		// Don't switch the symlink on shutdown because the dump is probably incomplete.
		if (!shutdown_triggered) {
			const char *dump_symlink_source= (dump_number == 0) ? "0" : "1";
			char *dump_symlink_dest= g_strdup_printf("%s/last_dump", output_directory);

			// We don't care if this fails
			g_unlink(dump_symlink_dest);

			if (symlink(dump_symlink_source, dump_symlink_dest) == -1) {
				g_critical("error setting last good dump symlink %s, %d", dump_symlink_dest, errno);
			}
			g_free(dump_symlink_dest);

			dump_number= (dump_number == 1) ? 0 : 1;
		}
	}
	return NULL;
}

void *binlog_thread(void *data) {
	(void) data;
	MYSQL_RES *master= NULL;
	MYSQL_ROW row;
	MYSQL *conn;
	conn = mysql_init(NULL);
	mysql_options(conn,MYSQL_READ_DEFAULT_GROUP,"mydumper");

	if (!mysql_real_connect(conn, hostname, username, password, db, port, socket_path, 0)) {
		g_critical("Error connecting to database: %s", mysql_error(conn));
		exit(EXIT_FAILURE);
	}

	mysql_query(conn,"SHOW MASTER STATUS");
	master= mysql_store_result(conn);
	if (master && (row= mysql_fetch_row(master))) {
		MYSQL *binlog_connection= NULL;
		binlog_connection= reconnect_for_binlog(binlog_connection);
		binlog_connect_id= mysql_thread_id(binlog_connection);
		guint64 start_position= g_ascii_strtoull(row[1], NULL, 10);
		gchar* filename= g_strdup(row[0]);
		mysql_free_result(master);
		mysql_close(conn);
		g_message("Continuous binlog thread connected using MySQL connection ID %lu", mysql_thread_id(binlog_connection));
		get_binlog_file(binlog_connection, filename, daemon_binlog_directory, start_position, 0, TRUE);
		g_free(filename);
		mysql_close(binlog_connection);
	} else {
		mysql_free_result(master);
		mysql_close(conn);
	}
	g_message("Continuous binlog thread shutdown");
	mysql_thread_end();
	return NULL;
}

void start_dump(MYSQL *conn, MYSQL *lock_conn)
{
	struct configuration conf = { 1, NULL, NULL, NULL, NULL, 0 };
	char *p;
	char *p2;
	char *p3;
	time_t t;
	struct db_table *dbt;
	int main_lock = 1;

	if (daemon_mode)
		p= g_strdup_printf("%s/%d/metadata.partial", output_directory, dump_number);
	else
		p= g_strdup_printf("%s/metadata.partial", output_directory);
	p2 = g_strndup(p, (unsigned)strlen(p)-8);

	FILE* mdfile=g_fopen(p,"w");
	if(!mdfile) {
		g_critical("Couldn't write metadata file (%d)",errno);
		exit(EXIT_FAILURE);
	}

	/* We check SHOW PROCESSLIST, and if there're queries
	   larger than preset value, we terminate the process.

	   This avoids stalling whole server with flush */

	if (mysql_query(conn, "SHOW PROCESSLIST")) {
		g_warning("Could not check PROCESSLIST, no long query guard enabled: %s", mysql_error(conn));
	} else {
		MYSQL_RES *res = mysql_store_result(conn);
		MYSQL_ROW row;

		/* Just in case PROCESSLIST output column order changes */
		MYSQL_FIELD *fields = mysql_fetch_fields(res);
		guint i;
		int tcol=-1, ccol=-1, icol=-1;
		for(i=0; i<mysql_num_fields(res); i++) {
			if (!strcasecmp(fields[i].name,"Command")) ccol=i;
			else if (!strcasecmp(fields[i].name,"Time")) tcol=i;
			else if (!strcasecmp(fields[i].name,"Id")) icol=i;
		}
		if ((tcol < 0) || (ccol < 0) || (icol < 0)) {
			g_critical("Error obtaining information from processlist");
			exit(EXIT_FAILURE);
		}
		while ((row=mysql_fetch_row(res))) {
			if (row[ccol] && strcmp(row[ccol],"Query"))
				continue;
			if (row[tcol] && atoi(row[tcol])>longquery) {
				if (killqueries) {
					if (mysql_query(conn,p3=g_strdup_printf("KILL %lu",atol(row[icol]))))
						g_warning("Could not KILL slow query: %s",mysql_error(conn));
					else
						g_warning("Killed a query that was running for %ss",row[tcol]);
					g_free(p3);
				} else {
					g_critical("There are queries in PROCESSLIST running longer than %us, aborting dump,\n\t"
						"use --long-query-guard to change the guard value, kill queries (--kill-long-queries) or use \n\tdifferent server for dump", longquery);
					exit(EXIT_FAILURE);
				}
			}
		}
		mysql_free_result(res);
	}

	if (!no_locks) {
		if (mysql_query(conn, "FLUSH TABLES WITH READ LOCK")) {
			g_critical("Couldn't acquire global lock, snapshots will not be consistent: %s",mysql_error(conn));
			errors++;
		}
	} else {
		g_warning("Executing in no-locks mode, snapshot will notbe consistent");
	}
	if (mysql_get_server_version(conn)) {
		mysql_query(conn, "CREATE TABLE IF NOT EXISTS mysql.mydumperdummy (a INT) ENGINE=INNODB");
		need_dummy_read=1;
	}
	mysql_query(conn, "START TRANSACTION /*!40108 WITH CONSISTENT SNAPSHOT */");
	if (need_dummy_read) {
		mysql_query(conn,"SELECT /*!40001 SQL_NO_CACHE */ * FROM mysql.mydumperdummy");
		MYSQL_RES *res=mysql_store_result(conn);
		if (res)
			mysql_free_result(res);
	}
	time(&t); localtime_r(&t,&tval);
	fprintf(mdfile,"Started dump at: %04d-%02d-%02d %02d:%02d:%02d\n",
		tval.tm_year+1900, tval.tm_mon+1, tval.tm_mday,
		tval.tm_hour, tval.tm_min, tval.tm_sec);

	g_message("Started dump at: %04d-%02d-%02d %02d:%02d:%02d\n",
		tval.tm_year+1900, tval.tm_mon+1, tval.tm_mday,
		tval.tm_hour, tval.tm_min, tval.tm_sec);

	if (detected_server == SERVER_TYPE_MYSQL) {
		mysql_query(conn, "/*!40101 SET NAMES binary*/");

		write_snapshot_info(conn, mdfile);
	}

	conf.queue = g_async_queue_new();
	conf.ready = g_async_queue_new();
	conf.unlock_tables= g_async_queue_new();

	guint n;
	GThread **threads = g_new(GThread*,num_threads);
	struct thread_data *td= g_new(struct thread_data, num_threads);
	for (n=0; n<num_threads; n++) {
		td[n].conf= &conf;
		td[n].thread_id= n+1;
		threads[n] = g_thread_create((GThreadFunc)process_queue,&td[n],TRUE,NULL);
		g_async_queue_pop(conf.ready);
	}
	g_async_queue_unref(conf.ready);

	if (db) {
		dump_database(conn, db);
	} else {
		MYSQL_RES *databases;
		MYSQL_ROW row;
		if(mysql_query(conn,"SHOW DATABASES") || !(databases = mysql_store_result(conn))) {
			g_critical("Unable to list databases: %s",mysql_error(conn));
			exit(EXIT_FAILURE);
		}

		while ((row=mysql_fetch_row(databases))) {
			if (!strcasecmp(row[0],"information_schema") || !strcasecmp(row[0], "performance_schema") || (!strcasecmp(row[0], "data_dictionary")))
				continue;
			dump_database(conn, row[0]);
		}
		mysql_free_result(databases);

	}

	if (!no_locks && less_locking) {
		GString *query = g_string_sized_new(1024);
		gchar *dt = NULL;

		if (!non_innodb_table) {
			g_async_queue_push(conf.unlock_tables, GINT_TO_POINTER(1));
		}else{
			int first = 1;
			/* Create a read lock for non_innodb_table */
			non_innodb_table_block = non_innodb_table;
			for (non_innodb_table_block= g_list_first(non_innodb_table_block); non_innodb_table_block; non_innodb_table_block= g_list_next(non_innodb_table_block)) {
				dbt= (struct db_table*) non_innodb_table_block->data;
				if(!(strcasecmp(dbt->database,"mysql") == 0 && (strcasecmp(dbt->table,"general_log") ==0 || strcasecmp(dbt->table,"slow_log") ==0 ))){
					dt = g_strjoin(".",dbt->database,dbt->table,NULL);
					if(first){
						g_string_printf(query, "LOCK TABLES ");
						first = 0;
					}else{
						g_string_append(query, ", ");
					}
					g_string_append(query, dt);
					g_string_append(query, " READ");
				}
			}
			g_free(dt);
			
			/* if the LT fails do not realease the FTWRL */
			if(mysql_query(lock_conn, query->str)){
				g_warning("MyISAM lock tables fail: %s", mysql_error(lock_conn));
			}else{
				g_message("Unlocking Innodb tables");
				mysql_query(conn, "UNLOCK TABLES");
				main_lock = 0;
			}
		}
		g_string_free(query, TRUE);
	}

	for (non_innodb_table= g_list_first(non_innodb_table); non_innodb_table; non_innodb_table= g_list_next(non_innodb_table)) {
		dbt= (struct db_table*) non_innodb_table->data;
		dump_table(conn, dbt->database, dbt->table, &conf, FALSE);
		g_atomic_int_inc(&non_innodb_table_counter);
	}
	g_list_free(g_list_first(non_innodb_table));

	g_atomic_int_inc(&non_innodb_done);

	for (innodb_tables= g_list_first(innodb_tables); innodb_tables; innodb_tables= g_list_next(innodb_tables)) {
		dbt= (struct db_table*) innodb_tables->data;
		dump_table(conn, dbt->database, dbt->table, &conf, TRUE);
	}
	g_list_free(g_list_first(innodb_tables));

	for (table_schemas= g_list_first(table_schemas); table_schemas; table_schemas= g_list_next(table_schemas)) {
		dbt= (struct db_table*) table_schemas->data;
		dump_schema(dbt->database, dbt->table, &conf);
		g_free(dbt->table);
		g_free(dbt->database);
		g_free(dbt);
	}
	g_list_free(g_list_first(table_schemas));

	if (need_binlogs) {
		get_binlogs(conn, &conf);
	}

	for (n=0; n<num_threads; n++) {
		struct job *j = g_new0(struct job,1);
		j->type = JOB_SHUTDOWN;
		g_async_queue_push(conf.queue,j);
	}

	if (!no_locks) {
		g_async_queue_pop(conf.unlock_tables);
		if(less_locking) {
			mysql_query(lock_conn, "UNLOCK TABLES /* Non Innodb */");
			g_message("Non-InnoDB dump complete, unlocking tables");
		}
		if(main_lock)
			mysql_query(conn, "UNLOCK TABLES");
	}

	for (n=0; n<num_threads; n++) {
		g_thread_join(threads[n]);
	}
	g_async_queue_unref(conf.queue);

	time(&t);localtime_r(&t,&tval);
	fprintf(mdfile,"Finished dump at: %04d-%02d-%02d %02d:%02d:%02d\n",
		tval.tm_year+1900, tval.tm_mon+1, tval.tm_mday,
		tval.tm_hour, tval.tm_min, tval.tm_sec);
	fclose(mdfile);
	g_rename(p, p2);
	g_free(p);
	g_free(p2);
	g_message("Finished dump at: %04d-%02d-%02d %02d:%02d:%02d\n",
		tval.tm_year+1900, tval.tm_mon+1, tval.tm_mday,
		tval.tm_hour, tval.tm_min, tval.tm_sec);

	g_free(td);
	g_free(threads);
}

/* Heuristic chunks building - based on estimates, produces list of ranges for datadumping
   WORK IN PROGRESS
*/
GList * get_chunks_for_table(MYSQL *conn, char *database, char *table, struct configuration *conf) {

	GList *chunks = NULL;
	MYSQL_RES *indexes=NULL, *minmax=NULL, *total=NULL;
	MYSQL_ROW row;
	char *field = NULL;
	int showed_nulls=0;

	/* first have to pick index, in future should be able to preset in configuration too */
	gchar *query = g_strdup_printf("SHOW INDEX FROM `%s`.`%s`",database,table);
	mysql_query(conn,query);
	g_free(query);
	indexes=mysql_store_result(conn);

	while ((row=mysql_fetch_row(indexes))) {
		if (!strcmp(row[2],"PRIMARY") && (!strcmp(row[3],"1"))) {
			/* Pick first column in PK, cardinality doesn't matter */
			field=row[4];
			break;
		}
	}

	/* If no PK found, try using first UNIQUE index */
	if (!field) {
		mysql_data_seek(indexes,0);
		while ((row=mysql_fetch_row(indexes))) {
			if(!strcmp(row[1],"0") && (!strcmp(row[3],"1"))) {
				/* Again, first column of any unique index */
				field=row[4];
				break;
			}
		}
	}

	/* Still unlucky? Pick any high-cardinality index */
	if (!field && conf->use_any_index) {
		guint64 max_cardinality=0;
		guint64 cardinality=0;

		mysql_data_seek(indexes,0);
		while ((row=mysql_fetch_row(indexes))) {
			if(!strcmp(row[3],"1")) {
				if (row[6])
					cardinality = strtoll(row[6],NULL,10);
				if (cardinality>max_cardinality) {
					field=row[4];
					max_cardinality=cardinality;
				}
			}
		}
	}
	/* Oh well, no chunks today - no suitable index */
	if (!field) goto cleanup;

	/* Get minimum/maximum */
	mysql_query(conn, query=g_strdup_printf("SELECT %s MIN(`%s`),MAX(`%s`) FROM `%s`.`%s`", (detected_server == SERVER_TYPE_MYSQL) ? "/*!40001 SQL_NO_CACHE */" : "", field, field, database, table));
	g_free(query);
	minmax=mysql_store_result(conn);

	if (!minmax)
		goto cleanup;

	row=mysql_fetch_row(minmax);
	MYSQL_FIELD * fields=mysql_fetch_fields(minmax);
	char *min=row[0];
	char *max=row[1];

	/* Got total number of rows, skip chunk logic if estimates are low */
	guint64 rows = estimate_count(conn, database, table, field, NULL, NULL);
	if (rows <= rows_per_file)
		goto cleanup;

	/* This is estimate, not to use as guarantee! Every chunk would have eventual adjustments */
	guint64 estimated_chunks = rows / rows_per_file;
	guint64 estimated_step, nmin, nmax, cutoff;

	/* Support just bigger INTs for now, very dumb, no verify approach */
	switch (fields[0].type) {
		case MYSQL_TYPE_LONG:
		case MYSQL_TYPE_LONGLONG:
		case MYSQL_TYPE_INT24:
			/* static stepping */
			nmin = strtoll(min,NULL,10);
			nmax = strtoll(max,NULL,10);
			estimated_step = (nmax-nmin)/estimated_chunks+1;
			cutoff = nmin;
			while(cutoff<=nmax) {
				chunks=g_list_append(chunks,g_strdup_printf("%s%s%s%s(`%s` >= %llu AND `%s` < %llu)",
						!showed_nulls?"`":"",
						!showed_nulls?field:"",
						!showed_nulls?"`":"",
						!showed_nulls?" IS NULL OR ":"",
						field, (unsigned long long)cutoff,
						field, (unsigned long long)(cutoff+estimated_step)));
				cutoff+=estimated_step;
				showed_nulls=1;
			}

		default:
			goto cleanup;
	}


cleanup:
	if (indexes)
		mysql_free_result(indexes);
	if (minmax)
		mysql_free_result(minmax);
	if (total)
		mysql_free_result(total);
	return chunks;
}

/* Try to get EXPLAIN'ed estimates of row in resultset */
guint64 estimate_count(MYSQL *conn, char *database, char *table, char *field, char *from, char *to) {
	char *querybase, *query;
	int ret;

	g_assert(conn && database && table);

	querybase = g_strdup_printf("EXPLAIN SELECT `%s` FROM `%s`.`%s`", (field?field:"*"), database, table);
	if (from || to) {
		g_assert(field != NULL);
		char *fromclause=NULL, *toclause=NULL;
		char *escaped;
		if (from) {
			escaped=g_new(char,strlen(from)*2+1);
			mysql_real_escape_string(conn,escaped,from,strlen(from));
			fromclause = g_strdup_printf(" `%s` >= \"%s\" ", field, escaped);
			g_free(escaped);
		}
		if (to) {
			escaped=g_new(char,strlen(to)*2+1);
			mysql_real_escape_string(conn,escaped,from,strlen(from));
			toclause = g_strdup_printf( " `%s` <= \"%s\"", field, escaped);
			g_free(escaped);
		}
		query = g_strdup_printf("%s WHERE `%s` %s %s", querybase, (from?fromclause:""), ((from&&to)?"AND":""), (to?toclause:""));

		if (toclause) g_free(toclause);
		if (fromclause) g_free(fromclause);
		ret=mysql_query(conn,query);
		g_free(querybase);
		g_free(query);
	} else {
		ret=mysql_query(conn,querybase);
		g_free(querybase);
	}

	if (ret) {
		g_warning("Unable to get estimates for %s.%s: %s",database,table,mysql_error(conn));
	}

	MYSQL_RES * result = mysql_store_result(conn);
	MYSQL_FIELD * fields = mysql_fetch_fields(result);

	guint i;
	for (i=0; i<mysql_num_fields(result); i++)  {
		if (!strcmp(fields[i].name,"rows"))
			break;
	}

	MYSQL_ROW row = NULL;

	guint64 count=0;

	if (result)
		row = mysql_fetch_row(result);

	if (row && row[i])
		count=strtoll(row[i],NULL,10);

	if (result)
		mysql_free_result(result);

	return(count);
}

void create_backup_dir(char *new_directory) {
	if (g_mkdir(new_directory, 0700) == -1)
	{
		if (errno != EEXIST)
		{
			g_critical("Unable to create `%s': %s",
				new_directory,
				g_strerror(errno));
			exit(EXIT_FAILURE);
		}
	}
}

void dump_database(MYSQL * conn, char *database) {

	char *query;
	mysql_select_db(conn,database);
	if (detected_server == SERVER_TYPE_MYSQL)
		query= g_strdup("SHOW TABLE STATUS");
	else
		query= g_strdup_printf("SELECT TABLE_NAME, ENGINE, TABLE_TYPE as COMMENT FROM DATA_DICTIONARY.TABLES WHERE TABLE_SCHEMA='%s'", database);

	if (mysql_query(conn, (query))) {
		g_critical("Error: DB: %s - Could not execute query: %s", database, mysql_error(conn));
		errors++;
		return;
	}

	g_free(query);

	MYSQL_RES *result = mysql_store_result(conn);
	MYSQL_FIELD *fields= mysql_fetch_fields(result);
	guint i;
	int ecol= -1, ccol= -1;
	for (i=0; i<mysql_num_fields(result); i++) {
		if (!strcasecmp(fields[i].name, "Engine")) ecol= i;
		else if (!strcasecmp(fields[i].name, "Comment")) ccol= i;
	}

	if (!result) {
		g_critical("Could not list tables for %s: %s", database, mysql_error(conn));
		errors++;
		return;
	}

	MYSQL_ROW row;
	while ((row = mysql_fetch_row(result))) {

		int dump=1;

		/* We no care about views!
			num_fields>1 kicks in only in case of 5.0 SHOW FULL TABLES or SHOW TABLE STATUS
			row[1] == NULL if it is a view in 5.0 'SHOW TABLE STATUS'
			row[1] == "VIEW" if it is a view in 5.0 'SHOW FULL TABLES'
		*/
		if ((detected_server == SERVER_TYPE_MYSQL) && ( row[ccol] == NULL || !strcmp(row[ccol],"VIEW") ))
			continue;

		/* Skip ignored engines, handy for avoiding Merge, Federated or Blackhole :-) dumps */
		if (ignore) {
			for (i = 0; ignore[i] != NULL; i++) {
				if (g_ascii_strcasecmp(ignore[i], row[ecol]) == 0) {
					dump = 0;
					break;
				}
			}
		}
		if (!dump)
			continue;

		/* In case of table-list option is enabled, check if table is part of the list */
		if (tables) {
			int table_found=0;
			for (i = 0; tables[i] != NULL; i++)
				if (g_ascii_strcasecmp(tables[i], row[0]) == 0)
					table_found = 1;

			if (!table_found)
				dump = 0;
		}
		if (!dump)
			continue;

		/* Checks PCRE expressions on 'database.table' string */
		if (regexstring && !check_regex(database,row[0]))
			continue;

		/* Green light! */
		struct db_table *dbt = g_new(struct db_table, 1);
		dbt->database= g_strdup(database);
		dbt->table= g_strdup(row[0]);
		if (!g_ascii_strcasecmp("InnoDB", row[ecol])) {
			innodb_tables= g_list_append(innodb_tables, dbt);

		} else {
			non_innodb_table= g_list_append(non_innodb_table, dbt);
		}
	        if (!no_schemas) {
			table_schemas= g_list_append(table_schemas, dbt);
        	}
	}
	mysql_free_result(result);
}

void dump_schema_data(MYSQL *conn, char *database, char *table, char *filename) {
	void *outfile;
	char *query = NULL;
	MYSQL_RES *result = NULL;
	MYSQL_ROW row;

	if (!compress_output)
		outfile= g_fopen(filename, "w");
	else
		outfile= (void*) gzopen(filename, "w");

	if (!outfile) {
		g_critical("Error: DB: %s Could not create output file %s (%d)", database, filename, errno);
		errors++;
		return;
	}
        GString* statement = g_string_sized_new(statement_size);

	if (detected_server == SERVER_TYPE_MYSQL) {
		g_string_printf(statement,"/*!40101 SET NAMES binary*/;\n");
		g_string_append(statement,"/*!40014 SET FOREIGN_KEY_CHECKS=0*/;\n\n");
	} else {
		g_string_printf(statement, "SET FOREIGN_KEY_CHECKS=0;\n");
	}

	if (!write_data((FILE *)outfile,statement)) {
		g_critical("Could not write schema data for %s.%s", database, table);
		errors++;
		return;
	}

	query= g_strdup_printf("SHOW CREATE TABLE `%s`.`%s`", database, table);
	if (mysql_query(conn, query) || !(result= mysql_use_result(conn))) {
		if(success_on_1146 && mysql_errno(conn) == 1146){
			g_warning("Error dumping schemas (%s.%s): %s", database, table, mysql_error(conn));
		}else{
			g_critical("Error dumping schemas (%s.%s): %s", database, table, mysql_error(conn));
			errors++;
		}
		g_free(query);
		return;
	}


	g_string_set_size(statement, 0);

	/* There should never be more than one row */
	row = mysql_fetch_row(result);
	g_string_append(statement, row[1]);
	g_string_append(statement, ";\n");
	if (!write_data((FILE *)outfile, statement)) {
		g_critical("Could not write schema for %s.%s", database, table);
		errors++;
	}
	g_free(query);

        if (!compress_output)
                fclose((FILE *)outfile);
        else
                gzclose((gzFile)outfile);


	g_string_free(statement, TRUE);
	if (result)
		mysql_free_result(result);

	return;
}

void dump_table_data_file(MYSQL *conn, char *database, char *table, char *where, char *filename) {
	void *outfile;

	if (!compress_output)
		outfile = g_fopen(filename, "w");
	else
		outfile = (void*) gzopen(filename, "w");

	if (!outfile) {
		g_critical("Error: DB: %s TABLE: %s Could not create output file %s (%d)", database, table, filename, errno);
		errors++;
		return;
	}
	guint64 rows_count = dump_table_data(conn, (FILE *)outfile, database, table, where, filename);

	if (!rows_count && !build_empty_files) {
		// dropping the useless file
		if (remove(filename)) {
 			g_warning("failed to remove empty file : %s\n", filename);
 			return;
		}
	}

}

void dump_schema(char *database, char *table, struct configuration *conf) {
	struct job *j = g_new0(struct job,1);
	struct schema_job *sj = g_new0(struct schema_job,1);
	j->job_data=(void*) sj;
	sj->database=g_strdup(database);
	sj->table=g_strdup(table);
	j->conf=conf;
	j->type=JOB_SCHEMA;
	if (daemon_mode)
		sj->filename = g_strdup_printf("%s/%d/%s.%s-schema.sql%s", output_directory, dump_number, database, table, (compress_output?".gz":""));
	else
		sj->filename = g_strdup_printf("%s/%s.%s-schema.sql%s", output_directory, database, table, (compress_output?".gz":""));
	g_async_queue_push(conf->queue,j);
	return;
}

void dump_table(MYSQL *conn, char *database, char *table, struct configuration *conf, gboolean is_innodb) {

	GList * chunks = NULL;
	if (rows_per_file)
		chunks = get_chunks_for_table(conn, database, table, conf);


	if (chunks) {
		int nchunk=0;
		for (chunks = g_list_first(chunks); chunks; chunks=g_list_next(chunks)) {
			struct job *j = g_new0(struct job,1);
			struct table_job *tj = g_new0(struct table_job,1);
			j->job_data=(void*) tj;
			tj->database=g_strdup(database);
			tj->table=g_strdup(table);
			j->conf=conf;
			j->type= is_innodb ? JOB_DUMP : JOB_DUMP_NON_INNODB;
			if (daemon_mode)
				tj->filename=g_strdup_printf("%s/%d/%s.%s.%05d.sql%s", output_directory, dump_number, database, table, nchunk,(compress_output?".gz":""));
			else
				tj->filename=g_strdup_printf("%s/%s.%s.%05d.sql%s", output_directory, database, table, nchunk,(compress_output?".gz":""));
			tj->where=(char *)chunks->data;
			g_async_queue_push(conf->queue,j);
			nchunk++;
		}
		g_list_free(g_list_first(chunks));
	} else {
		struct job *j = g_new0(struct job,1);
		struct table_job *tj = g_new0(struct table_job,1);
		j->job_data=(void*) tj;
		tj->database=g_strdup(database);
		tj->table=g_strdup(table);
		j->conf=conf;
		j->type= is_innodb ? JOB_DUMP : JOB_DUMP_NON_INNODB;
		if (daemon_mode)
			if (chunk_filesize)
				tj->filename = g_strdup_printf("%s/%d/%s.%s.00001.sql%s", output_directory, dump_number, database, table,(compress_output?".gz":""));
			else
				tj->filename = g_strdup_printf("%s/%d/%s.%s.sql%s", output_directory, dump_number, database, table,(compress_output?".gz":""));
		else
			if (chunk_filesize)	
				tj->filename = g_strdup_printf("%s/%s.%s.00001.sql%s", output_directory, database, table,(compress_output?".gz":""));
			else
				tj->filename = g_strdup_printf("%s/%s.%s.sql%s", output_directory, database, table,(compress_output?".gz":""));
		g_async_queue_push(conf->queue,j);
		return;
	}
}

/* Do actual data chunk reading/writing magic */
guint64 dump_table_data(MYSQL * conn, FILE *file, char *database, char *table, char *where, char *filename)
{
	guint i;
	guint fn = 1;
	guint st_in_file = 0;
	guint num_fields = 0;
	guint64 num_rows = 0;
	guint64 num_rows_st = 0;
	MYSQL_RES *result = NULL;
	char *query = NULL;
	gchar *fcfile = NULL;
	gchar* filename_prefix = NULL;
	
	if(chunk_filesize){
		gchar** split_filename= g_strsplit(filename, ".00001.sql", 0);
		filename_prefix= split_filename[0];
		g_free(split_filename);
	}
	
	/* Ghm, not sure if this should be statement_size - but default isn't too big for now */
	GString* statement = g_string_sized_new(statement_size);
	GString* statement_row = g_string_sized_new(0);
	
	if (detected_server == SERVER_TYPE_MYSQL) {
		g_string_printf(statement,"/*!40101 SET NAMES binary*/;\n");
		g_string_append(statement,"/*!40014 SET FOREIGN_KEY_CHECKS=0*/;\n");
		if (!skip_tz) {
		  g_string_append(statement,"/*!40103 SET TIME_ZONE='+00:00' */;\n");
		}
	} else {
		g_string_printf(statement,"SET FOREIGN_KEY_CHECKS=0;\n");
	}

	if (!write_data(file,statement)) {
		g_critical("Could not write out data for %s.%s", database, table);
		return num_rows;
	}

	/* Poor man's database code */
 	query = g_strdup_printf("SELECT %s * FROM `%s`.`%s` %s %s", (detected_server == SERVER_TYPE_MYSQL) ? "/*!40001 SQL_NO_CACHE */" : "", database, table, where?"WHERE":"",where?where:"");
	if (mysql_query(conn, query) || !(result=mysql_use_result(conn))) {
		//ERROR 1146 
		if(success_on_1146 && mysql_errno(conn) == 1146){
			g_warning("Error dumping table (%s.%s) data: %s ",database, table, mysql_error(conn));
		}else{
			g_critical("Error dumping table (%s.%s) data: %s ",database, table, mysql_error(conn));
			errors++;
		}
		g_free(query);
		return num_rows;
	}

	num_fields = mysql_num_fields(result);
	MYSQL_FIELD *fields = mysql_fetch_fields(result);

	/* Buffer for escaping field values */
	GString *escaped = g_string_sized_new(3000);

	MYSQL_ROW row;

	g_string_set_size(statement,0);

	/* Poor man's data dump code */
	while ((row = mysql_fetch_row(result))) {
		gulong *lengths = mysql_fetch_lengths(result);
		num_rows++;

		if (!statement->len){
			g_string_printf(statement, "INSERT INTO `%s` VALUES", table);
			num_rows_st = 0;
		}
		
		if (statement_row->len) {
			g_string_append(statement, statement_row->str);
			g_string_set_size(statement_row,0);
			num_rows_st++;
		}
		
		g_string_append(statement_row, "\n(");

		for (i = 0; i < num_fields; i++) {
			/* Don't escape safe formats, saves some time */
			if (!row[i]) {
				g_string_append(statement_row, "NULL");
			} else if (fields[i].flags & NUM_FLAG) {
				g_string_append(statement_row, row[i]);
			} else {
				/* We reuse buffers for string escaping, growing is expensive just at the beginning */
				g_string_set_size(escaped, lengths[i]*2+1);
				mysql_real_escape_string(conn, escaped->str, row[i], lengths[i]);
				g_string_append_c(statement_row,'\"');
				g_string_append(statement_row,escaped->str);
				g_string_append_c(statement_row,'\"');
			}
			if (i < num_fields - 1) {
				g_string_append_c(statement_row,',');
			} else {
				g_string_append_c(statement_row,')');
				/* INSERT statement is closed before over limit */
				if(statement->len+statement_row->len+1 > statement_size) {
					if(num_rows_st == 0){
						g_string_append(statement, statement_row->str);
						g_string_set_size(statement_row,0);
						g_warning("Row bigger than statement_size for %s.%s", database, table);
					}
					g_string_append(statement,";\n");

					if (!write_data(file,statement)) {
						g_critical("Could not write out data for %s.%s", database, table);
						goto cleanup;
					}else{
						st_in_file++;
						if(chunk_filesize && st_in_file*statement_size > chunk_filesize){
							fn++;
							fcfile = g_strdup_printf("%s.%05d.sql%s", filename_prefix,fn,(compress_output?".gz":""));
							if (!compress_output){
								fclose((FILE *)file);
								file = g_fopen(fcfile, "w");
                            } else {
								gzclose((gzFile)file);
                                file = (void*) gzopen(fcfile, "w");
                            }
							st_in_file = 0;
						}
					}
					g_string_set_size(statement,0);
				} else {
					if(num_rows > 1)
						g_string_append(statement,",");
					g_string_append(statement, statement_row->str);
					num_rows_st++;
					g_string_set_size(statement_row,0);
				}
			}
		}
	}
	if (mysql_errno(conn)) {
		g_critical("Could not read data from %s.%s: %s", database, table, mysql_error(conn));
	}

	if (statement->len > 0) {
		g_string_append(statement,";\n");
		if (!write_data(file,statement)) {
			g_critical("Could not write out closing newline for %s.%s, now this is sad!", database, table);
			goto cleanup;
		}
		st_in_file++;
	}

cleanup:
	g_free(query);

	g_string_free(escaped,TRUE);
	g_string_free(statement,TRUE);
	g_free(filename_prefix);
	

	if (result) {
		mysql_free_result(result);
	}
	
	if (!compress_output){
		fclose((FILE *)file);
	} else {
		gzclose((gzFile)file);
	}
	
	if (!st_in_file && !build_empty_files) {
		// dropping the useless file
		if (remove(fcfile)) {
 			g_warning("xxxfailed to remove empty file : %d : %s\n", st_in_file, fcfile);
		}
	}

	g_free(fcfile);
	
	return num_rows;
}

gboolean write_data(FILE* file,GString * data) {
	size_t written= 0;
	ssize_t r= 0;

	while (written < data->len) {
		if (!compress_output)
			r = write(fileno(file), data->str + written, data->len);
		else
			r = gzwrite((gzFile)file, data->str + written, data->len);

		if (r < 0) {
			g_critical("Couldn't write data to a file: %s", strerror(errno));
			errors++;
			return FALSE;
		}
		written += r;
	}

	return TRUE;
}

void write_log_file(const gchar *log_domain, GLogLevelFlags log_level, const gchar *message, gpointer user_data) {
	(void) log_domain;
	(void) user_data;

	gchar date[20];
	time_t rawtime;
	struct tm timeinfo;

	time(&rawtime);
	localtime_r(&rawtime, &timeinfo);
	strftime(date, 20, "%Y-%m-%d %H:%M:%S", &timeinfo);

	GString* message_out = g_string_new(date);
	if (log_level & G_LOG_LEVEL_DEBUG) {
		g_string_append(message_out, " [DEBUG] - ");
	} else if ((log_level & G_LOG_LEVEL_INFO)
		|| (log_level & G_LOG_LEVEL_MESSAGE)) {
		g_string_append(message_out, " [INFO] - ");
	} else if (log_level & G_LOG_LEVEL_WARNING) {
		g_string_append(message_out, " [WARNING] - ");
	} else if ((log_level & G_LOG_LEVEL_ERROR)
		|| (log_level & G_LOG_LEVEL_CRITICAL)) {
		g_string_append(message_out, " [ERROR] - ");
	}

	g_string_append_printf(message_out, "%s\n", message);
	if (write(fileno(logoutfile), message_out->str, message_out->len) <= 0) {
		fprintf(stderr, "Cannot write to log file with error %d.  Exiting...", errno);
	}
	g_string_free(message_out, TRUE);
}
