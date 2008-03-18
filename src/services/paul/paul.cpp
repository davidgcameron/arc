#ifdef HAVE_CONFIG_H
#include <config.h>
#endif

#include <glibmm.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>
#include <fstream>
#include <iostream>
#include <unistd.h>

#include <arc/URL.h>
#include <arc/loader/Loader.h>
#include <arc/loader/ServiceLoader.h>
#include <arc/loader/Plexer.h>
#include <arc/message/PayloadSOAP.h>
#include <arc/message/PayloadRaw.h>
#include <arc/message/PayloadStream.h>
#include <arc/ws-addressing/WSA.h>
#include <arc/Thread.h>
#include <arc/StringConv.h>
#include <arc/misc/ClientInterface.h>

#include "paul.h"

namespace Paul {

// Static initializator
static Arc::Service* get_service(Arc::Config *cfg,Arc::ChainContext*) 
{
    return new PaulService(cfg);
}

bool PaulService::information_collector(Arc::XMLNode &doc)
{
    // XXX make it system dependet and plugable
    Arc::XMLNode ad = doc.NewChild("AdminDomain");
    ad.NewChild("ID") = "foobar"; // XXX: URI required
    ad.NewChild("Name") = "DesktopPC";
    ad.NewChild("Distriuted") = "no";
    ad.NewChild("Owner") = "someone"; // get user name required
    Arc::XMLNode services = ad.NewChild("Services");
    Arc::XMLNode cs = services.NewChild("ComputingService");
    cs.NewAttribute("type") = "Service";
    cs.NewChild("ID") = "uuid"; // get uuid
    cs.NewChild("Type") = "org.nordugrid.paul";
    cs.NewChild("QualityLevel") = "production";
    cs.NewChild("Complexity") = "endpoint=1,share=1,resource=1";
    cs.NewChild("TotalJobs") = "0"; // XXX get length of jobqueue
    cs.NewChild("RunningJobs") = "0"; // get info from jobqueue
    cs.NewChild("WaitingJobs") = "0"; // get info from jobqueue
    cs.NewChild("StageingJobs") = "0"; // get info from jobqueue
    cs.NewChild("PreLRMSWaitingJobs") = "0"; // get info from jobqueue
    Arc::XMLNode endpoint = cs.NewChild("ComputingEndpoint");
    endpoint.NewAttribute("type") = "Endpoint";
    endpoint.NewChild("ID") = "0"; 
    // endpoint.NewChild("URL") = "none"; // Not exsits
    endpoint.NewChild("Specification") = "iBES";
    endpoint.NewChild("Implementator") = "Nordugrid";
    endpoint.NewChild("ImplementationName") = "paul";
    endpoint.NewChild("ImplementationVersion") = "0.1";
    endpoint.NewChild("QualityLevel") = "production";
    // endpoint.NewChild("WSDL") = "http://"; // not required this side
    endpoint.NewChild("HealthState") = "OK";
    // endpoint.NewChild("ServingState") = "open"; // meaingless
    // endpoint.NewChild("StartTime") = "now";
    endpoint.NewChild("StageingCapabilty") = "yes";
    Arc::XMLNode policy = endpoint.NewChild("Policy");
    policy.NewChild("ID") = "0";
    policy.NewChild("Scheme") = "BASIC";
    policy.NewChild("Rule") = "?";
    policy.NewChild("TrustedCA") = "?"; // XXX get DNs from config ca.pem or dir
#if 0
    Arc::XMLNode activities = endpoint.NewChild("Activities");
    JobList::iterator it;
    for (it = job_list.begin(); it != job_list.end(); it++) {
        Job j = (*it);
        Arc::XMLNode jn = activities.NewChild("ComputingActivity");
        jn.NewAttribute("type") = "Computing";
        jn.NewChild("ID") = j.getID();
        jn.NewChild("Type") = "Computing";
        jn.NewChild("Name") = j.getName(); // from jsdl
        jn.NewChild("State") = (std::string)j.getStatus(); // from metadata
    }
#endif
    Arc::XMLNode r = cs.NewChild("ComputingResource");
    r.NewAttribute("type") = "Resource";
    r.NewChild("ID") = "0";
    r.NewChild("Name") = "DesktopPC";
    r.NewChild("LRMSType") = "Fork/Internal";
    r.NewChild("LRMSVersion") = "0.1";
    r.NewChild("Total") = "1"; // number of exec env
    r.NewChild("Used") = "0"; // comes from running job
    r.NewChild("Homogenity") = "True";
    r.NewChild("SessionDirTotal") = "10000"; // XXX should calculated in MB
    r.NewChild("SessionDirFree") = "10000"; // XXX should calculated in MB
    r.NewChild("SessionDirLifetime") = "10"; // XXX should come from config in sec
    Arc::XMLNode ee = r.NewChild("ExecutionEnvironment");
    ee.NewChild("ID") = "0";
    ee.NewChild("Type") = "RealNode"; // XXX should come form config wheter it is running on VM or not
    ee.NewChild("TotalInstances") = "1";
    ee.NewChild("UsedInstances") = "0"; // if I have multicore machine running 1 job than this is 0 or 1 or what
    ee.NewChild("PhysicalCPUs") = "1"; // XXX should calculated
    ee.NewChild("LogicalCPUs") = "1"; // XXX should calculated
    ee.NewChild("MainMemorySize") = "512"; // XXX should calculated in MB
    ee.NewChild("VirtualMemorySize") = "512"; // XXX should calculated in MB
    ee.NewChild("OSName") = "Linux"; // XXX should calculated
    ee.NewChild("OSRelease") = "Debian"; // XXX should calculated
    ee.NewChild("OSVersion") = "3.1"; // XXX should calculated
    ee.NewChild("PlatformType") = "Intel"; // XXX should calculated
    ee.NewChild("ConnectivityIn") = "False";
    ee.NewChild("ConnectivityOut") = "True";
    r.NewChild("PhysicalCPUs") = "1"; // XXX should calculated: sum of physical cpus in execution environments
    r.NewChild("LogicalCPUs") = "1"; // XXX should calculated: sum of logical cpus in execution environments
    Arc::XMLNode s = cs.NewChild("ComputingShare");
    s.NewAttribute("type") = "Share";
    s.NewChild("ID") = "0";
    s.NewChild("Name") = "AllResources";
    Arc::XMLNode spolicy = s.NewChild("ComputingSharePolicy");
    spolicy.NewAttribute("type") = "SharePolicy";
    Arc::XMLNode sstate = s.NewChild("ComputingShareState");
    sstate.NewAttribute("type") = "ShareState";
    sstate.NewChild("TotalJobs") = "0";
    sstate.NewChild("RunningJobs") = "0";
    sstate.NewChild("LocalRunningJobs") = "0";
    sstate.NewChild("WaitingJobs") = "0";
    sstate.NewChild("State") = "?";
    s.NewChild(policy); // use the same policy
    return true;
}

std::list<Job> PaulService::GetActivities(std::string &url_str)
{
    std::list<Job> ret;
    // Collect information about resources
    // and create Glue compatibile Resource description
    Arc::NS glue2_ns;
    glue2_ns["glue2"] = "http://glue2";
    Arc::XMLNode glue2(glue2_ns, "Grid");
    if (information_collector(glue2) == false) {
        logger_.msg(Arc::ERROR, "Cannot collect resource information");
        return ret;
    }
    std::string str;
    // glue2.GetXML(str);
    // std::cout << str << std::endl;
    // Create client to url
    Arc::ClientSOAP *client;
    Arc::MCCConfig cfg;
    Arc::URL url(url_str);
    if (url.Protocol() == "https") {
        cfg.AddPrivateKey(pki["PrivateKey"]);
        cfg.AddCertificate(pki["CertificatePath"]);
        cfg.AddCAFile(pki["CACertificatePath"]);
    }
    client = new Arc::ClientSOAP(cfg, url.Host(), url.Port(), url.Protocol() == "https", url.Path());
    // invoke GetActivity SOAP call
    Arc::PayloadSOAP request(ns_);
    request.NewChild("ibes:GetActivities").NewChild(glue2);
    Arc::PayloadSOAP *response;
    Arc::MCC_Status status = client->process(&request, &response);
    if (!status) {
        logger_.msg(Arc::ERROR, "Request faild");
        if (response) {
           response->GetXML(str);
           std::cout << str << std::endl;
           delete response;
        }
        return ret;
    }
    if (!response) {
        logger_.msg(Arc::ERROR, "No response");
        return ret;
    }
    
    // Handle soap level error
    Arc::XMLNode fs;
    (*response)["Fault"]["faultstring"].New(fs);
    std::string faultstring = (std::string)fs;
    if (faultstring != "") {
        logger_.msg(Arc::ERROR, faultstring);
        return ret;
    }
    // Create jobs from response
    Arc::XMLNode activities;
    activities = (*response)["GetActivitiesResponse"];
    activities.GetXML(str);
    logger_.msg(Arc::DEBUG, str);
    /* Arc::XMLNode activity;
    for (int i = 0; (activity = activities.Child(i)) != false; i++) {
        JobRequest jr;
        activity.New(jr);
        Job j(jr);
        JobStatus status(ACCEPTING);
        j.setStatus(status);
        ret.push_back(j);
    } */
    return ret;
}

void PaulService::do_request(void)
{
    std::string url = (*schedulers.begin());
    logger_.msg(Arc::DEBUG, "Do Request: %s", url);
    // pickup scheduler randomly from schdeuler list
    std::list<Job> jobs = GetActivities(url);
    std::list<Job>::iterator it;
    for (it = jobs.begin(); it != jobs.end(); it++) {
        Job j = *it;
        std::cout << j.getID() << std::endl;
    }
    // invoke GetActivity function call aganst scheduler
    // if there was error: report and return
    // put job into queue
    // create thread to start and mointor job
    // return
}

// Main request loop
static void request_loop(void* arg) 
{
    PaulService *self = (PaulService *)arg;
    
    for (;;) {
        self->do_request();
        sleep(self->get_period());       
    }
}

// Constructor
PaulService::PaulService(Arc::Config *cfg):Service(cfg),logger_(Arc::Logger::rootLogger, "Paul") 
{
    // Define supported namespaces
    ns_["ibes"] = "http://www.nordugrid.org/schemas/ibes";
    ns_["glue2"] = "http://glue2";

    std::string sched_endpoint = (std::string)((*cfg)["SchedulerEndpoint"]);
    schedulers.push_back(sched_endpoint);
    period = Arc::stringtoi((std::string)((*cfg)["RequestPeriod"]));
    db_path = (std::string)((*cfg)["DataDirectoryPath"]);
    timeout = Arc::stringtoi((std::string)((*cfg)["Timeout"]));
    pki["CertificatePath"] = (std::string)((*cfg)["CertificatePath"]);
    pki["PrivateKey"] = (std::string)((*cfg)["PrivateKey"]);  
    pki["CACertificatePath"] = (std::string)((*cfg)["CACertificatePath"]);  
    Arc::CreateThreadFunction(&request_loop, this);
}

// Destructor
PaulService::~PaulService(void) 
{
    // NOP
}

}; // namespace Paul

service_descriptors ARC_SERVICE_LOADER = {
    { "paul", 0, &Paul::get_service },
    { NULL, 0, NULL }
};
