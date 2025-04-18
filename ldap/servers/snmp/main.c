/* --- BEGIN COPYRIGHT BLOCK ---
 * Copyright (C) 2021 Red Hat, Inc.
 * All rights reserved.
 *
 * License: GPL (version 3 or any later version).
 * See LICENSE for details.
 * --- END COPYRIGHT BLOCK --- */

#ifdef HAVE_CONFIG_H
#include <config.h>
#endif


#include <signal.h>
#include <string.h>
#include <stdio.h>
#include <unistd.h>
#include <inttypes.h>
#include <signal.h>
#include <sys/stat.h>
#include "ldap-agent.h"
#include "ldap.h"
#include "ldif.h"
#include <ctype.h>
#include <limits.h>
#include <errno.h>

static char *agentx_master = NULL;
static char *agent_logdir = NULL;
static char *pidfile = NULL;
server_instance *server_head = NULL;

static int keep_running;

RETSIGTYPE
stop_server(int signum)
{
    if (signum == SIGUSR1) {
        snmp_log(LOG_WARNING, "Detected attempt to start ldap-agent again.\n");
    } else {
        snmp_log(LOG_WARNING, "Received stop signal.  Stopping ldap-agent...\n");
        keep_running = 0;
    }
}

int
main(int argc, char *argv[])
{
    char *config_file = NULL;
    netsnmp_log_handler *log_hdl = NULL;
    int c, log_level = LOG_WARNING;
    struct stat logdir_s;
    pid_t child_pid = 0;
    FILE *pid_fp;

    /* Pause for the debugger if DEBUG_SLEEP is set in the environment */
    {
        char *s = getenv("DEBUG_SLEEP");
        if ((s != NULL) && isdigit(*s)) {
            char *endp = NULL;
            int64_t secs;
            errno = 0;

            secs = strtol(s, &endp, 10);
            if ( endp == s ||
                 *endp != '\0' ||
                 ((secs == LONG_MIN || secs == LONG_MAX) && errno == ERANGE) ||
                 secs < 1 )
            {
                /* Invalid value, default to 30 seconds */
                secs = 30;
            } else if (secs > 3600) {
                secs = 3600;
            }
            printf("%s pid is %d - sleeping for %" PRId64 "\n", argv[0], getpid(), secs);
            sleep(secs);
        }
    }

    /* Load options */
    while ((--argc > 0) && ((*++argv)[0] == '-')) {
        while ((c = *++argv[0])) {
            switch (c) {
            case 'D':
                log_level = LOG_DEBUG;
                break;
            default:
                printf("ldap-agent: illegal option %c\n", c);
                exit_usage();
            }
        }
    }

    if (argc != 1) {
        exit_usage();
    }

    /* load config file */
    if ((config_file = strdup(*argv)) == NULL) {
        printf("ldap-agent: Memory error loading config file\n");
        exit(1);
    }

    load_config(config_file);
    free(config_file);

    /* check if we're already running as another process */
    if ((pid_fp = fopen(pidfile, "r")) != NULL) {
        int rc = fscanf(pid_fp, "%d", &child_pid);
        if ((rc == 0) || (rc == EOF)) {
            printf("ldap-agent: Failed to get pid from %s\n", pidfile);
            exit(1);
        }
        fclose(pid_fp);
        if (kill(child_pid, SIGUSR1) == 0) {
            printf("ldap-agent: Already running as pid %d %s!\n", child_pid, pidfile);
            exit(1);
        } else {
            /* old pidfile exists, but the process doesn't. Cleanup pidfile */
            remove(pidfile);
        }
    }

    /* start logging */
    netsnmp_ds_set_boolean(NETSNMP_DS_LIBRARY_ID,
                           NETSNMP_DS_LIB_LOG_TIMESTAMP, 1);

    if ((log_hdl = netsnmp_register_loghandler(NETSNMP_LOGHANDLER_FILE,
                                               log_level)) != NULL) {
        if (agent_logdir != NULL) {
            /* Verify agent-logdir setting */
            if (stat(agent_logdir, &logdir_s) < 0) {
                printf("ldap-agent: Error reading logdir: %s\n", agent_logdir);
                exit(1);
            } else {
                /* Is it a directory? */
                if (S_ISDIR(logdir_s.st_mode)) {
                    /* Can we write to it? */
                    if (access(agent_logdir, W_OK) < 0) {
                        printf("ldap-agent: Unable to write to logdir: %s\n",
                               agent_logdir);
                        exit(1);
                    }
                } else {
                    printf("ldap-agent: agent-logdir setting must point to a directory.\n");
                    exit(1);
                }
            }

            /* agent-logdir setting looks ok */
            if ((log_hdl->token = malloc(strlen(agent_logdir) +
                                         strlen(LDAP_AGENT_LOGFILE) + 2)) != NULL) {
                memcpy((char *)log_hdl->token, agent_logdir, strlen(agent_logdir) + 1);
                /* add a trailing slash if needed */
                if (*(agent_logdir + strlen(agent_logdir)) != '/')
                    strcat((char *)log_hdl->token, "/");
                strcat((char *)log_hdl->token, LDAP_AGENT_LOGFILE);
                ((char *)log_hdl->token)[(strlen(agent_logdir) + strlen(LDAP_AGENT_LOGFILE) + 1)] = (char)0;
            }
        } else {
            /* agent-logdir not set */
            printf("ldap-agent: Error determining log directory.\n");
            exit(1);
        }

        snmp_enable_filelog((char *)log_hdl->token, 1);
    } else {
        printf("Error starting logging.");
        exit(1);
    }

    snmp_log(LOG_WARNING, "Starting ldap-agent...\n");

    /* setup agentx master */
    netsnmp_ds_set_boolean(NETSNMP_DS_APPLICATION_ID,
                           NETSNMP_DS_AGENT_ROLE, 1);
    if (agentx_master)
        netsnmp_ds_set_string(NETSNMP_DS_APPLICATION_ID,
                              NETSNMP_DS_AGENT_X_SOCKET, agentx_master);

    /* run as a daemon */
    if (netsnmp_daemonize(0, 0)) {
        int i;
        int rc;

        /* sleep to allow pidfile to be created by child */
        for (i = 0; i < 3; i++) {
            sleep(5);
            if ((pid_fp = fopen(pidfile, "r")) != NULL) {
                break;
            }
        }

        if (!pid_fp) {
            printf("ldap-agent: Not started after 15 seconds!  Check log file for details.\n");
            exit(1);
        }

        rc = fscanf(pid_fp, "%d", &child_pid);
        if ((rc == 0) || (rc == EOF)) {
            printf("ldap-agent: Failed to get pid from %s\n", pidfile);
            exit(1);
        }
        fclose(pid_fp);
        printf("ldap-agent: Started as pid %d\n", child_pid);
        exit(0);
    }

    /* initialize the agent */
    init_agent("ldap-agent");
    init_ldap_agent();
    init_snmp("ldap-agent");

    /* listen for signals */
    keep_running = 1;
    signal(SIGUSR1, stop_server);
    signal(SIGTERM, stop_server);
    signal(SIGINT, stop_server);

    /* create pidfile */
    child_pid = getpid();
    if ((pid_fp = fopen(pidfile, "w")) == NULL) {
        snmp_log(LOG_ERR, "Error creating pid file: %s\n", pidfile);
        exit(1);
    } else {
        if (fprintf(pid_fp, "%d", child_pid) < 0) {
            snmp_log(LOG_ERR, "Error writing pid file: %s\n", pidfile);
            exit(1);
        }
        fclose(pid_fp);
    }

    /* we're up and running! */
    snmp_log(LOG_WARNING, "Started ldap-agent as pid %d\n", child_pid);

    /* loop here until asked to stop */
    while (keep_running) {
        agent_check_and_process(1);
    }

    /* say goodbye */
    snmp_shutdown("ldap-agent");
    snmp_log(LOG_WARNING, "ldap-agent stopped.\n");

    /* remove pidfile */
    remove(pidfile);

    return 0;
}

