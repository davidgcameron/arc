#ifdef HAVE_CONFIG_H
#include <config.h>
#endif

#include <sys/stat.h>
#include <fcntl.h>

#include <arc/ArcLocation.h>
#include <arc/JobPerfLog.h>
#include <arc/credential/VOMSUtil.h>

#include "../files/ControlFileHandling.h"
#include "../run/RunParallel.h"
#include "../mail/send_mail.h"
#include "../log/JobLog.h"
#include "../log/JobsMetrics.h"
#include "../misc/proxy.h"
#include "../../delegation/DelegationStores.h"
#include "../../delegation/DelegationStore.h"
#include "../conf/GMConfig.h"

#include "ContinuationPlugins.h"
#include "DTRGenerator.h"
#include "JobsList.h"

namespace ARex {

/* max time to run submit-*-job/cancel-*-job before to
   start looking for alternative way to detect result.
   Only for protecting against lost child. */
#define CHILD_RUN_TIME_SUSPICIOUS (10*60)
/* max time to run submit-*-job/cancel-*-job before to
   decide that it is gone.
   Only for protecting against lost child. */
#define CHILD_RUN_TIME_TOO_LONG (60*60)

static Arc::Logger& logger = Arc::Logger::getRootLogger();

JobsList::JobsList(const GMConfig& gmconfig) :
    valid(false),
    config(gmconfig), staging_config(gmconfig),
    dtr_generator(config, *this),
    job_desc_handler(config), jobs_pending(0) {

  job_slow_polling_last = time(NULL);
  job_slow_polling_dir = NULL;

  for(int n = 0;n<JOB_STATE_NUM;n++) jobs_num[n]=0;
  jobs.clear();

  for (std::list<std::string>::const_iterator helper = config.Helpers().begin(); helper != config.Helpers().end(); ++helper) {
    helpers.push_back(*helper);
  }

  if(!dtr_generator) {
    logger.msg(Arc::ERROR, "Failed to start data staging threads");
    return;
  };

  valid = true;
}

JobsList::~JobsList(void) {
}

GMJobRef JobsList::FindJob(const JobId &id) {
  std::map<JobId,GMJobRef>::iterator ji = jobs.find(id);
  if(ji == jobs.end()) return GMJobRef();
  return ji->second;
}

bool JobsList::HasJob(const JobId &id) const {
  std::map<JobId,GMJobRef>::const_iterator ji = jobs.find(id);
  return (ji != jobs.end());
}

void JobsList::UpdateJobCredentials(GMJobRef i) {
  if(i) {
    if(GetLocalDescription(i)) {
      std::string delegation_id = i->local->delegationid;
      if(!delegation_id.empty()) {
        ARex::DelegationStores* delegs = config.delegations;
        if(delegs) {
          std::string cred;
          if((*delegs)[config.DelegationDir()].GetCred(delegation_id,i->local->DN,cred)) {
            (void)job_proxy_write_file(*i,config,cred);
          };
        };
      };
    };
  };
}

void JobsList::SetJobState(GMJobRef i, job_state_t new_state, const char* reason) {
  if(i) {
    if(i->job_state != new_state) {
      config.GetJobsMetrics()->ReportJobStateChange(new_state, i->job_state);
      std::string msg = Arc::Time().str(Arc::UTCTime);
      msg += " Job state change ";
      msg += i->get_state_name();
      msg += " -> ";
      msg += GMJob::get_state_name(new_state);
      if(reason) {
        msg += "   Reason: ";
        msg += reason;
      };
      msg += "\n";
      i->job_state = new_state;
      job_errors_mark_add(*i,config,msg);
      // During intermediate period job.proxy file must contain full delegated proxy.
      // To ensure its content is up to date even if proxy was updated in store here
      // we update content of that file on every job state change.
      UpdateJobCredentials(i);
    };
  };
}

bool JobsList::AddJobNoCheck(const JobId &id,uid_t uid,gid_t gid,job_state_t state){
  GMJobRef i(new GMJob(id,Arc::User(uid)));
  jobs[id] = i;
  i->keep_finished=config.keep_finished;
  i->keep_deleted=config.keep_deleted;
  i->job_state = state;
  if (!GetLocalDescription(i)) {
    // safest thing to do is add failure and move to FINISHED
    i->AddFailure("Internal error");
    SetJobState(i, JOB_STATE_FINISHED, "Internal failure");
    FailedJob(i, false);
    if(!job_state_write_file(*i,config,i->job_state)) {
      logger.msg(Arc::ERROR, "%s: Failed reading .local and changing state, job and "
                             "A-REX may be left in an inconsistent state", id);
    }
    RequestReprocess(i); // To make job being properly thrown from system
    return false;
  }
  i->session_dir = i->local->sessiondir;
  if (i->session_dir.empty()) i->session_dir = config.SessionRoot(id)+'/'+id;
  RequestAttention(i);
  return true;
}

int JobsList::AcceptedJobs() const {
  return jobs_num[JOB_STATE_ACCEPTED] +
         jobs_num[JOB_STATE_PREPARING] +
         jobs_num[JOB_STATE_SUBMITTING] +
         jobs_num[JOB_STATE_INLRMS] +
         jobs_num[JOB_STATE_FINISHING] +
         jobs_pending;
}

bool JobsList::RunningJobsLimitReached() const {
  if(config.max_jobs_running==-1) return false;
  int num = jobs_num[JOB_STATE_SUBMITTING] +
            jobs_num[JOB_STATE_INLRMS];
  return num >= config.max_jobs_running;
}

/*
int JobsList::ProcessingJobs() const {
  return jobs_num[JOB_STATE_PREPARING] +
         jobs_num[JOB_STATE_FINISHING];
}

int JobsList::PreparingJobs() const {
  return jobs_num[JOB_STATE_PREPARING];
}

int JobsList::FinishingJobs() const {
  return jobs_num[JOB_STATE_FINISHING];
}
*/


void JobsList::PrepareToDestroy(void) {
  for (std::list<ExternalHelper>::iterator i = helpers.begin(); i != helpers.end(); ++i) {
    i->stop();
  }
  for(std::map<JobId,GMJobRef>::iterator i=jobs.begin();i!=jobs.end();++i) {
    i->second->PrepareToDestroy();
  }
}

bool JobsList::RequestAttention(const JobId& id) {
  GMJobRef i = FindJob(id);
  if(!i) {
    // Must be new job arriving or finished job which got user request or whatever
    if(ScanNewJob(id) || ScanOldJob(id)) {
      i = FindJob(id);
    };
  };
  if(!i) return false;
  return RequestAttention(i);
}

bool JobsList::RequestAttention(GMJobRef i) {
  if(i) {
    Glib::Mutex::Lock lock_(jobs_attention_lock);
    logger.msg(Arc::ERROR, "--> job for attention(%u): %s", jobs_attention.size(), i->job_id);
    jobs_attention.remove(i);
    jobs_attention.push_back(i);
    jobs_attention_cond.signal();
    return true;
  };
  return false;
}

void JobsList::RequestAttention(void) {
  logger.msg(Arc::ERROR, "--> all for attention");
  jobs_attention_cond.signal();
}

bool JobsList::ScanOldJobs(void) {
  if(job_slow_polling_dir) {
    // continue already started scaning
    std::string file = job_slow_polling_dir->read_name();
    if(file.empty()) {
      delete job_slow_polling_dir; job_slow_polling_dir = NULL;
    }
    // extract job id and cause its processing
    // todo: implement fetching few jobs before passing them to attention
    // job id must contain at least one character
    int l=file.length();
    if(l>(4+7) && file.substr(0,4) == "job." && file.substr(l-7) == ".status") {
      JobId id(file.substr(4, l-7-4));
      logger.msg(Arc::ERROR, "++++ ScanOldJobs: new job: %s", id);
      RequestAttention(id);
    };
  } else {
    // Check if it is time for next scanning
    if((time(NULL) - job_slow_polling_last) >= job_slow_polling_period) {
      job_slow_polling_dir = new Glib::Dir(config.control_dir+"/"+subdir_old);
      if(job_slow_polling_dir) job_slow_polling_last = time(NULL);
    };
  };
  if(!job_slow_polling_dir) return false;
  return true;
}

void JobsList::WaitAttention(void) {
  // Check if condition signaled
  while(!jobs_attention_cond.wait(0)) {
    // Use spare time to process slow polling queue
    if(!ScanOldJobs()) {
      // If there is no scanning going on then simply wait and exit
      jobs_attention_cond.wait();
      return;
    };
  }; // while !jobs_attention_cond
}

bool JobsList::RequestWaitForRunning(GMJobRef i) {
  if(i) {
    Glib::Mutex::Lock lock_(jobs_wait_for_running_lock);
    logger.msg(Arc::ERROR, "--> job wait for running(%u): %s", jobs_wait_for_running.size(), i->job_id);
    jobs_wait_for_running.remove(i);
    jobs_wait_for_running.push_back(i);
    return true;
  };
  return false;
}

bool JobsList::RequestPolling(GMJobRef i) {
  if(i) {
    Glib::Mutex::Lock lock_(jobs_polling_lock);
    logger.msg(Arc::ERROR, "--> job for polling(%u): %s", jobs_polling.size(), i->job_id);
    jobs_polling.remove(i);
    jobs_polling.push_back(i);
    return true;
  };
  return false;
}

bool JobsList::RequestSlowPolling(GMJobRef i) {
  if(i) {
    logger.msg(Arc::ERROR, "--> job for slow polling: %s", i->job_id);
    return true;
  };
  return false;
}

bool JobsList::RequestReprocess(GMJobRef i) {
  if(i) {
    Glib::Mutex::Lock lock_(jobs_processing_lock);
    logger.msg(Arc::ERROR, "--> job for reprocess(%u): %s", jobs_processing.size(), i->job_id);
    jobs_processing.push_front(i);
    return true;
  };
  return false;
}

bool JobsList::ActJobsProcessing(void) {
  Glib::Mutex::Lock lock_(jobs_processing_lock);
  while(!jobs_processing.empty()) {
    GMJobRef i = *(jobs_processing.begin());
    logger.msg(Arc::ERROR, "<-- job in processing(%u): %s", jobs_processing.size(), i->job_id);
    jobs_processing.pop_front();
    lock_.release();
    ActJob(i);
    lock_.acquire();
    logger.msg(Arc::ERROR, "<-- jobs from processing(%u)", jobs_processing.size());
  };
  // Check limit on number of running jobs and activate some of them if possible
  if(!RunningJobsLimitReached()) {
    Glib::Mutex::Lock lock_wait_for_running_(jobs_wait_for_running_lock);
    if(jobs_wait_for_running.size() > 0) {
      // Processing one by one because some jobs may go to running and some may fail
      RequestAttention(*jobs_wait_for_running.begin());
      jobs_wait_for_running.pop_front();
    };
  };
}

bool JobsList::ActJobsAttention(void) {
  {
    Glib::Mutex::Lock lock_processing_(jobs_processing_lock);
    Glib::Mutex::Lock lock_attention_(jobs_attention_lock);
    logger.msg(Arc::ERROR, "<-- jobs in attention(%u)", jobs_attention.size());
    jobs_processing.splice(jobs_processing.end(), jobs_attention);
    logger.msg(Arc::ERROR, "<-- jobs from attention(%u)", jobs_attention.size());
  };
  ActJobsProcessing();
  return true;
}

bool JobsList::ActJobsPolling(void) {
  {
    Glib::Mutex::Lock lock_processing_(jobs_processing_lock);
    Glib::Mutex::Lock lock_polling_(jobs_polling_lock);
    logger.msg(Arc::ERROR, "<-- jobs in polling(%u)", jobs_polling.size());
    jobs_processing.splice(jobs_processing.end(), jobs_polling);
    logger.msg(Arc::ERROR, "<-- jobs from polling(%u)", jobs_polling.size());
  };
  ActJobsProcessing();
  // debug info on jobs per DN
  logger.msg(Arc::VERBOSE, "Current jobs in system (PREPARING to FINISHING) per-DN (%i entries)", jobs_dn.size());
  for (std::map<std::string, ZeroUInt>::iterator it = jobs_dn.begin(); it != jobs_dn.end(); ++it)
    logger.msg(Arc::VERBOSE, "%s: %i", it->first, (unsigned int)(it->second));
  return true;
}


/*
bool JobsList::ActJobs(void) {

//  JOB_STATE_ACCEPTED
//  JOB_STATE_PREPARING  A+P
//  JOB_STATE_SUBMITTING A
//  JOB_STATE_INLRMS
//  JOB_STATE_FINISHING  A
//  JOB_STATE_FINISHED
//  JOB_STATE_DELETED
//  JOB_STATE_CANCELING
//  JOB_STATE_UNDEFINED

  bool res = true;
  bool once_more = false;

  // first pass
  for(iterator i=jobs.begin();i!=jobs.end();) {
    if(i->job_state == JOB_STATE_UNDEFINED) { once_more=true; }
    if(i->job_state == JOB_STATE_SUBMITTING) { ++i; continue; } // temporary workaround
    if(i->job_state == JOB_STATE_PREPARING) { ++i; continue; } // temporary workaround
    if(i->job_state == JOB_STATE_FINISHING) { ++i; continue; } // temporary workaround
    res &= ActJob(i);
  }

  // second pass - process new jobs again
  if(once_more) for(iterator i=jobs.begin();i!=jobs.end();) {
    if(i->job_state == JOB_STATE_SUBMITTING) { ++i; continue; } // temporary workaround
    if(i->job_state == JOB_STATE_PREPARING) { ++i; continue; } // temporary workaround
    if(i->job_state == JOB_STATE_FINISHING) { ++i; continue; } // temporary workaround
    res &= ActJob(i);
  }

  // debug info on jobs per DN
  logger.msg(Arc::VERBOSE, "Current jobs in system (PREPARING to FINISHING) per-DN (%i entries)", jobs_dn.size());
  for (std::map<std::string, ZeroUInt>::iterator it = jobs_dn.begin(); it != jobs_dn.end(); ++it)
    logger.msg(Arc::VERBOSE, "%s: %i", it->first, (unsigned int)(it->second));

  return res;
}
*/

/*
bool JobsList::DestroyJob(JobsList::iterator &i,bool finished,bool active) {
  logger.msg(Arc::INFO,"%s: Destroying",i->job_id);
  job_state_t new_state=i->job_state;
  if(new_state == JOB_STATE_UNDEFINED) {
    // Try to obtain real job state
    if((new_state=job_state_read_file(i->job_id,config))==JOB_STATE_UNDEFINED) {
      logger.msg(Arc::ERROR,"%s: Can't read state - no comments, just cleaning",i->job_id);
      UnlockDelegation(i);
      job_clean_final(*i,config);
      i=jobs.erase(i);
      return true;
    }
    i->job_state = new_state;
  }
  if((new_state == JOB_STATE_FINISHED) && (!finished)) { ++i; return true; }
  if(!active) { ++i; return true; }
  if((new_state != JOB_STATE_INLRMS) ||
     (job_lrms_mark_check(i->job_id,config))) {
    logger.msg(Arc::INFO,"%s: Cleaning control and session directories",i->job_id);
    UnlockDelegation(i);
    job_clean_final(*i,config);
    i=jobs.erase(i);
    return true;
  }
  logger.msg(Arc::INFO,"%s: This job may be still running - canceling",i->job_id);
  bool state_changed = false;
  if(!state_canceling(i,state_changed)) {
    logger.msg(Arc::WARNING,"%s: Cancellation failed (probably job finished) - cleaning anyway",i->job_id);
    UnlockDelegation(i);
    job_clean_final(*i,config);
    i=jobs.erase(i);
    return true;
  }
  if(!state_changed) { ++i; return false; } // child still running
  logger.msg(Arc::INFO,"%s: Cancellation probably succeeded - cleaning",i->job_id);
  UnlockDelegation(i);
  job_clean_final(*i,config);
  i=jobs.erase(i);
  return true;
}
*/

bool JobsList::FailedJob(GMJobRef i,bool cancel) {
  bool r = true;
  // add failure mark
  if(job_failed_mark_add(*i,config,i->failure_reason)) {
    i->failure_reason = "";
  } else {
    r = false;
  }
  if(GetLocalDescription(i)) {
    i->local->uploads=0;
  } else {
    r=false;
  }
  // If the job failed during FINISHING then DTR deals with .output
  if (i->get_state() == JOB_STATE_FINISHING) {
    if (i->local) job_local_write_file(*i,config,*(i->local));
    return r;
  }
  // adjust output files to failure state
  // Not good looking code
  JobLocalDescription job_desc;
  if(job_desc_handler.parse_job_req(i->get_id(),job_desc) != JobReqSuccess) {
    r = false;
  }
  // Convert delegation ids to credential paths.
  std::string default_cred = config.control_dir + "/job." + i->get_id() + ".proxy";
  for(std::list<FileData>::iterator f = job_desc.outputdata.begin();
                                   f != job_desc.outputdata.end(); ++f) {
    if(f->has_lfn()) {
      if(f->cred.empty()) {
        f->cred = default_cred;
      } else {
        std::string path;
        ARex::DelegationStores* delegs = config.delegations;
        if(delegs && i->local) path = (*delegs)[config.DelegationDir()].FindCred(f->cred,i->local->DN);
        f->cred = path;
      }
      if(i->local) ++(i->local->uploads);
    }
  }
  // Add user-uploaded input files so that they are not deleted during
  // FINISHING and so resume will work. Credentials are not necessary for
  // these files. The real output list will be recreated from the job
  // description if the job is restarted.
  if (!cancel && job_desc.reruns > 0) {
    for(std::list<FileData>::iterator f = job_desc.inputdata.begin();
                                      f != job_desc.inputdata.end(); ++f) {
      if (f->lfn.find(':') == std::string::npos) {
        FileData fd(f->pfn, "");
        fd.iffailure = true; // make sure to keep file
        job_desc.outputdata.push_back(fd);
      }
    }
  }
  if(!job_output_write_file(*i,config,job_desc.outputdata,cancel?job_output_cancel:job_output_failure)) {
    r=false;
    logger.msg(Arc::ERROR,"%s: Failed writing list of output files: %s",i->job_id,Arc::StrError(errno));
  }
  if(i->local) job_local_write_file(*i,config,*(i->local));
  return r;
}

bool JobsList::GetLocalDescription(GMJobRef i) const {
  if(!i->GetLocalDescription(config)) {
    logger.msg(Arc::ERROR,"%s: Failed reading local information",i->job_id);
    return false;
  }
  return true;
}

bool JobsList::state_submitting(GMJobRef i,bool &state_changed) {
  bool const cancel = false;
  if(i->child == NULL) {
    // no child was running yet, or recovering from fault
    // write grami file for submit-X-job
    // TODO: read existing grami file to check if job is already submitted
    if(!(i->GetLocalDescription(config))) {
      logger.msg(Arc::ERROR,"%s: Failed reading local information",i->job_id);
      if(!cancel) i->AddFailure("Internal error: can't read local file");
      return false;
    };
    JobLocalDescription* job_desc = i->local;
    if(!cancel) {  // in case of cancel all preparations are already done
      if(!job_desc_handler.write_grami(*i)) {
        logger.msg(Arc::ERROR,"%s: Failed creating grami file",i->job_id);
        return false;
      }
      if(!job_desc_handler.set_execs(*i)) {
        logger.msg(Arc::ERROR,"%s: Failed setting executable permissions",i->job_id);
        return false;
      }
      // precreate file to store diagnostics from lrms
      job_diagnostics_mark_put(*i,config);
      job_lrmsoutput_mark_put(*i,config);
    }
    // submit/cancel job to LRMS using submit/cancel-X-job
    std::string cmd;
    if(cancel) { cmd=Arc::ArcLocation::GetDataDir()+"/cancel-"+job_desc->lrms+"-job"; }
    else { cmd=Arc::ArcLocation::GetDataDir()+"/submit-"+job_desc->lrms+"-job"; }
    if(!cancel) {
      logger.msg(Arc::INFO,"%s: state SUBMIT: starting child: %s",i->job_id,cmd);
    } else {
      if(!job_lrms_mark_check(i->job_id,config)) {
        logger.msg(Arc::INFO,"%s: state CANCELING: starting child: %s",i->job_id,cmd);
      } else {
        logger.msg(Arc::INFO,"%s: Job has completed already. No action taken to cancel",i->job_id);
        state_changed=true;
        return true;
      }
    }
    std::string grami = config.control_dir+"/job."+(*i).job_id+".grami";
    cmd += " --config " + config.conffile + " " + grami;
    job_errors_mark_put(*i,config);
    if(!RunParallel::run(config,*i,*this,cmd,&(i->child))) {
      if(!cancel) {
        i->AddFailure("Failed initiating job submission to LRMS");
        logger.msg(Arc::ERROR,"%s: Failed running submission process",i->job_id);
      } else {
        logger.msg(Arc::ERROR,"%s: Failed running cancellation process",i->job_id);
      }
      return false;
    }
    return true;
  }
  // child was run - check if exited and then exit code
  bool simulate_success = false;
  if(i->child->Running()) {
    // child is running - come later
    // Due to unknown reason sometimes child exit event is lost.
    // As workaround check if child is running for too long. If
    // it does then check in grami file for generated local id
    // or in case of cancel just assume child exited.
    if((Arc::Time() - i->child->RunTime()) > Arc::Period(CHILD_RUN_TIME_SUSPICIOUS)) {
      if(!cancel) {
        // Check if local id is already obtained
        std::string local_id=job_desc_handler.get_local_id(i->job_id);
        if(local_id.length() > 0) {
          simulate_success = true;
          logger.msg(Arc::ERROR,"%s: Job submission to LRMS takes too long, but ID is already obtained. Pretending submission is done.",i->job_id);
        }
      } else {
        // Check if diagnostics collection is done
        if(job_lrms_mark_check(i->job_id,config)) {
          simulate_success = true;
          logger.msg(Arc::ERROR,"%s: Job cancellation takes too long, but diagnostic collection seems to be done. Pretending cancellation succeeded.",i->job_id);
        }
      }
    }
    if((!simulate_success) && (Arc::Time() - i->child->RunTime()) > Arc::Period(CHILD_RUN_TIME_TOO_LONG)) {
      // In any case it is way too long. Job must fail. Otherwise it will hang forever.
      delete i->child; i->child=NULL;
      if(!cancel) {
        logger.msg(Arc::ERROR,"%s: Job submission to LRMS takes too long. Failing.",i->job_id);
        JobFailStateRemember(i,JOB_STATE_SUBMITTING);
        i->AddFailure("Job submission to LRMS failed");
        // It would be nice to cancel if job finally submits. But we do not know id.
        return false;
      } else {
        logger.msg(Arc::ERROR,"%s: Job cancellation takes too long. Failing.",i->job_id);
        delete i->child; i->child=NULL;
        return false;
      }
    }
    if(!simulate_success) return true;
  }
  if(!simulate_success) {
    // real processing
    if(!cancel) {
      logger.msg(Arc::INFO,"%s: state SUBMIT: child exited with code %i",i->job_id,i->child->Result());
    } else {
      if((i->child->ExitTime() != Arc::Time::UNDEFINED) &&
         ((Arc::Time() - i->child->ExitTime()) < (config.wakeup_period*2))) {
        // not ideal solution
        logger.msg(Arc::INFO,"%s: state CANCELING: child exited with code %i",i->job_id,i->child->Result());
      }
    }
    // Another workaround in Run class may also detect lost child.
    // It then sets exit code to -1. This value is also set in
    // case child was killed. So it is worth to check grami anyway.
    if((i->child->Result() != 0) && (i->child->Result() != -1)) {
      if(!cancel) {
        logger.msg(Arc::ERROR,"%s: Job submission to LRMS failed",i->job_id);
        JobFailStateRemember(i,JOB_STATE_SUBMITTING);
      } else {
        logger.msg(Arc::ERROR,"%s: Failed to cancel running job",i->job_id);
      }
      delete i->child; i->child=NULL;
      if(!cancel) i->AddFailure("Job submission to LRMS failed");
      return false;
    }
  } else {
    // Just pretend everything is alright
  }
  if(!cancel) {
    delete i->child; i->child=NULL;
    // success code - get LRMS job id
    std::string local_id=job_desc_handler.get_local_id(i->job_id);
    if(local_id.length() == 0) {
      logger.msg(Arc::ERROR,"%s: Failed obtaining lrms id",i->job_id);
      i->AddFailure("Failed extracting LRMS ID due to some internal error");
      JobFailStateRemember(i,JOB_STATE_SUBMITTING);
      return false;
    }
    // put id into local information file
    if(!GetLocalDescription(i)) {
      i->AddFailure("Internal error");
      return false;
    }
    i->local->localid=local_id;
    if(!job_local_write_file(*i,config,*(i->local))) {
      i->AddFailure("Internal error");
      logger.msg(Arc::ERROR,"%s: Failed writing local information: %s",i->job_id,Arc::StrError(errno));
      return false;
    }
  } else {
    // job diagnostics collection done in background (scan-*-job script)
    if(!job_lrms_mark_check(i->job_id,config)) {
      // job diag not yet collected - come later
      if((i->child->ExitTime() != Arc::Time::UNDEFINED) &&
         ((Arc::Time() - i->child->ExitTime()) > Arc::Period(Arc::Time::HOUR))) {
        // it takes too long
        logger.msg(Arc::ERROR,"%s: state CANCELING: timeout waiting for cancellation",i->job_id);
        delete i->child; i->child=NULL;
        return false;
      }
      return true;
    } else {
      logger.msg(Arc::INFO,"%s: state CANCELING: job diagnostics collected",i->job_id);
      delete i->child; i->child=NULL;
      job_diagnostics_mark_move(*i,config);
    }
  }
  // move to next state
  state_changed=true;
  return true;
}

bool JobsList::state_canceling(GMJobRef i,bool &state_changed) {
  if(i->child == NULL) {
    // no child was running yet, or recovering from fault
    // write grami file for cancel-X-job
    if(!(i->GetLocalDescription(config))) {
      logger.msg(Arc::ERROR,"%s: Failed reading local information",i->job_id);
      return false;
    };
    JobLocalDescription* job_desc = i->local;
    // cancel job to LRMS using cancel-X-job
    std::string cmd;
    cmd=Arc::ArcLocation::GetDataDir()+"/cancel-"+job_desc->lrms+"-job";
    if(!job_lrms_mark_check(i->job_id,config)) {
      logger.msg(Arc::INFO,"%s: state CANCELING: starting child: %s",i->job_id,cmd);
    } else {
      logger.msg(Arc::INFO,"%s: Job has completed already. No action taken to cancel",i->job_id);
      state_changed=true;
      return true;
    }
    std::string grami = config.control_dir+"/job."+(*i).job_id+".grami";
    cmd += " --config " + config.conffile + " " + grami;
    job_errors_mark_put(*i,config);
    if(!RunParallel::run(config,*i,*this,cmd,&(i->child))) {
      logger.msg(Arc::ERROR,"%s: Failed running cancellation process",i->job_id);
      return false;
    }
    return true;
  }
  // child was run - check if exited
  bool simulate_success = false;
  if(i->child->Running()) {
    // child is running - come later
    // Due to unknown reason sometimes child exit event is lost.
    // As workaround check if child is running for too long.
    // In case of cancel just assume child exited.
    if((Arc::Time() - i->child->RunTime()) > Arc::Period(CHILD_RUN_TIME_SUSPICIOUS)) {
      // Check if diagnostics collection is done
      if(job_lrms_mark_check(i->job_id,config)) {
        simulate_success = true;
        logger.msg(Arc::ERROR,"%s: Job cancellation takes too long, but diagnostic collection seems to be done. Pretending cancellation succeeded.",i->job_id);
      }
    }
    if((!simulate_success) && (Arc::Time() - i->child->RunTime()) > Arc::Period(CHILD_RUN_TIME_TOO_LONG)) {
      // In any case it is way too long. Job must fail. Otherwise it will hang forever.
      delete i->child; i->child=NULL;
      logger.msg(Arc::ERROR,"%s: Job cancellation takes too long. Failing.",i->job_id);
      delete i->child; i->child=NULL;
      return false;
    }
    if(!simulate_success) return true;
  }
  if(!simulate_success) {
    // real processing
    if((i->child->ExitTime() != Arc::Time::UNDEFINED) &&
       ((Arc::Time() - i->child->ExitTime()) < (config.wakeup_period*2))) {
      // not ideal solution
      logger.msg(Arc::INFO,"%s: state CANCELING: child exited with code %i",i->job_id,i->child->Result());
    }
    // Another workaround in Run class may also detect lost child.
    // It then sets exit code to -1. This value is also set in
    // case child was killed. So it is worth to check grami anyway.
    if((i->child->Result() != 0) && (i->child->Result() != -1)) {
      logger.msg(Arc::ERROR,"%s: Failed to cancel running job",i->job_id);
      delete i->child; i->child=NULL;
      return false;
    }
  } else {
    // Just pretend everything is alright
  }
  // job diagnostics collection done in background (scan-*-job script)
  if(!job_lrms_mark_check(i->job_id,config)) {
    // job diag not yet collected - come later
    if((i->child->ExitTime() != Arc::Time::UNDEFINED) &&
       ((Arc::Time() - i->child->ExitTime()) > Arc::Period(Arc::Time::HOUR))) {
      // it takes too long
      logger.msg(Arc::ERROR,"%s: state CANCELING: timeout waiting for cancellation",i->job_id);
      delete i->child; i->child=NULL;
      return false;
    }
    return true;
  } else {
    logger.msg(Arc::INFO,"%s: state CANCELING: job diagnostics collected",i->job_id);
    delete i->child; i->child=NULL;
    job_diagnostics_mark_move(*i,config);
  }
  // move to next state
  state_changed=true;
  return true;
}

bool JobsList::state_loading(GMJobRef i,bool &state_changed,bool up) {

  // first check if job is already in the system
  if (!dtr_generator.hasJob(*i)) {
    dtr_generator.receiveJob(*i);
    return true;
  }
  // if job has already failed then do not set failed state again if DTR failed
  bool already_failed = i->CheckFailure(config);
  // queryJobFinished() calls i->AddFailure() if any DTR failed
  if (dtr_generator.queryJobFinished(*i)) {
    // DTR part already finished. Do other checks if needed.

    bool done = true;
    bool result = true;

    // check for failure
    if (i->CheckFailure(config)) {
      if (!already_failed) JobFailStateRemember(i, (up ? JOB_STATE_FINISHING : JOB_STATE_PREPARING));
      result = false;
    }
    else {
      if (!up) { // check for user-uploadable files if downloading
        DTRGenerator::checkUploadedFilesResult res = dtr_generator.checkUploadedFiles(*i);
        if (res == DTRGenerator::uploadedFilesMissing) { // still going
          RequestPolling(i);
          done = false;
        }
        else if (res == DTRGenerator::uploadedFilesSuccess) { // finished successfully
          state_changed=true;
        }
        else { // error
          result = false;
        }
      }
      else { // if uploading we are done
        state_changed = true;
      }
    }
    if (done) dtr_generator.removeJob(*i);
    return result;
  }
  else {
    // not finished yet - should not happen
    logger.msg(Arc::VERBOSE, "%s: State: %s: still in data staging", i->job_id, (up ? "FINISHING" : "PREPARING"));
    RequestPolling(i);
    return true;
  }
}

bool JobsList::JobPending(GMJobRef i) {
  if(i->job_pending) return true;
  i->job_pending=true;
  return job_state_write_file(*i,config,i->job_state,true);
}

job_state_t JobsList::JobFailStateGet(GMJobRef i) {
  if(!GetLocalDescription(i)) return JOB_STATE_UNDEFINED;
  if(i->local->failedstate.empty()) { return JOB_STATE_UNDEFINED; }
  job_state_t state = GMJob::get_state(i->local->failedstate.c_str());
  if(state != JOB_STATE_UNDEFINED) {
    if(i->local->reruns <= 0) {
      logger.msg(Arc::ERROR,"%s: Job is not allowed to be rerun anymore",i->job_id);
      job_local_write_file(*i,config,*(i->local));
      return JOB_STATE_UNDEFINED;
    }
    i->local->failedstate="";
    i->local->failedcause="";
    i->local->reruns--;
    job_local_write_file(*i,config,*(i->local));
    return state;
  }
  logger.msg(Arc::ERROR,"%s: Job failed in unknown state. Won't rerun.",i->job_id);
  i->local->failedstate="";
  i->local->failedcause="";
  job_local_write_file(*i,config,*(i->local));
  return JOB_STATE_UNDEFINED;
}

bool JobsList::RecreateTransferLists(GMJobRef i) {
  // Recreate list of output and input files, excluding those already
  // transferred. For input files this is done by looking at the session dir,
  // for output files by excluding files in .output_status
  std::list<FileData> output_files;
  std::list<FileData> output_files_done;
  std::list<FileData> input_files;
  // keep local info
  if(!GetLocalDescription(i)) return false;
  // get output files already done
  job_output_status_read_file(i->job_id,config,output_files_done);
  // recreate lists by reprocessing job description
  JobLocalDescription job_desc; // placeholder
  if(!job_desc_handler.process_job_req(*i,job_desc)) {
    logger.msg(Arc::ERROR,"%s: Reprocessing job description failed",i->job_id);
    return false;
  }
  // Restore 'local'
  if(!job_local_write_file(*i,config,*(i->local))) return false;
  // Read new lists
  if(!job_output_read_file(i->job_id,config,output_files)) {
    logger.msg(Arc::ERROR,"%s: Failed to read reprocessed list of output files",i->job_id);
    return false;
  }
  if(!job_input_read_file(i->job_id,config,input_files)) {
    logger.msg(Arc::ERROR,"%s: Failed to read reprocessed list of input files",i->job_id);
    return false;
  }
  // remove already uploaded files
  i->local->uploads=0;
  for(std::list<FileData>::iterator i_new = output_files.begin();
                                    i_new!=output_files.end();) {
    if(!(i_new->has_lfn())) { // user file - keep
      ++i_new;
      continue;
    }
    std::list<FileData>::iterator i_done = output_files_done.begin();
    for(;i_done!=output_files_done.end();++i_done) {
      if((i_new->pfn == i_done->pfn) && (i_new->lfn == i_done->lfn)) break;
    }
    if(i_done == output_files_done.end()) {
      ++i_new;
      i->local->uploads++;
      continue;
    }
    i_new=output_files.erase(i_new);
  }
  if(!job_output_write_file(*i,config,output_files)) return false;
  // remove already downloaded files
  i->local->downloads=0;
  for(std::list<FileData>::iterator i_new = input_files.begin();
                                    i_new!=input_files.end();) {
    std::string path = i->session_dir+"/"+i_new->pfn;
    struct stat st;
    if(::stat(path.c_str(),&st) == -1) {
      ++i_new;
      i->local->downloads++;
    } else {
      i_new=input_files.erase(i_new);
    }
  }
  if(!job_input_write_file(*i,config,input_files)) return false;
  return true;
}

bool JobsList::JobFailStateRemember(GMJobRef i,job_state_t state,bool internal) {
  if(!(i->GetLocalDescription(config))) {
    logger.msg(Arc::ERROR,"%s: Failed reading local information",i->job_id);
    return false;
  }
  if(state == JOB_STATE_UNDEFINED) {
    i->local->failedstate="";
    i->local->failedcause=internal?"internal":"client";
    return job_local_write_file(*i,config,*(i->local));
  }
  if(i->local->failedstate.empty()) {
    i->local->failedstate=GMJob::get_state_name(state);
    i->local->failedcause=internal?"internal":"client";
    return job_local_write_file(*i,config,*(i->local));
  }
  return true;
}

time_t JobsList::PrepareCleanupTime(GMJobRef i,time_t& keep_finished) {
  JobLocalDescription job_desc;
  time_t t = -1;
  // read lifetime - if empty it wont be overwritten
  job_local_read_file(i->job_id,config,job_desc);
  if(!Arc::stringto(job_desc.lifetime,t)) t = keep_finished;
  if(t > keep_finished) t = keep_finished;
  time_t last_changed=job_state_time(i->job_id,config);
  t=last_changed+t; job_desc.cleanuptime=t;
  job_local_write_file(*i,config,job_desc);
  return t;
}

void JobsList::UnlockDelegation(GMJobRef i) {
  ARex::DelegationStores* delegs = config.delegations;
  if(delegs) (*delegs)[config.DelegationDir()].ReleaseCred(i->job_id,true,false);
}

JobsList::ActJobResult JobsList::ActJobUndefined(GMJobRef i) {
  ActJobResult job_result = JobDropped;
  // new job - read its status from status file, but first check if it is
  // under the limit of maximum jobs allowed in the system
  if((AcceptedJobs() < config.max_jobs) || (config.max_jobs == -1)) {
    job_state_t new_state=job_state_read_file(i->job_id,config);
    if(new_state == JOB_STATE_UNDEFINED) { // something failed
      logger.msg(Arc::ERROR,"%s: Reading status of new job failed",i->job_id);
      i->AddFailure("Failed reading status of the job");
      return JobFailed;
    }
          // By keeping once_more==false job does not cycle here but
          // goes out and registers its state in counters. This allows
          // to maintain limits properly after restart. Except FINISHED
          // jobs because they are not kept in memory and should be
          // processed immediately.
    SetJobState(i, new_state, "(Re)Accepting new job"); // this can be any state, after A-REX restart
    job_result = JobSuccess;
    if(new_state == JOB_STATE_ACCEPTED) {
//!!            state_changed = true; // to trigger email notification, etc.
      // first phase of job - just  accepted - parse request
      logger.msg(Arc::INFO,"%s: State: ACCEPTED: parsing job description",i->job_id);
      if(!job_desc_handler.process_job_req(*i,*i->local)) {
        logger.msg(Arc::ERROR,"%s: Processing job description failed",i->job_id);
        i->AddFailure("Could not process job description");
        return JobFailed; // go to next job
      }
      job_state_write_file(*i,config,i->job_state);
      // prepare information for logger
      // This call is not needed here because at higher level make_file()
      // is called for every state change
      //config.job_log->make_file(*i,config);
logger.msg(Arc::ERROR, "++++ ActJobUndefined: new job: %s", i->job_id);
      RequestAttention(i); // process ASAP
    } else if(new_state == JOB_STATE_FINISHED) {
      //!!job_state_write_file(*i,config,i->job_state);
      RequestReprocess(i); // process immediately
    } else if(new_state == JOB_STATE_DELETED) {
      //!!job_state_write_file(*i,config,i->job_state);
      RequestReprocess(i); // process immediately
    } else {
      // Generic case
      logger.msg(Arc::INFO,"%s: %s: New job belongs to %i/%i",i->job_id.c_str(),
          GMJob::get_state_name(new_state),i->get_user().get_uid(),i->get_user().get_gid());
      // Make it clean state after restart
      job_state_write_file(*i,config,i->job_state);
      i->Start();
logger.msg(Arc::ERROR, "++++ ActJobUndefined: old job: %s", i->job_id);
      RequestAttention(i); // process ASAP
    }
  } // Not doing JobPending here because that job kind of does not exist.
  return job_result;
}

JobsList::ActJobResult JobsList::ActJobAccepted(GMJobRef i) {
  // accepted state - job was just accepted by A-REX and we already
  // know that it is accepted - now we are analyzing/parsing request,
  // or it can also happen we are waiting for user specified time
  logger.msg(Arc::VERBOSE,"%s: State: ACCEPTED",i->job_id);
  if(!GetLocalDescription(i)) {
    i->AddFailure("Internal error");
    return JobFailed; // go to next job
  }
  if(i->local->dryrun) {
    logger.msg(Arc::INFO,"%s: State: ACCEPTED: dryrun",i->job_id);
    i->AddFailure("User requested dryrun. Job skipped.");
    return JobFailed; // go to next job
  }
  // check per-DN limit on processing jobs
  // TODO: do it in ActJobUndefined. Otherwise one DN can block others if total limit is reached.
  if (config.max_jobs_per_dn > 0 && jobs_dn[i->local->DN] >= config.max_jobs_per_dn) {
    JobPending(i);
    RequestPolling(i);
    return JobSuccess;
  }
  // check for user specified time
  if(i->local->processtime != -1 && (i->local->processtime) > time(NULL)) {
    logger.msg(Arc::INFO,"%s: State: ACCEPTED: has process time %s",i->job_id.c_str(),
        i->local->processtime.str(Arc::UserTime));
    RequestPolling(i);
    return JobSuccess;
  }
  logger.msg(Arc::INFO,"%s: State: ACCEPTED: moving to PREPARING",i->job_id);
  SetJobState(i, JOB_STATE_PREPARING, "Starting job processing");
  i->Start();

  // gather some frontend specific information for user, do it only once
  // Runs user-supplied executable placed at "frontend-info-collector"
  std::string cmd = Arc::ArcLocation::GetToolsDir()+"/frontend-info-collector";
  char const * const args[2] = { cmd.c_str(), NULL };
  job_controldiag_mark_put(*i,config,args);
  RequestReprocess(i);
  return JobSuccess;
}

JobsList::ActJobResult JobsList::ActJobPreparing(GMJobRef i) {
  // preparing state - job is in data staging system, so check if it has
  // finished and whether all user uploadable files have been uploaded.
  logger.msg(Arc::VERBOSE,"%s: State: PREPARING",i->job_id);
  bool state_changed = false;
  // TODO: avoid re-checking all conditions while job is pending
  if(i->job_pending || state_loading(i,state_changed,false)) {
    if(i->job_pending || state_changed) {
      // check for rest of state changing condition
      if(!GetLocalDescription(i)) {
        logger.msg(Arc::ERROR,"%s: Failed obtaining local job information.",i->job_id);
        i->AddFailure("Internal error");
        return JobFailed;
      }
      // For jobs with free stage in check if user reported complete stage in.
      bool stagein_complete = true;
      if(i->local->freestagein) {
        stagein_complete = false;
        std::list<std::string> ifiles;
        if(job_input_status_read_file(i->job_id,config,ifiles)) {
          for(std::list<std::string>::iterator ifile = ifiles.begin();
                             ifile != ifiles.end(); ++ifile) {
            if(*ifile == "/") {
              stagein_complete = true;
              break;
            }
          }
        }
      }
      // Here we have branch. Either job is ordinary one and goes to SUBMIT
      // or it has no executable and hence goes to FINISHING
      if(!stagein_complete) {
        // Wait for user to report complete staging keeping job in PENDING
        JobPending(i);
        // The complete stagein will be reported and will cause RequestAttention()
        // RequestPolling(i);
      } else if(i->local->exec.size() > 0) {
        // Job has executable
        if(!RunningJobsLimitReached()) {
          // And limit of running jobs is not reached
          SetJobState(i, JOB_STATE_SUBMITTING, "Pre-staging finished, passing job to LRMS");
          RequestReprocess(i); // act on new state immediately
        } else {
          // Wait for running jobs to fall below limit keeping job in PENDING
          JobPending(i);
          RequestWaitForRunning(i);
        }
      } else {
        // No execution requested
        SetJobState(i, JOB_STATE_FINISHING, "Job does NOT define executable. Going directly to post-staging.");
        RequestReprocess(i); // act on new state immediately
      }
    }
    return JobSuccess;
  }
  else {
    if(!i->CheckFailure(config)) i->AddFailure("Data download failed");
    return JobFailed;
  }
}

JobsList::ActJobResult JobsList::ActJobSubmitting(GMJobRef i) {
  // everything is ready for submission to batch system or currently submitting
  logger.msg(Arc::VERBOSE,"%s: State: SUBMIT",i->job_id);
  bool state_changed = false;
  if(state_submitting(i,state_changed)) {
    if(state_changed) {
      SetJobState(i, JOB_STATE_INLRMS, "Job is passed to LRMS");
      RequestReprocess(i);
      return JobSuccess;
    } else {
      //return JobSuccess; // state_submitting will report job for attention
      RequestPolling(i); // hack for hanging child !!
      return JobSuccess;
    }
  } else {
    return JobFailed;
  }
}

JobsList::ActJobResult JobsList::ActJobCanceling(GMJobRef i) {
  // This state is like submitting, only -cancel instead of -submit
  logger.msg(Arc::VERBOSE,"%s: State: CANCELING",i->job_id);
  bool state_changed = false;
  if(state_canceling(i,state_changed)) {
    if(state_changed) {
      SetJobState(i, JOB_STATE_FINISHING, "Job cancelation succeeded");
      RequestReprocess(i);
      return JobSuccess;
    } else {
      //return JobSuccess; // state_canceling will report job for attention
      RequestPolling(i); // hack for hanging child !!
      return JobSuccess;
    }
  } else {
    return JobFailed;
  }
}

JobsList::ActJobResult JobsList::ActJobInlrms(GMJobRef i) {
  // Job is currently running in LRMS, check if it has finished
  logger.msg(Arc::VERBOSE,"%s: State: INLRMS",i->job_id);
  if(!GetLocalDescription(i)) {
    i->AddFailure("Failed reading local job information");
    return JobFailed; // go to next job
  }
logger.msg(Arc::ERROR,"%s: State: INLRMS - checking for pending(%u) and mark",i->job_id, (unsigned int)(i->job_pending));
  if(i->job_pending || job_lrms_mark_check(i->job_id,config)) {
logger.msg(Arc::ERROR,"%s: State: INLRMS - checking for not pending",i->job_id);
    if(!i->job_pending) {
      logger.msg(Arc::INFO,"%s: Job finished",i->job_id);
      job_diagnostics_mark_move(*i,config);
      LRMSResult ec = job_lrms_mark_read(i->job_id,config);
      if(ec.code() != i->local->exec.successcode) {
        logger.msg(Arc::INFO,"%s: State: INLRMS: exit message is %i %s",i->job_id,ec.code(),ec.description());
        i->AddFailure("LRMS error: ("+
              Arc::tostring(ec.code())+") "+ec.description());
        JobFailStateRemember(i,JOB_STATE_INLRMS);
        // This does not require any special postprocessing and
        // can go to next state directly
        return JobFailed;
      }
    }
    SetJobState(i, JOB_STATE_FINISHING, "Job finished executing in LRMS");
    RequestReprocess(i);
    return JobSuccess;
  } else {
logger.msg(Arc::ERROR,"%s: State: INLRMS - no mark found",i->job_id);
    // Do polling while waiting for execution to finish - TODO: replace with report from job scanner
    RequestPolling(i);
    return JobSuccess;
  }
}

JobsList::ActJobResult JobsList::ActJobFinishing(GMJobRef i) {
  // Batch job has finished and now ready to upload output files, or
  // upload is already on-going
  logger.msg(Arc::VERBOSE,"%s: State: FINISHING",i->job_id);
  bool state_changed = false;
  if(state_loading(i,state_changed,true)) {
    if(state_changed) {
      SetJobState(i, JOB_STATE_FINISHED, "Stage-out finished.");
      RequestReprocess(i);
    } else {
      // still in data staging
    }
    return JobSuccess;
  } else {
//!!          state_changed=true; // to send mail
    if(!i->CheckFailure(config)) i->AddFailure("Data upload failed");
    return JobFailed;
  }
}

JobsList::ActJobResult JobsList::ActJobFinished(GMJobRef i) {
  // Job has completely finished, check for user requests to restart or
  // clean up job, and if it is time to move to DELETED
  if(job_clean_mark_check(i->job_id,config)) {
    // request to clean job
    logger.msg(Arc::INFO,"%s: Job is requested to clean - deleting",i->job_id);
    UnlockDelegation(i);
    // delete everything
    job_clean_final(*i,config);
    return JobDropped;
  }
  if(job_restart_mark_check(i->job_id,config)) {
    job_restart_mark_remove(i->job_id,config);
    // request to rerun job - check if we can
    // Get information about failed state and forget it
    job_state_t state_ = JobFailStateGet(i);
    if(state_ == JOB_STATE_PREPARING) {
      if(RecreateTransferLists(i)) {
        job_failed_mark_remove(i->job_id,config);
        SetJobState(i, JOB_STATE_ACCEPTED, "Request to restart failed job");
        JobPending(i); // make it go to end of state immediately
logger.msg(Arc::ERROR, "++++ ActJobFinished: restarted job: %s", i->job_id);
        RequestAttention(i); // make it start ASAP
        return JobSuccess;
      }
    } else if((state_ == JOB_STATE_SUBMITTING) ||
              (state_ == JOB_STATE_INLRMS)) {
      if(RecreateTransferLists(i)) {
        job_failed_mark_remove(i->job_id,config);
        if(i->local->downloads > 0) {
          // missing input files has to be re-downloaded
          SetJobState(i, JOB_STATE_ACCEPTED, "Request to restart failed job (some input files are missing)");
        } else {
          SetJobState(i, JOB_STATE_PREPARING, "Request to restart failed job (no input files are missing)");
        }
        JobPending(i); // make it go to end of state immediately
        // TODO: check for order of processing
logger.msg(Arc::ERROR, "++++ ActJobFinished: restarted job2: %s", i->job_id);
        RequestAttention(i); // make it start ASAP
        return JobSuccess;
      }
    } else if(state_ == JOB_STATE_FINISHING) {
      if(RecreateTransferLists(i)) {
        job_failed_mark_remove(i->job_id,config);
        SetJobState(i, JOB_STATE_INLRMS, "Request to restart failed job");
        JobPending(i); // make it go to end of state immediately
logger.msg(Arc::ERROR, "++++ ActJobFinished: restarted job3: %s", i->job_id);
        RequestAttention(i); // make it start ASAP
        return JobSuccess;
      }
    } else if(state_ == JOB_STATE_UNDEFINED) {
      logger.msg(Arc::ERROR,"%s: Can't rerun on request",i->job_id);
    } else {
      logger.msg(Arc::ERROR,"%s: Can't rerun on request - not a suitable state",i->job_id);
    }
  }
  // process cleanup time
  time_t t = -1;
  if(!job_local_read_cleanuptime(i->job_id,config,t)) {
    // must be first time - create cleanuptime
    t=PrepareCleanupTime(i,i->keep_finished);
  }
  // check if it is time to move job to DELETED
  if(((int)(time(NULL)-t)) >= 0) {
    logger.msg(Arc::INFO,"%s: Job is too old - deleting",i->job_id);
    UnlockDelegation(i);
    // Check either configured to keep job in DELETED state
    if(i->keep_deleted) {
      // here we have to get the cache per-job dirs to be deleted
      std::list<std::string> cache_per_job_dirs;
      CacheConfig cache_config(config.cache_params);
      cache_config.substitute(config, i->user);
      std::vector<std::string> conf_caches = cache_config.getCacheDirs();
      // add each dir to our list
      for (std::vector<std::string>::iterator it = conf_caches.begin(); it != conf_caches.end(); it++) {
        cache_per_job_dirs.push_back(it->substr(0, it->find(" "))+"/joblinks");
      }
      // add remote caches
      std::vector<std::string> remote_caches = cache_config.getRemoteCacheDirs();
      for (std::vector<std::string>::iterator it = remote_caches.begin(); it != remote_caches.end(); it++) {
        cache_per_job_dirs.push_back(it->substr(0, it->find(" "))+"/joblinks");
      }
      // add draining caches
      std::vector<std::string> draining_caches = cache_config.getDrainingCacheDirs();
      for (std::vector<std::string>::iterator it = draining_caches.begin(); it != draining_caches.end(); it++) {
        cache_per_job_dirs.push_back(it->substr(0, it->find(" "))+"/joblinks");
      }
      job_clean_deleted(*i,config,cache_per_job_dirs);
      SetJobState(i, JOB_STATE_DELETED, "Job stayed unattended too long");
      RequestSlowPolling(i);
      // DELETED jobs not kept in memory. So it is not important if return is Success or Dropped
      return JobDropped;
    } else {
      // delete everything
      job_clean_final(*i,config);
      return JobDropped;
    }
  } else {
    RequestSlowPolling(i);
    // FINISHED jobs not kept in memory. So it is not important if return is Success or Dropped
    return JobDropped;
  }
}

JobsList::ActJobResult JobsList::ActJobDeleted(GMJobRef i) {
  // Job only has a few control files left, check if is it time to
  // remove all traces
  time_t t = -1;
  if(!job_local_read_cleanuptime(i->job_id,config,t) || ((time(NULL)-(t+i->keep_deleted)) >= 0)) {
    logger.msg(Arc::INFO,"%s: Job is ancient - delete rest of information",i->job_id);
    UnlockDelegation(i); // not needed here but in case someting went wrong previously
    // delete everything
    job_clean_final(*i,config);
    return JobDropped;
  }
  RequestSlowPolling(i);
  return JobDropped;
}

bool JobsList::CheckJobCancelRequest(GMJobRef i) {
  // some states can not be canceled (or there is no sense to do that)
  if((i->job_state != JOB_STATE_CANCELING) &&
     (i->job_state != JOB_STATE_FINISHED) &&
     (i->job_state != JOB_STATE_DELETED) &&
     (i->job_state != JOB_STATE_SUBMITTING)) {
    if(job_cancel_mark_check(i->job_id,config)) {
      logger.msg(Arc::INFO,"%s: Canceling job because of user request",i->job_id);
      if (i->job_state == JOB_STATE_PREPARING || i->job_state == JOB_STATE_FINISHING) {
        dtr_generator.cancelJob(*i);
      }
      // kill running child
      if(i->child) {
        i->child->Kill(0);
        delete i->child; i->child=NULL;
      }
      // put some explanation
      i->AddFailure("User requested to cancel the job");
      JobFailStateRemember(i,i->job_state,false);
      // behave like if job failed
      if(!FailedJob(i,true)) {
        // DO NOT KNOW WHAT TO DO HERE !!!!!!!!!!
      }
      // special processing for INLRMS case
      if(i->job_state == JOB_STATE_INLRMS) {
        SetJobState(i, JOB_STATE_CANCELING, "Request to cancel job");
      }
      // if FINISHING we wait to get back all DTRs
      else if (i->job_state != JOB_STATE_PREPARING) {
        SetJobState(i, JOB_STATE_FINISHING, "Request to cancel job");
      }
      job_cancel_mark_remove(i->job_id,config);
      RequestReprocess(i);
      return true;
    }
  }
  return false;
}

bool JobsList::CheckJobContinuePlugins(GMJobRef i) {
  bool plugins_result = true;
  if(config.cont_plugins) {
    std::list<ContinuationPlugins::result_t> results;
    config.cont_plugins->run(*i,config,results);
    std::list<ContinuationPlugins::result_t>::iterator result = results.begin();
    while(result != results.end()) {
      // analyze results
      if(result->action == ContinuationPlugins::act_fail) {
        logger.msg(Arc::ERROR,"%s: Plugin at state %s : %s",
            i->job_id.c_str(),i->get_state_name(),
            result->response);
        i->AddFailure(std::string("Plugin at state ")+
        i->get_state_name()+" failed: "+(result->response));
        plugins_result = false;;
      } else if(result->action == ContinuationPlugins::act_log) {
        // Scream but go ahead
        logger.msg(Arc::WARNING,"%s: Plugin at state %s : %s",
            i->job_id.c_str(),i->get_state_name(),
            result->response);
      } else if(result->action == ContinuationPlugins::act_pass) {
        // Just continue quietly
      } else {
        logger.msg(Arc::ERROR,"%s: Plugin execution failed",i->job_id);
        i->AddFailure(std::string("Failed running plugin at state ")+
            i->get_state_name());
        plugins_result = false;;
      }
      ++result;
    }
  }
  return plugins_result;
}


bool JobsList::NextJob(GMJobRef i, job_state_t old_state, bool old_pending) {
  bool at_limit = RunningJobsLimitReached();
  // update counters
  if(!old_pending) {
    jobs_num[old_state]--;
  } else {
    jobs_pending--;
  }
  if(!i->job_pending) {
    jobs_num[i->job_state]++;
  } else {
    jobs_pending++;
  }
  if(at_limit && !RunningJobsLimitReached()) {
    // Report about change in conditions
logger.msg(Arc::ERROR, "++++ NextJob: changed: %s", i->job_id);
    RequestAttention();
  };
  return i;
}

bool JobsList::DropJob(GMJobRef& i, job_state_t old_state, bool old_pending) {
  bool at_limit = RunningJobsLimitReached();
  // update counters
  if(!old_pending) {
    jobs_num[old_state]--;
  } else {
    jobs_pending--;
  }
  if(at_limit && !RunningJobsLimitReached()) {
    // Report about change in conditions
logger.msg(Arc::ERROR, "++++ DropJob: changed: %s", i->job_id);
    RequestAttention();
  };
  jobs.erase(i->job_id);
  i.Destroy();
  return true;
}

#define IS_ACTIVE_STATE(state) ((state >= JOB_STATE_PREPARING) && (state <= JOB_STATE_FINISHING))

bool JobsList::ActJob(GMJobRef& i) {
  Arc::JobPerfRecord perfrecord(*config.GetJobPerfLog(), i->job_id);
  job_state_t perflog_start_state = i->job_state;

  job_state_t old_state = i->job_state;
  job_state_t old_reported_state = i->job_state;
  bool old_pending = i->job_pending;

  ActJobResult job_result = JobSuccess;
  if(!CheckJobCancelRequest(i)) {
    switch(i->job_state) {
      case JOB_STATE_UNDEFINED: {
        job_result = ActJobUndefined(i);
      } break;
      case JOB_STATE_ACCEPTED: {
        job_result = ActJobAccepted(i);
      } break;
      case JOB_STATE_PREPARING: {
        job_result = ActJobPreparing(i);
      } break;
      case JOB_STATE_SUBMITTING: {
        job_result = ActJobSubmitting(i);
      } break;
      case JOB_STATE_CANCELING: {
        job_result = ActJobCanceling(i);
      } break;
      case JOB_STATE_INLRMS: {
        job_result = ActJobInlrms(i);
      } break;
      case JOB_STATE_FINISHING: {
        job_result = ActJobFinishing(i);
      } break;
      case JOB_STATE_FINISHED: {
        job_result = ActJobFinished(i);
      } break;
      case JOB_STATE_DELETED: {
        job_result = ActJobDeleted(i);
      } break;
      default: { // should destroy job with unknown state ?!
      } break;
    };
  };

  if(job_result == JobFailed) {
    // Process errors which happened during processing this job
    job_result = ActJobFailed(i);
    // normally it must not be JobFailed here
  }

  // Process state changes, also those generated by error processing
  if(old_reported_state != i->job_state) {
    if(old_reported_state != JOB_STATE_UNDEFINED) {
      // Report state change into log
      logger.msg(Arc::INFO,"%s: State: %s from %s",
            i->job_id.c_str(),GMJob::get_state_name(i->job_state),
            GMJob::get_state_name(old_reported_state));
    }
    old_reported_state=i->job_state;
  };

  if(old_state != i->job_state) {
    // Job changed state
    i->job_pending=false; //!!!!!!!???????
    if(!job_state_write_file(*i,config,i->job_state)) {
      i->AddFailure("Failed writing job status: "+Arc::StrError(errno));
      job_result = ActJobFailed(i); // immedaitely process failure
    } else {
      // Talk to external plugin to ask if we can proceed
      // Jobs with ACCEPTED state or UNDEFINED previous state
      // could be ignored here. But there is tiny possibility
      // that service failed while processing ContinuationPlugins.
      // Hence here we have duplicate call for ACCEPTED state.
      // TODO: maybe introducing job state prefix VALIDATING:
      // could be used to resolve this situation.
      if(!CheckJobContinuePlugins(i)) {
        // No need for AddFailure. It is filled inside CheckJobContinuePlugins.
        job_result = ActJobFailed(i); // immedaitely process failure
      };
      // Processing to be done on relatively successful state changes
      config.job_log->make_file(*i,config);
      if(i->job_state == JOB_STATE_FINISHED) {
        job_clean_finished(i->job_id,config);
        config.job_log->finish_info(*i,config);
        PrepareCleanupTime(i,i->keep_finished);
      } else if(i->job_state == JOB_STATE_PREPARING) {
        config.job_log->start_info(*i,config);
      };
//            SetJobState(i, JOB_STATE_FINISHED, "Job processing error");
//            SetJobState(i, JOB_STATE_FINISHING, "Job processing error");
    };
    // send mail after error and change are processed
    // do not send if something really wrong happened to avoid email DoS
    if(job_result != JobFailed) send_mail(*i,config);

    // Manage per-DN counter
    // Any job state change goes through here
    if(!IS_ACTIVE_STATE(old_state)) {
      if(IS_ACTIVE_STATE(i->job_state)) {
        if(i->GetLocalDescription(config)) {
          // add to DN map
          if (i->local->DN.empty()) {
             logger.msg(Arc::WARNING, "Failed to get DN information from .local file for job %s", i->job_id);
          }
          ++(jobs_dn[i->local->DN]);
        };
      };
    } else if(IS_ACTIVE_STATE(old_state)) {
      if(!IS_ACTIVE_STATE(i->job_state)) {
        if(i->GetLocalDescription(config)) {
          if (--(jobs_dn[i->local->DN]) == 0) jobs_dn.erase(i->local->DN);
        };
      };
    };
  };

  if(job_result == JobFailed) {
    // If it is still job failed then just forse everything down
    logger.msg(Arc::ERROR,"%s: Delete request due to internal problems",i->job_id);
    SetJobState(i, JOB_STATE_FINISHED, "Job processing failed"); // move to finished in order to remove from list
    i->job_pending=false;
    job_state_write_file(*i,config,i->job_state);
    i->AddFailure("Serious troubles (problems during processing problems)");
    FailedJob(i,false);  // put some marks
    job_clean_finished(i->job_id,config);  // clean status files
    job_result = JobDropped;
  };

  if(perfrecord.Started()) {
    job_state_t perflog_end_state = i->job_state;
    std::string name(GMJob::get_state_name(perflog_start_state));
    name += "-";
    name += GMJob::get_state_name(perflog_end_state);
    perfrecord.End(name);
  };

  // Job in special state or specifically requested to be removed (TODO: remove check for job state)
  if((job_result == JobDropped) ||
     (i->job_state == JOB_STATE_FINISHED) ||
     (i->job_state == JOB_STATE_DELETED) ||
     (i->job_state == JOB_STATE_UNDEFINED)) {
    // Such jobs are not kept in memory
    // this is the ONLY place where jobs are removed from memory
    DropJob(i, old_state, old_pending);
  }
  else {
    NextJob(i, old_state, old_pending);
  }

  return true;
}


JobsList::ActJobResult JobsList::ActJobFailed(GMJobRef i) {
  // Failed job - move it to proper state
  logger.msg(Arc::ERROR,"%s: Job failure detected",i->job_id);
  if(!FailedJob(i,false)) { // something is really wrong
    i->AddFailure("Failed during processing failure");
    return JobFailed;
  } else { // just move job to proper state
    if((i->job_state == JOB_STATE_FINISHED) ||
       (i->job_state == JOB_STATE_DELETED)) {
      // Normally these stages should not generate errors
      // so ignore them
      return JobDropped;
    } else if(i->job_state == JOB_STATE_FINISHING) {
      // No matter if FINISHING fails - it still goes to FINISHED
      SetJobState(i, JOB_STATE_FINISHED, "Job failure detected");
      RequestReprocess(i);
    } else {
      // Other states are moved to FINISHING and start post-staging
      SetJobState(i, JOB_STATE_FINISHING, "Job failure detected");
      RequestReprocess(i);
    };
    // Reset pending (it is useless for any post-failure state anyway)
    i->job_pending=false;
  };
  return JobSuccess;
}

// Description of job status file
class JobFDesc {
 public:
  JobId id;
  uid_t uid;
  gid_t gid;
  time_t t;
  JobFDesc(const std::string& s):id(s),uid(0),gid(0),t(-1) { }
  bool operator<(const JobFDesc &right) const { return (t < right.t); }
};

bool JobsList::RestartJobs(const std::string& cdir,const std::string& odir) {
  bool res = true;
  try {
    Glib::Dir dir(cdir);
    for(;;) {
      std::string file=dir.read_name();
      if(file.empty()) break;
      int l=file.length();
      // job id contains at least 1 character
      if(l>(4+7) && file.substr(0,4) == "job." && file.substr(l-7) == ".status") {
        uid_t uid;
        gid_t gid;
        time_t t;
        std::string fname=cdir+'/'+file.c_str();
        std::string oname=odir+'/'+file.c_str();
        if(check_file_owner(fname,uid,gid,t)) {
          if(::rename(fname.c_str(),oname.c_str()) != 0) {
            logger.msg(Arc::ERROR,"Failed to move file %s to %s",fname,oname);
            res=false;
          }
        }
      }
    }
    dir.close();
  } catch(Glib::FileError& e) {
    logger.msg(Arc::ERROR,"Failed reading control directory: %s",cdir);
    return false;
  }
  return res;
}

// This code is run at service restart
bool JobsList::RestartJobs(void) {
  std::string cdir=config.control_dir;
  // Jobs from old version
  bool res1 = RestartJobs(cdir,cdir+"/"+subdir_rew);
  // Jobs after service restart
  bool res2 = RestartJobs(cdir+"/"+subdir_cur,cdir+"/"+subdir_rew);
  return res1 && res2;
}

bool JobsList::ScanJob(const std::string& cdir, JobFDesc& id) {
  if(!FindJob(id.id)) {
    std::string fname=cdir+'/'+"job."+id.id+".status";
    uid_t uid;
    gid_t gid;
    time_t t;
    if(check_file_owner(fname,uid,gid,t)) {
      id.uid=uid; id.gid=gid; id.t=t;
      return true;
    };
  };
  return false;
}

bool JobsList::ScanJobs(const std::string& cdir,std::list<JobFDesc>& ids) const {
  Arc::JobPerfRecord perfrecord(*config.GetJobPerfLog(), "*");

  try {
    Glib::Dir dir(cdir);
    for(;;) {
      std::string file=dir.read_name();
      if(file.empty()) break;
      int l=file.length();
      // job id contains at least 1 character
      if(l>(4+7) && file.substr(0,4) == "job." && file.substr(l-7) == ".status") {
        JobFDesc id(file.substr(4,l-7-4));
        if(!HasJob(id.id)) {
          std::string fname=cdir+'/'+file.c_str();
          uid_t uid;
          gid_t gid;
          time_t t;
          if(check_file_owner(fname,uid,gid,t)) {
            // add it to the list
            id.uid=uid; id.gid=gid; id.t=t;
            ids.push_back(id);
          }
        }
      }
    }
  } catch(Glib::FileError& e) {
    logger.msg(Arc::ERROR,"Failed reading control directory: %s: %s",config.control_dir, e.what());
    return false;
  }

  perfrecord.End("SCAN-JOBS");
  return true;
}

bool JobsList::ScanMarks(const std::string& cdir,const std::list<std::string>& suffices,std::list<JobFDesc>& ids) {
  Arc::JobPerfRecord perfrecord(*config.GetJobPerfLog(), "*");

  try {
    Glib::Dir dir(cdir);
    for(;;) {
      std::string file=dir.read_name();
      if(file.empty()) break;
      int l=file.length();
      // job id contains at least 1 character
      if(l>(4+7) && file.substr(0,4) == "job.") {
        for(std::list<std::string>::const_iterator sfx = suffices.begin();
            sfx != suffices.end();++sfx) {
          int ll = sfx->length();
          if(l > (ll+4) && file.substr(l-ll) == *sfx) {
            JobFDesc id(file.substr(4,l-ll-4));
            if(!FindJob(id.id)) {
              std::string fname=cdir+'/'+file.c_str();
              uid_t uid;
              gid_t gid;
              time_t t;
              if(check_file_owner(fname,uid,gid,t)) {
                // add it to the list
                id.uid=uid; id.gid=gid; id.t=t;
                ids.push_back(id);
              }
            }
            break;
          }
        }
      }
    }
  } catch(Glib::FileError& e) {
    logger.msg(Arc::ERROR,"Failed reading control directory: %s",config.control_dir);
    return false;
  }

  perfrecord.End("SCAN-MARKS");
  return true;
}

bool JobsList::ScanNewJob(const JobId& id) {
  // New jobs will be accepted only if number of jobs being processed
  if((AcceptedJobs() < config.max_jobs) || (config.max_jobs == -1)) {
    JobFDesc fid(id);
    std::string cdir=config.control_dir;
    std::string ndir=cdir+"/"+subdir_new;
    if(!ScanJob(ndir,fid)) return false;
    return AddJobNoCheck(fid.id,fid.uid,fid.gid);
  }
  return false;
}

bool JobsList::ScanOldJob(const JobId& id) {
  JobFDesc fid(id);
  std::string cdir=config.control_dir;
  std::string ndir=cdir+"/"+subdir_old;
  if(!ScanJob(ndir,fid)) return false;
  job_state_t st = job_state_read_file(id,config);
  if(st == JOB_STATE_FINISHED || st == JOB_STATE_DELETED) {
    return AddJobNoCheck(fid.id,fid.uid,fid.gid,st);
  };
  return false;
}

// find new jobs - sort by date to implement FIFO
bool JobsList::ScanNewJobs(void) {
  Arc::JobPerfRecord perfrecord(*config.GetJobPerfLog(), "*");
  // New jobs will be accepted only if number of jobs being processed
  // does not exceed allowed. So avoid scanning if no jobs will be allowed.
  std::string cdir=config.control_dir;
  if((config.max_jobs == -1) || (AcceptedJobs() < config.max_jobs)) {
    std::list<JobFDesc> ids;
    // For picking up jobs after service restart
    std::string odir=cdir+"/"+subdir_rew;
    if(!ScanJobs(odir,ids)) return false;
    // sorting by date
    ids.sort();
    for(std::list<JobFDesc>::iterator id=ids.begin();id!=ids.end();++id) {
      if((config.max_jobs != -1) && (AcceptedJobs() >= config.max_jobs)) break;
      AddJobNoCheck(id->id,id->uid,id->gid);
    };
  };
  if((config.max_jobs == -1) || (AcceptedJobs() < config.max_jobs)) {
    std::list<JobFDesc> ids;
    // For new jobs
    std::string ndir=cdir+"/"+subdir_new;
    if(!ScanJobs(ndir,ids)) return false;
    // sorting by date
    ids.sort();
    for(std::list<JobFDesc>::iterator id=ids.begin();id!=ids.end();++id) {
      if((config.max_jobs != -1) && (AcceptedJobs() >= config.max_jobs)) break;
      // adding job with file's uid/gid
      AddJobNoCheck(id->id,id->uid,id->gid);
    };
  };
  perfrecord.End("SCAN-JOBS-NEW");
  return true;
}

bool JobsList::ScanNewMarks(void) {
  Arc::JobPerfRecord perfrecord(*config.GetJobPerfLog(), "*");

  std::string cdir=config.control_dir;
  std::string ndir=cdir+"/"+subdir_new;
  std::list<JobFDesc> ids;
  std::list<std::string> sfx;
  sfx.push_back(sfx_clean);
  sfx.push_back(sfx_restart);
  sfx.push_back(sfx_cancel);
  if(!ScanMarks(ndir,sfx,ids)) return false;
  ids.sort();
  std::string last_id;
  for(std::list<JobFDesc>::iterator id=ids.begin();id!=ids.end();++id) {
    if(id->id == last_id) continue; // already processed
    last_id = id->id;
    job_state_t st = job_state_read_file(id->id,config);
    if((st == JOB_STATE_UNDEFINED) || (st == JOB_STATE_DELETED)) {
      // Job probably does not exist anymore
      job_clean_mark_remove(id->id,config);
      job_restart_mark_remove(id->id,config);
      job_cancel_mark_remove(id->id,config);
    }
    // Check if such job finished and add it to list.
    if(st == JOB_STATE_FINISHED) {
      // That will activate its processing at least for one step.
      AddJobNoCheck(id->id,id->uid,id->gid,st);
    }
  }

  perfrecord.End("SCAN-MARKS-NEW");
  return true;
}

// For simply collecting all jobs. Only used by gm-jobs.
bool JobsList::GetAllJobs(std::list<GMJobRef>& alljobs) const {
  std::list<std::string> subdirs;
  subdirs.push_back(std::string("/")+subdir_rew); // For picking up jobs after service restart
  subdirs.push_back(std::string("/")+subdir_new); // For new jobs
  subdirs.push_back(std::string("/")+subdir_cur); // For active jobs
  subdirs.push_back(std::string("/")+subdir_old); // For done jobs
  for(std::list<std::string>::iterator subdir = subdirs.begin();
                               subdir != subdirs.end();++subdir) {
    std::string cdir=config.control_dir;
    std::list<JobFDesc> ids;
    std::string odir=cdir+(*subdir);
    if(!ScanJobs(odir,ids)) return false;
    // sorting by date
    ids.sort();
    for(std::list<JobFDesc>::iterator id=ids.begin();id!=ids.end();++id) {
      GMJobRef i(new GMJob(id->id,Arc::User(id->uid)));
      if (GetLocalDescription(i)) {
        i->session_dir = i->local->sessiondir;
        if (i->session_dir.empty()) i->session_dir = config.SessionRoot(id->id)+'/'+id->id;
        alljobs.push_back(i);
      }
    }
  }
  return true;
}

// For simply collecting all job ids.
bool JobsList::GetAllJobIds(std::list<JobId>& alljobs) const {
  std::list<std::string> subdirs;
  subdirs.push_back(std::string("/")+subdir_rew); // For picking up jobs after service restart
  subdirs.push_back(std::string("/")+subdir_new); // For new jobs
  subdirs.push_back(std::string("/")+subdir_cur); // For active jobs
  subdirs.push_back(std::string("/")+subdir_old); // For done jobs
  for(std::list<std::string>::iterator subdir = subdirs.begin();
                               subdir != subdirs.end();++subdir) {
    std::string cdir=config.control_dir;
    std::list<JobFDesc> ids;
    std::string odir=cdir+(*subdir);
    if(!ScanJobs(odir,ids)) return false;
    // sorting by date
    ids.sort();
    for(std::list<JobFDesc>::iterator id=ids.begin();id!=ids.end();++id) {
      alljobs.push_back(id->id);
    }
  }
  return true;
}
// Only used by gm-jobs
GMJobRef JobsList::GetJob(const JobId& id) const {
  std::list<std::string> subdirs;
  subdirs.push_back(std::string("/")+subdir_rew); // For picking up jobs after service restart
  subdirs.push_back(std::string("/")+subdir_new); // For new jobs
  subdirs.push_back(std::string("/")+subdir_cur); // For active jobs
  subdirs.push_back(std::string("/")+subdir_old); // For done jobs
  for(std::list<std::string>::iterator subdir = subdirs.begin();
                               subdir != subdirs.end();++subdir) {
    std::string cdir=config.control_dir;
    std::string odir=cdir+(*subdir);
    std::string fname=odir+'/'+"job."+id+".status";
    uid_t uid;
    gid_t gid;
    time_t t;
    if(check_file_owner(fname,uid,gid,t)) {
      GMJobRef i(new GMJob(id,Arc::User(uid)));
      if (GetLocalDescription(i)) {
        i->session_dir = i->local->sessiondir;
        if (i->session_dir.empty()) i->session_dir = config.SessionRoot(id)+'/'+id;
        return i;
      }
    }
  }
  return GMJobRef();
}

// For simply counting all jobs.
int JobsList::CountAllJobs() const {
  int count;
  std::list<std::string> subdirs;
  subdirs.push_back(std::string("/")+subdir_rew); // For picking up jobs after service restart
  subdirs.push_back(std::string("/")+subdir_new); // For new jobs
  subdirs.push_back(std::string("/")+subdir_cur); // For active jobs
  subdirs.push_back(std::string("/")+subdir_old); // For done jobs
  for(std::list<std::string>::iterator subdir = subdirs.begin();
                               subdir != subdirs.end();++subdir) {
    std::string cdir=config.control_dir;
    std::list<JobFDesc> ids;
    std::string odir=cdir+(*subdir);
    if(ScanJobs(odir,ids)) {
      count += ids.size();
    };
  };
  return count;
}

JobsList::ExternalHelper::ExternalHelper(const std::string &cmd) {
  command = cmd;
  proc = NULL;
}

JobsList::ExternalHelper::~ExternalHelper() {
  if(proc != NULL) {
    delete proc;
    proc=NULL;
  }
}

static void ExternalHelperInitializer(void* arg) {
  const char* logpath = reinterpret_cast<const char*>(arg);
  // just set good umask
  umask(0077);
  // set up stdin,stdout and stderr
  int h;
  h = ::open("/dev/null",O_RDONLY);
  if(h != 0) { if(dup2(h,0) != 0) { sleep(10); _exit(1); }; close(h); };
  h = ::open("/dev/null",O_WRONLY);
  if(h != 1) { if(dup2(h,1) != 1) { sleep(10); _exit(1); }; close(h); };
  if(logpath && logpath[0]) {
    h = ::open(logpath,O_WRONLY | O_CREAT | O_APPEND,S_IRUSR | S_IWUSR);
    if(h==-1) { h = ::open("/dev/null",O_WRONLY); };
  }
  else { h = ::open("/dev/null",O_WRONLY); };
  if(h != 2) { if(dup2(h,2) != 2) { sleep(10); exit(1); }; close(h); };
}

static void ExternalHelperKicker(void* arg) {
  JobsList* jobs = reinterpret_cast<JobsList*>(arg);
  if(jobs) jobs->RequestAttention();
}

bool JobsList::ExternalHelper::run(JobsList& jobs) {
  if (proc != NULL) {
    if (proc->Running()) {
      return true; // it is already/still running
    }
    delete proc;
    proc = NULL;
  }
  // start/restart
  if (command.empty()) return true;  // has anything to run ?
  logger.msg(Arc::VERBOSE, "Starting helper process: %s", command);

  proc = new Arc::Run(command);
  proc->KeepStdin(true);
  proc->KeepStdout(true);
  proc->KeepStderr(true);
  proc->AssignInitializer(&ExternalHelperInitializer, const_cast<char*>(jobs.config.HelperLog().c_str()));
  proc->AssignKicker(&ExternalHelperKicker, &jobs);
  if (proc->Start()) return true;
  delete proc;
  logger.msg(Arc::ERROR, "Helper process start failed: %s", command);
  // start failed, doing nothing - maybe in the future
  return false;
}

void JobsList::ExternalHelper::stop() {
  if (proc && proc->Running()) {
    logger.msg(Arc::VERBOSE, "Stopping helper process %s", command);
    proc->Kill(1);
  }
}

bool JobsList::RunHelpers() {
  bool started = true;
  for (std::list<ExternalHelper>::iterator i = helpers.begin(); i != helpers.end(); ++i) {
    started &= i->run(*this);
  }
  return started;
}


} // namespace ARex
