#ifdef HAVE_CONFIG_H
#include <config.h>
#endif

#include <fcntl.h>
#include <errno.h>

#include <arc/FileAccess.h>
#include <arc/FileUtils.h>
#include <arc/FileLock.h>

#include "../run/run_redirected.h"

#include "info_files.h"

// Files in control dir, job.id.sfx
const char * const sfx_failed      = ".failed";        // Description of failure
const char * const sfx_cancel      = ".cancel";        // Mark to tell A-REX to cancel job
const char * const sfx_restart     = ".restart";       // Mark to tell A-REX to restart job
const char * const sfx_clean       = ".clean";         // Mark to tell A-REX to clean job
const char * const sfx_status      = ".status";        // Current job status
const char * const sfx_local       = ".local";         // Local information about job
const char * const sfx_errors      = ".errors";        // Log of data staging and job submission
const char * const sfx_rsl         = ".description";   // Job description sent by user
const char * const sfx_diag        = ".diag";          // Diagnostic info about finished job
const char * const sfx_lrmsoutput  = ".comment";       // Additional information from LRMS
const char * const sfx_acl         = ".acl";           // ACL information for job
const char * const sfx_proxy       = ".proxy";         // Delegated proxy
const char * const sfx_xml         = ".xml";           // XML description of job
const char * const sfx_input       = ".input";         // Input files required by job
const char * const sfx_output      = ".output";        // Output files written by job
const char * const sfx_inputstatus = ".input_status";  // Input files staged by client
const char * const sfx_outputstatus = ".output_status";// Output files already staged out
const char * const sfx_statistics  = ".statistics";    // Statistical information on data staging

// Sub-directories for different jobs states
const char * const subdir_new      = "accepting";      // Submitted but not yet picked up by A-REX
const char * const subdir_cur      = "processing";     // Being processed by A-REX
const char * const subdir_old      = "finished";       // Finished or deleted jobs
const char * const subdir_rew      = "restarting";     // Jobs waiting to restart

static Arc::Logger& logger = Arc::Logger::getRootLogger();

static job_state_t job_state_read_file(const std::string &fname,bool &pending);
static bool job_state_write_file(const std::string &fname,job_state_t state,bool pending = false);
static bool job_mark_put(Arc::FileAccess& fa, const std::string &fname);
static bool job_dir_create(Arc::FileAccess& fa,const std::string &dname);
static bool job_mark_remove(Arc::FileAccess& fa,const std::string &fname);


bool fix_file_permissions(const std::string &fname,bool executable) {
  mode_t mode = S_IRUSR | S_IWUSR;
  if(executable) { mode |= S_IXUSR; };
  return (chmod(fname.c_str(),mode) == 0);
}

static bool fix_file_permissions(Arc::FileAccess& fa,const std::string &fname,bool executable = false) {
  mode_t mode = S_IRUSR | S_IWUSR;
  if(executable) { mode |= S_IXUSR; };
  return fa.fa_chmod(fname.c_str(),mode);
}

bool fix_file_permissions(const std::string &fname,const JobDescription &desc,const JobUser &user) {
  mode_t mode = S_IRUSR | S_IWUSR;
  uid_t uid = desc.get_uid();
  gid_t gid = desc.get_gid();
  if( uid == 0 ) {
    uid=user.get_uid(); gid=user.get_gid();
  };
  if(!user.match_share_uid(uid)) {
    mode |= S_IRGRP;
    if(!user.match_share_gid(gid)) {
      mode |= S_IROTH;
    };
  };
  return (chmod(fname.c_str(),mode) == 0);
}

bool fix_file_permissions_in_session(const std::string &fname,const JobDescription &desc,const JobUser &user,bool executable) {
  mode_t mode = S_IRUSR | S_IWUSR;
  if(executable) { mode |= S_IXUSR; };
  if(user.StrictSession()) {
    uid_t uid = user.get_uid()==0?desc.get_uid():user.get_uid();
    uid_t gid = user.get_uid()==0?desc.get_gid():user.get_gid();
    Arc::FileAccess fa;
    if(!fa.fa_setuid(uid,gid)) return false;
    return fa.fa_chmod(fname,mode);
  };
  return (chmod(fname.c_str(),mode) == 0);
}

bool fix_file_owner(const std::string &fname,const JobUser &user) {
  if(getuid() == 0) {
    if(lchown(fname.c_str(),user.get_uid(),user.get_gid()) == -1) {
      logger.msg(Arc::ERROR,"Failed setting file owner: %s",fname);
      return false;
    };
  };
  return true;
}

