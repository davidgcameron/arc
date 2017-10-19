// -*- indent-tabs-mode: nil -*-

#ifdef HAVE_CONFIG_H
#include <config.h>
#endif

#include <string>
#include <sstream>

#include <sys/stat.h>

#include <glibmm.h>

#include <arc/CheckSum.h>
#include <arc/StringConv.h>
#include <arc/UserConfig.h>
#include <arc/compute/ExecutionTarget.h>
#include <arc/compute/Job.h>
#include <arc/compute/JobDescription.h>
#include <arc/compute/SubmissionStatus.h>
#include <arc/message/MCC.h>

#include "SubmitterPluginARC1.h"
#include "AREXClient.h"

namespace Arc {

  Logger SubmitterPluginARC1::logger(Logger::getRootLogger(), "SubmitterPlugin.ARC1");

  void SubmitterPluginARC1::SetUserConfig(const UserConfig& uc) {
    SubmitterPlugin::SetUserConfig(uc);
    clients.SetUserConfig(uc);
  }

  bool SubmitterPluginARC1::isEndpointNotSupported(const std::string& endpoint) const {
    const std::string::size_type pos = endpoint.find("://");
    return pos != std::string::npos && lower(endpoint.substr(0, pos)) != "http" && lower(endpoint.substr(0, pos)) != "https";
  }

  SubmissionStatus SubmitterPluginARC1::Submit(const std::list<JobDescription>& jobdescs, const std::string& endpoint, EntityConsumer<Job>& jc, std::list<const JobDescription*>& notSubmitted) {
    URL url((endpoint.find("://") == std::string::npos ? "https://" : "") + endpoint, false, 443, "/arex");

    // TODO: Determine extended BES interface interface (A-REX WS)
    bool arex_features = true; //et.ComputingService->Type == "org.nordugrid.execution.arex";

    AutoPointer<AREXClient> ac(clients.acquire(url, arex_features));

    SubmissionStatus retval;
    for (std::list<JobDescription>::const_iterator it = jobdescs.begin(); it != jobdescs.end(); ++it) {
      JobDescription preparedjobdesc(*it);
  
      if (!preparedjobdesc.Prepare()) {
        logger.msg(INFO, "Failed to prepare job description");
        notSubmitted.push_back(&*it);
        retval |= SubmissionStatus::DESCRIPTION_NOT_SUBMITTED;
        continue;
      }

      // !! TODO: For regular BES ordinary JSDL is needed - keeping nordugrid:jsdl so far
      std::string product;
      JobDescriptionResult ures = preparedjobdesc.UnParse(product, "nordugrid:jsdl");
      if (!ures) {
        logger.msg(INFO, "Unable to submit job. Job description is not valid in the %s format: %s", "nordugrid:jsdl", ures.str());
        notSubmitted.push_back(&*it);
        retval |= SubmissionStatus::DESCRIPTION_NOT_SUBMITTED;
        continue;
      }

      std::string idFromEndpoint;
      if (!ac->submit(product, idFromEndpoint, arex_features && (url.Protocol() == "https"))) {
        notSubmitted.push_back(&*it);
        retval |= SubmissionStatus::DESCRIPTION_NOT_SUBMITTED;
        retval |= SubmissionStatus::ERROR_FROM_ENDPOINT;
        continue;
      }
  
      if (idFromEndpoint.empty()) {
        logger.msg(INFO, "No job identifier returned by BES service");
        notSubmitted.push_back(&*it);
        retval |= SubmissionStatus::DESCRIPTION_NOT_SUBMITTED;
        retval |= SubmissionStatus::ERROR_FROM_ENDPOINT;
        continue;
      }
  
      XMLNode activityIdentifier(idFromEndpoint);
      URL jobid;
      if (activityIdentifier["ReferenceParameters"]["a-rex:JobID"]) { // Service seems to be A-REX. Extract job ID, and upload files.
        jobid = URL((std::string)(activityIdentifier["ReferenceParameters"]["JobSessionDir"]));
        // compensate for time between request and response on slow networks
        URL sessionurl = jobid;
        sessionurl.AddOption("threads=3",false);
        sessionurl.AddOption("encryption=optional",false);
        if(arex_features) {
          sessionurl.AddOption("httpputpartial=yes",false);
          sessionurl.AddOption("blocksize=5242880",true);
        }
    
        if (!PutFiles(preparedjobdesc, sessionurl)) {
          logger.msg(INFO, "Failed uploading local input files");
          notSubmitted.push_back(&*it);
          retval |= SubmissionStatus::DESCRIPTION_NOT_SUBMITTED;
          retval |= SubmissionStatus::ERROR_FROM_ENDPOINT;
          continue;
        }
      } else {
        if (activityIdentifier["Address"]) {
          jobid = URL((std::string)activityIdentifier["Address"]);
        } else {
          jobid = url;
        }
        Time t;
        // Since BES doesn't specify a simple unique ID, but rather an EPR, a unique non-reproduceable (to arcsync) job ID is created below.
        jobid.ChangePath(jobid.Path() + "/BES" + tostring(t.GetTime()) + tostring(t.GetTimeNanoseconds()));
      }
    
      Job j;
      
      AddJobDetails(preparedjobdesc, j);
      
      // Proposed mandatory attributes for ARC 3.0
      j.JobID = jobid.fullstr();
      j.ServiceInformationURL = url;
      j.ServiceInformationInterfaceName = "org.nordugrid.wsrfglue2";
      j.JobStatusURL = url;
      j.JobStatusInterfaceName = "org.nordugrid.xbes";
      j.JobManagementURL = url;
      j.JobManagementInterfaceName = "org.nordugrid.xbes";
      j.IDFromEndpoint = (std::string)activityIdentifier["ReferenceParameters"]["a-rex:JobID"];
      
      jc.addEntity(j);
    }
  
    clients.release(ac.Release());
    return retval;
  }

