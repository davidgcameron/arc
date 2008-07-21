#ifdef HAVE_CONFIG_H
#include <config.h>
#endif

#include <unistd.h>

#include <arc/Thread.h>
#include <arc/URL.h>
#include <arc/message/PayloadSOAP.h>
#include "InfoRegister.h"
#ifdef WIN32
#include <arc/win32.h>
#endif

namespace Arc
{

static void reg_thread(void *data)
{
    Arc::InfoRegister *self = (Arc::InfoRegister *)data;
    for (;;) {
        sleep(self->getPeriod());
        self->registration();
    }
}

InfoRegister::InfoRegister(Arc::XMLNode &cfg, Arc::Service *service):logger_(Logger::rootLogger, "InfoRegister"),reg_period_(0)
{
    service_ = service;
    ns_["isis"] = ISIS_NAMESPACE;
    ns_["glue2"] = GLUE2_D42_NAMESPACE;
    ns_["register"] = REGISTRATION_NAMESPACE;

    // parse config tree
    std::string s_reg_period = (std::string)cfg["Period"];
    if (!s_reg_period.empty()) { 
        reg_period_ = strtol(s_reg_period.c_str(), NULL, 10);
    }
    Arc::XMLNode peers = cfg["Peers"];
    Arc::XMLNode n;
    for (int i = 0; (n = peers["URL"][i]) != false; i++) {
         std::string url_str = (std::string)n;
         Arc::URL url(url_str);
         peers_.push_back(url);
    }
    
    // Start Registartion thread
    if (reg_period_ > 0) {
        Arc::CreateThreadFunction(&reg_thread, this);
    }
}

InfoRegister::~InfoRegister(void)
{
    // Service should not delete here!
    // Nope
}

void InfoRegister::registration(void)
{
    logger_.msg(Arc::DEBUG, "Registartion Start");
    if (service_ == NULL) {
        logger_.msg(Arc::ERROR, "Invalid service");
        return;
    }
    Arc::NS reg_ns;
    reg_ns["glue2"] = ns_["glue2"];
    reg_ns["isis"] = ns_["isis"];
    Arc::XMLNode reg_doc(reg_ns, "isis:Advertisements");
    Arc::XMLNode services_doc(reg_ns, "Services");
    service_->RegistrationCollector(services_doc);
    // Check where is service registartion
    std::list<Arc::XMLNode> services = services_doc.XPathLookup("//*[normalize-space(@BaseType)='Service']", reg_ns);
    if (services.size() == 0) {
        logger_.msg(Arc::WARNING, "No service registartion entries");
        return;
    }
    // create advertisement
    std::list<Arc::XMLNode>::iterator sit;
    for (sit = services.begin(); sit != services.end(); sit++) {
        // filter may come here
        Arc::XMLNode ad = reg_doc.NewChild("isis:Advertisement");
        ad.NewChild((*sit));
        // XXX metadata
    }
    
    // send to all peer
    std::list<Arc::URL>::iterator it;
    for (it = peers_.begin(); it != peers_.end(); it++) {
        std::string isis_name = (*it).fullstr();
        bool tls;
        if ((*it).Protocol() == "http") {
            tls = false;
        } else if ((*it).Protocol() == "https") {
            tls = true;
        } else {
            logger_.msg(Arc::WARNING, "unsupported protocol: %s", isis_name);
            continue;
        }
        cli_ = new Arc::ClientSOAP(mcc_cfg_, (*it).Host(), (*it).Port(), tls, (*it).Path());
        logger_.msg(DEBUG, "Registering to %s ISIS", isis_name);
        Arc::PayloadSOAP request(ns_);
        Arc::XMLNode op = request.NewChild("isis:Register");
        
        // create header
        op.NewChild("isis:Header").NewChild("isis:SourcePath").NewChild("isis:ID") = service_->getID();
        
        // create body
        op.NewChild(reg_doc);   
        
        // send
        Arc::PayloadSOAP *response;
        Arc::MCC_Status status = cli_->process(&request, &response);
        if ((!status) || (!response)) {
            logger_.msg(ERROR, "Error during registration to %s ISIS", isis_name);
         } else {
            Arc::XMLNode fault = (*response)["Fault"];
            if(!fault)  {
                logger_.msg(DEBUG, "Successful registration to ISIS (%s)", isis_name); 
            } else {
                logger_.msg(DEBUG, "Failed to register to ISIS (%s) - %s", isis_name, std::string(fault["Description"])); 
            }
        }
        
        delete cli_;
        cli_ = NULL;
    }
}

} // namespace