bool fix_file_owner(const std::string &fname,const JobDescription &desc,const JobUser &user) {
  if(getuid() == 0) {
    uid_t uid = desc.get_uid();
    gid_t gid = desc.get_gid();
    if( uid == 0 ) {
      uid=user.get_uid(); gid=user.get_gid();
    };
    if(lchown(fname.c_str(),uid,gid) == -1) {
      logger.msg(Arc::ERROR,"Failed setting file owner: %s",fname);
      return false;
    };
  };
  return true;
}

bool check_file_owner(const std::string &fname,const JobUser &user) {
  uid_t uid;
  gid_t gid;
  time_t t;
  return check_file_owner(fname,user,uid,gid,t);
}

bool check_file_owner(const std::string &fname,const JobUser &user,uid_t &uid,gid_t &gid) {
  time_t t;
  return check_file_owner(fname,user,uid,gid,t);
}

bool check_file_owner(const std::string &fname,const JobUser &user,uid_t &uid,gid_t &gid,time_t &t) {
  struct stat st;
  if(lstat(fname.c_str(),&st) != 0) return false;
  if(!S_ISREG(st.st_mode)) return false;
  uid=st.st_uid; gid=st.st_gid; t=st.st_ctime;
  /* superuser can't run jobs */
  if(uid == 0) return false;
  /* accept any file if superuser */
  if(user.get_uid() != 0) {
    if(uid != user.get_uid()) return false;
  };
  return true;
}

bool job_lrms_mark_check(const JobId &id,const JobUser &user) {
  std::string fname = user.ControlDir() + "/job." + id + ".lrms_done";
  return job_mark_check(fname);
}

bool job_lrms_mark_remove(const JobId &id,const JobUser &user) {
  std::string fname = user.ControlDir() + "/job." + id + ".lrms_done";
  return job_mark_remove(fname);
}

LRMSResult job_lrms_mark_read(const JobId &id,const JobUser &user) {
  std::string fname = user.ControlDir() + "/job." + id + ".lrms_done";
  LRMSResult r("-1 Internal error");
  std::ifstream f(fname.c_str()); if(! f.is_open() ) return r;
  f>>r;
  return r;
}

bool job_cancel_mark_put(const JobDescription &desc,const JobUser &user) {
  std::string fname = user.ControlDir() + "/" + subdir_new + "/job." + desc.get_id() + sfx_cancel;
  return job_mark_put(fname) & fix_file_owner(fname,desc,user) & fix_file_permissions(fname);
}

bool job_cancel_mark_check(const JobId &id,const JobUser &user) {
  std::string fname = user.ControlDir() + "/" + subdir_new + "/job." + id + sfx_cancel;
  return job_mark_check(fname);
}

bool job_cancel_mark_remove(const JobId &id,const JobUser &user) {
  std::string fname = user.ControlDir() + "/" + subdir_new + "/job." + id + sfx_cancel;
  return job_mark_remove(fname);
}

bool job_restart_mark_put(const JobDescription &desc,const JobUser &user) {
  std::string fname = user.ControlDir() + "/" + subdir_new + "/job." + desc.get_id() + sfx_restart;
  return job_mark_put(fname) & fix_file_owner(fname,desc,user) & fix_file_permissions(fname);
}

bool job_restart_mark_check(const JobId &id,const JobUser &user) {
  std::string fname = user.ControlDir() + "/" + subdir_new + "/job." + id + sfx_restart;
  return job_mark_check(fname);
}

bool job_restart_mark_remove(const JobId &id,const JobUser &user) {
  std::string fname = user.ControlDir() + "/" + subdir_new + "/job." + id + sfx_restart;
  return job_mark_remove(fname);
}

bool job_clean_mark_put(const JobDescription &desc,const JobUser &user) {
  std::string fname = user.ControlDir() + "/" + subdir_new + "/job." + desc.get_id() + sfx_clean;
  return job_mark_put(fname) & fix_file_owner(fname,desc,user) & fix_file_permissions(fname);
}

bool job_clean_mark_check(const JobId &id,const JobUser &user) {
  std::string fname = user.ControlDir() + "/" + subdir_new + "/job." + id + sfx_clean;
  return job_mark_check(fname);
}

bool job_clean_mark_remove(const JobId &id,const JobUser &user) {
  std::string fname = user.ControlDir() + "/" + subdir_new + "/job." + id + sfx_clean;
  return job_mark_remove(fname);
}

