#ifdef HAVE_CONFIG_H
#include <config.h>
#endif

#include <arc/message/SOAPEnvelope.h>
#include <arc/ws-addressing/WSA.h>
#include "grid-manager/jobs/job_config.h"
#include "job.h"

#include "arex.h"

namespace ARex {

// TODO: configurable
#define MAX_ACTIVITIES (10)

static bool max_jobs_reached(const JobsListConfig& jobs_cfg, unsigned int all_jobs) {
  // Apply limit on total number of jobs.
  //  Using collected glue states to evaluate number of jobs. - Not anymore
//!!  glue_states_lock_.lock();
//!!  int jobs_total = glue_states_.size();
//!!  glue_states_lock_.unlock();
  int jobs_total = all_jobs;
  int max_active;
  int max_running;
  int max_per_dn;
  int max_total;
  jobs_cfg.GetMaxJobs(max_active,max_running,max_per_dn,max_total);
  if(max_total > 0 && jobs_total >= max_total) return true;
  return false;
};
 

Arc::MCC_Status ARexService::CreateActivity(ARexGMConfig& config,Arc::XMLNode in,Arc::XMLNode out,const std::string& clientid) {
  /*
  CreateActivity
    ActivityDocument
      jsdl:JobDefinition

  CreateActivityResponse
    ActivityIdentifier (wsa:EndpointReferenceType)
    ActivityDocument
      jsdl:JobDefinition

  NotAuthorizedFault
  NotAcceptingNewActivitiesFault
  UnsupportedFeatureFault
  InvalidRequestMessageFault
  */
  if(Arc::VERBOSE >= logger_.getThreshold()) {
    std::string s;
    in.GetXML(s);
    logger_.msg(Arc::VERBOSE, "CreateActivity: request = \n%s", s);
  };
  Arc::XMLNode jsdl = in["ActivityDocument"]["JobDefinition"];
  if(!jsdl) {
    // Wrongly formated request
    logger_.msg(Arc::ERROR, "CreateActivity: no job description found");
    Arc::SOAPFault fault(out.Parent(),Arc::SOAPFault::Sender,"Can't find JobDefinition element in request");
    InvalidRequestMessageFault(fault,"jsdl:JobDefinition","Element is missing");
    out.Destroy();
    return Arc::MCC_Status();
  };

  if(max_jobs_reached(*jobs_cfg_,all_jobs_count_)) {
    logger_.msg(Arc::ERROR, "CreateActivity: max jobs total limit reached");
    Arc::SOAPFault fault(out.Parent(),Arc::SOAPFault::Sender,"Reached limit of total allowed jobs");
    GenericFault(fault);
    out.Destroy();
    return Arc::MCC_Status();
  };

  // HPC Basic Profile 1.0 comply (these fault handlings are defined in the KnowARC standards 
  // conformance roadmap 2nd release)

 // End of the HPC BP 1.0 fault handling part

  std::string delegation;
  Arc::XMLNode delegated_token = in["arcdeleg:DelegatedToken"];
  if(delegated_token) {
    // Client wants to delegate credentials
    if(!delegation_stores_.DelegatedToken(config.User()->DelegationDir(),delegated_token,config.GridName(),delegation)) {
      // Failed to accept delegation (report as bad request)
      logger_.msg(Arc::ERROR, "CreateActivity: Failed to accept delegation");
      Arc::SOAPFault fault(out.Parent(),Arc::SOAPFault::Sender,"Failed to accept delegation");
      InvalidRequestMessageFault(fault,"arcdeleg:DelegatedToken","This token does not exist");
      out.Destroy();
      return Arc::MCC_Status();
    };
  };
  JobIDGeneratorARC idgenerator(config.Endpoint());
  ARexJob job(jsdl,config,delegation,clientid,logger_,idgenerator);
  if(!job) {
    ARexJobFailure failure_type = job;
    std::string failure = job.Failure();
    switch(failure_type) {
      case ARexJobDescriptionUnsupportedError: {
        Arc::SOAPFault fault(out.Parent(),Arc::SOAPFault::Sender,"Unsupported feature in job description");
        UnsupportedFeatureFault(fault,failure);
      }; break;
      case ARexJobDescriptionMissingError: {
        Arc::SOAPFault fault(out.Parent(),Arc::SOAPFault::Sender,"Missing needed element in job description");
        UnsupportedFeatureFault(fault,failure);
      }; break;
      case ARexJobDescriptionLogicalError: {
        std::string element;
        std::string::size_type pos = failure.find(' ');
        if(pos != std::string::npos) {
          element=failure.substr(0,pos);
          failure=failure.substr(pos+1);
        };
        Arc::SOAPFault fault(out.Parent(),Arc::SOAPFault::Sender,"Logical error in job description");
        InvalidRequestMessageFault(fault,element,failure);
      }; break;
      default: {
        logger_.msg(Arc::ERROR, "CreateActivity: Failed to create new job: %s",failure);
        // Failed to create new job (no corresponding BES fault defined - using generic SOAP error)
        logger_.msg(Arc::ERROR, "CreateActivity: Failed to create new job");
        Arc::SOAPFault fault(out.Parent(),Arc::SOAPFault::Sender,("Failed to create new activity: "+failure).c_str());
        GenericFault(fault);
      };
    };
    out.Destroy();
    return Arc::MCC_Status();
  };
  // Make SOAP response
  Arc::WSAEndpointReference identifier(out.NewChild("bes-factory:ActivityIdentifier"));
  // Make job's ID
  identifier.Address(config.Endpoint()); // address of service
  identifier.ReferenceParameters().NewChild("a-rex:JobID")=job.ID();
  identifier.ReferenceParameters().NewChild("a-rex:JobSessionDir")=config.Endpoint()+"/"+job.ID();
  out.NewChild(in["ActivityDocument"]);
  logger_.msg(Arc::VERBOSE, "CreateActivity finished successfully");
  if(Arc::VERBOSE >= logger_.getThreshold()) {
    std::string s;
    out.GetXML(s);
    logger_.msg(Arc::VERBOSE, "CreateActivity: response = \n%s", s);
  };
  return Arc::MCC_Status(Arc::STATUS_OK);
}

Arc::MCC_Status ARexService::ESCreateActivities(ARexGMConfig& config,Arc::XMLNode in,Arc::XMLNode out,const std::string& clientid) {
  /*
    CreateActivity
      adl:ActivityDescription - http://www.eu-emi.eu/es/2010/12/adl 1-unbounded

    CreateActivityResponse
      ActivityCreationResponse 1-
        types:ActivityID
        types:ActivityMgmtEndpointURL (anyURI)
        types:ResourceInfoEndpointURL (anyURI)
        types:ActivityStatus
        ETNSC (dateTime) 0-1
        StageInDirectory 0-1
          URL 1-
        SessionDirectory 0-1
          URL 1-
        StageOutDirectory 0-1
          URL 1-
        - or -
        types:InternalBaseFault
        types:AccessControlFault
        InvalidActivityDescriptionFault
        InvalidActivityDescriptionSemanticFault
        UnsupportedCapabilityFault

    types:VectorLimitExceededFault
    types:InternalBaseFault
    types:AccessControlFault
  */

  if(Arc::VERBOSE >= logger_.getThreshold()) {
    std::string s;
    in.GetXML(s);
    logger_.msg(Arc::VERBOSE, "EMIES:CreateActivity: request = \n%s", s);
  };
  Arc::XMLNode adl = in["ActivityDescription"];
  unsigned int n = 0;
  for(;(bool)adl;++adl) {
    if((++n) > MAX_ACTIVITIES) {
      logger_.msg(Arc::ERROR, "EMIES:CreateActivity: too many activity descriptions");
      ESVectorLimitExceededFault(Arc::SOAPFault(out.Parent(),Arc::SOAPFault::Sender,"Too many activity descriptions"),MAX_ACTIVITIES,"Too many activity descriptions");
      out.Destroy();
      return Arc::MCC_Status(Arc::STATUS_OK);
    };
  };
  adl = in["ActivityDescription"];
  if(!adl) {
    // Wrongly formated request
    logger_.msg(Arc::ERROR, "EMIES:CreateActivity: no job description found");
    Arc::SOAPFault fault(out.Parent(),Arc::SOAPFault::Sender,"ActivityDescription element is missing");
    ESInternalBaseFault(fault,"ActivityDescription element is missing");
    out.Destroy();
    return Arc::MCC_Status();
  };
  if(max_jobs_reached(*jobs_cfg_,all_jobs_count_)) {
    logger_.msg(Arc::ERROR, "EMIES:CreateActivity: max jobs total limit reached");
    Arc::SOAPFault fault(out.Parent(),Arc::SOAPFault::Sender,"Reached limit of total allowed jobs");
    ESInternalBaseFault(fault,"Reached limit of total allowed jobs");
    out.Destroy();
    return Arc::MCC_Status();
  };
  for(;(bool)adl;++adl) {
    JobIDGeneratorES idgenerator(config.Endpoint());
    ARexJob job(adl,config,"",clientid,logger_,idgenerator);
    // Make SOAP response
    Arc::XMLNode resp = out.NewChild("escreate:ActivityCreationResponse");
    if(!job) {
      Arc::XMLNode fault = resp.NewChild("dummy");
      ARexJobFailure failure_type = job;
      std::string failure = job.Failure();
      switch(failure_type) {
        case ARexJobDescriptionUnsupportedError: {
          ESUnsupportedCapabilityFault(fault,failure);
        }; break;
        case ARexJobDescriptionMissingError: {
          ESInvalidActivityDescriptionSemanticFault(fault,failure);
        }; break;
        case ARexJobDescriptionLogicalError: {
          ESInvalidActivityDescriptionFault(fault,failure);
        }; break;
        default: {
          logger_.msg(Arc::ERROR, "ES:CreateActivity: Failed to create new job: %s",failure);
          ESInternalBaseFault(fault,"Failed to create new activity. "+failure);
        };
      };
    } else {
      resp.NewChild("estypes:ActivityID")=job.ID();
      resp.NewChild("estypes:ActivityMgmtEndpointURL")=config.Endpoint();
      resp.NewChild("estypes:ResourceInfoEndpointURL")=config.Endpoint();
      Arc::XMLNode rstatus = addActivityStatusES(resp,"ACCEPTED",Arc::XMLNode(),false,false);
      //resp.NewChild("escreate:ETNSC");
      resp.NewChild("escreate:StageInDirectory").NewChild("escreate:URL")=config.Endpoint()+"/"+job.ID();
      resp.NewChild("escreate:SessionDirectory").NewChild("escreate:URL")=config.Endpoint()+"/"+job.ID();
      resp.NewChild("escreate:StageOutDirectory").NewChild("escreate:URL")=config.Endpoint()+"/"+job.ID();
      // TODO: move into addActivityStatusES()
      rstatus.NewChild("estypes:Timestamp")=Arc::Time().str(Arc::ISOTime);
      //rstatus.NewChild("estypes:Description")=;
      logger_.msg(Arc::VERBOSE, "EMIES:CreateActivity finished successfully");
      if(Arc::VERBOSE >= logger_.getThreshold()) {
        std::string s;
        out.GetXML(s);
        logger_.msg(Arc::VERBOSE, "EMIES:CreateActivity: response = \n%s", s);
      }; 
    };
  };
  return Arc::MCC_Status(Arc::STATUS_OK);
}

} // namespace ARex

