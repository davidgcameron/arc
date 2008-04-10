#ifdef HAVE_CONFIG_H
#include <config.h>
#endif

#include <arc/URL.h>
#include <arc/message/PayloadSOAP.h>
#include "InfoRegister.h"
#ifdef WIN32
#define NOGDI
#include <objbase.h>
#endif

#include <unistd.h>

namespace Arc
{

InfoRegister::InfoRegister(const std::string &sid, long int period, Arc::Config &cfg):logger(Arc::Logger::rootLogger, "InfoRegister")
{
    ns["isis"] = "http://www.nordugrid.org/schemas/isis/2007/06";
    service_id = sid;
    reg_period = period;
    // XXX how to get plugin paths from cfg?
    mcc_cfg.AddPluginsPath("../../hed/mcc/tcp/.libs/");
    mcc_cfg.AddPluginsPath("../../hed/mcc/tls/.libs/");
    mcc_cfg.AddPluginsPath("../../hed/mcc/soap/.libs/");
    mcc_cfg.AddPluginsPath("../../hed/mcc/http/.libs/");
}

void InfoRegister::AddUrl(const std::string &url)
{
    urls.push_back(url);
}

void InfoRegister::registration_forever(void)
{
    for (;;) {
        registration();
#ifndef WIN32
        sleep(reg_period);
#else
	Sleep(reg_period*1000);
#endif
    }
}

void InfoRegister::registration(void)
{
    std::list<std::string>::iterator it;

    for (it = urls.begin(); it != urls.end(); it++) {
        Arc::URL u(*it);
        const char *isis_name = (*it).c_str();
        bool tls;
        if (u.Protocol() == "http") {
            tls = false;
        } else if (u.Protocol() == "https") {
            tls = true;
        } else {
            logger.msg(Arc::WARNING, "unsupported protocol: %s", isis_name);
            continue;
        }
        cli = new Arc::ClientSOAP(mcc_cfg, u.Host(), u.Port(), tls, u.Path());
        logger.msg(Arc::DEBUG, "Start registration to %s ISIS", isis_name);
        Arc::PayloadSOAP request(ns);
        Arc::XMLNode op = request.NewChild("isis:Register");
        op.NewChild("Header").NewChild("RequesterID") = service_id;
        Arc::XMLNode re = op.NewChild("RegEntry");
        re.NewChild("ID") = service_id;
        Arc::XMLNode sa = re.NewChild("SrvAdv");
        // sa.NewChild("Type") = 
        // XXX read data from InfoCache
        // request.NewChild()
        Arc::PayloadSOAP *response;
        Arc::MCC_Status status = cli->process(&request, &response);
        if (!status) {
            if (response) {
                // XXX process response
                logger.msg(Arc::DEBUG, "Succesful registration to %s ISIS", isis_name); 
            } else {
                logger.msg(Arc::ERROR, "Error during registration to %s ISIS", isis_name);
            }
        } else {
            logger.msg(Arc::ERROR, "Error during registration to %s ISIS", isis_name);
        }
        
        delete cli;
    }
}

} // namespace
