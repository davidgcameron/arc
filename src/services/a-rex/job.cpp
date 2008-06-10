#ifdef HAVE_CONFIG_H
#include <config.h>
#endif

#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>

#include <glibmm/thread.h>
#include <arc/StringConv.h>
#include <arc/security/ArcPDP/Evaluator.h>
#include <arc/security/ArcPDP/EvaluatorLoader.h>

#include "grid-manager/conf/environment.h"
#include "grid-manager/conf/conf_pre.h"
#include "grid-manager/jobs/job.h"
#include "grid-manager/jobs/plugins.h"
#include "grid-manager/jobs/job_request.h"
#include "grid-manager/jobs/commfifo.h"
#include "grid-manager/run/run_plugin.h"
#include "grid-manager/files/info_files.h"
 
#include "job.h"

using namespace ARex;

#define JOB_POLICY_OPERATION_URN "http://www.nordugrid.org/schemas/policy-arc/types/a-rex/joboperation"

static bool env_initialized = false;
Glib::StaticMutex env_lock = GLIBMM_STATIC_MUTEX_INIT;

bool ARexGMConfig::InitEnvironment(const std::string& configfile) {
  if(env_initialized) return true;
  env_lock.lock();
  if(!env_initialized) {
    if(!configfile.empty()) nordugrid_config_loc=configfile;
    env_initialized=read_env_vars();
  };
  env_lock.unlock();
  return env_initialized;
}

ARexGMConfig::~ARexGMConfig(void) {
  if(user_) delete user_;
}

ARexGMConfig::ARexGMConfig(const std::string& configfile,const std::string& uname,const std::string& grid_name,const std::string& service_endpoint):user_(NULL),readonly_(false),grid_name_(grid_name),service_endpoint_(service_endpoint) {
  if(!InitEnvironment(configfile)) return;
  // const char* uname = user_s.get_uname();
  //if((bool)job_map) uname=job_map.unix_name();
  user_=new JobUser(uname);
  if(!user_->is_valid()) { delete user_; user_=NULL; return; };
  if(nordugrid_loc.empty() != 0) { delete user_; user_=NULL; return; };
  /* read configuration */
  std::string session_root;
  std::string control_dir;
  std::string default_lrms;
  std::string default_queue;
  ContinuationPlugins* cont_plugins = NULL;
  RunPlugin* cred_plugin = new RunPlugin;
  std::string allowsubmit;
  bool strict_session;
  if(!configure_user_dirs(uname,control_dir,session_root,
                          default_lrms,default_queue,queues_,
                          *cont_plugins,*cred_plugin,
                          allowsubmit,strict_session)) {
    // olog<<"Failed processing grid-manager configuration"<<std::endl;
    delete user_; user_=NULL; delete cred_plugin; return;
  };
  delete cred_plugin;
  if(default_queue.empty() && (queues_.size() == 1)) {
    default_queue=*(queues_.begin());
  };
  user_->SetControlDir(control_dir);
  user_->SetSessionRoot(session_root);
  user_->SetLRMS(default_lrms,default_queue);
  user_->SetStrictSession(strict_session);
  //for(;allowsubmit.length();) {
  //  std::string group = config_next_arg(allowsubmit);
  //  if(group.length()) readonly=true;
  //  if(user_a.check_group(group)) { readonly=false; break; };
  //};
  //if(readonly) olog<<"This user is denied to submit new jobs."<<std::endl;
  /*
          * link to the class for direct file access *
          std::string direct_config = "mount "+session_root+"\n";
          direct_config+="dir / nouser read cd dirlist delete append overwrite";          direct_config+=" create "+
             inttostring(user->get_uid())+":"+inttostring(user->get_gid())+
             " 600:600";
          direct_config+=" mkdir "+
             inttostring(user->get_uid())+":"+inttostring(user->get_gid())+
             " 700:700\n";
          direct_config+="end\n";
#ifdef HAVE_SSTREAM
          std::stringstream fake_cfile(direct_config);
#else
          std::strstream fake_cfile;
          fake_cfile<<direct_config;
#endif
          direct_fs = new DirectFilePlugin(fake_cfile,user_s);
          if((bool)job_map) {
            olog<<"Job submission user: "<<uname<<
                  " ("<<user->get_uid()<<":"<<user->get_gid()<<")"<<std::endl;
  */
}