bool job_failed_mark_put(const JobDescription &desc,const JobUser &user,const std::string &content) {
  std::string fname = user.ControlDir() + "/job." + desc.get_id() + sfx_failed;
  if(job_mark_size(fname) > 0) return true;
  return job_mark_write_s(fname,content) & fix_file_owner(fname,desc,user) & fix_file_permissions(fname,desc,user);
}

bool job_failed_mark_add(const JobDescription &desc,const JobUser &user,const std::string &content) {
  std::string fname = user.ControlDir() + "/job." + desc.get_id() + sfx_failed;
  return job_mark_add_s(fname,content) & fix_file_owner(fname,desc,user) & fix_file_permissions(fname,desc,user);
}

bool job_failed_mark_check(const JobId &id,const JobUser &user) {
  std::string fname = user.ControlDir() + "/job." + id + sfx_failed;
  return job_mark_check(fname);
}

bool job_failed_mark_remove(const JobId &id,const JobUser &user) {
  std::string fname = user.ControlDir() + "/job." + id + sfx_failed;
  return job_mark_remove(fname);
}

std::string job_failed_mark_read(const JobId &id,const JobUser &user) {
  std::string fname = user.ControlDir() + "/job." + id + sfx_failed;
  return job_mark_read_s(fname);
}

bool job_controldiag_mark_put(const JobDescription &desc,const JobUser &user,char const * const args[]) {
  std::string fname = user.ControlDir() + "/job." + desc.get_id() + sfx_diag;
  if(!job_mark_put(fname)) return false;
  if(!fix_file_owner(fname,desc,user)) return false;
  if(!fix_file_permissions(fname)) return false;
  if(args == NULL) return true;
  struct stat st;
  if(args[0] && stat(args[0], &st) != 0) return true;
  int h = open(fname.c_str(),O_WRONLY);
  if(h == -1) return false;
  int r;
  int t = 10;
  JobUser tmp_user(user.Env(),(uid_t)0,(gid_t)0);
  r=RunRedirected::run(tmp_user,"job_controldiag_mark_put",-1,h,-1,(char**)args,t);
  close(h);
  if(r != 0) return false;
  return true;
}

bool job_diagnostics_mark_put(const JobDescription &desc,const JobUser &user) {
  std::string fname = desc.SessionDir() + sfx_diag;
  if(user.StrictSession()) {
    uid_t uid = user.get_uid()==0?desc.get_uid():user.get_uid();
    uid_t gid = user.get_uid()==0?desc.get_gid():user.get_gid();
    Arc::FileAccess fa;
    if(!fa.fa_setuid(uid,gid)) return false;
    return job_mark_put(fa,fname) & fix_file_permissions(fa,fname);
  };
  return job_mark_put(fname) & fix_file_owner(fname,desc,user) & fix_file_permissions(fname);
}

bool job_diagnostics_mark_remove(const JobDescription &desc,const JobUser &user) {
  std::string fname = user.ControlDir() + "/job." + desc.get_id() + sfx_diag;
  bool res1 = job_mark_remove(fname);
  fname = desc.SessionDir() + sfx_diag;
  if(user.StrictSession()) {
    uid_t uid = user.get_uid()==0?desc.get_uid():user.get_uid();
    uid_t gid = user.get_uid()==0?desc.get_gid():user.get_gid();
    Arc::FileAccess fa;
    if(!fa.fa_setuid(uid,gid)) return res1;
    return (res1 | job_mark_remove(fa,fname));
  };
  return (res1 | job_mark_remove(fname));
}

bool job_diagnostics_mark_move(const JobDescription &desc,const JobUser &user) {
  std::string fname1;
  if (desc.get_local() && !desc.get_local()->sessiondir.empty()) fname1 = desc.get_local()->sessiondir + sfx_diag;
  else fname1 = user.SessionRoot(desc.get_id()) + "/" + desc.get_id() + sfx_diag;
  std::string fname2 = user.ControlDir() + "/job." + desc.get_id() + sfx_diag;

  std::string data;
  uid_t uid = getuid();
  gid_t gid = getgid();
  if(user.StrictSession()) {
    uid = user.get_uid()==0?desc.get_uid():user.get_uid();
    gid = user.get_uid()==0?desc.get_gid():user.get_gid();
  }
  Arc::FileRead(fname1, data, uid, gid);
  Arc::FileDelete(fname1, uid, gid);
  // behaviour is to create file in control dir even if reading fails
  return Arc::FileCreate(fname2, data) & fix_file_owner(fname2,desc,user) & fix_file_permissions(fname2,desc,user);
}

