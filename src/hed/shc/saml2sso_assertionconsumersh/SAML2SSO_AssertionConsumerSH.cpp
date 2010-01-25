#ifdef HAVE_CONFIG_H
#include <config.h>
#endif

#include <iostream>
#include <fstream>

#include <arc/message/PayloadSOAP.h>
#include <arc/message/PayloadRaw.h>
#include <arc/URL.h>
#include <arc/DateTime.h>
#include <arc/GUID.h>
#include <arc/XMLNode.h>
#include <arc/xmlsec/XmlSecUtils.h>
#include <arc/xmlsec/saml_util.h>

#include "SAML2SSO_AssertionConsumerSH.h"

static Arc::Logger logger(Arc::Logger::rootLogger, "SAMLSSO_AssertionConsumerSH");

Arc::Plugin* ArcSec::SAML2SSO_AssertionConsumerSH::get_sechandler(Arc::PluginArgument* arg) {
    ArcSec::SecHandlerPluginArgument* shcarg =
            arg?dynamic_cast<ArcSec::SecHandlerPluginArgument*>(arg):NULL;
    if(!shcarg) return NULL;
    return new ArcSec::SAML2SSO_AssertionConsumerSH((Arc::Config*)(*shcarg),(Arc::ChainContext*)(*shcarg));
}

/*
sechandler_descriptors ARC_SECHANDLER_LOADER = {
    { "saml2ssoassertionconsumer.handler", 0, &get_sechandler},
    { NULL, 0, NULL }
};
*/

namespace ArcSec {
using namespace Arc;

SAML2SSO_AssertionConsumerSH::SAML2SSO_AssertionConsumerSH(Config *cfg,ChainContext*):SecHandler(cfg), SP_service_loader(NULL){
  if(!init_xmlsec()) return;
}

SAML2SSO_AssertionConsumerSH::~SAML2SSO_AssertionConsumerSH() {
  final_xmlsec();
  if(SP_service_loader) delete SP_service_loader;
}

bool SAML2SSO_AssertionConsumerSH::Handle(Arc::Message* msg) const {
  //Explaination: The SPService checks the authentication result (generated by IdP and sent by 
  //user agent) and records those successful authentication result into "SAMLAssertion". Since 
  //the real client (client to normal service) and user agent (client to SP service) share the 
  //same tcp/tls session (this is why only one copy of base class ClientTCP is needed in the 
  //ClientSAMLInterface), we can use the authentication result from user-agent/SPService for 
  //the decision-making in normal Client/Service interaction.
  //
  //Here the message which includes the endpoint "/saml2sp" is avoided, because
  //this specific endpoint is supposed to participate the SAML2 SSO prifile, check 
  //the authentication result from IdP and record the authentication information 
  //for later saml2ssp_serviceprovider handler, and itself should not be enforced 
  //the saml2sso_serviceprovider handler.
  std::string http_endpoint = msg->Attributes()->get("HTTP:ENDPOINT");
  std::size_t pos = http_endpoint.find("saml2sp");
  if(pos == std::string::npos) {  
    SecAttr* sattr = msg->Auth()->get("SAMLAssertion");
    if(!sattr) {
      logger.msg(ERROR, "Can not get SAMLAssertion SecAttr from message context");
      return false;
    }

    std::string str;
    XMLNode saml_assertion_nd;
    if(!(sattr->Export(SecAttr::SAML, saml_assertion_nd))) return false;
    saml_assertion_nd.GetXML(str);
    std::cout<<"SAML Assertion parsed by SP service: "<<str<<std::endl;



    return true;
  }
  else { return true; }

  return false;

  //TODO: Consume the saml assertion from client side (Push model): assertion inside soap message,
  //assertion inside x509 certificate as exention;
  //Or contact the IdP and get back the saml assertion related to the client(Pull model)

}

}