  SubmissionStatus SubmitterPluginARC1::Submit(const std::list<JobDescription>& jobdescs, const ExecutionTarget& et, EntityConsumer<Job>& jc, std::list<const JobDescription*>& notSubmitted) {
    URL url(et.ComputingEndpoint->URLString);
    bool arex_features = (et.ComputingService->Type == "org.nordugrid.execution.arex") ||
                         (et.ComputingService->Type == "org.nordugrid.arex");
    AutoPointer<AREXClient> ac(clients.acquire(url, arex_features));

    SubmissionStatus retval;
    for (std::list<JobDescription>::const_iterator it = jobdescs.begin(); it != jobdescs.end(); ++it) {
      JobDescription preparedjobdesc(*it);
  
      if (arex_features && !preparedjobdesc.Prepare(et)) {
        logger.msg(INFO, "Failed to prepare job description to target resources");
        notSubmitted.push_back(&*it);
        retval |= SubmissionStatus::DESCRIPTION_NOT_SUBMITTED;
        continue;
      }
  
      // !! TODO: For regular BES ordinary JSDL is needed - keeping nordugrid:jsdl so far
      std::string product;
      JobDescriptionResult ures = preparedjobdesc.UnParse(product, "nordugrid:jsdl");
      if (!ures) {
        logger.msg(INFO, "Unable to submit job. Job description is not valid in the %s format: %s", "nordugrid:jsdl", ures.str());
        notSubmitted.push_back(&*it);
        retval |= SubmissionStatus::DESCRIPTION_NOT_SUBMITTED;
        continue;
      }

      std::string idFromEndpoint;
      if (!ac->submit(product, idFromEndpoint, arex_features && (url.Protocol() == "https"))) {
        notSubmitted.push_back(&*it);
        retval |= SubmissionStatus::DESCRIPTION_NOT_SUBMITTED;
        retval |= SubmissionStatus::ERROR_FROM_ENDPOINT;
        continue;
      }
  
      if (idFromEndpoint.empty()) {
        logger.msg(INFO, "No job identifier returned by BES service");
        notSubmitted.push_back(&*it);
        retval |= SubmissionStatus::DESCRIPTION_NOT_SUBMITTED;
        retval |= SubmissionStatus::ERROR_FROM_ENDPOINT;
        continue;
      }
  
      XMLNode activityIdentifier(idFromEndpoint);
      URL jobid;
      if (activityIdentifier["ReferenceParameters"]["a-rex:JobID"]) { // Service seems to be A-REX. Extract job ID, and upload files.
        jobid = URL((std::string)(activityIdentifier["ReferenceParameters"]["JobSessionDir"]));
        // compensate for time between request and response on slow networks
        URL sessionurl = jobid;
        sessionurl.AddOption("threads=3",false);
        sessionurl.AddOption("encryption=optional",false);
        if(arex_features) {
          sessionurl.AddOption("httpputpartial=yes",false);
          sessionurl.AddOption("blocksize=5242880",true);
        }
    
        if (!PutFiles(preparedjobdesc, sessionurl)) {
          logger.msg(INFO, "Failed uploading local input files");
          notSubmitted.push_back(&*it);
          retval |= SubmissionStatus::DESCRIPTION_NOT_SUBMITTED;
          retval |= SubmissionStatus::ERROR_FROM_ENDPOINT;
          continue;
        }
      } else {
        if (activityIdentifier["Address"]) {
          jobid = URL((std::string)activityIdentifier["Address"]);
        } else {
          jobid = url;
        }
        Time t;
        // Since BES doesn't specify a simple unique ID, but rather an EPR, a unique non-reproduceable (to arcsync) job ID is created below.
        jobid.ChangePath(jobid.Path() + "/BES" + tostring(t.GetTime()) + tostring(t.GetTimeNanoseconds()));
      }
    
      Job j;
      AddJobDetails(preparedjobdesc, j);
      
      // Proposed mandatory attributes for ARC 3.0
      j.JobID = jobid.fullstr();
      j.ServiceInformationURL = url;
      j.ServiceInformationInterfaceName = "org.nordugrid.wsrfglue2";
      j.JobStatusURL = url;
      j.JobStatusInterfaceName = "org.nordugrid.xbes";
      j.JobManagementURL = url;
      j.JobManagementInterfaceName = "org.nordugrid.xbes";
      j.IDFromEndpoint = (std::string)activityIdentifier["ReferenceParameters"]["a-rex:JobID"];
      j.StageInDir = jobid;
      j.StageOutDir = jobid;
      j.SessionDir = jobid;
      
      jc.addEntity(j);
    }
  
    clients.release(ac.Release());
    return retval;
  }