bool job_lrmsoutput_mark_put(const JobDescription &desc,const JobUser &user) {
  std::string fname = desc.SessionDir() + sfx_lrmsoutput;
  if(user.StrictSession()) {
    uid_t uid = user.get_uid()==0?desc.get_uid():user.get_uid();
    uid_t gid = user.get_uid()==0?desc.get_gid():user.get_gid();
    Arc::FileAccess fa;
    if(!fa.fa_setuid(uid,gid)) return false;
    return job_mark_put(fa,fname) & fix_file_permissions(fa,fname);
  };
  return job_mark_put(fname) & fix_file_owner(fname,desc,user) & fix_file_permissions(fname);
}

bool job_session_create(const JobDescription &desc,const JobUser &user) {
  std::string dname = desc.SessionDir();
  if(user.StrictSession()) {
    uid_t uid = user.get_uid()==0?desc.get_uid():user.get_uid();
    uid_t gid = user.get_uid()==0?desc.get_gid():user.get_gid();
    Arc::FileAccess fa;
    if(!fa.fa_setuid(uid,gid)) return false;
    return job_dir_create(fa,dname) & fix_file_permissions(fa,dname,true);
  };
  return job_dir_create(dname) & fix_file_owner(dname,desc,user) & fix_file_permissions(dname,true);
}

bool job_lrmsoutput_mark_remove(const JobDescription &desc,const JobUser &user) {
  std::string fname = desc.SessionDir() + sfx_lrmsoutput;
  if(user.StrictSession()) {
    uid_t uid = user.get_uid()==0?desc.get_uid():user.get_uid();
    uid_t gid = user.get_uid()==0?desc.get_gid():user.get_gid();
    Arc::FileAccess fa;
    if(!fa.fa_setuid(uid,gid)) return false;
    return job_mark_remove(fa,fname);
  };
  return job_mark_remove(fname);
}


bool job_dir_create(const std::string &dname) {
  int err=mkdir(dname.c_str(),S_IRUSR | S_IWUSR | S_IXUSR);
  return (err==0);
}

static bool job_dir_create(Arc::FileAccess& fa,const std::string &dname) {
  return fa.fa_mkdir(dname,S_IRUSR | S_IWUSR | S_IXUSR);
}

std::string job_mark_read_s(const std::string &fname) {
  std::string s("");
  Arc::FileRead(fname, s);
  return s;
}

long int job_mark_read_i(const std::string &fname) {
  std::ifstream f(fname.c_str()); if(! f.is_open() ) return -1;
  char buf[32]; f.getline(buf,30); f.close();
  char* e; long int i;
  i=strtol(buf,&e,10); if((*e) == 0) return i;
  return -1;
}

bool job_mark_write_s(const std::string &fname,const std::string &content) {
  int h=open(fname.c_str(),O_WRONLY | O_CREAT | O_TRUNC,S_IRUSR | S_IWUSR);
  if(h==-1) return false;
  write(h,(const void *)content.c_str(),content.length());
  close(h); return true;
}

bool job_mark_add_s(const std::string &fname,const std::string &content) {
  int h=open(fname.c_str(),O_WRONLY | O_CREAT | O_APPEND,S_IRUSR | S_IWUSR);
  if(h==-1) return false;
  write(h,(const void *)content.c_str(),content.length());
  close(h); return true;
}

bool job_mark_put(const std::string &fname) {
  int h=open(fname.c_str(),O_WRONLY | O_CREAT,S_IRUSR | S_IWUSR);
  if(h==-1) return false; close(h); return true;
}

static bool job_mark_put(Arc::FileAccess& fa, const std::string &fname) {
  if(!fa.fa_open(fname,O_WRONLY | O_CREAT,S_IRUSR | S_IWUSR)) return false;
  fa.fa_close(); return true;
}

bool job_mark_check(const std::string &fname) {
  struct stat st;
  if(lstat(fname.c_str(),&st) != 0) return false;
  if(!S_ISREG(st.st_mode)) return false;
  return true;
}

bool job_mark_remove(const std::string &fname) {
  if(unlink(fname.c_str()) != 0) { if(errno != ENOENT) return false; };
  return true;
}

static bool job_mark_remove(Arc::FileAccess& fa,const std::string &fname) {
  if(!fa.fa_unlink(fname)) {
    if(fa.geterrno() != ENOENT) return false;
  };
  return true;
}

time_t job_mark_time(const std::string &fname) {
  struct stat st;
  if(lstat(fname.c_str(),&st) != 0) return 0;
  return st.st_mtime;
}

