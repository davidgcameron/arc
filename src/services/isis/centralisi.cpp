#ifdef HAVE_CONFIG_H
#include <config.h>
#endif

#include <arc/loader/Loader.h>
#include <arc/loader/ServiceLoader.h>
#include <arc/message/PayloadSOAP.h>
#include <arc/ws-addressing/WSA.h>
#include <arc/URL.h>
#include <arc/Thread.h>
#include <glibmm.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>
#include <fstream>

#include "centralisi.h"

namespace CentralISI {

static Arc::Service *get_service(Arc::Config *cfg, Arc::ChainContext *) { 
    return new CentralISIService(cfg);
}

static void infocollector_thread(void *arg)
{
    if (!arg) return;
    CentralISIService *srv = (CentralISIService *)arg;
    srv->InformationCollector();
}

/*
static void register_thread(void *arg)
{
    if (!arg) return;
    CentralISIService *srv = (CentralISIService *)arg;
    srv->reg->registration_forever();
}
*/

void CentralISIService::InformationCollector(void)
{
    for (;;) {
        logger.msg(Arc::DEBUG, "Information collector");
        // Create infodoc
        Arc::NS ns;
        Arc::XMLNode node(ns, "InfoDoc");
        Arc::XMLNode service_node = node.NewChild("Service");
        service_node.NewChild("ID") = service_id;
        service_node.NewChild("Name") = "CentralISIService";
        service_node.NewChild("Type") = "InformationIndexingService";
        Arc::XMLNode ep = service_node.NewChild("Endpoint");
        ep.NewChild("ID") = "0";
        ep.NewChild("URL") = "https://localhost:60000/isis";
        // Write data to info cache
        icache->Set("/", node);
        sleep(60); // XXX: make it configurable
    }
}

CentralISIService::CentralISIService(Arc::Config *cfg):Service(cfg),logger(Arc::Logger::rootLogger, "CentralISI")
{
    // Define supported namespaces
    ns["isis"]="http://www.nordugrid.org/schemas/isis/2007/06";
    ns["wsa"]="http://www.w3.org/2005/08/addressing";
    ns["wsrf-bf"]="http://docs.oasis-open.org/wsrf/bf-2";
    ns["wsrf-rp"]="http://docs.oasis-open.org/wsrf/rp-2";
    ns["wsrf-rpw"]="http://docs.oasis-open.org/wsrf/rpw-2";
    ns["wsrf-rw"]="http://docs.oasis-open.org/wsrf/rw-2";
    
    logger.msg(Arc::DEBUG, "Central ISIS initialized");
    std::string dsn = (std::string)(*cfg)["DSN"];
    Arc::URL dsn_url(dsn);
    if (dsn_url.Protocol() != "file") {
        logger.msg(Arc::ERROR, "remote database not supported");
    } else {
        db_path = dsn_url.Path();
        logger.msg(Arc::INFO, "Intialize database %s", db_path.c_str());
        if (!Glib::file_test(db_path, Glib::FILE_TEST_IS_DIR)) {
            // create root directory
#ifndef WIN32
            if (mkdir(db_path.c_str(), 0700) != 0) 
#else
            if (mkdir(db_path.c_str()) != 0) 
#endif
            {
                logger.msg(Arc::ERROR, "cannot create directory: %s", db_path.c_str());
                db_path = "";
            }        
        }
    }
    std::string s_reg_period = (std::string)(*cfg)["Register"]["Period"];
    long int reg_period = strtol(s_reg_period.c_str(), NULL, 10);
    service_id = (std::string)(*cfg)["ID"];
    reg = new Arc::Register(service_id, reg_period, (*cfg));
    icache = new Arc::InfoCache(*cfg, service_id);
    // CreateThreadFunction(&register_thread, this);
    CreateThreadFunction(&infocollector_thread, this);
}

CentralISIService::~CentralISIService(void)
{
    // delete reg;
    delete icache;
}

Arc::MCC_Status CentralISIService::process(Arc::Message &inmsg, Arc::Message &outmsg)
{
    logger.msg(Arc::DEBUG, "process called");
    if (db_path == "") {
        logger.msg(Arc::ERROR, "invalid database path");
        return Arc::MCC_Status();
    }
    // Both input and output are supposed to be SOAP
    // Extracting payload
    Arc::PayloadSOAP* inpayload = NULL;
    try {
      inpayload = dynamic_cast<Arc::PayloadSOAP*>(inmsg.Payload());
    } catch(std::exception& e) { };
    if(!inpayload) {
      logger.msg(Arc::ERROR, "input is not SOAP");
      return make_soap_fault(outmsg);
    };
    // Analyzing request
    Arc::XMLNode op = inpayload->Child(0);
    if(!op) {
      logger.msg(Arc::ERROR, "input does not define operation");
      return make_soap_fault(outmsg);
    }; 
    logger.msg(Arc::DEBUG, "process: operation: %s", op.Name().c_str());
    Arc::PayloadSOAP* outpayload = new Arc::PayloadSOAP(ns);
    Arc::PayloadSOAP& res = *outpayload;
    Arc::MCC_Status ret = Arc::STATUS_OK;
    if(MatchXMLName(op, "Register")) {
        Arc::XMLNode r = res.NewChild("isis:RegisterResponse");
        ret = Register(op, r);
    } else if(MatchXMLName(op, "RemoveRegistrations")) {
        Arc::XMLNode r = res.NewChild("isis:RemoveRegistrationsResponse");
        ret = RemoveRegistrations(op, r);
    } else if(MatchXMLName(op, "GetRegistrationStatuses")) {
        Arc::XMLNode r = res.NewChild("isis:GetRegistrationStatusesResponse");
        ret = GetRegistrationStatuses(op, r);
    } else if(MatchXMLName(op, "GetIISList")) {
        Arc::XMLNode r = res.NewChild("isis:GetIISListResponse");
        ret = GetIISList(op, r);
    } else if(MatchXMLName(op, "Get")) {
        Arc::XMLNode r = res.NewChild("isis:GetResponse");
        ret = Get(op, r);
    } else if(MatchXMLName(op, "DelegateCredentialsInit")) {
        if(!delegation.DelegateCredentialsInit(*inpayload,*outpayload)) {
          delete inpayload;
          return make_soap_fault(outmsg);
        }
    // WS-Property
    } else if(MatchXMLNamespace(op, "http://docs.oasis-open.org/wsrf/rp-2")) {
        Arc::SOAPEnvelope* out_ = infodoc.Process(*inpayload);
        if(out_) {
          *outpayload=*out_;
          delete out_;
        } else {
          delete inpayload; delete outpayload;
          return make_soap_fault(outmsg);
        };
    } else {
        logger.msg(Arc::ERROR,"SOAP operation is not supported: %s", op.Name().c_str());
        return make_soap_fault(outmsg);
    };
    // Set output
    outmsg.Payload(outpayload);
    return Arc::MCC_Status(ret);
}

Arc::MCC_Status CentralISIService::make_soap_fault(Arc::Message& outmsg)
{
    Arc::PayloadSOAP* outpayload = new Arc::PayloadSOAP(ns,true);
    Arc::SOAPFault* fault = outpayload?outpayload->Fault():NULL;
    if(fault) {
        fault->Code(Arc::SOAPFault::Sender);
        fault->Reason("Failed processing request");
    };
    outmsg.Payload(outpayload);
    return Arc::MCC_Status(Arc::STATUS_OK);
}

Arc::MCC_Status CentralISIService::Register(Arc::XMLNode &in, Arc::XMLNode &/*out*/)
{
    int i;
    Arc::XMLNode entry;
    for (i = 0; (entry = in["RegEntry"][i]) == true; i++) {
        std::string id = (std::string)entry["ID"];
        std::string data;
        entry.GetXML(data);
        std::string entry_file = Glib::build_filename(db_path, id);
        std::ofstream out(entry_file.c_str(), std::ios::out);
        if (!out) {
            continue;
        }
        out << "<?xml version=\"1.0\" encoding=\"utf-8\"?>" << std::endl;
        out << data;
        out.close();
    }
    return Arc::MCC_Status(Arc::STATUS_OK);
}

Arc::MCC_Status CentralISIService::RemoveRegistrations(Arc::XMLNode &in, Arc::XMLNode &/*out*/)
{
    int i;
    Arc::XMLNode entry;
    for (i = 0; (entry = in["ID"][i]) == true; i++) {
        std::string id = (std::string)entry;
        std::string path = Glib::build_filename(db_path, id);
        unlink(path.c_str());
    }
    return Arc::MCC_Status(Arc::STATUS_OK);
}

Arc::MCC_Status CentralISIService::GetRegistrationStatuses(Arc::XMLNode &in, Arc::XMLNode &out)
{
    int i;
    Arc::XMLNode entry;
    for (i = 0; (entry = in["ID"][i]) == true; i++) {
        std::string id = (std::string)entry;
        std::string path = Glib::build_filename(db_path, id);
        if (Glib::file_test(path, Glib::FILE_TEST_EXISTS) == true) {
            std::string data = Glib::file_get_contents(path);
            Arc::XMLNode d(data);
            Arc::XMLNode status = d["MetaSrcAdv"]["Status"];
            if(status == true) {
                out.NewChild("RegEntry");
                out["RegEntry"].NewChild(entry);
                out["RegEntry"].NewChild(status);
            }
        }
    }
    std::string str;
    out.GetXML(str);
    return Arc::MCC_Status(Arc::STATUS_OK);
}

Arc::MCC_Status CentralISIService::GetIISList(Arc::XMLNode &/*in*/, Arc::XMLNode &/*out*/)
{
    return Arc::MCC_Status();
}

Arc::MCC_Status CentralISIService::Get(Arc::XMLNode &/*in*/, Arc::XMLNode &/*out*/)
{
    return Arc::MCC_Status();
}

} // namespace CentralISI

service_descriptors ARC_SERVICE_LOADER = {
    { "centralisi", 0, &CentralISI::get_service },
    { NULL, 0, NULL }
};