  bool SubmitterPluginARC1::Migrate(const std::string& jobid, const JobDescription& jobdesc,
                             const ExecutionTarget& et,
                             bool forcemigration, Job& job) {
    URL url(et.ComputingEndpoint->URLString);

    AutoPointer<AREXClient> ac(clients.acquire(url,true));

    std::string idstr;
    AREXClient::createActivityIdentifier(jobid, idstr);

    JobDescription preparedjobdesc(jobdesc);

    // Modify the location of local files and files residing in a old session directory.
    for (std::list<InputFileType>::iterator it = preparedjobdesc.DataStaging.InputFiles.begin();
         it != preparedjobdesc.DataStaging.InputFiles.end(); it++) {
      if (!it->Sources.front() || it->Sources.front().Protocol() == "file") {
        it->Sources.front() = URL(jobid + "/" + it->Name);
      }
      else {
        // URL is valid, and not a local file. Check if the source reside at a
        // old job session directory.
        const size_t foundRSlash = it->Sources.front().str().rfind('/');
        if (foundRSlash == std::string::npos) continue;

        const std::string uriPath = it->Sources.front().str().substr(0, foundRSlash);
        // Check if the input file URI is pointing to a old job session directory.
        for (std::list<std::string>::const_iterator itAOID = preparedjobdesc.Identification.ActivityOldID.begin();
             itAOID != preparedjobdesc.Identification.ActivityOldID.end(); itAOID++) {
          if (uriPath == *itAOID) {
            it->Sources.front() = URL(jobid + "/" + it->Name);
            break;
          }
        }
      }
    }

    if (!preparedjobdesc.Prepare(et)) {
      logger.msg(INFO, "Failed adapting job description to target resources");
      clients.release(ac.Release());
      return false;
    }

    // Add ActivityOldID.
    preparedjobdesc.Identification.ActivityOldID.push_back(jobid);

    std::string product;
    JobDescriptionResult ures = preparedjobdesc.UnParse(product, "nordugrid:jsdl");
    if (!ures) {
      logger.msg(INFO, "Unable to migrate job. Job description is not valid in the %s format: %s", "nordugrid:jsdl", ures.str());
      clients.release(ac.Release());
      return false;
    }

    std::string sNewjobid;
    if (!ac->migrate(idstr, product, forcemigration, sNewjobid,
                    url.Protocol() == "https")) {
      clients.release(ac.Release());
      return false;
    }

    if (sNewjobid.empty()) {
      logger.msg(INFO, "No job identifier returned by A-REX");
      clients.release(ac.Release());
      return false;
    }

    XMLNode xNewjobid(sNewjobid);
    URL newjobid((std::string)(xNewjobid["ReferenceParameters"]["JobSessionDir"]));

    URL sessionurl = newjobid;
    sessionurl.AddOption("threads=3",false);
    sessionurl.AddOption("encryption=optional",false);
    sessionurl.AddOption("httpputpartial=yes",false); // for A-REX
    sessionurl.AddOption("blocksize=5242880",true);

    if (!PutFiles(preparedjobdesc, sessionurl)) {
      logger.msg(INFO, "Failed uploading local input files");
      clients.release(ac.Release());
      return false;
    }

    AddJobDetails(preparedjobdesc, job);
    
    // Proposed mandatory attributes for ARC 3.0
    job.JobID = newjobid;
    job.ServiceInformationURL = url;
    job.ServiceInformationInterfaceName = "org.nordugrid.wsrfglue2";
    job.JobStatusURL = url;
    job.JobStatusInterfaceName = "org.nordugrid.xbes";
    job.JobManagementURL = url;
    job.JobManagementInterfaceName = "org.nordugrid.xbes";
    job.IDFromEndpoint = (std::string)xNewjobid["ReferenceParameters"]["a-rex:JobID"];
    job.StageInDir = sessionurl;
    job.StageOutDir = sessionurl;
    job.SessionDir = sessionurl;

    clients.release(ac.Release());
    return true;
  }
} // namespace Arc