long int job_mark_size(const std::string &fname) {
  struct stat st;
  if(lstat(fname.c_str(),&st) != 0) return 0;
  if(!S_ISREG(st.st_mode)) return 0;
  return st.st_size;
}

bool job_errors_mark_put(const JobDescription &desc,const JobUser &user) {
  std::string fname = user.ControlDir() + "/job." + desc.get_id() + sfx_errors;
  return job_mark_put(fname) & fix_file_owner(fname,desc,user) & fix_file_permissions(fname);
}

std::string job_errors_filename(const JobId &id, const JobUser &user) {
  return user.ControlDir() + "/job." + id + sfx_errors;
}


time_t job_state_time(const JobId &id,const JobUser &user) {
  std::string fname = user.ControlDir() + "/job." + id + sfx_status;
  time_t t = job_mark_time(fname);
  if(t != 0) return t;
  fname = user.ControlDir() + "/" + subdir_cur + "/job." + id + sfx_status;
  t = job_mark_time(fname);
  if(t != 0) return t;
  fname = user.ControlDir() + "/" + subdir_new + "/job." + id + sfx_status;
  t = job_mark_time(fname);
  if(t != 0) return t;
  fname = user.ControlDir() + "/" + subdir_rew + "/job." + id + sfx_status;
  t = job_mark_time(fname);
  if(t != 0) return t;
  fname = user.ControlDir() + "/" + subdir_old + "/job." + id + sfx_status;
  return job_mark_time(fname);
}

job_state_t job_state_read_file(const JobId &id,const JobUser &user) {
  bool pending;
  return job_state_read_file(id, user, pending);
}

job_state_t job_state_read_file(const JobId &id,const JobUser &user,bool& pending) {
  std::string fname = user.ControlDir() + "/job." + id + sfx_status;
  job_state_t st = job_state_read_file(fname,pending);
  if(st != JOB_STATE_DELETED) return st;
  fname = user.ControlDir() + "/" + subdir_cur + "/job." + id + sfx_status;
  st = job_state_read_file(fname,pending);
  if(st != JOB_STATE_DELETED) return st;
  fname = user.ControlDir() + "/" + subdir_new + "/job." + id + sfx_status;
  st = job_state_read_file(fname,pending);
  if(st != JOB_STATE_DELETED) return st;
  fname = user.ControlDir() + "/" + subdir_rew + "/job." + id + sfx_status;
  st = job_state_read_file(fname,pending);
  if(st != JOB_STATE_DELETED) return st;
  fname = user.ControlDir() + "/" + subdir_old + "/job." + id + sfx_status;
  return job_state_read_file(fname,pending);
}

bool job_state_write_file(const JobDescription &desc,const JobUser &user,job_state_t state,bool pending) {
  std::string fname;
  if(state == JOB_STATE_ACCEPTED) { 
    fname = user.ControlDir() + "/" + subdir_old + "/job." + desc.get_id() + sfx_status; remove(fname.c_str());
    fname = user.ControlDir() + "/" + subdir_cur + "/job." + desc.get_id() + sfx_status; remove(fname.c_str());
    fname = user.ControlDir() + "/" + subdir_rew + "/job." + desc.get_id() + sfx_status; remove(fname.c_str());
    fname = user.ControlDir() + "/job." + desc.get_id() + sfx_status; remove(fname.c_str());
    fname = user.ControlDir() + "/" + subdir_new + "/job." + desc.get_id() + sfx_status;
  } else if((state == JOB_STATE_FINISHED) || (state == JOB_STATE_DELETED)) {
    fname = user.ControlDir() + "/" + subdir_new + "/job." + desc.get_id() + sfx_status; remove(fname.c_str());
    fname = user.ControlDir() + "/" + subdir_cur + "/job." + desc.get_id() + sfx_status; remove(fname.c_str());
    fname = user.ControlDir() + "/" + subdir_rew + "/job." + desc.get_id() + sfx_status; remove(fname.c_str());
    fname = user.ControlDir() + "/job." + desc.get_id() + sfx_status; remove(fname.c_str());
    fname = user.ControlDir() + "/" + subdir_old + "/job." + desc.get_id() + sfx_status;
  } else {
    fname = user.ControlDir() + "/" + subdir_new + "/job." + desc.get_id() + sfx_status; remove(fname.c_str());
    fname = user.ControlDir() + "/" + subdir_old + "/job." + desc.get_id() + sfx_status; remove(fname.c_str());
    fname = user.ControlDir() + "/" + subdir_rew + "/job." + desc.get_id() + sfx_status; remove(fname.c_str());
    fname = user.ControlDir() + "/job." + desc.get_id() + sfx_status; remove(fname.c_str());
    fname = user.ControlDir() + "/" + subdir_cur + "/job." + desc.get_id() + sfx_status;
  };
  return job_state_write_file(fname,state,pending) & fix_file_owner(fname,desc,user) & fix_file_permissions(fname,desc,user);
}

