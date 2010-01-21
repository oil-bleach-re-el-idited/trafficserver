/** @file

  A brief file description

  @section license License

  Licensed to the Apache Software Foundation (ASF) under one
  or more contributor license agreements.  See the NOTICE file
  distributed with this work for additional information
  regarding copyright ownership.  The ASF licenses this file
  to you under the Apache License, Version 2.0 (the
  "License"); you may not use this file except in compliance
  with the License.  You may obtain a copy of the License at

      http://www.apache.org/licenses/LICENSE-2.0

  Unless required by applicable law or agreed to in writing, software
  distributed under the License is distributed on an "AS IS" BASIS,
  WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
  See the License for the specific language governing permissions and
  limitations under the License.
 */

/***************************************************************************
 LogStandalone.cc

 
 ***************************************************************************/

#include "inktomi++.h"
#include "signals.h"
#include "DiagsConfig.h"
#include "Main.h"

#include "Error.h"
#include "P_EventSystem.h"
#include "P_Net.h"
#include "P_RecProcess.h"

#include "ProcessManager.h"
#include "MgmtUtils.h"
#include "RecordsConfig.h"

#define LOG_ReadConfigString REC_ReadConfigString

#define HttpBodyFactory		int

// globals the rest of the system depends on
extern int use_accept_thread;
extern int fds_limit;
extern int cluster_port_number;

int diags_init = 0;
int command_flag = 0;
int http_accept_port_number = 0;
int http_accept_file_descriptor = 0;
int remote_management_flag = 0;
int nntp_accept_file_descriptor = -1;
int ftp_accept_file_descriptor = -1;
int auto_clear_hostdb_flag = 0;
char proxy_name[256] = "unknown";

char system_config_directory[PATH_NAME_MAX + 1] = "conf/yts";
char management_directory[256] = "conf/yts";
char error_tags[1024] = "";
char action_tags[1024] = "";
char command_string[512] = "";
char system_root_dir[PATH_NAME_MAX + 1] = DEFAULT_ROOT_DIRECTORY;

Diags *diags = NULL;
DiagsConfig *diagsConfig = NULL;
HttpBodyFactory *body_factory = NULL;
AppVersionInfo appVersionInfo;


/*------------------------------------------------------------------------- 
  max_out_limit
  -------------------------------------------------------------------------*/
#if (HOST_OS == linux)
   /* Stupid PICKY stupid (did I mention that?) C++ compielrs */
#define RLIMCAST enum __rlimit_resource
#else
#define RLIMCAST int
#endif

static rlim_t
max_out_limit(int which, bool max_it)
{
  struct rlimit rl;

  ink_release_assert(getrlimit((RLIMCAST) which, &rl) >= 0);
  if (max_it) {
    rl.rlim_cur = rl.rlim_max;
    ink_release_assert(setrlimit((RLIMCAST) which, &rl) >= 0);
  }

  ink_release_assert(getrlimit((RLIMCAST) which, &rl) >= 0);
  rlim_t ret = rl.rlim_cur;

  return ret;
}

/*------------------------------------------------------------------------- 
  init_system
  -------------------------------------------------------------------------*/

void
init_system()
{

  fds_limit = max_out_limit(RLIMIT_NOFILE, true);


  init_signals();
  syslog(LOG_NOTICE, "NOTE: --- SAC Starting ---");
  syslog(LOG_NOTICE, "NOTE: SAC Version: %s", appVersionInfo.FullVersionInfoStr);
}

/*------------------------------------------------------------------------- 
  initialize_process_manager
  -------------------------------------------------------------------------*/