/* ldif_read_record lineno argument type depends on openldap version */
#if LDAP_VENDOR_VERSION >= 20434 /* changed in 2.4.34 */
typedef unsigned long int ldif_record_lineno_t;
#else
typedef int ldif_record_lineno_t;
#endif

/************************************************************************
 * load_config
 *
 * Loads subagent config file and reads directory server config files.
 */
void
load_config(char *conf_path)
{
    server_instance *serv_p = NULL;
    FILE *conf_file = NULL;
    LDIFFP *dse_fp = NULL;
    int buflen = 0;
    ldif_record_lineno_t lineno = 0;
    char line[MAXLINE];
    char *p = NULL;
    int error = 0;

    /* Make sure we are getting an absolute path */
    if (*conf_path != '/') {
        printf("ldap-agent: Error opening config file: %s\n", conf_path);
        printf("ldap-agent: You must specify the absolute path to your config file\n");
        error = 1;
        goto close_and_exit;
    }

    /* Open config file */
    if ((conf_file = fopen(conf_path, "r")) == NULL) {
        printf("ldap-agent: Error opening config file: %s\n", conf_path);
        error = 1;
        goto close_and_exit;
    }

    /* set pidfile path */
    if ((pidfile = malloc(strlen(LOCALRUNDIR) + strlen("/dirsrv/") +
                          strlen(LDAP_AGENT_PIDFILE) + 1)) != NULL) {
        strncpy(pidfile, LOCALRUNDIR, strlen(LOCALRUNDIR) + 1);
        /* The above will likely not be NULL terminated, but we need to
         * be sure that we're properly NULL terminated for the below
         * strcat() to work properly. */
        pidfile[strlen(LOCALRUNDIR)] = (char)0;
        strcat(pidfile, "/dirsrv/");
        strcat(pidfile, LDAP_AGENT_PIDFILE);
    } else {
        printf("ldap-agent: malloc error processing config file\n");
        error = 1;
        goto close_and_exit;
    }

    /* set default logdir to location of config file */
    for (p = (conf_path + strlen(conf_path) - 1); p >= conf_path; p--) {
        if (*p == '/') {
            if ((agent_logdir = malloc((p - conf_path) + 1)) != NULL) {
                strncpy(agent_logdir, conf_path, (p - conf_path));
                agent_logdir[(p - conf_path)] = (char)0;
                break;
            } else {
                printf("ldap-agent: malloc error processing config file\n");
                error = 1;
                goto close_and_exit;
            }
        }
    }

    while (fgets(line, MAXLINE, conf_file) != NULL) {
        /* Ignore comment lines in config file */
        if (line[0] == '#')
            continue;

        if ((p = strstr(line, "agentx-master")) != NULL) {
            /* load agentx-master setting */
            p = p + 13;
            if ((p = strtok(p, " \t\n")) != NULL) {
                if (agentx_master) {
                    free(agentx_master);
                }
                if ((agentx_master = (char *)malloc(strlen(p) + 1)) != NULL)
                    strcpy(agentx_master, p);
            }
        } else if ((p = strstr(line, "agent-logdir")) != NULL) {
            /* free the default logdir setting */
            if (agent_logdir != NULL) {
                free(agent_logdir);
                agent_logdir = NULL;
            }

            /* load agent-logdir setting */
            p = p + 12;
            if ((p = strtok(p, " \t\n")) != NULL) {
                if ((agent_logdir = (char *)malloc(strlen(p) + 1)) != NULL)
                    strcpy(agent_logdir, p);
            }
        } else if ((p = strstr(line, "server")) != NULL) {
            int got_port = 0;
            int got_rundir = 0;
            int got_snmp_index = 0;
            long snmp_index = 0;
            char *entry = NULL;
            char *instancename = NULL;

            lineno = 0;
            /* Allocate a server_instance */
            if ((serv_p = malloc(sizeof(server_instance))) == NULL) {
                printf("ldap-agent: malloc error processing config file\n");
                error = 1;
                goto close_and_exit;
            }

            /* load server setting */
            p = p + 6;
            if ((p = strtok(p, " \t\n")) != NULL) {
                /* first token is the instance name */
                instancename = strdup(p);
                serv_p->dse_ldif = malloc(strlen(p) + strlen(SYSCONFDIR) +
                                          strlen(PACKAGE_NAME) + 12);
                if (serv_p->dse_ldif != NULL) {
                    snprintf(serv_p->dse_ldif, strlen(p) + strlen(SYSCONFDIR) +
                                                   strlen(PACKAGE_NAME) + 12,
                             "%s/%s/%s/dse.ldif",
                             SYSCONFDIR, PACKAGE_NAME, p);
                    serv_p->dse_ldif[(strlen(p) + strlen(SYSCONFDIR) +
                                      strlen(PACKAGE_NAME) + 11)] = (char)0;
                } else {
                    printf("ldap-agent: malloc error processing config file\n");
                    error = 1;
                    free(instancename);
                    instancename = NULL;
                    goto close_and_exit;
                }

                /* set the semaphore name */
                /* "/" + ".stats" + \0 = 8 */
                serv_p->stats_sem_name = malloc(strlen(p) + 8);
                if (serv_p->stats_sem_name != NULL) {
                    snprintf(serv_p->stats_sem_name, strlen(p) + 8, "/%s.stats", p);
                } else {
                    printf("ldap-agent: malloc error processing config file\n");
                    error = 1;
                    free(instancename);
                    instancename = NULL;
                    goto close_and_exit;
                }
            } else {
                printf("ldap-agent: missing instance name\n");
                error = 1;
                goto close_and_exit;
            }

            /* Open dse.ldif */
            dse_fp = ldif_open(serv_p->dse_ldif, "r");
            buflen = 0;
            if (dse_fp == NULL) {
                printf("ldap-agent: Error opening server config file: %s\n",
                       serv_p->dse_ldif);
                error = 1;
                free(instancename);
                instancename = NULL;
                goto close_and_exit;
            }

            /* ldif_get_entry will realloc the entry if it's not null,
             * so we can just free it when we're done fetching entries
             * from the dse.ldif.  Unfortunately, ldif_getline moves
             * the pointer that is passed to it, so we need to save a
             * pointer to the beginning of the entry so we can free it
             * later. */
            while (ldif_read_record(dse_fp, &lineno, &entry, &buflen))
            {
                char *entryp = entry;
                char *attr = NULL;
                char *val = NULL;
                ber_len_t vlen;
                /* Check if this is the cn=config entry */
                if (ldif_parse_line(ldif_getline(&entryp), &attr, &val, &vlen)) {
                    printf("ldap-agent: error parsing ldif line from [%s]\n", serv_p->dse_ldif);
                }

                if ((strcmp(attr, "dn") == 0) && (strcmp(val, "cn=config") == 0)) {
                    char *dse_line = NULL;
                    /* Free both outer values and attr */
                    free(attr);
                    free(val);
                    attr = NULL;
                    val = NULL;

                    /* Look for port and rundir attributes */
                    while ((dse_line = ldif_getline(&entryp)) != NULL) {
                        if (ldif_parse_line(dse_line, &attr, &val, &vlen) != 0) {
                            /* can't parse these, try next line instead */
                            continue;
                        } else if (strcmp(attr, "nsslapd-snmp-index") == 0) {
                            snmp_index = atol(val);
                            got_snmp_index = 1;
                        } else if (strcmp(attr, "nsslapd-port") == 0) {
                            serv_p->port = atol(val);
                            got_port = 1;
                        } else if (strcmp(attr, "nsslapd-rundir") == 0) {
                            /* 8 =  "/" + ".stats" + \0 */
                            serv_p->stats_file = calloc(1, vlen + (instancename ? strlen(instancename) : 0) + 8);
                            if (serv_p->stats_file && instancename) {
                                snprintf(serv_p->stats_file, vlen + strlen(instancename) + 7,
                                         "%s/%s.stats", val, instancename);
                            } else {
                                printf("ldap-agent: malloc error processing config file\n");
                                free(entry);
                                error = 1;
                                free(instancename);
                                instancename = NULL;
                                goto close_and_exit;
                            }
                            got_rundir = 1;
                        }
                        free(attr);
                        free(val);
                        attr = NULL;
                        val = NULL;

                        /* Stop processing this entry if we found the
                         *  port and rundir and snmp_index settings */
                        if (got_port && got_rundir && got_snmp_index) {
                            break;
                        }
                    }
                    /* The port and rundir settings must be in the
                     * cn=config entry, so we can stop reading through
                     * the dse.ldif now. */
                    break;
                } else {
                    free(attr);
                    free(val);
                    attr = NULL;
                    val = NULL;
                }
            }

            free(instancename);
            instancename = NULL;
            /* We're done reading entries from dse_ldif now, so
             * we can free entry */
            free(entry);

            /* Make sure we were able to read the port and
             * location of the stats file. */
            if (!got_port) {
                printf("ldap-agent: Error reading nsslapd-port from "
                       "server config file: %s\n",
                       serv_p->dse_ldif);
                error = 1;
                goto close_and_exit;
            } else if (!got_rundir) {
                printf("ldap-agent: Error reading nsslapd-rundir from "
                       "server config file: %s\n",
                       serv_p->dse_ldif);
                error = 1;
                goto close_and_exit;
            }

            /* in case a snmp index is specified, it replace the nsslapd-port
             * This would allow to give an index to a snmp report, rather than using
             * the TCP interface port number (because the same port may be listen on multiple interfaces).
             * For snmp_index values <= 0 (disabled), let's keep the port
             */
            if (got_snmp_index && (snmp_index > 0)) {
                serv_p->port = snmp_index;
            }

            /* Insert server instance into linked list */
            serv_p->next = server_head;
            server_head = serv_p;
        }
    }

    /* check for at least one directory server instance */
    if (server_head == NULL) {
        printf("ldap-agent: No server instances defined in config file\n");
        error = 1;
        goto close_and_exit;
    }

close_and_exit:
    if (conf_file)
        fclose(conf_file);
    if (dse_fp) {
        ldif_close(dse_fp);
    }
    if (error)
        exit(error);
}

/************************************************************************
 * exit_usage
 *
 * Prints usage message and exits program.
 */
void
exit_usage(void)
{
    printf("Usage: ldap-agent [-D] configfile\n");
    printf("       -D    Enable debug logging\n");
    exit(1);
}
