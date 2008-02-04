#ifdef HAVE_CONFIG_H
#include <config.h>
#endif


#include "job.h"
#include "grid_sched.h"

#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>

#include <arc/StringConv.h>

namespace GridScheduler {

Arc::MCC_Status GridSchedulerService::CreateActivity(Arc::XMLNode& in,Arc::XMLNode& out) {
  /*
  CreateActivity
    ActivityDocument
      jsdl:JobDefinition

  CreateActivityResponse
    ActivityIdentifier (wsa:EndpointReferenceType)
    ActivityDocument
      jsdl:JobDefinition

  InvalidRequestMessageFault
    InvalidElement
    Message
  */
  {
    std::string s;
    in.GetXML(s);
    logger_.msg(Arc::DEBUG, "CreateActivity: request = \n%s", s.c_str());
  };
  Arc::XMLNode jsdl = in["ActivityDocument"]["JobDefinition"];
  if(!jsdl) {
    // Wrongly formated request
    logger_.msg(Arc::ERROR, "CreateActivity: no job description found");
    Arc::SOAPEnvelope fault(ns_,true);
    if(fault) {
      fault.Fault()->Code(Arc::SOAPFault::Sender);
      fault.Fault()->Reason("Can't find JobDefinition element in request");
      Arc::XMLNode f = fault.Fault()->Detail(true).NewChild("bes-factory:InvalidRequestMessageFault");
      f.NewChild("bes-factory:InvalidElement")="jsdl:JobDefinition";
      f.NewChild("bes-factory:Message")="Element is missing";
      out.Replace(fault.Child());
    };
    return Arc::MCC_Status();
  };
  std::string delegation;
  Arc::XMLNode delegated_token = in["deleg:DelegatedToken"];
  if(delegated_token) {
    // Client wants to delegate credentials
    if(!delegations_.DelegatedToken(delegation,delegated_token)) {
      // Failed to accept delegation (generic SOAP error)
      Arc::SOAPEnvelope fault(ns_,true);
      if(fault) {
        fault.Fault()->Code(Arc::SOAPFault::Receiver);
        fault.Fault()->Reason("Failed to accept delegation");
        out.Replace(fault.Child());
      };
      return Arc::MCC_Status();
    };
  };
 
  int reruns = 10;

  JobDescription job_desc(jsdl);
  JobSchedMetaData sched_meta(reruns);
  Job sched_job(job_desc, sched_meta);

  if(!job_desc) {
    std::string failure = sched_job.Failure();
    logger_.msg(Arc::ERROR, "CreateActivity: Failed to create new job: %s",failure.c_str());
    // Failed to create new job (generic SOAP error)
    Arc::SOAPEnvelope fault(ns_,true);
    if(fault) {
      fault.Fault()->Code(Arc::SOAPFault::Receiver);
      fault.Fault()->Reason(("Can't create new activity: "+failure).c_str());
      out.Replace(fault.Child());
    };
    return Arc::MCC_Status();
  };

  // Make SOAP response
  Arc::WSAEndpointReference identifier(out.NewChild("bes-factory:ActivityIdentifier"));
  // Make job's ID

  std::string endpoint = "https://localhost:60000/sched";

  identifier.Address(endpoint); // address of service
  identifier.ReferenceParameters().NewChild("sched:JobID")=sched_job.getID();
  identifier.ReferenceParameters().NewChild("sched:JobSessionDir")=endpoint+"/" + sched_job.getID();
  out.NewChild(in["ActivityDocument"]);
  logger_.msg(Arc::DEBUG, "CreateActivity finished successfully");
  {
    std::string s;
    out.GetXML(s);
    logger_.msg(Arc::DEBUG, "CreateActivity: response = \n%s", s.c_str());
  };

  sched_job.setStatus(PENDING);
  sched_queue.addJob(sched_job);

  return Arc::MCC_Status(Arc::STATUS_OK);
}

} // namespace GridScheduler