static job_state_t job_state_read_file(const std::string &fname,bool &pending) {

  std::string data;
  if(!Arc::FileRead(fname, data)) {
    if(!job_mark_check(fname)) return JOB_STATE_DELETED; /* job does not exist */
    return JOB_STATE_UNDEFINED; /* can't open file */
  };
  data = data.substr(0, data.find('\n'));
  /* interpret information */
  if(data.substr(0, 8) == "PENDING:") {
    data = data.substr(8); pending=true;
  } else {
    pending=false;
  };
  for(int i = 0;states_all[i].name != NULL;i++) {
    if(states_all[i].name == data) {
      return states_all[i].id;
    };
  };
  return JOB_STATE_UNDEFINED; /* broken file */
}

static bool job_state_write_file(const std::string &fname,job_state_t state,bool pending) {
  std::string data;
  if (pending) data += "PENDING:";
  data += states_all[state].name;
  return Arc::FileCreate(fname, data);
}

time_t job_description_time(const JobId &id,const JobUser &user) {
  std::string fname = user.ControlDir() + "/job." + id + sfx_rsl;
  return job_mark_time(fname);
}

bool job_description_read_file(const JobId &id,const JobUser &user,std::string &rsl) {
  std::string fname = user.ControlDir() + "/job." + id + sfx_rsl;
  return job_description_read_file(fname,rsl);
}

bool job_description_read_file(const std::string &fname,std::string &rsl) {
  if (!Arc::FileRead(fname, rsl)) return false;
  while (rsl.find('\n') != std::string::npos) rsl.erase(rsl.find('\n'), 1);
  return true;
}

bool job_description_write_file(const std::string &fname,const std::string &rsl) {
  return Arc::FileCreate(fname, rsl);
}

bool job_acl_read_file(const JobId &id,const JobUser &user,std::string &acl) {
  std::string fname = user.ControlDir() + "/job." + id + sfx_acl;
  return job_description_read_file(fname,acl);
}

bool job_acl_write_file(const JobId &id,const JobUser &user,const std::string &acl) {
  std::string fname = user.ControlDir() + "/job." + id + sfx_acl;
  return job_description_write_file(fname,acl);
}

bool job_xml_read_file(const JobId &id,const JobUser &user,std::string &xml) {
  std::string fname = user.ControlDir() + "/job." + id + sfx_xml;
  return job_description_read_file(fname,xml);
}

bool job_xml_write_file(const JobId &id,const JobUser &user,const std::string &xml) {
  std::string fname = user.ControlDir() + "/job." + id + sfx_xml;
  return job_description_write_file(fname,xml);
}

bool job_local_write_file(const JobDescription &desc,const JobUser &user,const JobLocalDescription &job_desc) {
  std::string fname = user.ControlDir() + "/job." + desc.get_id() + sfx_local;
  return job_local_write_file(fname,job_desc) & fix_file_owner(fname,desc,user) & fix_file_permissions(fname,desc,user);
}

bool job_local_write_file(const std::string &fname,const JobLocalDescription &job_desc) {
  return job_desc.write(fname);
}

bool job_local_read_file(const JobId &id,const JobUser &user,JobLocalDescription &job_desc) {
  std::string fname = user.ControlDir() + "/job." + id + sfx_local;
  return job_local_read_file(fname,job_desc);
}

bool job_local_read_file(const std::string &fname,JobLocalDescription &job_desc) {
  return job_desc.read(fname);
}

bool job_local_read_var(const std::string &fname,const std::string &vnam,std::string &value) {
  return JobLocalDescription::read_var(fname,vnam,value);
}

bool job_local_read_cleanuptime(const JobId &id,const JobUser &user,time_t &cleanuptime) {
  std::string fname = user.ControlDir() + "/job." + id + sfx_local;
  std::string str;
  if(!job_local_read_var(fname,"cleanuptime",str)) return false;
  cleanuptime=Arc::Time(str).GetTime();
  return true;
}

bool job_local_read_failed(const JobId &id,const JobUser &user,std::string &state,std::string &cause) {
  state = "";
  cause = "";
  std::string fname = user.ControlDir() + "/job." + id + sfx_local;
  job_local_read_var(fname,"failedstate",state);
  job_local_read_var(fname,"failedcause",cause);
  return true;
}