bool ARexJob::is_allowed(void) {
  allowed_to_see_=false;
  allowed_to_maintain_=false;
  // Temporarily checking only user's grid name against owner
  if(config_.GridName() == job_.DN) {
    allowed_to_see_=true;
    allowed_to_maintain_=true;
    return true;
  };
/*
  // Using ARC Policy
  std::string acl;
  if(job_acl_read_file(id_,*config.User(),acl)) {
    if(!acl.empty()) {
      ArcSec::Evaluator* eval =
          ArcSec::EvaluatorLoader().getEvaluator("arc.evaluator");
      eval->addPolicy(Source(acl));
    };
  };
  NS ns;
  ns["ar"]="http://www.nordugrid.org/schemas/request-arc";
  XMLNode request("ar:Request",ns);
  XMLNode item = val.NewChild("ar:RequestItem");
  // Resource is not needed
  // Possible operations are Modify and Read
  XMLNode action;
  action=item.NewChild("ar:Action");
  action="Read"; action.NewAttribute("Type")="string";
  action.NewAttribute("AttributeId")=JOB_POLICY_OPERATION_URN;
  action=item.NewChild("ar:Action");
  action="Modify"; action.NewAttribute("Type")="string";
  action.NewAttribute("AttributeId")=JOB_POLICY_OPERATION_URN;
  // Get all user identities




  ArcSec::Response *resp = eval->evaluate(request);
  if(resp) {
    ResponseList rlist = resp->getResponseItems();
    //It is supposed all of the RequestItem needs to satisfy the policy in order to authorization
    if((rlist.size()) == (resp->getRequestSize())){
      olog<<"Authorized from job"<<std::endl;
      if(resp) delete resp;
      return true;
    };
    else {
      olog<<"UnAuthorized from job; Some of the RequestItem does not satisfy Policy"<<std::endl;
      if(resp) delete resp;
      return false;
    };
  };
*/
  return true;
}

ARexJob::ARexJob(const std::string& id,ARexGMConfig& config):id_(id),config_(config) {
  if(id_.empty()) return;
  if(!config_) { id_.clear(); return; };
  // Reading essential information about job
  if(!job_local_read_file(id_,*config_.User(),job_)) { id_.clear(); return; };
  // Checking if user is allowed to do anything with that job
  if(!is_allowed()) { id_.clear(); return; };
  if(!(allowed_to_see_ || allowed_to_maintain_)) { id_.clear(); return; };
}