static void
initialize_process_manager()
{
  ProcessRecords *precs;

  mgmt_use_syslog();

  // Temporary Hack to Enable Communuication with LocalManager
  if (getenv("PROXY_REMOTE_MGMT")) {
    remote_management_flag = true;
  }
  //
  // Remove excess '/'
  //
  if (management_directory[strlen(management_directory) - 1] == '/')
    management_directory[strlen(management_directory) - 1] = 0;

  // diags should have been initialized by caller, e.g.: sac.cc
  ink_assert(diags);

  RecProcessInit(remote_management_flag ? RECM_CLIENT : RECM_STAND_ALONE, diags);

  if (!remote_management_flag) {
    LibRecordsConfigInit();
  }
  //
  // Start up manager
  //
  //    precs = NEW (new ProcessRecords(management_directory,
  //          "records.config","lm.config"));
  precs = NEW(new ProcessRecords(management_directory, "records.config", NULL));
  pmgmt = NEW(new ProcessManager(remote_management_flag, management_directory, precs));

  pmgmt->start();

  RecProcessInitMessage(remote_management_flag ? RECM_CLIENT : RECM_STAND_ALONE);

  pmgmt->reconfigure();

  LOG_ReadConfigString(system_config_directory, "proxy.config.config_dir", PATH_NAME_MAX);

  //
  // Define version info records
  //
  RecRegisterStatString(RECT_PROCESS, "proxy.process.version.server.short", appVersionInfo.VersionStr, RECP_NULL);
  RecRegisterStatString(RECT_PROCESS,
                        "proxy.process.version.server.long", appVersionInfo.FullVersionInfoStr, RECP_NULL);
  RecRegisterStatString(RECT_PROCESS, "proxy.process.version.server.build_number", appVersionInfo.BldNumStr, RECP_NULL);
  RecRegisterStatString(RECT_PROCESS, "proxy.process.version.server.build_time", appVersionInfo.BldTimeStr, RECP_NULL);
  RecRegisterStatString(RECT_PROCESS, "proxy.process.version.server.build_date", appVersionInfo.BldDateStr, RECP_NULL);
  RecRegisterStatString(RECT_PROCESS,
                        "proxy.process.version.server.build_machine", appVersionInfo.BldMachineStr, RECP_NULL);
  RecRegisterStatString(RECT_PROCESS,
                        "proxy.process.version.server.build_person", appVersionInfo.BldPersonStr, RECP_NULL);
//    RecRegisterStatString(RECT_PROCESS,
//                         "proxy.process.version.server.build_compile_flags",
//                         appVersionInfo.BldCompileFlagsStr,
//                         RECP_NULL);
}

/*------------------------------------------------------------------------- 
  shutdown_system
  -------------------------------------------------------------------------*/

void
shutdown_system()
{
}


/*------------------------------------------------------------------------- 
  check_lockfile
  -------------------------------------------------------------------------*/

static void
check_lockfile(char *config_dir, char *pgm_name)
{
  int err;
  pid_t holding_pid;
  char lockfile[PATH_NAME_MAX + 1];

  int nn = snprintf(lockfile, sizeof(lockfile), "%s/internal/%s_lock", config_dir, pgm_name);
  ink_assert(nn > 0);

  Lockfile server_lockfile(lockfile);
  err = server_lockfile.Get(&holding_pid);

  if (err != 1) {
    char *reason = strerror(-err);
    fprintf(stderr, "FATAL: Can't acquire lockfile '%s'", lockfile);

    if ((err == 0) && (holding_pid != -1)) {
      fprintf(stderr, " (Lock file held by process ID %d)\n", holding_pid);
    } else if ((err == 0) && (holding_pid == -1)) {
      fprintf(stderr, " (Lock file exists, but can't read process ID)\n");
    } else if (reason) {
      fprintf(stderr, " (%s)\n", reason);
    } else {
      fprintf(stderr, "\n");
    }
    _exit(1);
  }
}

/*------------------------------------------------------------------------- 
  syslog_thr_init

  For the DEC alpha, the syslog call must be made for each thread.
  -------------------------------------------------------------------------*/

void
syslog_thr_init()
{
}

/*------------------------------------------------------------------------- 
  init_log_standalone

  This routine should be called from the main() function of the standalone
  program.
  -------------------------------------------------------------------------*/

void
init_log_standalone(char *pgm_name, bool one_copy)
{
  // ensure that only one copy of the sac is running
  //
  if (one_copy) {
    check_lockfile(system_config_directory, pgm_name);
  }
  // set stdin/stdout to be unbuffered
  //
  setbuf(stdin, NULL);
  setbuf(stdout, NULL);

  openlog(pgm_name, LOG_PID | LOG_NDELAY | LOG_NOWAIT, LOG_DAEMON);

  init_system();
  initialize_process_manager();
  diagsConfig = NEW(new DiagsConfig(error_tags, action_tags));
  diags = diagsConfig->diags;
  diags_init = 1;
}

/*------------------------------------------------------------------------- 
  init_log_standalone_basic

  This routine is similar to init_log_standalone, but it is intended for
  simple standalone applications that do not read the records.config file
  and that do not need a process manager, thus it:

  1) does not call initialize_process_manager
  2) initializes the diags with use_records = false
  3) does not call create_this_machine
  4) assumes multiple copies of the application can run, so does not
     do lock checking
  -------------------------------------------------------------------------*/

void
init_log_standalone_basic(char *pgm_name)
{
  openlog(pgm_name, LOG_PID | LOG_NDELAY | LOG_NOWAIT, LOG_DAEMON);

  init_system();
  const bool use_records = false;
  diagsConfig = NEW(new DiagsConfig(error_tags, action_tags, use_records));
  diags = diagsConfig->diags;
  // set stdin/stdout to be unbuffered
  //
  setbuf(stdin, NULL);
  setbuf(stdout, NULL);

  diags_init = 1;
}