/* job.ID.input functions */

bool job_input_write_file(const JobDescription &desc,const JobUser &user,std::list<FileData> &files) {
  std::string fname = user.ControlDir() + "/job." + desc.get_id() + sfx_input;
  return job_Xput_write_file(fname,files) & fix_file_owner(fname,desc,user) & fix_file_permissions(fname);
}

bool job_input_read_file(const JobId &id,const JobUser &user,std::list<FileData> &files) {
  std::string fname = user.ControlDir() + "/job." + id + sfx_input;
  return job_Xput_read_file(fname,files);
}

bool job_input_status_add_file(const JobDescription &desc,const JobUser &user,const std::string& file) {
  // 1. lock
  // 2. add
  // 3. unlock
  std::string fname = user.ControlDir() + "/job." + desc.get_id() + sfx_inputstatus;
  Arc::FileLock lock(fname);
  for (int i = 10; !lock.acquire() && i >= 0; --i)  {
    if (i == 0) return false;
    sleep(1);
  }
  std::string data;
  if (!Arc::FileRead(fname, data) && errno != ENOENT) {
    lock.release();
    return false;
  }
  std::ostringstream line;
  line<<file<<"\n";
  data += line.str();
  bool r = Arc::FileCreate(fname, data);
  lock.release();
  return r & fix_file_owner(fname,desc,user) & fix_file_permissions(fname);
}

bool job_input_status_read_file(const JobId &id,const JobUser &user,std::list<std::string>& files) {
  std::string fname = user.ControlDir() + "/job." + id + sfx_inputstatus;
  Arc::FileLock lock(fname);
  for (int i = 10; !lock.acquire() && i >= 0; --i) {
    if (i == 0) return false;
    sleep(1);
  }
  bool r = Arc::FileRead(fname, files);
  lock.release();
  return r;
}

/* job.ID.output functions */
bool job_output_write_file(const JobDescription &desc,const JobUser &user,std::list<FileData> &files,job_output_mode mode) {
  std::string fname = user.ControlDir() + "/job." + desc.get_id() + sfx_output;
  return job_Xput_write_file(fname,files,mode) & fix_file_owner(fname,desc,user) & fix_file_permissions(fname);
}

bool job_output_read_file(const JobId &id,const JobUser &user,std::list<FileData> &files) {
  std::string fname = user.ControlDir() + "/job." + id + sfx_output;
  return job_Xput_read_file(fname,files);
}

bool job_output_status_add_file(const JobDescription &desc,const JobUser &user,const FileData& file) {
  // Not using lock here because concurrent read/write is not expected
  std::string fname = user.ControlDir() + "/job." + desc.get_id() + sfx_outputstatus;
  std::string data;
  if (!Arc::FileRead(fname, data) && errno != ENOENT) return false;
  std::ostringstream line;
  line<<file<<"\n";
  data += line.str();
  return Arc::FileCreate(fname, data) & fix_file_owner(fname,desc,user) & fix_file_permissions(fname);
}

bool job_output_status_write_file(const JobDescription &desc,const JobUser &user,std::list<FileData> &files) {
  std::string fname = user.ControlDir() + "/job." + desc.get_id() + sfx_outputstatus;
  return job_Xput_write_file(fname,files) & fix_file_owner(fname,desc,user) & fix_file_permissions(fname);
}

bool job_output_status_read_file(const JobId &id,const JobUser &user,std::list<FileData> &files) {
  std::string fname = user.ControlDir() + "/job." + id + sfx_outputstatus;
  return job_Xput_read_file(fname,files);
}

/* common functions */

bool job_Xput_write_file(const std::string &fname,std::list<FileData> &files,job_output_mode mode, uid_t uid, gid_t gid) {
  std::ostringstream s;
  for(FileData::iterator i=files.begin();i!=files.end(); ++i) { 
    if(mode == job_output_all) {
      s << (*i) << std::endl;
    } else if(mode == job_output_success) {
      if(i->ifsuccess) {
        s << (*i) << std::endl;
      } else {
        // This case is handled at higher level
      };
    } else if(mode == job_output_cancel) {
      if(i->ifcancel) {
        s << (*i) << std::endl;
      } else {
        // This case is handled at higher level
      };
    } else if(mode == job_output_failure) {
      if(i->iffailure) {
        s << (*i) << std::endl;
      } else {
        // This case is handled at higher level
      };
    };
  };
  if (!Arc::FileCreate(fname, s.str(), uid, gid)) return false;
  return true;
}