ARexJob::ARexJob(Arc::XMLNode jsdl,ARexGMConfig& config,const std::string& credentials,const std::string& clientid):id_(""),config_(config) {
  if(!config_) return;
  // New job is created here
  // First get and acquire new id
  if(!make_job_id()) return;
  // Turn JSDL into text
  std::string job_desc_str;
  // Make full XML doc out of subtree
  {
    Arc::XMLNode jsdldoc;
    jsdl.New(jsdldoc);
    jsdldoc.GetDoc(job_desc_str);
  };
  // Store description
  std::string fname = config_.User()->ControlDir() + "/job." + id_ + ".description";
  if(!job_description_write_file(fname,job_desc_str)) {
    delete_job_id();
    failure_="Failed to store job RSL description.";
    return;
  };
  // Analyze JSDL (checking, substituting, etc)
  std::string acl("");
  if(!parse_job_req(fname.c_str(),job_,&acl)) {
    failure_="Failed to parse job/action description.";
    delete_job_id();
    return;
  };
  // Check for proper LRMS name in request. If there is no LRMS name
  // in user configuration that means service is opaque frontend and
  // accepts any LRMS in request.
  if((!job_.lrms.empty()) && (!config_.User()->DefaultLRMS().empty())) {
    if(job_.lrms != config_.User()->DefaultLRMS()) {
      failure_="Request for LRMS "+job_.lrms+" is not allowed.";
      delete_job_id();
      return;
    };
  };
  if(job_.lrms.empty()) job_.lrms=config_.User()->DefaultLRMS();
  // Check for proper queue in request.
  if(job_.queue.empty()) job_.queue=config_.User()->DefaultQueue();
  if(job_.queue.empty()) {
    failure_="Request has no queue defined.";
    delete_job_id();
    return;
  };
  if(config_.Queues().size() > 0) { // If no queues configured - service takes any
    for(std::list<std::string>::const_iterator q = config_.Queues().begin();;++q) {
      if(q == config_.Queues().end()) {
        failure_="Requested queue "+job_.queue+" does not match any of available queues.";
        delete_job_id();
        return;
      };
      if(*q == job_.queue) break;
    };
  };
  if(!preprocess_job_req(fname,config_.User()->SessionRoot()+"/"+id_,id_)) {
    failure_="Failed to preprocess job description.";
    delete_job_id();
    return;
  };
  // Start local file 
  /* !!!!! some parameters are unchecked here - rerun,diskspace !!!!! */
  job_.jobid=id_;
  job_.starttime=time(NULL);
  job_.DN=config_.GridName();
  job_.clientname=clientid;
  // Try to create proxy
  if(!credentials.empty()) {
    std::string fname=config.User()->ControlDir()+"/job."+id_+".proxy";
    int h=::open(fname.c_str(),O_WRONLY | O_CREAT | O_EXCL,0600);
    if(h == -1) {
      failure_="Failed to store credentials.";
      delete_job_id();
      return;
    };
    fix_file_owner(fname,*config.User());
    const char* s = credentials.c_str();
    int ll = credentials.length();
    int l = 0;
    for(;(ll>0) && (l!=-1);s+=l,ll-=l) l=::write(h,s,ll);
    if(l==-1) {
      ::close(h);
      failure_="Failed to store credentials.";
      delete_job_id();
      return;
    };
    ::close(h);
    //@ try {
    //@   Certificate ci(PROXY,fname);
    //@   job_desc.expiretime = ci.Expires().GetTime();
    //@ } catch (std::exception) {
    //@   job_desc.expiretime = time(NULL);
    //@ };
  };
  // Write local file
  JobDescription job(id_,"",JOB_STATE_ACCEPTED);
  if(!job_local_write_file(job,*config_.User(),job_)) {
    delete_job_id();
    failure_="Failed to create job description.";
    return;
  };
  // Write ACL file
  if(!acl.empty()) {
    if(!job_acl_write_file(id_,*config.User(),acl)) {
      delete_job_id();
      failure_="Failed to process/store job ACL.";
      return;
    };
  };
/*
  // Call authentication/authorization plugin/exec
  int result;
  std::string response;
  // talk to external plugin to ask if we can proceed
  ContinuationPlugins::action_t act =
                 cont_plugins->run(job,*user,result,response);
  // analyze result
  if(act == ContinuationPlugins::act_fail) {
    olog<<"Failed to run external plugin: "<<response<<std::endl;
    delete_job_id();
    error_description="Job is not allowed by external plugin: "+response;
    return 1;
  } else if(act == ContinuationPlugins::act_log) {
    // Scream but go ahead
    olog<<"Failed to run external plugin: "<<response<<std::endl;
  } else if(act == ContinuationPlugins::act_pass) {
    // Just continue
    if(response.length()) olog<<"Plugin response: "<<response<<std::endl;
  } else {
    olog<<"Failed to run external plugin"<<std::endl;
    delete_job_id();
    error_description="Failed to pass external plugin.";
    return 1;
  };
*/ 
/*@
  // Make access to filesystem on behalf of local user
  if(cred_plugin && (*cred_plugin)) {
    job_subst_t subst_arg;
    subst_arg.user=user;
    subst_arg.job=&job_id;
    subst_arg.reason="new";
    // run external plugin to acquire non-unix local credentials
    if(!cred_plugin->run(job_subst,&subst_arg)) {
      olog << "Failed to run plugin" << std::endl;
      delete_job_id();
      error_description="Failed to obtain external credentials.";
      return 1;
    };
    if(cred_plugin->result() != 0) {
      olog << "Plugin failed: " << cred_plugin->result() << std::endl;
      delete_job_id();
      error_description="Failed to obtain external credentials.";
      return 1;
    };
  };
*/
  // Create session directory
  std::string dir=config_.User()->SessionRoot()+"/"+id_;
/*@
  if((getuid()==0) && (user) && (user->StrictSession())) {
    SET_USER_UID;
  };
*/
  if(mkdir(dir.c_str(),0700) != 0) {
/*@
    if((getuid()==0) && (user) && (user->StrictSession())) {
      RESET_USER_UID;
    };
*/
    delete_job_id();
    failure_="Failed to create session directory.";
    return;
  };
/*@
  if((getuid()==0) && (user) && (user->StrictSession())) {
    RESET_USER_UID;
  };
*/
  fix_file_owner(dir,*config_.User());
  // Create status file (do it last so GM picks job up here)
  if(!job_state_write_file(job,*config_.User(),JOB_STATE_ACCEPTED)) {
    delete_job_id();
    failure_="Failed registering job in grid-manager.";
    return;
  };
  SignalFIFO(*config_.User());
  return;
}

bool ARexJob::GetDescription(Arc::XMLNode& jsdl) {
  if(id_.empty()) return false;
  std::string sdesc;
  if(!job_description_read_file(id_,*config_.User(),sdesc)) return false;
  Arc::XMLNode xdesc(sdesc);
  if(!xdesc) return false;
  jsdl.Replace(xdesc);
  return true;
}

bool ARexJob::Cancel(void) {
  if(id_.empty()) return false;
  JobDescription job_desc(id_,"");
  if(!job_cancel_mark_put(job_desc,*config_.User())) return false;
  return true;
}

bool ARexJob::Clean(void) {
  if(id_.empty()) return false;
  JobDescription job_desc(id_,"");
  if(!job_clean_mark_put(job_desc,*config_.User())) return false;
  return true;
}

bool ARexJob::Resume(void) {
  if(id_.empty()) return false;
  return false;
}

std::string ARexJob::State(void) {
  if(id_.empty()) return "";
  bool job_pending;
  job_state_t state = job_state_read_file(id_,*config_.User(),job_pending);
  if(state > JOB_STATE_UNDEFINED) state=JOB_STATE_UNDEFINED;
  return states_all[state].name;
}

bool ARexJob::Failed(void) {
  if(id_.empty()) return false;
  return job_failed_mark_check(id_,*config_.User());
}

/*
bool ARexJob::make_job_id(const std::string &id) {
  if((id.find('/') != std::string::npos) || (id.find('\n') != std::string::npos)) {
    olog<<"ID contains forbidden characters"<<std::endl;
    return false;
  };
  if((id == "new") || (id == "info")) return false;
  // claim id by creating empty description file
  std::string fname=user->ControlDir()+"/job."+id+".description";
  struct stat st;
  if(stat(fname.c_str(),&st) == 0) return false;
  int h = ::open(fname.c_str(),O_RDWR | O_CREAT | O_EXCL,S_IRWXU);
  // So far assume control directory is on local fs.
  // TODO: add locks or links for NFS
  if(h == -1) return false;
  fix_file_owner(fname,*user);
  close(h);
  delete_job_id();
  job_id=id;
  return true;
}
*/

bool ARexJob::make_job_id(void) {
  if(!config_) return false;
  int i;
  //@ delete_job_id();
  for(i=0;i<100;i++) {
    id_=Arc::tostring((unsigned int)getpid())+
        Arc::tostring((unsigned int)time(NULL))+
        Arc::tostring(rand(),1);
    std::string fname=config_.User()->ControlDir()+"/job."+id_+".description";
    struct stat st;
    if(stat(fname.c_str(),&st) == 0) continue;
    int h = ::open(fname.c_str(),O_RDWR | O_CREAT | O_EXCL,0600);
    // So far assume control directory is on local fs.
    // TODO: add locks or links for NFS
    int err = errno;
    if(h == -1) {
      if(err == EEXIST) continue;
      //@ olog << "Failed to create file in " << user->ControlDir()<< std::endl;
      id_="";
      return false;
    };
    fix_file_owner(fname,*config_.User());
    close(h);
    return true;
  };
  //@ olog << "Out of tries while allocating new job id in " << user->ControlDir() << std:endl;
  id_="";
  return false;
}

bool ARexJob::delete_job_id(void) {
  if(!config_) return true;
  if(!id_.empty()) {
    job_clean_final(JobDescription(id_,
                config_.User()->SessionRoot()+"/"+id_),*config_.User());
    id_="";
  };
  return true;
}

int ARexJob::TotalJobs(ARexGMConfig& config) {
  ContinuationPlugins plugins;
  JobsList jobs(*config.User(),plugins);
  jobs.ScanNewJobs();
  return jobs.size();
}

std::list<std::string> ARexJob::Jobs(ARexGMConfig& config) {
  std::list<std::string> jlist;
  ContinuationPlugins plugins;
  JobsList jobs(*config.User(),plugins);
  jobs.ScanNewJobs();
  JobsList::iterator i = jobs.begin();
  for(;i!=jobs.end();++i) {
    // Check users's DN ?!?!?!?!
    ARexJob job(i->get_id(),config);
    if(job) jlist.push_back(i->get_id());
  };
  return jlist;
}

std::string ARexJob::SessionDir(void) {
  if(id_.empty()) return "";
  return config_.User()->SessionRoot()+"/"+id_;
}

int ARexJob::CreateFile(const std::string& filename) {
  if(id_.empty()) return -1;
  std::string fname = config_.User()->SessionRoot()+"/"+id_+"/"+filename;
  struct stat st;
  if(lstat(fname.c_str(),&st) == 0) {
    if(!S_ISREG(st.st_mode)) return -1;
  } else {
    // Create sudirectories
    std::string::size_type n = fname.length()-filename.length();
    for(;;) {
      if(strncmp("../",fname.c_str()+n,3) == 0) return -1;
      n=fname.find('/',n);
      if(n == std::string::npos) break;
      std::string dname = fname.substr(0,n);
      ++n;
      if(lstat(dname.c_str(),&st) == 0) {
        if(!S_ISDIR(st.st_mode)) return -1;
      } else {
        if(mkdir(dname.c_str(),S_IRUSR | S_IWUSR) != 0) return -1;
        fix_file_owner(dname,*config_.User());
      };
    };
  };
  int h = open(fname.c_str(),O_WRONLY | O_CREAT,S_IRUSR | S_IWUSR);
  if(h != -1) fix_file_owner(fname,*config_.User());
  return h;
}