bool job_Xput_read_file(const std::string &fname,std::list<FileData> &files, uid_t uid, gid_t gid) {
  std::list<std::string> file_content;
  if (!Arc::FileRead(fname, file_content, uid, gid)) return false;
  for(std::list<std::string>::iterator i = file_content.begin(); i != file_content.end(); ++i) {
    FileData fd;
    std::istringstream s(*i);
    s >> fd;
    if(!fd.pfn.empty()) files.push_back(fd);
  };
  return true;
}

std::string job_proxy_filename(const JobId &id, const JobUser &user){
  return user.ControlDir() + "/job." + id + sfx_proxy;
}

bool job_clean_finished(const JobId &id,const JobUser &user) {
  std::string fname;
  fname = user.ControlDir()+"/job."+id+".proxy.tmp"; remove(fname.c_str());
  fname = user.ControlDir()+"/job."+id+".lrms_done"; remove(fname.c_str());
  return true;
}

bool job_clean_deleted(const JobDescription &desc,const JobUser &user,std::list<std::string> cache_per_job_dirs) {
  std::string id = desc.get_id();
  job_clean_finished(id,user);
  std::string session;
  if(desc.get_local() && !desc.get_local()->sessiondir.empty()) session = desc.get_local()->sessiondir;
  else session = user.SessionRoot(id)+"/"+id;
  std::string fname;
  fname = user.ControlDir()+"/job."+id+sfx_proxy; remove(fname.c_str());
  fname = user.ControlDir()+"/"+subdir_new+"/job."+id+sfx_restart; remove(fname.c_str());
  fname = user.ControlDir()+"/job."+id+sfx_errors; remove(fname.c_str());
  fname = user.ControlDir()+"/"+subdir_new+"/job."+id+sfx_cancel; remove(fname.c_str());
  fname = user.ControlDir()+"/"+subdir_new+"/job."+id+sfx_clean;  remove(fname.c_str());
  fname = user.ControlDir()+"/job."+id+sfx_output; remove(fname.c_str());
  fname = user.ControlDir()+"/job."+id+sfx_input; remove(fname.c_str());
  fname = user.ControlDir()+"/job."+id+".grami_log"; remove(fname.c_str());
  fname = session+sfx_lrmsoutput; remove(fname.c_str());
  fname = user.ControlDir()+"/job."+id+sfx_outputstatus; remove(fname.c_str());
  fname = user.ControlDir()+"/job."+id+sfx_inputstatus; remove(fname.c_str());
  fname = user.ControlDir()+"/job."+id+sfx_statistics; remove(fname.c_str());
  /* remove session directory */
  if(user.StrictSession()) {
    uid_t uid = user.get_uid()==0?desc.get_uid():user.get_uid();
    uid_t gid = user.get_uid()==0?desc.get_gid():user.get_gid();
    Arc::DirDelete(session, true, uid, gid);
  } else {
    Arc::DirDelete(session);
  }
  // remove cache per-job links, in case this failed earlier
  for (std::list<std::string>::iterator i = cache_per_job_dirs.begin(); i != cache_per_job_dirs.end(); i++) {
    Arc::DirDelete((*i) + "/" + id);
  }
  return true;
}

bool job_clean_final(const JobDescription &desc,const JobUser &user) {
  std::string id = desc.get_id();
  job_clean_finished(id,user);
  job_clean_deleted(desc,user);
  std::string fname;
  fname = user.ControlDir()+"/job."+id+sfx_local;  remove(fname.c_str());
  fname = user.ControlDir()+"/job."+id+".grami"; remove(fname.c_str());
  fname = user.ControlDir()+"/job."+id+sfx_failed; remove(fname.c_str());
  job_diagnostics_mark_remove(desc,user);
  job_lrmsoutput_mark_remove(desc,user);
  fname = user.ControlDir()+"/job."+id+sfx_status; remove(fname.c_str());
  fname = user.ControlDir()+"/"+subdir_new+"/job."+id+sfx_status; remove(fname.c_str());
  fname = user.ControlDir()+"/"+subdir_cur+"/job."+id+sfx_status; remove(fname.c_str());
  fname = user.ControlDir()+"/"+subdir_old+"/job."+id+sfx_status; remove(fname.c_str());
  fname = user.ControlDir()+"/"+subdir_rew+"/job."+id+sfx_status; remove(fname.c_str());
  fname = user.ControlDir()+"/job."+id+sfx_rsl; remove(fname.c_str());
  fname = user.ControlDir()+"/job."+id+sfx_xml; remove(fname.c_str());
  return true;
}
